#include "profile_rig.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/core/types.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace
{
using namespace popcorn::testing::profile_rig;

constexpr address scratch_code = 0x2002'f000;

struct captured
{
  std::vector<core::trace_record> records;
  core::hook_handle               handle;

  captured(
      core::machine&                      target,
      core::trace_kinds                   kinds,
      std::vector<popcorn::address_range> ranges = {})
      : handle{target.add_trace_sink(
            kinds,
            [this](std::span<core::trace_record const> batch) { records.append_range(batch); },
            std::move(ranges))}
  {}

  auto of(core::trace_kind kind) const -> std::vector<core::trace_record>
  {
    return records
         | std::views::filter([kind](core::trace_record const& r) { return r.kind == kind; })
         | std::ranges::to<std::vector>();
  }
};

void awake_in_main(stm32_fixture& f)
{
  auto at = sym("tim_ticks", "main");
  for(;; at += 2)
  {
    auto const half = f.target->mem().read(at, 2);
    REQUIRE(half.has_value());
    if(*half == 0xbf30)
    {
      break;
    }
  }
  f.run_for(ms_ps / 2);
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = at}).reason
      == stop_reason::address);
}

void place(stm32_fixture& f, std::vector<std::uint16_t> const& halves)
{
  awake_in_main(f);
  for(auto const [index, half] : std::views::enumerate(halves))
  {
    REQUIRE(f.target->mem().write(scratch_code + 2 * static_cast<address>(index), 2, half));
  }
  f.set("pc", scratch_code);
}

auto hz_of(core::trace_record const& record) -> std::uint64_t
{
  return std::uint64_t{record.payload[0]} | std::uint64_t{record.payload[1]} << 32;
}
}

TEST_CASE("a WFI sleep ends in a sleep record naming the reason, its length and the waking entry")
{
  auto     f = tim_machine();
  captured c{
      *f->target, {core::trace_kind::sleep, core::trace_kind::irq}
  };
  f->run_for(10 * ms_ps);
  c.handle.reset();
  auto const sleeps = c.of(core::trace_kind::sleep);
  REQUIRE(sleeps.size() == 10);
  CHECK(sleeps.front().payload[0] == 3958);
  for(auto const& sleep : sleeps | std::views::take(9))
  {
    CHECK(sleep.flags == 1);
    CHECK(sleep.aux == tim2_exception);
  }
  for(auto const& sleep : sleeps | std::views::drop(1) | std::views::take(8))
  {
    CHECK(sleep.payload[0] == 3979);
  }
  CHECK(sleeps.back().aux == 0);
  CHECK(sleeps.back().flags == (1 | core::trace_sleep_partial));
  auto const first =
      std::ranges::find(c.records, core::trace_kind::sleep, &core::trace_record::kind);
  REQUIRE(first != c.records.end());
  REQUIRE(std::next(first) != c.records.end());
  CHECK(std::next(first)->kind == core::trace_kind::irq);
  CHECK(std::next(first)->flags == 1);
  CHECK(std::next(first)->dt == 0);
}

TEST_CASE("a WFE sleep woken by an entry and a masked WFI woken with no entry")
{
  auto f = tim_machine();
  place(*f, {0xbf20, 0xbf20, 0xe7fe});
  {
    captured c{*f->target, {core::trace_kind::sleep}};
    f->run_for(2 * ms_ps);
    c.handle.reset();
    auto const sleeps = c.of(core::trace_kind::sleep);
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps.front().flags == 2);
    CHECK(sleeps.front().aux == tim2_exception);
  }
  f = tim_machine();
  place(*f, {0xb672, 0xbf30, 0xe7fe});
  captured c{*f->target, {core::trace_kind::sleep}};
  f->run_for(2 * ms_ps);
  c.handle.reset();
  auto const sleeps = c.of(core::trace_kind::sleep);
  REQUIRE(sleeps.size() == 1);
  CHECK(sleeps.front().flags == 1);
  CHECK(sleeps.front().aux == 0);
}

TEST_CASE("a held core is a gap, not a sleep")
{
  auto f = tim_machine();
  place(*f, {0xb672, 0xe7fe});
  profiled p{*f->target};
  f->target->hold_core(true);
  f->run_for(ms_ps);
  f->target->hold_core(false);
  f->run_for(ms_ps / 10);
  auto const result = p.take();
  CHECK(result.sleeps == obs::profile_gaps{});
  CHECK(result.other_gaps.total == 4000);
  CHECK(result.instructions + gaps(result) == result.elapsed);
}

TEST_CASE("a sleep spanning many slices gives the same profile as one slice")
{
  auto     whole = tim_machine();
  profiled one{*whole->target};
  whole->run_for(30 * ms_ps);
  auto const expected = one.take();

  auto       sliced = tim_machine();
  profiled   many{*sliced->target};
  auto const end = vtime{sliced->now().ps + 30 * ms_ps};
  for(;;)
  {
    auto const ran = sliced->run(
        {.max_instructions = icount{700},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = end});
    if(ran.stopped)
    {
      break;
    }
  }
  CHECK(many.take() == expected);
  CHECK(expected.sleeps.count == 30);
  CHECK(expected.partial_sleeps == 1);
}

TEST_CASE("clock records give the end time across the PLL switch")
{
  auto       f     = spi_machine();
  auto const start = f->now();
  auto const from  = f->target->virtual_clock().now();
  captured   c{*f->target, {core::trace_kind::clock}};
  f->run_for(5 * ms_ps);
  auto const end_count = f->target->virtual_clock().now();
  c.handle.reset();
  auto const clocks = c.of(core::trace_kind::clock);
  REQUIRE(clocks.size() >= 2);
  CHECK(hz_of(clocks.front()) == 4'000'000);
  CHECK(hz_of(clocks.back()) == 160'000'000);
  std::uint64_t at = from.value;
  std::uint64_t ps = start.ps;
  std::uint64_t hz = 0;
  for(auto const& record : c.records)
  {
    if(record.kind != core::trace_kind::clock)
    {
      continue;
    }
    at += record.dt;
    if(hz != 0)
    {
      ps += popcorn::vtime_from_instructions(record.dt, hz).ps;
    }
    hz = hz_of(record);
  }
  ps += popcorn::vtime_from_instructions(end_count.value - at, hz).ps;
  CHECK(ps == f->now().ps);
}

TEST_CASE("a reset with no record after it still cuts, and two resets cut twice")
{
  auto     f = tim_machine();
  profiled p{*f->target};
  f->run_for(ms_ps);
  f->target->reset();
  f->target->reset();
  auto const result = p.take();
  CHECK(result.cuts == 2);
}

TEST_CASE("each reset writes a flagged time record at once to a sink with no clock")
{
  auto     f = tim_machine();
  captured c{*f->target, {core::trace_kind::block}};
  f->run_for(ms_ps);
  f->target->reset();
  f->target->reset();
  c.handle.reset();
  auto const rebases = std::ranges::count_if(c.records, [](core::trace_record const& r) {
    return r.kind == core::trace_kind::time and r.flags == core::trace_rebase_flag;
  });
  CHECK(rebases == 2);
  CHECK(c.records.back().kind == core::trace_kind::time);
}

TEST_CASE("mmio records name the debugger and leave the CPU's unflagged")
{
  auto     f = tim_machine();
  captured c{*f->target, {core::trace_kind::mmio}};
  f->run_for(2 * ms_ps);
  REQUIRE(f->target->mem().read(tim2_cnt, 4).has_value());
  c.handle.reset();
  auto const mmio = c.of(core::trace_kind::mmio);
  REQUIRE(mmio.size() > 1);
  CHECK((mmio.back().flags & core::trace_debugger_flag) != 0);
  CHECK((mmio.back().flags & core::trace_dma_flag) == 0);
  for(auto const& record : mmio | std::views::take(mmio.size() - 1))
  {
    CHECK((record.flags & (core::trace_debugger_flag | core::trace_dma_flag)) == 0);
  }
}

TEST_CASE("a run sliced at fixed time stops profiles as one run")
{
  auto     whole = spi_machine();
  profiled one{*whole->target, {.code = obs::machine_code_reader(*whole->target)}};
  whole->run_for(20 * ms_ps);
  auto const expected = one.take();

  auto       sliced = spi_machine();
  profiled   many{*sliced->target, {.code = obs::machine_code_reader(*sliced->target)}};
  auto const base = sliced->now().ps;
  for(std::uint64_t slice = 1; slice <= 40; ++slice)
  {
    REQUIRE(
        sliced
            ->run_to_stop(
                {.max_instructions = icount{1'000'000'000},
                 .until_time       = vtime{base + slice * ms_ps / 2}})
            .reason
        == stop_reason::time);
  }
  auto const got = many.take();
  CHECK(sliced->now() == whole->now());
  CHECK(got.code == expected.code);
  CHECK(got.instructions == expected.instructions);
  CHECK(got.elapsed == expected.elapsed);
  CHECK(got.frames == expected.frames);
  CHECK(got.instructions + gaps(got) == got.elapsed);
}

TEST_CASE("a ranged sink gets the clock and sleep records whatever its ranges")
{
  auto spi  = spi_machine();
  auto idle = sym("spi_flash", "prvIdleTask");
  {
    captured c{
        *spi->target,
        {core::trace_kind::clock, core::trace_kind::block},
        {{.start = idle, .size = 2}}
    };
    spi->run_for(5 * ms_ps);
    c.handle.reset();
    auto const clocks = c.of(core::trace_kind::clock);
    REQUIRE(clocks.size() == 2);
    CHECK(hz_of(clocks.front()) == 4'000'000);
    CHECK(hz_of(clocks.back()) == 160'000'000);
  }
  auto     tim = tim_machine();
  captured c{
      *tim->target,
      {core::trace_kind::sleep, core::trace_kind::block},
      {{.start = sym("tim_ticks", "main"), .size = 2}}
  };
  tim->run_for(10 * ms_ps);
  c.handle.reset();
  CHECK(c.of(core::trace_kind::sleep).size() == 10);
}

TEST_CASE("a sleep on exception exit ends with the next entry, not tail-chained")
{
  auto f = tim_machine();
  awake_in_main(*f);
  auto const scr = f->target->mem().read(0xe000'ed10, 4);
  REQUIRE(scr.has_value());
  REQUIRE(f->target->mem().write(0xe000'ed10, 4, *scr | 2u));
  captured c{
      *f->target, {core::trace_kind::sleep, core::trace_kind::irq}
  };
  profiled p{*f->target};
  f->run_for(10 * ms_ps);
  c.handle.reset();
  auto const  result = p.take();
  auto const& tim2   = result.exceptions.at(tim2_exception);
  CHECK(result.sleep_reasons == std::array<std::uint64_t, 4>{0, 1, 0, 10});
  CHECK(result.partial_sleeps == 1);
  CHECK(tim2.tail_chained == 0);
  CHECK(tim2.wakes.count == tim2.entries);
  auto const exit = std::ranges::find_if(c.records, [](core::trace_record const& r) {
    return r.kind == core::trace_kind::irq and r.flags == 0;
  });
  REQUIRE(std::distance(exit, c.records.end()) >= 3);
  CHECK(std::next(exit)->kind == core::trace_kind::sleep);
  CHECK(std::next(exit)->flags == 3);
  CHECK(std::next(exit, 2)->kind == core::trace_kind::irq);
  CHECK(std::next(exit, 2)->flags == 1);
}

TEST_CASE("a restore while the core sleeps cuts once and the next sleep starts at the restore")
{
  auto f = tim_machine();
  f->run_for(ms_ps / 2);
  profiled   p{*f->target};
  auto const asleep = f->target->snapshot();
  f->run_for(2 * ms_ps);
  f->target->restore(asleep);
  f->run_for(2 * ms_ps);
  auto const result = p.take();
  CHECK(result.cuts == 1);
  CHECK(result.other_gaps == obs::profile_gaps{});
  CHECK(result.sleeps.longest <= 3979);
  CHECK(result.instructions + gaps(result) == result.elapsed);
}

TEST_CASE("DMA accesses are counted apart from the CPU's")
{
  auto     f = gpdma_machine();
  profiled p{*f->target};
  REQUIRE(
      f->run_to_stop({.max_instructions = icount{100'000'000},
                      .until            = sym("gpdma_uart", "all_transfers_done")})
          .reason
      == stop_reason::address);
  auto const result = p.take();
  REQUIRE(not result.dma_registers.empty());
  CHECK(result.debugger_registers.empty());
  for(auto const& reg : result.dma_registers)
  {
    CHECK(reg.reads + reg.writes > 0);
  }
}

TEST_CASE("a block sink gets batches of at most 4096 records during a long slice")
{
  auto                     f = spi_machine();
  std::vector<std::size_t> sizes;
  auto const               handle = f->target->add_trace_sink(
      {core::trace_kind::block},
      [&sizes](std::span<core::trace_record const> batch) { sizes.push_back(batch.size()); });
  f->run_for(2 * ms_ps);
  REQUIRE(sizes.size() > 2);
  CHECK(sizes.front() == 4096);
  CHECK(std::ranges::max(sizes) == 4096);
}

TEST_CASE("a block after a hold past the 32-bit dt range keeps its full distance")
{
  auto f = tim_machine();
  place(*f, {0xb672, 0xe7fe});
  auto const cr1 = f->target->mem().read(tim2_cnt - 0x24, 4);
  REQUIRE(cr1.has_value());
  REQUIRE(f->target->mem().write(tim2_cnt - 0x24, 4, *cr1 & ~1u));
  auto const start = f->target->virtual_clock().now();
  captured   c{*f->target, {core::trace_kind::block}};
  f->target->hold_core(true);
  f->run_for(1100 * 1000 * ms_ps, icount{5'000'000'000});
  f->target->hold_core(false);
  f->run_for(ms_ps / 1000);
  c.handle.reset();
  REQUIRE(not c.records.empty());
  auto at = start.value;
  for(auto const& record : c.records)
  {
    if(record.kind == core::trace_kind::time)
    {
      at = std::uint64_t{record.payload[0]} | std::uint64_t{record.payload[1]} << 32;
      continue;
    }
    at += record.dt;
    break;
  }
  CHECK(c.records.front().kind == core::trace_kind::time);
  CHECK(at - start.value >= 4'400'000'000);
}

TEST_CASE(
    "loop records aggregate by head: runs, passes, instructions, "
    "the furthest block and the enders")
{
  auto const loop = [](address head, std::uint8_t last, std::uint32_t passes,
                       std::uint32_t instructions, core::loop_ender ender,
                       std::uint32_t exception = 0) {
    return core::trace_record{
        .kind    = core::trace_kind::loop,
        .flags   = last,
        .aux     = core::trace_loop_ender(ender, exception),
        .dt      = instructions,
        .payload = {head, passes, instructions},
    };
  };
  auto const result = fed(
      {loop(0x0800'0200, 4, 10, 31, core::loop_ender::exception, 15),
       loop(0x0800'0100, 0, 3, 9, core::loop_ender::progress),
       loop(0x0800'0200, 254, 5, 16, core::loop_ender::exception, 15),
       loop(0x0800'0200, 2, 1, 4, core::loop_ender::sleep),
       loop(0x0800'0100, 0, 7, 22, core::loop_ender::rebase),
       loop(0x0800'0100, 0, 2, 7, core::loop_ender::end),
       loop(0x0800'0100, 0, 1, 3, core::loop_ender::call)});
  REQUIRE(result.loops.size() == 2);
  auto const& low = result.loops[0];
  CHECK(low.head == 0x0800'0100);
  CHECK(low.last == 0x0800'0100);
  CHECK(low.runs == 4);
  CHECK(low.passes == 13);
  CHECK(low.instructions == 41);
  CHECK(low.enders == std::array<std::uint64_t, 6>{1, 0, 0, 1, 1, 1});
  auto const& high = result.loops[1];
  CHECK(high.head == 0x0800'0200);
  CHECK(high.last == 0x0800'02fe);
  CHECK(high.runs == 3);
  CHECK(high.passes == 16);
  CHECK(high.instructions == 51);
  CHECK(high.enders == std::array<std::uint64_t, 6>{0, 2, 1, 0, 0, 0});
  CHECK(result.instructions == 0);
}

TEST_CASE("a loop run reaching before the sink's start or its last rebase counts from there")
{
  auto const loop = [](std::uint32_t dt, std::uint32_t instructions) {
    return core::trace_record{
        .kind    = core::trace_kind::loop,
        .aux     = core::trace_loop_ender(core::loop_ender::end),
        .dt      = dt,
        .payload = {0x0800'0100, 7, instructions},
    };
  };
  auto const result = fed({
      loop(40, 0xffff'ffff),
      core::trace_record{
                         .kind = core::trace_kind::time, .flags = core::trace_rebase_flag, .payload = {10, 0}},
      loop(25, 900)
  });
  REQUIRE(result.loops.size() == 1);
  CHECK(result.loops.front().runs == 2);
  CHECK(result.loops.front().instructions == 40 + 25);
}

TEST_CASE("the FreeRTOS idle loop is one engine loop from prvCheckTasksWaitingTermination")
{
  auto     f = spi_machine();
  profiled p{*f->target};
  f->target->set_loop_reports(1);
  f->run_for(100 * ms_ps);
  f->target->set_loop_reports(0);
  auto const result = p.take();
  auto const check  = sym("spi_flash", "prvCheckTasksWaitingTermination") & ~address{1};
  for(auto const& loop : result.loops)
  {
    CAPTURE(loop.head);
    CHECK(loop.last >= loop.head);
    CHECK(loop.last - loop.head < core::loop_window_bytes);
  }
  auto const idle = std::ranges::find(result.loops, check, &obs::profile_loop::head);
  REQUIRE(idle != result.loops.end());
  CHECK(idle->head == 0x0800'e00c);
  CHECK(idle->last == 0x0800'e05c);
  CHECK(idle->runs == 171);
  CHECK(idle->passes == 1'046'932);
  CHECK(idle->instructions == 11'517'562);
  CHECK(idle->enders == std::array<std::uint64_t, 6>{0, 171, 0, 0, 0, 0});
  CHECK(result.instructions == 15'829'458);
}
