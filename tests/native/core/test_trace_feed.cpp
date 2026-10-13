#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/trace.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace arch = popcorn::arch;
using core::trace_kind;
using core::trace_record;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address unmapped_page = 0x3000'0000;

auto of_kind(std::vector<trace_record> const& records, trace_kind kind) -> std::vector<trace_record>
{
  return records
       | std::views::filter([kind](trace_record const& r) { return r.kind == kind; })
       | std::ranges::to<std::vector>();
}

struct collector
{
  std::vector<trace_record> records{};
  int                       batches = 0;

  auto sink() -> core::trace_sink
  {
    return [this](std::span<trace_record const> batch) {
      ++batches;
      records.insert(records.end(), batch.begin(), batch.end());
    };
  }

  auto end_of_stream(icount start) const -> icount
  {
    auto at = start;
    for(auto const& record : records)
    {
      at = record.kind == trace_kind::time
             ? icount{record.payload[0] | std::uint64_t{record.payload[1]} << 32}
             : icount{at.value + record.dt};
    }
    return at;
  }
};

void count_block(uc_struct*, std::uint64_t, std::uint32_t, void* user)
{
  ++*static_cast<std::uint64_t*>(user);
}

void note_block(uc_struct*, std::uint64_t addr, std::uint32_t size, void* user)
{
  static_cast<std::vector<std::pair<address, std::uint32_t>>*>(user)->emplace_back(
      static_cast<address>(addr), size);
}

auto ring_blocks_of(fixture const& f) -> std::vector<trace_record>
{
  return of_kind(f.target->trace().records(), trace_kind::block);
}

auto run_alu_noting(fixture& f) -> std::vector<std::pair<address, std::uint32_t>>
{
  std::vector<std::pair<address, std::uint32_t>> noted;
  uc_hook                                        hook = 0;
  REQUIRE(
      uc_hook_add(
          f.module->attached_engine(), &hook, UC_HOOK_BLOCK_ICOUNT,
          reinterpret_cast<void*>(&note_block), &noted, 1, 0)
      == UC_ERR_OK);
  core::slice_request until;
  until.until = sym("alu", "alu_done");
  REQUIRE(f.run_to_stop(until).reason == stop_reason::address);
  uc_hook_del(f.module->attached_engine(), hook);
  return noted;
}

auto alu_fixture() -> fixture
{
  fixture f{m33(), "alu", {}};
  f.set("r0", 1000);
  return f;
}

}

TEST_CASE("the ring holds the CPU's MMIO accesses with their registers")
{
  core::machine_options options;
  fixture               f{m33(), "periph", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.set("pc", sym("periph", "cpu_accesses"));
  auto const before = f.target->trace().appended();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const all  = f.target->trace().records();
  auto const mmio = of_kind(
      std::vector<trace_record>(
          all.end() - static_cast<std::ptrdiff_t>(f.target->trace().appended() - before),
          all.end()),
      trace_kind::mmio);
  REQUIRE(mmio.size() == 10);
  auto const& desc = f.target->description();
  auto const  name = [&](trace_record const& r) {
    auto const reg = popcorn::register_at(desc, r.aux);
    REQUIRE(reg.has_value());
    return desc.peripherals[reg->peripheral].registers[reg->reg].name;
  };
  CHECK(name(mmio[0]) == "RW");
  CHECK((mmio[0].flags & core::trace_write_flag) != 0);
  CHECK((mmio[0].flags & 0x7f) == 4);
  CHECK(mmio[0].payload[1] == 0x4001'0008);
  CHECK(mmio[0].payload[2] == 0xa5a5'a5a5);
  CHECK(name(mmio[1]) == "RW");
  CHECK((mmio[1].flags & core::trace_write_flag) == 0);
  CHECK(mmio[1].payload[2] == 0xa5a5'a5a5);
  CHECK(mmio[2].payload[1] == 0x4001'0009);
  CHECK((mmio[2].flags & 0x7f) == 1);
  CHECK(name(mmio[4]) == "W_MODIFY");
  CHECK(name(mmio[6]) == "R_CLEAR");
  CHECK(mmio[8].payload[1] == 0x5001'0008);
  CHECK(name(mmio[8]) == "RW");
  CHECK(mmio[0].payload[0] == sym("periph", "cpu_accesses"));
}

TEST_CASE("debugger and DMA accesses reach sinks with pc 0, not the ring")
{
  fixture f{m33(), "periph", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  collector  seen;
  auto const sink   = f.target->add_trace_sink({trace_kind::mmio}, seen.sink());
  auto const before = f.target->trace().appended();
  REQUIRE(f.target->mem().write(0x4001'0008, 4, 5));
  REQUIRE(f.module->attached_services().write(0x4001'0008, 4, 6, popcorn::initiator::dma));
  CHECK(f.target->trace().appended() == before);
  f.set("pc", sym("periph", "unmodelled_reads"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(seen.records.size() >= 2);
  CHECK(seen.records[0].payload[0] == 0);
  CHECK(seen.records[0].payload[2] == 5);
  CHECK(seen.records[1].payload[0] == 0);
  CHECK(seen.records[1].payload[2] == 6);
}

TEST_CASE("an intercepted call leaves a call record naming its symbol")
{
  fixture f{m33(), "intercepts", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::reset_request);
  auto const seven = sym("intercepts", "seven");
  auto const hook  = f.target->add_intercept(
      seven, [](core::intercept_call const&) { return core::intercept_result::proceed(); });
  f.set("r0", 0x1234);
  std::ignore = f.target->call(
      {.entry           = sym("intercepts", "call_seven"),
       .returns         = arch::value_kind::i32,
       .abi             = arch::float_abi::soft,
       .return_sentinel = unmapped_page},
      icount{10'000});
  auto const calls = of_kind(f.target->trace().records(), trace_kind::call);
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].payload[0] == seven);
  auto const& symbols = f.target->loaded_images().front().symbols;
  REQUIRE(calls[0].aux < symbols.size());
  CHECK(symbols[calls[0].aux].name == "seven");
}

TEST_CASE("every stop ends with a stop record carrying its reason")
{
  auto                f    = alu_fixture();
  auto const          last = [&] { return f.target->trace().records().back(); };
  core::slice_request until;
  until.until        = sym("alu", "alu_done");
  auto const reached = f.run_to_stop(until);
  CHECK(reached.reason == stop_reason::address);
  CHECK(last().kind == trace_kind::stop);
  CHECK(last().aux == std::to_underlying(stop_reason::address));
  CHECK(last().payload[0] == reached.where.pc);
  f.set("pc", sym("alu", "alu_entry"));
  f.set("r0", 1000);
  core::slice_request budget;
  budget.budget_left = icount{50};
  CHECK(f.run_to_stop(budget).reason == stop_reason::budget);
  CHECK(last().aux == std::to_underlying(stop_reason::budget));
  f.set("pc", unmapped_page);
  CHECK(f.run_to_stop().reason == stop_reason::fault);
  CHECK(last().aux == std::to_underlying(stop_reason::fault));
}

TEST_CASE("a stop names the function holding its pc")
{
  auto                f = alu_fixture();
  core::slice_request request;
  request.until      = sym("alu", "alu_body") + 4;
  auto const stopped = f.run_to_stop(request);
  CHECK(stopped.where.symbol == "alu_body");
  CHECK(stopped.where.offset == 4);
  f.set("pc", unmapped_page);
  auto const fault = f.run_to_stop();
  CHECK(fault.where.symbol.empty());
}

TEST_CASE("a block sink sees every block, with a dt stream summing to the clock")
{
  auto          f       = alu_fixture();
  std::uint64_t counted = 0;
  uc_hook       counter = 0;
  REQUIRE(
      uc_hook_add(
          f.module->attached_engine(), &counter, UC_HOOK_BLOCK_ICOUNT,
          reinterpret_cast<void*>(&count_block), &counted, 1, 0)
      == UC_ERR_OK);
  collector           seen;
  auto const          start = f.target->virtual_clock().now();
  auto const          sink  = f.target->add_trace_sink({trace_kind::block}, seen.sink());
  core::slice_request until;
  until.until = sym("alu", "alu_done");
  REQUIRE(f.run_to_stop(until).reason == stop_reason::address);
  uc_hook_del(f.module->attached_engine(), counter);
  auto const blocks = of_kind(seen.records, trace_kind::block);
  CHECK(blocks.size() == seen.records.size());
  CHECK(blocks.size() == counted);
  CHECK(blocks.size() == 1001);
  std::uint64_t instructions = 0;
  for(auto const& record : blocks)
  {
    instructions += record.aux;
  }
  CHECK(instructions == f.target->virtual_clock().now().value - start.value);
  CHECK(seen.end_of_stream(start) == f.target->virtual_clock().now());
}

TEST_CASE("a sink with ranges gets only the records of code inside them")
{
  auto       f = alu_fixture();
  collector  seen;
  auto const entry = sym("alu", "alu_entry");
  auto const sink  = f.target->add_trace_sink(
      {
          trace_kind::block
  },
      seen.sink(), {{.name = "entry", .start = entry, .size = 4}});
  core::slice_request until;
  until.until = sym("alu", "alu_done");
  REQUIRE(f.run_to_stop(until).reason == stop_reason::address);
  REQUIRE_FALSE(seen.records.empty());
  for(auto const& record : seen.records)
  {
    CHECK(record.payload[0] >= entry);
    CHECK(record.payload[0] < entry + 4);
  }
}

TEST_CASE("a mem sink reports pc, address and value, reads as memory holds them")
{
  fixture f{m33(), "intercepts", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::reset_request);
  auto const counter = sym("intercepts", "counter");
  REQUIRE(f.target->mem().write(counter, 4, 41));
  collector  seen;
  auto const bump = sym("intercepts", "bump");
  auto const sink = f.target->add_trace_sink(
      {
          trace_kind::mem
  },
      seen.sink(), {{.name = "bump", .start = bump, .size = 0x20}});
  f.set("pc", bump);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const at_counter =
      seen.records
      | std::views::filter([&](trace_record const& r) { return r.payload[1] == counter; })
      | std::ranges::to<std::vector>();
  REQUIRE(at_counter.size() == 3);
  CHECK((at_counter[0].flags & core::trace_write_flag) == 0);
  CHECK(at_counter[0].payload[2] == 41);
  CHECK((at_counter[1].flags & core::trace_write_flag) != 0);
  CHECK(at_counter[1].payload[0] == sym("intercepts", "bump_store"));
  CHECK(at_counter[1].payload[2] == 42);
  CHECK(at_counter[2].payload[2] == 42);
  CHECK((at_counter[2].flags & 0x7f) == 4);
}

TEST_CASE("each sink has its own dt stream and sees a time record after a restore")
{
  auto                f = alu_fixture();
  collector           blocks;
  collector           stops;
  auto const          start      = f.target->virtual_clock().now();
  auto const          block_sink = f.target->add_trace_sink({trace_kind::block}, blocks.sink());
  auto const          stop_sink  = f.target->add_trace_sink({trace_kind::stop}, stops.sink());
  core::slice_request budget;
  budget.budget_left = icount{100};
  REQUIRE(f.run_to_stop(budget).reason == stop_reason::budget);
  CHECK(blocks.end_of_stream(start) == f.target->virtual_clock().now());
  CHECK(stops.end_of_stream(start) == f.target->virtual_clock().now());
  CHECK(stops.records.size() == 1);
  auto const taken = f.target->snapshot();
  REQUIRE(f.run_to_stop(budget).reason == stop_reason::budget);
  f.target->restore(taken);
  auto const blocks_before = blocks.records.size();
  auto const stops_before  = stops.records.size();
  REQUIRE(f.run_to_stop(budget).reason == stop_reason::budget);
  CHECK(blocks.records[blocks_before].kind == trace_kind::time);
  CHECK(stops.records[stops_before].kind == trace_kind::time);
  CHECK(blocks.end_of_stream(start) == f.target->virtual_clock().now());
  CHECK(stops.end_of_stream(start) == f.target->virtual_clock().now());
}

TEST_CASE("a sink dropped by a handler delivers its batch once at slice end")
{
  fixture f{m33(), "intercepts", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::reset_request);
  collector                        seen;
  std::optional<core::hook_handle> sink =
      f.target->add_trace_sink({trace_kind::block}, seen.sink());
  auto const hook =
      f.target->add_intercept(sym("intercepts", "seven"), [&](core::intercept_call const&) {
        sink.reset();
        return core::intercept_result::proceed();
      });
  auto const call = [&] {
    std::ignore = f.target->call(
        {.entry           = sym("intercepts", "call_seven"),
         .returns         = arch::value_kind::i32,
         .abi             = arch::float_abi::soft,
         .return_sentinel = unmapped_page},
        icount{10'000});
  };
  call();
  CHECK(seen.batches == 1);
  auto const delivered = seen.records.size();
  CHECK(delivered > 1);
  call();
  CHECK(seen.batches == 1);
  CHECK(seen.records.size() == delivered);
}

TEST_CASE("a sink may drop its handle while a batch is delivered outside a slice")
{
  fixture f{m33(), "periph", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  int                              batches   = 0;
  std::size_t                      delivered = 0;
  std::optional<core::hook_handle> sink;
  sink = f.target->add_trace_sink({trace_kind::mmio}, [&](std::span<trace_record const> batch) {
    sink.reset();
    ++batches;
    delivered += batch.size();
  });
  auto&                     services = f.module->attached_services();
  std::array<std::byte, 64> bytes{};
  for(int round = 0; round < 300; ++round)
  {
    REQUIRE(services.read_block(0x4001'0000, bytes, popcorn::initiator::dma));
  }
  CHECK(batches == 1);
  CHECK(delivered == 4096);
  CHECK_FALSE(sink.has_value());
}

TEST_CASE("a sized symbol that misses the pc does not hide an earlier one that holds it")
{
  fixture               f{m33(), "basic", {}};
  popcorn::image::image extra;
  extra.symbols = {
      {.name  = "outer",
       .value = 0x081f'0100,
       .size  = 0x100,
       .kind  = popcorn::image::symbol_kind::function},
      {.name  = "inner",
       .value = 0x081f'0180,
       .size  = 4,
       .kind  = popcorn::image::symbol_kind::object  },
  };
  f.target->load(extra);
  auto const at = [&](address pc) {
    f.set("pc", pc);
    core::slice_request request;
    request.exact_instructions = icount{0};
    auto const stopped         = f.run(request).stopped;
    REQUIRE(stopped.has_value());
    return stopped->where;
  };
  auto const middle = at(0x081f'0190);
  CHECK(middle.pc == 0x081f'0190);
  CHECK(middle.symbol == "outer");
  CHECK(middle.offset == 0x90);
  auto const inside = at(0x081f'0182);
  CHECK(inside.symbol == "inner");
  CHECK(inside.offset == 2);
  CHECK(at(0x081f'0200).symbol.empty());
}

TEST_CASE("the ring keeps a block record per executed block only while ring_blocks is set")
{
  SUBCASE("set")
  {
    auto f = alu_fixture();
    CHECK_FALSE(f.target->ring_blocks());
    f.target->set_ring_blocks(true);
    CHECK(f.target->ring_blocks());
    auto const start  = f.target->virtual_clock().now();
    auto const noted  = run_alu_noting(f);
    auto const blocks = ring_blocks_of(f);
    REQUIRE(blocks.size() == 1001);
    REQUIRE(blocks.size() == noted.size());
    std::uint64_t instructions = 0;
    for(auto const [record, block] : std::views::zip(blocks, noted))
    {
      CHECK(record.payload[0] == block.first);
      CHECK(record.aux == block.second);
      instructions += record.aux;
    }
    CHECK(instructions == f.target->virtual_clock().now().value - start.value);
    CHECK(f.target->trace().records().back().kind == trace_kind::stop);
  }
  SUBCASE("not set")
  {
    auto f      = alu_fixture();
    std::ignore = run_alu_noting(f);
    CHECK(ring_blocks_of(f).empty());
    CHECK(f.target->trace().records().back().kind == trace_kind::stop);
  }
}

TEST_CASE("a block sink and the ring switch see each block once, apart")
{
  auto       f = alu_fixture();
  collector  seen;
  auto const sink = f.target->add_trace_sink({trace_kind::block}, seen.sink());
  SUBCASE("both")
  {
    f.target->set_ring_blocks(true);
    auto const noted = run_alu_noting(f);
    CHECK(of_kind(seen.records, trace_kind::block).size() == noted.size());
    CHECK(ring_blocks_of(f).size() == noted.size());
  }
  SUBCASE("sink only")
  {
    auto const noted = run_alu_noting(f);
    CHECK(of_kind(seen.records, trace_kind::block).size() == noted.size());
    CHECK(ring_blocks_of(f).empty());
  }
}

TEST_CASE("ring_blocks is refused from a handler and kept across a restore")
{
  auto       f     = alu_fixture();
  auto const taken = f.target->snapshot();
  f.target->set_ring_blocks(true);
  bool       refused = false;
  auto const sink =
      f.target->add_trace_sink({trace_kind::stop}, [&](std::span<trace_record const>) {
        try
        {
          f.target->set_ring_blocks(false);
        }
        catch(popcorn::machine_running const&)
        {
          refused = true;
        }
      });
  core::slice_request budget;
  budget.budget_left = icount{100};
  REQUIRE(f.run_to_stop(budget).reason == stop_reason::budget);
  CHECK(refused);
  CHECK(f.target->ring_blocks());
  f.target->restore(taken);
  CHECK(f.target->ring_blocks());
  f.target->trace().clear(f.target->virtual_clock().now());
  REQUIRE(f.run_to_stop(budget).reason == stop_reason::budget);
  CHECK_FALSE(ring_blocks_of(f).empty());
}
