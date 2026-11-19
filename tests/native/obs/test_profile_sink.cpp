#include "owned_sink.hpp"
#include "profile_rig.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/obs/pctr.hpp>

#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
using namespace popcorn::testing::profile_rig;

auto cpu_register(
    obs::profile_result const& result,
    core::machine const&       target,
    std::string_view           peripheral,
    std::string_view           reg) -> obs::profile_register
{
  auto const& description = target.description();
  for(auto const [index, held] : std::views::enumerate(description.peripherals))
  {
    if(held.name != peripheral)
    {
      continue;
    }
    auto const found = std::ranges::find(held.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != held.registers.end());
    auto const id = popcorn::flat_register_id(
        description, {.peripheral = static_cast<std::uint32_t>(index),
                      .reg        = static_cast<std::uint32_t>(found - held.registers.begin())});
    auto const counted = std::ranges::find(result.cpu_registers, id, &obs::profile_register::id);
    REQUIRE(counted != result.cpu_registers.end());
    return *counted;
  }
  FAIL("no peripheral " << peripheral);
  return {};
}

auto file_bytes(std::filesystem::path const& path) -> std::string
{
  std::ifstream in{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, {}};
}

auto scratch(std::string_view name) -> std::filesystem::path
{
  char const* const dir = std::getenv("POPCORN_OBS_SCRATCH");
  REQUIRE(dir != nullptr);
  auto const own = std::filesystem::path{dir} / "profile_sink" / std::to_string(::getpid());
  std::filesystem::create_directories(own);
  return own / name;
}
}

TEST_CASE("an empty profile has no counts and starts where the sink was added")
{
  auto const result = fed({});
  CHECK(result.start == icount{1000});
  CHECK(result.elapsed == 0);
  CHECK(result.records == 0);
  CHECK(result.code.empty());
  CHECK(result.exceptions.empty());
  CHECK(result.cuts == 0);
}

TEST_CASE("the span from the start to the first record is a gap")
{
  auto const result = fed({block(0x100, 4, 10)});
  CHECK(result.other_gaps == obs::profile_gaps{.count = 1, .total = 6, .longest = 6});
  CHECK(result.instructions + gaps(result) == result.elapsed);
}

TEST_CASE("one block counts its entry, its instructions and the time to it")
{
  auto const result = fed({block(0x100, 7, 7)});
  CHECK(
      result.code
      == std::vector<obs::profile_block>{
          {.pc = 0x100, .entries = 1, .instructions = 7}
  });
  CHECK(result.instructions == 7);
  CHECK(result.blocks == 1);
  CHECK(result.elapsed == 7);
  CHECK(result.other_gaps == obs::profile_gaps{});
}

TEST_CASE("blocks merge by pc and a span with no record is a gap kept by the record ending it")
{
  auto const result = fed({
      block(0x100, 4, 4),
      block(0x108, 2, 2),
      block(0x100, 4, 4),
      block(0x108, 2, 17),
      slept(20, 30, 20),
      enter(20, 0),
      block(0x200, 3, 3),
      leave(20, 0),
  });
  CHECK(
      result.code
      == std::vector<obs::profile_block>{
          {.pc = 0x100, .entries = 2, .instructions = 8},
          {.pc = 0x108, .entries = 2, .instructions = 4},
          {.pc = 0x200, .entries = 1, .instructions = 3},
  });
  CHECK(result.other_gaps == obs::profile_gaps{.count = 2, .total = 25, .longest = 15});
  CHECK(result.sleeps == obs::profile_gaps{.count = 1, .total = 20, .longest = 20});
  CHECK(
      result.exceptions.at(20).wakes == obs::profile_gaps{.count = 1, .total = 20, .longest = 20});
  CHECK(result.sleep_reasons == std::array<std::uint64_t, 4>{0, 1, 0, 0});
  CHECK(result.instructions + gaps(result) == result.elapsed);
  CHECK(result.elapsed == 60);
}

TEST_CASE("three nested exceptions split inclusive and self time and count preemption")
{
  auto const  result = fed({
      enter(30, 0),
      block(0x300, 5, 5),
      enter(40, 0),
      block(0x400, 3, 3),
      enter(50, 0),
      block(0x500, 2, 2),
      leave(50, 0),
      block(0x400, 1, 1),
      leave(40, 0),
      block(0x300, 4, 4),
      leave(30, 0),
  });
  auto const& outer  = result.exceptions.at(30);
  auto const& middle = result.exceptions.at(40);
  auto const& inner  = result.exceptions.at(50);
  CHECK(outer.inclusive == 15);
  CHECK(outer.self == 9);
  CHECK(middle.inclusive == 6);
  CHECK(middle.self == 4);
  CHECK(inner.inclusive == 2);
  CHECK(inner.self == 2);
  CHECK(outer.max_depth == 1);
  CHECK(middle.max_depth == 2);
  CHECK(inner.max_depth == 3);
  CHECK(
      outer.preempted_by
      == std::map<std::uint16_t, std::uint64_t>{
          {40, 1}
  });
  CHECK(
      middle.preempted_by
      == std::map<std::uint16_t, std::uint64_t>{
          {50, 1}
  });
  CHECK(inner.preempted_by.empty());
  CHECK(outer.longest == 15);
  CHECK(result.unmatched_exits == 0);
}

TEST_CASE("an entry right after an exit is tail-chained, one after a block is not")
{
  auto const result = fed({
      enter(30, 0),
      block(0x300, 2, 2),
      leave(30, 0),
      enter(31, 0),
      block(0x310, 2, 2),
      leave(31, 0),
      block(0x100, 1, 1),
      enter(30, 0),
      block(0x300, 2, 2),
      leave(30, 0),
      block(0x110, 1, 0),
      enter(32, 0),
      leave(32, 1),
  });
  CHECK(result.exceptions.at(31).tail_chained == 1);
  CHECK(result.exceptions.at(30).tail_chained == 0);
  CHECK(result.exceptions.at(30).entries == 2);
  CHECK(result.exceptions.at(32).tail_chained == 0);
}

TEST_CASE("an entry that wakes the core after a sleep on exit is a wake, not a tail chain")
{
  auto const result = fed({
      enter(30, 0),
      block(0x300, 2, 2),
      leave(30, 0),
      slept(5, 5, 30, 3),
      enter(30, 0),
  });
  CHECK(result.exceptions.at(30).tail_chained == 0);
  CHECK(result.exceptions.at(30).wakes == obs::profile_gaps{.count = 1, .total = 5, .longest = 5});
  CHECK(result.sleep_reasons == std::array<std::uint64_t, 4>{0, 0, 0, 1});
  auto const unrecorded = fed({enter(30, 0), block(0x300, 2, 2), leave(30, 0), enter(30, 5)});
  CHECK(unrecorded.exceptions.at(30).tail_chained == 0);
}

TEST_CASE("an exit before a cut does not tail-chain the first entry after it")
{
  auto const result = fed({
      enter(30, 0),
      block(0x300, 2, 2),
      leave(30, 0),
      time_to(1002),
      enter(31, 0),
  });
  CHECK(result.cuts == 1);
  CHECK(result.exceptions.at(31).tail_chained == 0);
}

TEST_CASE("the burst is the most entries inside one sliding window")
{
  std::vector<core::trace_record> records;
  for(std::uint32_t const dt : {0u, 10u, 10u, 10u, 100u, 5u, 5u, 5u, 5u, 89u})
  {
    records.push_back(enter(30, dt));
    records.push_back(leave(30, 0));
  }
  records.insert(records.begin(), clocked(popcorn::ps_per_second));
  CHECK(fed(records, {.burst_window_ps = 30}).exceptions.at(30).burst == 5);
  CHECK(fed(records, {.burst_window_ps = 20}).exceptions.at(30).burst == 4);
  CHECK(fed(records, {.burst_window_ps = 15}).exceptions.at(30).burst == 3);
  CHECK(fed(records, {.burst_window_ps = 1}).exceptions.at(30).burst == 1);
  CHECK(fed(records, {.burst_window_ps = 200}).exceptions.at(30).burst == 9);
  CHECK(fed(records).exceptions.at(30).burst == 0);
}

TEST_CASE("the burst window keeps its length in picoseconds across a rate change")
{
  std::vector<core::trace_record> records{clocked(4'000'000)};
  for(std::uint32_t const dt : {0u, 5u, 5u})
  {
    records.push_back(enter(30, dt));
    records.push_back(leave(30, 0));
  }
  records.push_back(clocked(160'000'000));
  for(std::uint32_t const dt : {100u, 100u, 100u})
  {
    records.push_back(enter(31, dt));
    records.push_back(leave(31, 0));
  }
  auto const result = fed(records, {.burst_window_ps = 1'000'000});
  CHECK(result.exceptions.at(30).burst == 1);
  CHECK(result.exceptions.at(31).burst == 2);
}

TEST_CASE("time totals in picoseconds convert each part of a span at its own rate")
{
  auto const result = fed({
      clocked(4'000'000),
      enter(30, 10),
      clocked(160'000'000, 10),
      leave(30, 30),
      slept(20, 20, 31),
      core::trace_record{
                         .kind    = core::trace_kind::loop,
                         .aux     = core::trace_loop_ender(core::loop_ender::end),
                         .payload = {0x0800'0100, 7, 60},
                         },
      clocked(4'000'000),
  });
  CHECK(result.elapsed == 70);
  CHECK(result.elapsed_ps == 5'312'500);
  REQUIRE(result.clocks.size() == 3);
  CHECK(result.clocks[2].instructions == 0);
  CHECK(result.clocks[0].instructions == 20);
  CHECK(result.clocks[0].ps == 5'000'000);
  CHECK(result.clocks[1].instructions == 50);
  CHECK(result.clocks[1].ps == 312'500);
  CHECK(
      result.other_gaps
      == obs::profile_gaps{
          .count = 2, .total = 50, .longest = 40, .total_ps = 5'187'500, .longest_ps = 2'687'500});
  auto const& handler = result.exceptions.at(30);
  CHECK(handler.inclusive == 40);
  CHECK(handler.inclusive_ps == 2'687'500);
  CHECK(handler.self_ps == 2'687'500);
  CHECK(handler.longest_ps == 2'687'500);
  CHECK(
      result.sleeps
      == obs::profile_gaps{
          .count = 1, .total = 20, .longest = 20, .total_ps = 125'000, .longest_ps = 125'000});
  CHECK(result.exceptions.at(31).wakes.total_ps == 125'000);
  REQUIRE(result.loops.size() == 1);
  CHECK(result.loops[0].instructions == 60);
  CHECK(result.loops[0].ps == 2'812'500);
}

TEST_CASE("a restore back in time converts the new segment with its own clock records")
{
  auto const result = fed({
      clocked(4'000'000),
      clocked(160'000'000, 2),
      clocked(4'000'000, 8),
      time_to(1005),
      clocked(4'000'000),
      enter(30, 2),
      clocked(160'000'000, 3),
      leave(30, 10),
  });
  CHECK(result.cuts == 1);
  CHECK(result.elapsed == 25);
  CHECK(result.elapsed_ps == 500'000 + 50'000 + 1'250'000 + 62'500);
  CHECK(result.exceptions.at(30).inclusive_ps == 3 * 250'000 + 10 * 6'250);
}

TEST_CASE("a preempting exception's picoseconds leave the preempted one's self time")
{
  auto const  result = fed({
      clocked(4'000'000),
      enter(30, 0),
      clocked(160'000'000, 4),
      enter(40, 0),
      leave(40, 16),
      leave(30, 16),
  });
  auto const& outer  = result.exceptions.at(30);
  auto const& inner  = result.exceptions.at(40);
  CHECK(inner.inclusive_ps == 100'000);
  CHECK(outer.inclusive_ps == 1'000'000 + 200'000);
  CHECK(outer.self_ps == 1'000'000 + 100'000);
}

TEST_CASE("an entry with no exit is open at the end and adds no time")
{
  auto const result = fed({enter(30, 0), block(0x300, 2, 2), enter(40, 0), block(0x400, 2, 2)});
  CHECK(result.exceptions.at(30).open == 1);
  CHECK(result.exceptions.at(40).open == 1);
  CHECK(result.exceptions.at(30).inclusive == 0);
  CHECK(result.exceptions.at(30).self == 0);
  CHECK(result.exceptions.at(30).cut == 0);
}

TEST_CASE("an exit with no matching entry is counted and closes nothing")
{
  auto const result = fed({leave(30, 0), enter(40, 3), leave(30, 1), leave(40, 1)});
  CHECK(result.unmatched_exits == 2);
  CHECK(result.exceptions.at(40).inclusive == 2);
}

TEST_CASE("a time record back cuts the stream: frames close as cut and totals stay non-negative")
{
  auto const result = fed({
      enter(30, 0),
      block(0x300, 2, 2),
      time_to(10),
      block(0x100, 3, 3),
      leave(30, 0),
      enter(40, 0),
      block(0x400, 2, 2),
      leave(40, 0),
      block(0x100, 1, 9),
  });
  CHECK(result.cuts == 1);
  CHECK(result.exceptions.at(30).cut == 1);
  CHECK(result.exceptions.at(30).inclusive == 0);
  CHECK(result.exceptions.at(30).open == 0);
  CHECK(result.exceptions.at(40).inclusive == 2);
  CHECK(result.exceptions.at(40).wakes == obs::profile_gaps{});
  CHECK(result.exceptions.at(40).tail_chained == 1);
  CHECK(result.unmatched_exits == 1);
  CHECK(result.elapsed == 16);
  CHECK(result.other_gaps == obs::profile_gaps{.count = 1, .total = 8, .longest = 8});
}

TEST_CASE("a flagged time record cuts at any distance, an unflagged one continues the stream")
{
  constexpr std::uint64_t ahead   = 1000 + (std::uint64_t{1} << 32) + 5;
  auto const              rebased = fed({enter(30, 0), time_to(ahead), leave(30, 1)});
  CHECK(rebased.cuts == 1);
  CHECK(rebased.exceptions.at(30).cut == 1);
  CHECK(rebased.elapsed == 1);
  constexpr std::uint64_t far  = 1000 + (std::uint64_t{1} << 32) + 5;
  auto const              kept = fed({enter(30, 0), time_to(far, false), leave(30, 1)});
  CHECK(kept.cuts == 0);
  CHECK(kept.exceptions.at(30).inclusive == far + 1 - 1000);
  CHECK(kept.elapsed == far + 1 - 1000);
}

TEST_CASE("same-value writes restart after a cut")
{
  auto const result = fed({
      access(7, 0x100, 0x4000'0010, 5, true),
      access(7, 0x100, 0x4000'0010, 5, true),
      time_to(0),
      access(7, 0x100, 0x4000'0010, 5, true),
  });
  CHECK(result.cpu_registers.at(0).same_writes == 1);
}

TEST_CASE("mmio counts CPU accesses by register, pc 0 apart, overflowed ids by address")
{
  auto const result = fed({
      access(7, 0x100, 0x4000'0010, 1, false),
      access(7, 0x100, 0x4000'0010, 2, true),
      access(7, 0x100, 0x4000'0010, 2, true),
      access(7, 0x100, 0x4000'0010, 3, true),
      access(7, 0, 0x4000'0010, 9, false),
      access(core::trace_id_overflow, 0x100, 0x5000'0000, 4, false),
      access(core::trace_id_overflow, 0x100, 0x5000'0004, 4, true),
      access(3, 0x104, 0x4000'0000, 8, false),
  });
  CHECK(
      result.cpu_registers
      == std::vector<obs::profile_register>{
          {.id = 3, .first_address = 0x4000'0000, .reads = 1, .last_value = 8},
          {.id            = 7,
           .first_address = 0x4000'0010,
           .reads         = 1,
           .writes        = 3,
           .same_writes   = 1,
           .last_value    = 3},
          {.id            = core::trace_id_overflow,
           .first_address = 0x5000'0000,
           .reads         = 1,
           .last_value    = 4},
          {.id            = core::trace_id_overflow,
           .first_address = 0x5000'0004,
           .writes        = 1,
           .last_value    = 4},
  });
  CHECK(
      result.debugger_registers
      == std::vector<obs::profile_register>{
          {.id = 7, .first_address = 0x4000'0010, .reads = 1, .last_value = 9},
  });
  CHECK(result.code.empty());
  CHECK(result.other_gaps == obs::profile_gaps{});
}

TEST_CASE("a write log keeps CPU writes of its registers up to the limit and counts the rest")
{
  auto const result = fed(
      {
          access(7, 0x100, 0x4000'0010, 1, true, 3),
          access(7, 0x100, 0x4000'0010, 2, false, 1),
          access(7, 0, 0x4000'0010, 3, true, 1),
          access(8, 0x100, 0x4000'0014, 4, true, 1),
          access(7, 0x100, 0x4000'0010, 5, true, 1),
          time_to(0),
          access(7, 0x100, 0x4000'0010, 6, true, 2),
          access(7, 0x100, 0x4000'0010, 7, true, 1),
  },
      {.logged_registers = {7, 9}, .write_log_limit = 3});
  CHECK(
      result.write_logs
      == std::vector<obs::profile_write_log>{
          {.id = 7,
           .writes =
               {{.at = icount{1003}, .value = 1, .segment = 0, .pc = 0x100, .record = 1},
                {.at = icount{1007}, .value = 5, .segment = 0, .pc = 0x100, .record = 5},
                {.at = icount{2}, .value = 6, .segment = 1, .pc = 0x100, .record = 6}},
           .dropped = 1},
          {.id = 9},
  });
}

TEST_CASE("first hits and logged writes carry their profile time across a cut")
{
  auto const result = fed(
      {
          clocked(4'000'000),
          domain_clocked(3, 0),
          domain_clocked(5, 99),
          block(0x100, 4, 10),
          domain_clocked(3, 32'000, 1),
          access(7, 0x104, 0x4000'0010, 1, true, 1),
          domain_clocked(5, 7, 1),
          time_to(0),
          clocked(160'000'000),
          domain_clocked(3, 250),
          block(0x200, 2, 6),
          block(0x100, 4, 4),
          access(7, 0x204, 0x4000'0010, 2, true, 1),
      },
      {.logged_registers = {7}, .write_domain = core::clock_domain{3}});
  REQUIRE(result.code.size() == 2);
  CHECK(result.code[0].pc == 0x100);
  CHECK(result.code[0].entries == 2);
  CHECK(result.code[0].first_ps == 6 * 250'000);
  CHECK(result.code[1].first_ps == 13 * 250'000 + 4 * 6'250);
  CHECK(result.records == 7);
  REQUIRE(result.write_logs.size() == 1);
  CHECK(
      result.write_logs[0].writes
      == std::vector<obs::profile_write>{
          {.at       = icount{1012},
           .value    = 1,
           .segment  = 0,
           .pc       = 0x104,
           .ps       = 12 * 250'000,
           .clock_hz = 32'000,
           .record   = 3},
          {.at       = icount{11},
           .value    = 2,
           .segment  = 1,
           .pc       = 0x204,
           .ps       = 13 * 250'000 + 11 * 6'250,
           .clock_hz = 250,
           .record   = 7},
  });
  CHECK(
      result.domain_rates
      == std::vector<obs::profile_rate>{
          {.at = icount{1000}, .segment = 0, .ps = 0,            .hz = 0,      .record = 1},
          {.at = icount{1011}, .segment = 0, .ps = 11 * 250'000, .hz = 32'000, .record = 2},
          {.at = icount{0},    .segment = 1, .ps = 13 * 250'000, .hz = 250,    .record = 4},
  });
  CHECK(result.elapsed_ps == 13 * 250'000 + 11 * 6'250);
}

TEST_CASE("a sink with no clock domain keeps no rate and writes record 0 Hz")
{
  auto const result = fed(
      {clocked(4'000'000), domain_clocked(3, 32'000), access(7, 0x104, 0x4000'0010, 1, true, 2)},
      {.logged_registers = {7}});
  CHECK(result.domain_rates.empty());
  CHECK(result.records == 2);
  REQUIRE(result.write_logs[0].writes.size() == 1);
  CHECK(result.write_logs[0].writes[0].clock_hz == 0);
}

TEST_CASE("more distinct blocks than the initial table keep every count")
{
  std::vector<core::trace_record> records;
  constexpr std::uint32_t         distinct = 100'000;
  for(std::uint32_t round = 0; round < 2; ++round)
  {
    for(std::uint32_t at = 0; at < distinct; ++at)
    {
      records.push_back(block(0x0800'0000 + 2 * at, static_cast<std::uint16_t>(1 + at % 7), 1));
    }
  }
  auto const result = fed(records);
  REQUIRE(result.code.size() == distinct);
  for(std::uint32_t at = 0; at < distinct; ++at)
  {
    REQUIRE(result.code[at].pc == 0x0800'0000 + 2 * at);
    REQUIRE(result.code[at].entries == 2);
    REQUIRE(result.code[at].instructions == 2 * (1 + at % 7));
  }
}

TEST_CASE("a pc of 0 is a block like any other")
{
  auto const result = fed({block(0, 3, 3), block(0, 3, 3)});
  CHECK(
      result.code
      == std::vector<obs::profile_block>{
          {.pc = 0, .entries = 2, .instructions = 6}
  });
}

TEST_CASE(
    "the timer example sleeps 3000 times between TIM2 interrupts in 3 s, the last cut at the end")
{
  auto     f = tim_machine();
  profiled p{*f->target, {.burst_window_ps = ms_ps}};
  f->run_for(3'000 * ms_ps);
  auto const  result = p.take();
  auto const& tim2   = result.exceptions.at(tim2_exception);
  CHECK(tim2.entries == 2999);
  CHECK(tim2.burst == 1);
  CHECK(tim2.max_depth == 1);
  CHECK(tim2.inclusive == tim2.self);
  CHECK(gaps(result) == 11'936'889);
  CHECK(result.sleeps.total == 11'936'889);
  CHECK(result.sleeps.count == 3001);
  CHECK(result.sleeps.longest == 3979);
  CHECK(result.sleep_reasons == std::array<std::uint64_t, 4>{0, 3001, 0, 0});
  CHECK(result.other_gaps == obs::profile_gaps{});
  CHECK(tim2.wakes.count == 2999);
  CHECK(tim2.wakes.longest == 3979);
  CHECK(result.exceptions.at(tim16_exception).wakes.count == 1);
  CHECK(result.exceptions.at(tim16_exception).wakes.total == 21);
  CHECK(result.partial_sleeps == 1);
  REQUIRE(result.clocks.size() == 1);
  CHECK(result.clocks[0].hz == 4'000'000);
  CHECK(result.instructions == 63'111);
  CHECK(result.cuts == 0);
  CHECK(result.unmatched_exits == 0);
  CHECK(tim2.self == 56'981);
  CHECK(tim2.longest == 19);
  CHECK(tim2.tail_chained == 0);
  CHECK(result.elapsed == 12'000'000);
  CHECK(result.instructions + gaps(result) == result.elapsed);
  CHECK(cpu_register(result, *f->target, "TIM2", "SR").same_writes == 2999);
  CHECK(cpu_register(result, *f->target, "TIM16", "SR").same_writes == 1);
}

TEST_CASE("the FreeRTOS example runs 15829458 instructions in 100 ms, blocks as coverage counts")
{
  auto f = spi_machine();
  f->target->enable_coverage({});
  profiled p{*f->target, {.burst_window_ps = ms_ps}};
  f->run_for(100 * ms_ps);
  auto const result   = p.take();
  auto const coverage = f->target->coverage();
  CHECK(result.elapsed == 15'829'458);
  CHECK(gaps(result) == 0);
  CHECK(result.cuts == 0);
  std::vector<obs::profile_block> covered;
  for(auto const& counters : coverage.blocks)
  {
    for(auto const [index, hits] : std::views::enumerate(counters.hits))
    {
      if(hits != 0)
      {
        covered.push_back(
            {.pc = static_cast<address>(counters.range.start + 2 * index), .entries = hits});
      }
    }
  }
  std::ranges::sort(covered, {}, &obs::profile_block::pc);
  auto const entries = result.code
                     | std::views::transform([](obs::profile_block const& b) {
                         return obs::profile_block{.pc = b.pc, .entries = b.entries};
                       })
                     | std::ranges::to<std::vector>();
  CHECK(entries == covered);
  CHECK(coverage.cuts.size() == 4);
  CHECK(result.instructions == result.elapsed);
  CHECK(result.instructions == 15'829'458);
  CHECK(result.unmatched_exits == 0);
  auto const& spi1 = result.exceptions.at(spi1_exception);
  CHECK(spi1.entries == 892);
  CHECK(spi1.self == 44'919);
  CHECK(spi1.longest == 158);
  CHECK(spi1.max_depth == 2);
  CHECK(spi1.tail_chained == 241);
  auto const& pendsv = result.exceptions.at(pendsv_exception);
  CHECK(pendsv.entries == 406);
  CHECK(pendsv.self == 23'748);
  CHECK(pendsv.longest == 108);
  CHECK(pendsv.tail_chained == 100);
  CHECK(
      pendsv.preempted_by
      == std::map<std::uint16_t, std::uint64_t>{
          {spi1_exception, 5}
  });
  auto const& systick = result.exceptions.at(systick_exception);
  CHECK(systick.entries == 98);
  CHECK(systick.self == 10'976);
  CHECK(systick.longest == 112);
  CHECK(
      (result.exceptions | std::views::keys | std::ranges::to<std::vector>())
      == std::vector<std::uint16_t>{
          svcall_exception, pendsv_exception, systick_exception, tim6_exception, spi1_exception});
  CHECK(result.exceptions.at(svcall_exception).entries == 1);
  CHECK(spi1.burst == 892);
  CHECK(pendsv.burst == 212);
  CHECK(systick.burst == 2);
  auto const& tim6 = result.exceptions.at(tim6_exception);
  CHECK(tim6.burst == 2);
  CHECK(tim6.entries == 98);
  CHECK(tim6.self == 5194);
  CHECK(tim6.longest == 53);
  auto const reg = [&](std::string_view peripheral, std::string_view name) {
    return cpu_register(result, *f->target, peripheral, name);
  };
  CHECK(reg("SPI1", "SR").reads == 2439);
  CHECK(reg("SPI1", "IER").reads == 2128);
  CHECK(reg("SPI1", "CR1").reads == 620);
  CHECK(reg("SPI1", "RXDR").reads == 449);
  CHECK(reg("SPI1", "CR2").same_writes == 201);
  CHECK(reg("TIM6", "SR").same_writes == 97);
  CHECK(reg("SPI1", "TXDR").same_writes == 18);
}

TEST_CASE("a reset inside a profile cuts once and every total stays non-negative")
{
  auto f = tim_machine();
  f->target->enable_coverage({});
  profiled p{*f->target};
  f->run_for(500 * ms_ps);
  f->target->reset();
  REQUIRE(
      f->run_to_stop({.max_instructions = icount{100'000'000},
                      .until            = sym("tim_ticks", "main"),
                      .budget_left      = icount{1'000'000}})
          .reason
      == stop_reason::address);
  f->run_for(500 * ms_ps);
  auto const result = p.take();
  CHECK(result.cuts == 1);
  CHECK(result.unmatched_exits == 0);
  auto const& tim2 = result.exceptions.at(tim2_exception);
  CHECK(tim2.entries == 998);
  CHECK(tim2.inclusive == tim2.self);
  CHECK(tim2.inclusive < result.elapsed);
  CHECK(gaps(result) < result.elapsed);
  CHECK(result.elapsed == 3'996'242);
  CHECK(result.sleeps.count == 1001);
  CHECK(result.sleeps.longest == 3979);
  CHECK(result.partial_sleeps == 1);
  CHECK(result.instructions + gaps(result) == result.elapsed);
}

TEST_CASE("a restore back and a restore forward each cut the profile")
{
  auto     f = tim_machine();
  profiled p{*f->target};
  f->run_for(10 * ms_ps);
  auto const early = f->target->snapshot();
  f->run_for(10 * ms_ps);
  auto const late = f->target->snapshot();
  f->target->restore(early);
  f->run_for(5 * ms_ps);
  f->target->restore(late);
  f->run_for(5 * ms_ps);
  auto const result = p.take();
  CHECK(result.cuts == 2);
  CHECK(result.exceptions.at(tim2_exception).entries == 29);
}

TEST_CASE("a restore forward by less than one window restarts the burst count")
{
  auto       f = tim_machine();
  profiled   p{*f->target, {.burst_window_ps = 10 * ms_ps}};
  auto const origin = f->target->snapshot();
  f->run_for(5 * ms_ps);
  auto const ahead = f->target->snapshot();
  f->target->restore(origin);
  f->run_for(5 * ms_ps);
  f->target->restore(ahead);
  f->run_for(5 * ms_ps);
  auto const result = p.take();
  CHECK(result.cuts == 2);
  CHECK(result.exceptions.at(tim2_exception).burst == 5);
}

TEST_CASE("a profile opened while the core sleeps counts the rest of that sleep as a wake")
{
  auto f = tim_machine();
  f->run_for(ms_ps / 2);
  profiled p{*f->target};
  f->run_for(100 * ms_ps);
  auto const  result = p.take();
  auto const& tim2   = result.exceptions.at(tim2_exception);
  CHECK(tim2.wakes.count == tim2.entries);
  CHECK(result.instructions + gaps(result) == result.elapsed);
}

TEST_CASE("debugger reads between slices change only the non-CPU table")
{
  auto     plain = tim_machine();
  profiled quiet{*plain->target};
  plain->run_for(20 * ms_ps);
  auto const expected = quiet.take();

  auto     polled = tim_machine();
  profiled noisy{*polled->target};
  for(int slice = 0; slice < 40; ++slice)
  {
    polled->run_for(ms_ps / 2);
    REQUIRE(polled->target->mem().read(tim2_cnt, 4).has_value());
  }
  auto const got = noisy.take();
  CHECK(got.code == expected.code);
  CHECK(got.exceptions == expected.exceptions);
  CHECK(got.cpu_registers == expected.cpu_registers);
  CHECK(got.instructions == expected.instructions);
  CHECK(got.other_gaps == expected.other_gaps);
  REQUIRE(got.debugger_registers.size() == 1);
  CHECK(got.debugger_registers[0].first_address == tim2_cnt);
  CHECK(got.debugger_registers[0].reads == 40);
}

TEST_CASE("a free run with reads landing while it runs profiles as the parked run")
{
  auto     parked = tim_machine();
  profiled quiet{*parked->target};
  parked->run_for(50 * ms_ps);
  auto const expected = quiet.take();

  auto       f      = tim_machine();
  auto       sink   = std::make_unique<obs::profile_sink>(f->target->virtual_clock().now());
  auto       handle = f->target->add_trace_sink(obs::profile_sink::kinds, sink->sink());
  auto const start  = f->now().ps;
  auto const until  = vtime{start + 50 * ms_ps};
  std::vector<std::unique_ptr<core::machine>> members;
  members.push_back(std::move(f->target));
  core::host on{std::move(members), {.max_instructions = icount{5'000}}};
  std::vector<std::pair<vtime, core::host_future<vtime>>> reads;
  for(std::uint64_t read = 1; read <= 199; ++read)
  {
    auto       held = on.lock();
    auto const at   = vtime{start + read * ms_ps / 4};
    reads.emplace_back(
        at,
        on.post(
            held, 0, [](core::machine& m) { REQUIRE(m.mem().read(tim2_cnt, 4).has_value()); }, at));
  }
  auto done = [&] {
    auto held = on.lock();
    return on.run(held, 0, {.until_time = until});
  }();
  CHECK(done.get().stopped.reason == stop_reason::time);
  for(auto const& [at, read] : reads)
  {
    auto const landed = read.get();
    CHECK(landed == at);
    CHECK(landed < until);
  }
  auto const got = core::access(on, 0, [&](core::machine&) {
    handle.reset();
    return sink->result();
  });
  CHECK(got.code == expected.code);
  CHECK(got.exceptions == expected.exceptions);
  CHECK(got.cpu_registers == expected.cpu_registers);
  CHECK(got.other_gaps == expected.other_gaps);
  REQUIRE(got.debugger_registers.size() == 1);
  CHECK(got.debugger_registers[0].reads == 199);
}

TEST_CASE("a profile sink leaves a concurrent trace file, the ring and the stop unchanged")
{
  auto const run = [](bool with_profile, std::filesystem::path const& path) {
    auto f      = tim_machine();
    auto writer = obs::pctr_writer_for(*f->target, path);
    auto file   = f->target->add_trace_sink(
        {core::trace_kind::block, core::trace_kind::mmio, core::trace_kind::irq}, writer->sink());
    std::optional<profiled> p;
    if(with_profile)
    {
      p.emplace(*f->target);
    }
    auto const stopped = f->run_to_stop(
        {.max_instructions = icount{100'000'000}, .until_time = vtime{f->now().ps + 30 * ms_ps}});
    file.reset();
    writer->close();
    auto bytes = file_bytes(path);
    std::filesystem::remove(path);
    std::error_code kept;
    std::filesystem::remove(path.parent_path(), kept);
    return std::tuple{
        stopped.instructions, stopped.time, f->target->trace().records(), std::move(bytes)};
  };
  auto const [plain_count, plain_time, plain_ring, plain_file] =
      run(false, scratch("profile_plain.pctr"));
  auto const [count, at, ring, file] = run(true, scratch("profile_with.pctr"));
  CHECK(count == plain_count);
  CHECK(at == plain_time);
  CHECK(ring == plain_ring);
  CHECK(file == plain_file);
  CHECK(not file.empty());
}

TEST_CASE("a profile sink whose handle a handler drops inside a slice lives until the slice end")
{
  auto f    = tim_machine();
  auto sink = std::make_shared<obs::profile_sink>(f->target->virtual_clock().now());
  std::weak_ptr<obs::profile_sink> const watched = sink;
  std::optional<core::hook_handle>       hook{
      f->target->add_trace_sink(obs::profile_sink::kinds, obs::owned_sink(sink), {}, 1)};
  std::vector<bool> alive_after_drop;
  auto const        intercept = f->target->add_intercept(
      sym("tim_ticks", "TIM2_IRQHandler"), [&](core::intercept_call const&) {
        if(hook)
        {
          hook.reset();
          sink.reset();
          alive_after_drop.push_back(not watched.expired());
        }
        return core::intercept_result::proceed();
      });
  f->run_for(3 * ms_ps);
  CHECK(alive_after_drop == std::vector{true});
  CHECK(watched.expired());
  CHECK(f->target->loop_reports() == 0);
}

TEST_CASE("a trace writer whose handle a handler drops inside a slice writes until the slice end")
{
  auto const path = scratch("dropped_in_slice.pctr");
  {
    auto                                  f       = tim_machine();
    std::shared_ptr<obs::pctr_writer>     writer  = obs::pctr_writer_for(*f->target, path);
    std::weak_ptr<obs::pctr_writer> const watched = writer;
    std::optional<core::hook_handle>      hook{f->target->add_trace_sink(
        {core::trace_kind::block, core::trace_kind::irq}, obs::owned_sink(writer))};
    std::vector<bool>                     alive_after_drop;
    auto const                            intercept = f->target->add_intercept(
        sym("tim_ticks", "TIM2_IRQHandler"), [&](core::intercept_call const&) {
          if(hook)
          {
            hook.reset();
            writer.reset();
            alive_after_drop.push_back(not watched.expired());
          }
          return core::intercept_result::proceed();
        });
    f->run_for(3 * ms_ps);
    CHECK(alive_after_drop == std::vector{true});
    CHECK(watched.expired());
  }
  CHECK(file_bytes(path).size() > 16);
}

namespace
{
constexpr std::uint16_t reload_key     = 7;
constexpr std::uint16_t reload_prescl  = 8;
constexpr std::uint16_t reload_reload  = 9;
constexpr std::uint16_t refresh_value  = 0xAAAA;
constexpr std::uint16_t start_value    = 0xCCCC;
constexpr std::uint64_t instruction_ps = 250'000;

auto tracker_options(std::size_t limit = 4096) -> obs::profile_options
{
  return {
      .write_log_limit = limit,
      .write_domain    = core::clock_domain{3},
      .reload_tracker =
          obs::profile_reload_tracker{
                                            .key_ids       = {reload_key},
                                            .change_ids    = {reload_prescl, reload_reload},
                                            .reload_values = {refresh_value, start_value},
                                            },
  };
}

auto tracked_prologue() -> std::vector<core::trace_record>
{
  return {clocked(4'000'000), domain_clocked(3, 32'000)};
}

auto key_write(std::uint32_t value, std::uint32_t dt) -> core::trace_record
{
  return access(reload_key, 0x100, 0x4000'3000, value, true, dt);
}

auto change_write(std::uint16_t id, std::uint32_t value, std::uint32_t dt) -> core::trace_record
{
  return access(id, 0x120, 0x4000'3004, value, true, dt);
}

auto track_of(std::vector<core::trace_record> records, obs::profile_options options)
    -> obs::profile_reload_track
{
  auto all = tracked_prologue();
  all.insert(all.end(), records.begin(), records.end());
  auto const result = fed(all, std::move(options));
  REQUIRE(result.reload_tracks.size() == 1);
  return result.reload_tracks[0];
}

auto refreshes_at(std::initializer_list<std::uint32_t> gaps) -> std::vector<core::trace_record>
{
  std::vector<core::trace_record> out;
  for(auto const dt : gaps)
  {
    out.push_back(key_write(refresh_value, dt));
  }
  return out;
}
}

TEST_CASE("the reload tracker counts every reload and key write past the log limit")
{
  std::vector<core::trace_record> records;
  for(auto i = 0; i < 9'999; ++i)
  {
    records.push_back(key_write(refresh_value, 1));
  }
  records.push_back(key_write(0x1234'0000 | refresh_value, 1));
  records.push_back(key_write(0x5555, 1));
  records.push_back(key_write(0x5555, 1));
  auto all = tracked_prologue();
  all.insert(all.end(), records.begin(), records.end());
  auto options             = tracker_options(4);
  options.logged_registers = {reload_key};
  auto const  result       = fed(all, std::move(options));
  auto const& track        = result.reload_tracks.at(0);
  CHECK(track.counts == std::vector<std::uint64_t>{10'000, 0});
  CHECK(track.key_writes == 10'002);
  CHECK(result.write_logs.at(0).writes.size() == 4);
  CHECK(result.write_logs.at(0).dropped == 9'998);
  REQUIRE(track.tails.size() == 1);
  CHECK(track.tails[0].last.value == (0x1234'0000 | refresh_value));
  CHECK(track.tails[0].last.record == 10'001);
}

TEST_CASE("the reload tracker keeps the longest gap of a kind")
{
  auto const track = track_of(refreshes_at({10, 10, 30, 10}), tracker_options());
  REQUIRE(track.epochs.size() == 1);
  REQUIRE(track.epochs[0].longest[0].has_value());
  auto const& gap = *track.epochs[0].longest[0];
  CHECK(gap.start.at == icount{1020});
  CHECK(gap.end.at == icount{1050});
  CHECK(gap.ps == 30 * instruction_ps);
  CHECK(gap.start.ps == 20 * instruction_ps);
  CHECK(gap.end.ps == 50 * instruction_ps);
  CHECK_FALSE(track.epochs[0].longest[1].has_value());
}

TEST_CASE("equal longest gaps report the first")
{
  auto const track = track_of(refreshes_at({10, 10, 20, 10, 20}), tracker_options());
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->start.at == icount{1020});
  CHECK(track.epochs[0].longest[0]->end.at == icount{1040});
}

TEST_CASE("a gap belongs to the kind of its first reload")
{
  auto const track = track_of(
      {key_write(start_value, 10), key_write(refresh_value, 10), key_write(refresh_value, 40)},
      tracker_options());
  CHECK(track.counts == std::vector<std::uint64_t>{2, 1});
  REQUIRE(track.epochs[0].longest[1].has_value());
  CHECK(track.epochs[0].longest[1]->ps == 10 * instruction_ps);
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 40 * instruction_ps);
}

TEST_CASE("a change write opens an epoch and the gap across it stays in the earlier one")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 10),
       change_write(reload_prescl, 3, 5), key_write(refresh_value, 45),
       key_write(refresh_value, 10), time_to(0), clocked(4'000'000),
       change_write(reload_reload, 0x100, 1)},
      tracker_options());
  REQUIRE(track.epochs.size() == 4);
  CHECK(track.epochs[0].kind == obs::epoch_kind::start);
  CHECK(track.epochs[0].key_before == ~std::uint32_t{0});
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 50 * instruction_ps);
  CHECK(track.epochs[1].kind == obs::epoch_kind::change);
  CHECK(track.epochs[1].change_id == reload_prescl);
  CHECK(track.epochs[1].change.value == 3);
  CHECK(track.epochs[1].key_before == refresh_value);
  REQUIRE(track.epochs[1].longest[0].has_value());
  CHECK(track.epochs[1].longest[0]->ps == 10 * instruction_ps);
  CHECK(track.epochs[2].kind == obs::epoch_kind::cut);
  CHECK(track.epochs[3].kind == obs::epoch_kind::change);
  CHECK(track.epochs[3].key_before == ~std::uint32_t{0});
}

TEST_CASE("a cut pairs no reloads across it and gives each segment a tail")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 10), time_to(0), clocked(4'000'000),
       key_write(refresh_value, 50), key_write(refresh_value, 4)},
      tracker_options());
  REQUIRE(track.epochs.size() == 2);
  CHECK(track.epochs[1].kind == obs::epoch_kind::cut);
  CHECK(track.epochs[1].segment == 1);
  REQUIRE(track.epochs[0].longest[0].has_value());
  REQUIRE(track.epochs[1].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 10 * instruction_ps);
  CHECK(track.epochs[1].longest[0]->ps == 4 * instruction_ps);
  REQUIRE(track.tails.size() == 2);
  CHECK(track.tails[0].segment == 0);
  CHECK(track.tails[0].last.at == icount{1020});
  CHECK(track.tails[0].epoch == 0);
  CHECK(track.tails[1].segment == 1);
  CHECK(track.tails[1].last.at == icount{54});
  CHECK(track.tails[1].epoch == 1);
}

TEST_CASE("the longest gap is the one with the most ticks of the domain across a rate change")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 400), domain_clocked(3, 64'000),
       key_write(refresh_value, 300)},
      tracker_options());
  REQUIRE(track.epochs[0].longest[0].has_value());
  auto const& gap = *track.epochs[0].longest[0];
  CHECK(gap.ps == 300 * instruction_ps);
  CHECK(gap.start.clock_hz == 32'000);
  CHECK(gap.end.clock_hz == 64'000);
}

TEST_CASE("a pair across a rate change counts the ticks of each part at its own rate")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 400), domain_clocked(3, 64'000, 100),
       key_write(refresh_value, 140)},
      tracker_options());
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 400 * instruction_ps);
}

TEST_CASE("a stopped domain accumulates no ticks")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 10), domain_clocked(3, 0),
       key_write(refresh_value, 980)},
      tracker_options());
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 10 * instruction_ps);
}

TEST_CASE("the epoch table holds at most the write log limit and counts the epochs past it")
{
  auto const track = track_of(
      {key_write(refresh_value, 1), change_write(reload_prescl, 1, 1),
       change_write(reload_prescl, 2, 1), change_write(reload_reload, 3, 1),
       change_write(reload_prescl, 4, 1), change_write(reload_prescl, 5, 1),
       key_write(refresh_value, 1)},
      tracker_options(2));
  CHECK(track.epochs.size() == 2);
  CHECK(track.epochs[1].change.value == 1);
  CHECK(track.epochs_dropped == 4);
  CHECK(track.counts == std::vector<std::uint64_t>{2, 0});
  CHECK(track.key_writes == 2);
}

TEST_CASE("a reload tracker without a write domain is refused")
{
  auto options = tracker_options();
  options.write_domain.reset();
  CHECK_THROWS_AS(fed({}, std::move(options)), std::invalid_argument);
}

TEST_CASE("only CPU writes to the listed registers reach the tracker")
{
  auto dma = key_write(refresh_value, 1);
  dma.flags |= core::trace_dma_flag;
  auto const track = track_of(
      {access(10, 0x100, 0x4000'3008, refresh_value, true, 1),
       access(reload_key, 0, 0x4000'3000, refresh_value, true, 1), dma,
       access(reload_key, 0x100, 0x4000'3000, refresh_value, false, 1),
       access(11, 0x100, 0x4000'300c, 0, true, 1)},
      tracker_options());
  CHECK(track.key_writes == 0);
  CHECK(track.counts == std::vector<std::uint64_t>{0, 0});
  CHECK(track.epochs.size() == 1);
  CHECK(track.tails.empty());
}

TEST_CASE("past the epoch cap a dropped change write freezes the gaps and the tails")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 10),
       change_write(reload_prescl, 1, 5), change_write(reload_prescl, 2, 5),
       key_write(refresh_value, 970)},
      tracker_options(2));
  CHECK(track.epochs.size() == 2);
  CHECK(track.epochs_dropped == 1);
  CHECK(track.frozen_ps == 30 * instruction_ps);
  CHECK(track.frozen_segment == 0);
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 10 * instruction_ps);
  REQUIRE(track.tails.size() == 1);
  CHECK(track.tails[0].last.at == icount{1020});
  CHECK(track.counts == std::vector<std::uint64_t>{3, 0});
  CHECK(track.key_writes == 3);
}

TEST_CASE("a change write that fills the table drops nothing")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 10),
       change_write(reload_prescl, 1, 5), key_write(refresh_value, 970)},
      tracker_options(2));
  CHECK(track.epochs_dropped == 0);
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 975 * instruction_ps);
}

TEST_CASE("past the epoch cap a dropped cut keeps the tail it ended and freezes the next segment")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 10),
       change_write(reload_prescl, 1, 5), time_to(0), clocked(4'000'000),
       key_write(refresh_value, 5), key_write(refresh_value, 400)},
      tracker_options(2));
  CHECK(track.epochs.size() == 2);
  CHECK(track.epochs_dropped == 1);
  CHECK(track.frozen_ps == 25 * instruction_ps);
  CHECK(track.frozen_segment == 1);
  REQUIRE(track.tails.size() == 1);
  CHECK(track.tails[0].segment == 0);
  CHECK(track.tails[0].last.at == icount{1020});
  CHECK(track.counts == std::vector<std::uint64_t>{4, 0});
}

TEST_CASE("a gap of more than 576 seconds at 32 kHz is longer than a stored gap of one second")
{
  auto const track = track_of(
      {key_write(refresh_value, 10), key_write(refresh_value, 4'000'000),
       key_write(refresh_value, 2'308'000'000)},
      tracker_options());
  REQUIRE(track.epochs[0].longest[0].has_value());
  CHECK(track.epochs[0].longest[0]->ps == 577'000'000'000'000);
}

TEST_CASE("a core rate that does not divide a second still converts each point exactly")
{
  auto const result =
      fed({clocked(3'000'000), domain_clocked(3, 32'000), key_write(refresh_value, 10),
           key_write(refresh_value, 10), key_write(refresh_value, 11)},
          tracker_options());
  auto const& track = result.reload_tracks.at(0);
  REQUIRE(track.epochs[0].longest[0].has_value());
  auto const& gap = *track.epochs[0].longest[0];
  CHECK(gap.start.ps == 20 * 1'000'000'000'000 / 3'000'000);
  CHECK(gap.end.ps == 31 * 1'000'000'000'000 / 3'000'000);
  CHECK(track.tails.at(0).last.ps == 31 * 1'000'000'000'000 / 3'000'000);
  CHECK(result.elapsed_ps == 31 * 1'000'000'000'000 / 3'000'000);
}

TEST_CASE("a reload tracker with a write log limit of 0 is refused")
{
  CHECK_THROWS_AS(fed({}, tracker_options(0)), std::invalid_argument);
}
