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
