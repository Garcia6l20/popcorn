#include <popcorn/core/trace.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <meta>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;

auto block(std::uint32_t pc) -> core::trace_record
{
  return core::trace_record{
      .kind = core::trace_kind::block, .aux = 4, .payload = {pc, 0, 0}
  };
}
}

static_assert(std::is_trivially_copyable_v<core::trace_record>);
static_assert(not std::is_copy_constructible_v<core::trace_ring>);
static_assert(not std::is_move_constructible_v<core::trace_ring>);

TEST_CASE("trace record matches the largest fixed pctr record")
{
  CHECK(sizeof(core::trace_record) == 20);
  CHECK(std::to_underlying(core::trace_kind::block) == 0);
  CHECK(std::to_underlying(core::trace_kind::time) == 6);
  CHECK(std::to_underlying(core::trace_kind::stop) == 7);
  static_assert(core::trace_id(0xfffe) == 0xfffe);
  static_assert(core::trace_id(0xffff) == core::trace_id_overflow);
  static_assert(core::trace_id(4717) == 4717);
  CHECK(core::pctr_payload_words(core::trace_kind::stop) == 1);
  CHECK(std::format("{}", core::trace_kind::mmio) == "mmio");
  CHECK(core::pctr_header_size + 4 * core::pctr_payload_words(core::trace_kind::block) == 12);
  CHECK(core::pctr_payload_words(core::trace_kind::mem) == 3);
  CHECK(core::pctr_payload_words(core::trace_kind::irq) == 0);
  CHECK(core::pctr_payload_words(core::trace_kind::time) == 2);
}

TEST_CASE("trace kinds hold every kind reflection enumerates")
{
  constexpr auto every = core::trace_kinds::all();
  template for(constexpr auto e :
               std::define_static_array(std::meta::enumerators_of(^^core::trace_kind)))
  {
    CHECK(every.contains([:e:]));
  }
  constexpr core::trace_kinds some{core::trace_kind::irq, core::trace_kind::call};
  static_assert(some.contains(core::trace_kind::irq));
  static_assert(not some.contains(core::trace_kind::block));
  static_assert(core::trace_kinds{} != some);
  CHECK_FALSE(core::trace_kinds{}.contains(core::trace_kind::irq));
}

TEST_CASE("trace ring stamps deltas and keeps the newest records")
{
  core::trace_ring ring{3};
  CHECK(ring.capacity() == 3);
  CHECK(ring.size() == 0);

  ring.append(block(0x100), popcorn::icount{10});
  ring.append(block(0x104), popcorn::icount{15});
  auto records = ring.records();
  REQUIRE(records.size() == 2);
  CHECK(records[0].dt == 10);
  CHECK(records[1].dt == 5);
  CHECK(ring.origin() == popcorn::icount{0});

  ring.append(block(0x108), popcorn::icount{22});
  ring.append(block(0x10c), popcorn::icount{30});
  records = ring.records();
  REQUIRE(records.size() == 3);
  CHECK(ring.appended() == 4);
  CHECK(records[0].payload[0] == 0x104);
  CHECK(records[2].payload[0] == 0x10c);
  CHECK(ring.origin() == popcorn::icount{10});
  CHECK(ring.origin().value + records[0].dt == 15);
}

TEST_CASE("trace ring writes a time record when a delta overflows dt")
{
  core::trace_ring    ring{4};
  std::uint64_t const far = 0x1'2345'6789;
  ring.append(block(0x100), popcorn::icount{far});
  auto const records = ring.records();
  REQUIRE(records.size() == 2);
  CHECK(records[0].kind == core::trace_kind::time);
  CHECK(records[0].payload[0] == 0x2345'6789);
  CHECK(records[0].payload[1] == 1);
  CHECK(records[1].dt == 0);

  ring.append(block(0x104), popcorn::icount{far + 3});
  ring.append(block(0x108), popcorn::icount{far + 4});
  ring.append(block(0x10c), popcorn::icount{far + 9});
  CHECK(ring.origin() == popcorn::icount{far});
  CHECK(ring.records().front().payload[0] == 0x100);

  ring.clear(popcorn::icount{far + 100});
  CHECK(ring.size() == 0);
  CHECK(ring.appended() == 0);
  ring.append(block(0x110), popcorn::icount{far + 102});
  CHECK(ring.records().front().dt == 2);
  CHECK(ring.origin() == popcorn::icount{far + 100});
}

TEST_CASE("trace ring rebases after a restore moves time backwards")
{
  core::trace_ring ring{8};
  ring.append(block(0x100), popcorn::icount{100});
  ring.rebase(popcorn::icount{20});
  ring.append(block(0x104), popcorn::icount{25});
  auto records = ring.records();
  REQUIRE(records.size() == 3);
  CHECK(records[0].dt == 100);
  CHECK(records[1].kind == core::trace_kind::time);
  CHECK(records[1].payload[0] == 20);
  CHECK(records[2].dt == 5);

  ring.append(block(0x108), popcorn::icount{10});
  records = ring.records();
  REQUIRE(records.size() == 5);
  CHECK(records[3].kind == core::trace_kind::time);
  CHECK(records[3].payload[0] == 10);
  CHECK(records[4].dt == 0);
}

TEST_CASE("trace ring moves the symbol ids of kept call records past an insertion point")
{
  core::trace_ring ring{4};
  auto const       call = [](std::uint16_t id) {
    return core::trace_record{.kind = core::trace_kind::call, .aux = id};
  };
  ring.append(call(3), popcorn::icount{1});
  ring.append(call(7), popcorn::icount{2});
  ring.append(call(0xfffe), popcorn::icount{3});
  ring.append({.kind = core::trace_kind::mmio, .aux = 9}, popcorn::icount{4});
  ring.shift_call_symbols(5, 2);
  auto const records = ring.records();
  REQUIRE(records.size() == 4);
  CHECK(records[0].aux == 3);
  CHECK(records[1].aux == 9);
  CHECK(records[2].aux == core::trace_id_overflow);
  CHECK(records[3].aux == 9);
}

TEST_CASE("trace ring shift moves ids from the insertion point on, across a wrapped ring")
{
  constexpr std::uint16_t from = 5;
  core::trace_ring        ring{4};
  auto const              call = [](std::uint16_t id) {
    return core::trace_record{.kind = core::trace_kind::call, .aux = id};
  };
  ring.append(call(from), popcorn::icount{1});
  ring.append(call(from), popcorn::icount{2});
  ring.append(call(from - 1), popcorn::icount{3});
  ring.append(call(from), popcorn::icount{4});
  ring.append(call(from + 3), popcorn::icount{5});
  ring.append(call(from), popcorn::icount{6});
  REQUIRE(ring.appended() == 6);
  ring.shift_call_symbols(from, 2);
  auto const records = ring.records();
  REQUIRE(records.size() == 4);
  CHECK(records[0].aux == from - 1);
  CHECK(records[1].aux == from + 2);
  CHECK(records[2].aux == from + 5);
  CHECK(records[3].aux == from + 2);
}

TEST_CASE("a trace ring keeps the newest records of the full history at every capacity")
{
  constexpr std::uint32_t total = 70;
  auto const              when  = [](std::uint32_t i) {
    return popcorn::icount{std::uint64_t{i} * 3 + (i % 7 == 0 ? 5'000'000'000ull : 0)};
  };
  core::trace_ring full{total * 2};
  for(std::uint32_t i = 0; i < total; ++i)
  {
    full.append(block(i), when(i));
  }
  auto const history = full.records();
  REQUIRE(full.origin().value == 0);
  for(std::size_t const capacity : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 16u, 31u, 32u, 33u, 69u, 70u})
  {
    core::trace_ring ring{capacity};
    for(std::uint32_t i = 0; i < total; ++i)
    {
      ring.append(block(i), when(i));
    }
    REQUIRE(ring.appended() == history.size());
    auto const kept    = ring.records();
    auto const dropped = history.size() - capacity;
    REQUIRE(kept.size() == capacity);
    auto origin = popcorn::icount{0};
    for(std::size_t i = 0; i < dropped; ++i)
    {
      auto const& gone = history[i];
      origin           = gone.kind == core::trace_kind::time
                           ? popcorn::icount{gone.payload[0] | std::uint64_t{gone.payload[1]} << 32}
                           : popcorn::icount{origin.value + gone.dt};
    }
    CHECK(ring.origin() == origin);
    for(std::size_t i = 0; i < capacity; ++i)
    {
      CHECK(kept[i] == history[dropped + i]);
    }
  }
}
