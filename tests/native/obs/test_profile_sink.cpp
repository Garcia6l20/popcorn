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

auto unrun_tails(
    obs::profile_result const& result,
    core::coverage_data const& coverage,
    core::machine&             target) -> std::uint64_t
{
  std::uint64_t unrun = 0;
  for(auto const& cut : coverage.cuts)
  {
    auto const found = std::ranges::find(result.code, cut.start, &obs::profile_block::pc);
    REQUIRE(found != result.code.end());
    REQUIRE(found->instructions % found->entries == 0);
    std::uint64_t ran = 0;
    for(address at = cut.start; at < cut.at; ++ran)
    {
      auto const half = target.mem().read(at, 2);
      REQUIRE(half.has_value());
      at += (*half >> 11) >= 0x1d ? 4 : 2;
    }
    unrun += cut.hits * (found->instructions / found->entries - ran);
  }
  return unrun;
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
      enter(20, 30),
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
  CHECK(result.other_gaps == obs::profile_gaps{.count = 1, .total = 15, .longest = 15});
  CHECK(
      result.exceptions.at(20).wakes == obs::profile_gaps{.count = 1, .total = 30, .longest = 30});
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
      enter(30, 5),
  });
  CHECK(result.exceptions.at(30).tail_chained == 0);
  CHECK(result.exceptions.at(30).wakes == obs::profile_gaps{.count = 1, .total = 5, .longest = 5});
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
  CHECK(fed(records, {.burst_window = icount{30}}).exceptions.at(30).burst == 5);
  CHECK(fed(records, {.burst_window = icount{20}}).exceptions.at(30).burst == 4);
  CHECK(fed(records, {.burst_window = icount{15}}).exceptions.at(30).burst == 3);
  CHECK(fed(records, {.burst_window = icount{1}}).exceptions.at(30).burst == 1);
  CHECK(fed(records, {.burst_window = icount{200}}).exceptions.at(30).burst == 9);
  CHECK(fed(records).exceptions.at(30).burst == 0);
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

TEST_CASE("a time record forward within a dt is a rebase, one past it continues the stream")
{
  auto const rebased = fed({enter(30, 0), time_to(5000), leave(30, 1)});
  CHECK(rebased.cuts == 1);
  CHECK(rebased.exceptions.at(30).cut == 1);
  CHECK(rebased.elapsed == 1);
  constexpr std::uint64_t far  = 1000 + (std::uint64_t{1} << 32) + 5;
  auto const              kept = fed({enter(30, 0), time_to(far), leave(30, 1)});
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
      result.other_registers
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
               {{.at = icount{1003}, .value = 1, .segment = 0},
                {.at = icount{1007}, .value = 5, .segment = 0},
                {.at = icount{2}, .value = 6, .segment = 1}},
           .dropped = 1},
          {.id = 9},
  });
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

TEST_CASE("the timer example sleeps between 2999 TIM2 interrupts in 3 s")
{
  auto     f = tim_machine();
  profiled p{*f->target, {.burst_window = icount{4'000}}};
  f->run_for(3'000 * ms_ps);
  auto const  result = p.take();
  auto const& tim2   = result.exceptions.at(tim2_exception);
  CHECK(tim2.entries == 2999);
  CHECK(tim2.burst == 1);
  CHECK(tim2.max_depth == 1);
  CHECK(tim2.inclusive == tim2.self);
  CHECK(gaps(result) == 11'932'983);
  CHECK(tim2.wakes.count == 2999);
  CHECK(result.instructions == 63'111);
  CHECK(result.cuts == 0);
  CHECK(result.unmatched_exits == 0);
  CHECK(tim2.self == 56'981);
  CHECK(tim2.longest == 19);
  CHECK(tim2.tail_chained == 0);
  CHECK(result.elapsed == 11'996'094);
  CHECK(result.instructions + gaps(result) == result.elapsed);
  CHECK(cpu_register(result, *f->target, "TIM2", "SR").same_writes == 2999);
  CHECK(cpu_register(result, *f->target, "TIM16", "SR").same_writes == 1);
}

TEST_CASE("the FreeRTOS example spans 15829458 instructions in 100 ms, blocks as coverage counts")
{
  auto f = spi_machine();
  f->target->enable_coverage({});
  profiled p{*f->target, {.burst_window = icount{160'000}}};
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
  auto const unrun = unrun_tails(result, coverage, *f->target);
  CHECK(coverage.cuts.size() == 4);
  CHECK(unrun == 1650);
  CHECK(result.instructions == result.elapsed + unrun);
  CHECK(result.instructions == 15'831'108);
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
  CHECK(result.elapsed == 3'992'336);
  CHECK(
      result.instructions + gaps(result)
      == result.elapsed + unrun_tails(result, f->target->coverage(), *f->target));
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
  profiled   p{*f->target, {.burst_window = icount{40'000}}};
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
  REQUIRE(got.other_registers.size() == 1);
  CHECK(got.other_registers[0].first_address == tim2_cnt);
  CHECK(got.other_registers[0].reads == 40);
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
  REQUIRE(got.other_registers.size() == 1);
  CHECK(got.other_registers[0].reads == 199);
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
