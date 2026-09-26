#include "arch_fixture.hpp"

#include "cortex_m.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/trace.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::words_at;

constexpr address       icsr         = 0xE000'ED04;
constexpr address       syst_csr     = 0xE000'E010;
constexpr std::uint32_t pendstset    = 1u << 26;
constexpr std::uint64_t period       = 4000;
constexpr std::uint64_t ps_per_instr = 250'000;

auto fn(std::string_view name) -> address
{
  return sym("timers", name);
}

auto code(std::string_view name) -> address
{
  return fn(name) & ~address{1};
}

auto booted(core::machine_options options = {}) -> arch_fixture
{
  options.trace_capacity = 16384;
  arch_fixture f{m33(), "timers", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.set("pc", fn("timers_setup"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto ticks(arch_fixture const& f) -> std::uint32_t
{
  return f.word(fn("ticks"));
}

auto log_of(arch_fixture const& f) -> std::vector<std::uint32_t>
{
  return words_at(*f.target, fn("log_buf"), f.word(fn("log_len")));
}

auto start(arch_fixture const& f, std::string_view entry) -> popcorn::stop
{
  f.set("pc", fn(entry));
  return f.run_to_stop();
}

auto run_to_instruction(arch_fixture const& f, std::uint64_t target) -> core::slice_result
{
  auto const& clock = f.target->virtual_clock();
  REQUIRE(target >= clock.now().value);
  auto const at     = vtime{clock.time().ps + (target - clock.now().value) * ps_per_instr};
  auto const result = f.run(
      {.max_instructions = icount{400'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until_time       = at});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::time);
  return result;
}

auto phase(arch_fixture const& f) -> std::uint64_t
{
  auto const stopped = start(f, "tick_phase");
  REQUIRE(stopped.reason == stop_reason::user);
  auto const value = f.reg("r0");
  return stopped.instructions.value - (3999 - value + 1);
}

auto tick_entries(arch_fixture const& f) -> std::vector<std::uint64_t>
{
  auto&                      ring = f.target->trace();
  std::uint64_t              at   = ring.origin().value;
  std::vector<std::uint64_t> out;
  for(auto const& record : ring.records())
  {
    at += record.dt;
    if(record.kind == core::trace_kind::irq and record.flags == 1 and record.aux == 15)
    {
      out.push_back(at);
    }
  }
  return out;
}
}

TEST_CASE("SysTick at 1 kHz ticks 1000 times a virtual second on the anchor's phase")
{
  auto       f     = booted();
  auto const e     = phase(f);
  auto const first = run_to_instruction(f, e + 1000 * period + period / 2);
  CHECK(ticks(f) == 1000);
  MESSAGE("executed for 1000 ticks: " << first.executed.value);
  CHECK(first.executed.value < 40'000);
  auto const entries = tick_entries(f);
  REQUIRE(entries.size() == 1000);
  for(std::size_t k = 0; k < entries.size(); ++k)
  {
    CAPTURE(k);
    CHECK(entries[k] == e + (k + 1) * period);
  }
  run_to_instruction(f, e + 10'000 * period + period / 2);
  CHECK(ticks(f) == 10'000);
}

TEST_CASE("COUNTFLAG sets once per wrap and a wfi with TICKINT clear stops idle")
{
  auto       f       = booted();
  auto const stopped = start(f, "countflag");
  CHECK(stopped.reason == stop_reason::idle);
  auto const log = log_of(f);
  REQUIRE(log.size() == 5);
  for(std::size_t index = 1; index < log.size(); ++index)
  {
    CHECK(log[index] == log[1]);
  }
}

TEST_CASE("each paired counter read and its bkpt share one 1 KB page")
{
  auto const& image = popcorn::testing::elf("timers");
  for(std::string_view const name :
      {"tick_phase", "cvr_pair", "cyc_pair", "cyc_same_block", "cyc_frozen", "cyc_restart",
       "rvr_race"})
  {
    CAPTURE(name);
    auto const found = std::ranges::find(image.symbols, name, &popcorn::image::symbol::name);
    REQUIRE(found != image.symbols.end());
    REQUIRE(found->size != 0);
    CHECK(found->value / 1024 == (found->value + found->size - 1) / 1024);
  }
}

TEST_CASE("CVR and CYCCNT count executed instructions between two reads")
{
  {
    auto       f  = booted();
    auto const i1 = start(f, "cvr_pair").instructions.value;
    auto const r4 = f.reg("r4");
    auto const i2 = f.run_to_stop().instructions.value;
    CHECK(r4 - f.reg("r5") == i2 - i1);
  }
  {
    auto       f  = booted();
    auto const i1 = start(f, "cyc_pair").instructions.value;
    auto const r4 = f.reg("r4");
    auto const i2 = f.run_to_stop().instructions.value;
    CHECK(f.reg("r5") - r4 == i2 - i1);
  }
  {
    auto f = booted();
    REQUIRE(start(f, "cyc_same_block").reason == stop_reason::user);
    CHECK(f.reg("r4") == f.reg("r5"));
    CHECK(f.reg("r4") != 0);
  }
  {
    auto f = booted();
    REQUIRE(start(f, "cyc_frozen").reason == stop_reason::user);
    CHECK(f.reg("r4") == f.reg("r5"));
  }
  {
    auto f = booted();
    REQUIRE(start(f, "cyc_restart").reason == stop_reason::user);
    CHECK(f.reg("r4") == 0);
  }
}

TEST_CASE("an RVR write in the block that crosses zero takes that tick once")
{
  auto f = booted();
  REQUIRE(start(f, "rvr_race").reason == stop_reason::user);
  auto const cycles  = f.reg("r4");
  auto const pending = (f.word(icsr) & pendstset) != 0 ? 1u : 0u;
  CHECK(cycles / 100 >= 10);
  CHECK(ticks(f) + pending == cycles / 100);
}

TEST_CASE("a restore re-arms SysTick from the saved state without a queue mirror")
{
  auto       f   = booted();
  auto const e   = phase(f);
  auto const mid = e + 5 * period + period / 2;
  run_to_instruction(f, mid);
  REQUIRE(ticks(f) == 5);
  auto const taken = f.target->snapshot();
  REQUIRE(f.target->mem().write(syst_csr, 4, 0));
  for(int round = 0; round < 2; ++round)
  {
    CAPTURE(round);
    f.target->restore(taken);
    run_to_instruction(f, mid + 10 * period);
    CHECK(ticks(f) == 15);
  }
}

TEST_CASE("a serialized snapshot restored into a reset-twice machine ticks once per period")
{
  auto       f   = booted();
  auto const e   = phase(f);
  auto const mid = e + 5 * period + period / 2;
  run_to_instruction(f, mid);
  auto const bytes = core::encode_snapshot(f.target->snapshot().data());

  auto other = booted();
  other.target->reset();
  other.target->reset();
  other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  run_to_instruction(other, mid + 10 * period);
  CHECK(ticks(other) == 15);
}

TEST_CASE("a slice stopped at a crossing holds SysTick pending, and a restore replays it once")
{
  std::vector<std::vector<std::byte>> encoded;
  for(int machine = 0; machine < 2; ++machine)
  {
    auto       f       = booted();
    auto const e       = phase(f);
    auto const instant = e + 5 * period;
    run_to_instruction(f, instant);
    CHECK(ticks(f) == 4);
    CHECK((f.word(icsr) & pendstset) != 0);
    auto const taken = f.target->snapshot();
    encoded.push_back(core::encode_snapshot(taken.data()));
    run_to_instruction(f, instant + 10 * period + period / 2);
    CHECK(ticks(f) == 4 + 11);
    f.target->restore(taken);
    run_to_instruction(f, instant + 10 * period + period / 2);
    CHECK(ticks(f) == 4 + 11);
  }
  CHECK(encoded[0] == encoded[1]);
}

TEST_CASE("wfi with nothing queued stops idle past the wfi, narrow and wide")
{
  {
    auto       f       = booted();
    auto const stopped = start(f, "wfi_empty");
    CHECK(stopped.reason == stop_reason::idle);
    CHECK(stopped.where.pc == code("wfi_empty") + 2);
  }
  {
    auto       f       = booted();
    auto const stopped = start(f, "wfi_wide");
    CHECK(stopped.reason == stop_reason::idle);
    CHECK(stopped.where.pc == code("wfi_wide") + 4);
  }
}

TEST_CASE("until right after a sleeping wfi stops asleep, and the next slice sleeps to the tick")
{
  auto       f   = booted();
  auto const wfi = code("wfi_tick_at");
  f.set("pc", fn("wfi_tick"));
  auto const stopped = f.run_to_stop({.until = wfi + 2});
  REQUIRE(stopped.reason == stop_reason::address);
  CHECK(f.module->idle());
  auto const before = f.target->virtual_clock().now().value;
  auto const slice =
      f.run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{1}});
  CHECK(slice.executed.value == 1);
  CHECK(f.target->virtual_clock().now().value - before > 1);
  CHECK((f.reg("xpsr") & 0x1ff) == 15);
}

TEST_CASE("an engine exit right after a wfi reached by a return is not taken for the wfi")
{
  auto       f   = booted();
  auto const wfi = code("wfi_spot");
  f.set("pc", fn("return_only"));
  f.set("lr", (wfi + 2) | 1);
  auto const stopped = f.run_to_stop({.until = wfi + 2});
  REQUIRE(stopped.reason == stop_reason::address);
  CHECK(stopped.where.pc == wfi + 2);
  CHECK_FALSE(f.module->idle());
  auto const before = f.target->virtual_clock().now().value;
  auto const slice =
      f.run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{1}});
  CHECK(slice.executed.value == 1);
  CHECK(f.target->virtual_clock().now().value - before == 1);
}

TEST_CASE("wfi with nothing queued sleeps to until_time, the same again after a restore")
{
  auto f = booted();
  REQUIRE(start(f, "wfi_empty").reason == stop_reason::idle);
  auto const saved  = f.target->snapshot();
  auto const target = popcorn::vtime{f.target->virtual_clock().time().ps + 1'000'000'000};
  auto const sleep  = [&f, target] {
    auto const stopped = f.run_to_stop({.until_time = target});
    CHECK(stopped.reason == stop_reason::time);
    CHECK(stopped.time == target);
    CHECK(stopped.where.pc == code("wfi_empty") + 2);
    CHECK(f.module->idle());
    return stopped.instructions;
  };
  auto const first = sleep();
  f.target->restore(saved);
  CHECK(sleep() == first);
}

TEST_CASE("sleep-on-exit sleeps to an until_time between two ticks")
{
  auto f = booted();
  f.set("pc", fn("sleep_on_exit"));
  auto const begin = f.target->virtual_clock().now().value;
  run_to_instruction(f, begin + 5 * period);
  auto const before  = ticks(f);
  auto const clock   = &f.target->virtual_clock();
  auto const current = f.target->mem().read(0xE000'E018, 4);
  REQUIRE(current.has_value());
  auto const ahead = std::uint64_t{*current} + period / 2;
  auto const target =
      popcorn::vtime{clock->time().ps + ahead * 1'000'000'000'000 / clock->instruction_hz()};
  auto const stopped = f.run_to_stop({.until_time = target});
  CHECK(stopped.reason == stop_reason::time);
  CHECK(stopped.time == target);
  CHECK(ticks(f) == before + 1);
  CHECK(f.module->idle());
  CHECK(f.word(fn("thread_counter")) == 0);
}

TEST_CASE("a pc write while asleep runs from the new pc and never completes the old wfi")
{
  auto f = booted();
  REQUIRE(start(f, "wfi_empty").reason == stop_reason::idle);
  REQUIRE(f.module->idle());
  f.set("pc", fn("moved_here"));
  CHECK_FALSE(f.module->idle());
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(log_of(f) == std::vector<std::uint32_t>{0x3E7});
}

TEST_CASE("a wake with PRIMASK set resumes past the wfi and the handler runs after cpsie")
{
  auto f = booted();
  REQUIRE(start(f, "wake_masked").reason == stop_reason::user);
  CHECK(log_of(f) == std::vector<std::uint32_t>{0, 1});
}

TEST_CASE("a poll loop with SysTick and a tail-chained PendSV reading a register is a spin")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  auto       f           = booted(std::move(options));
  auto const stopped     = start(f, "spin_tick");
  REQUIRE(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.find("UNMODELLED.STAT") != std::string::npos);
  CHECK(stopped.detail.find("TESTDEV") == std::string::npos);
}

TEST_CASE("a CVR write restarts the period and orphans the queued tick event")
{
  auto       f   = booted();
  auto const e   = phase(f);
  auto const mid = e + 5 * period + period / 2;
  run_to_instruction(f, mid);
  REQUIRE(ticks(f) == 5);
  auto const written = f.target->virtual_clock().now().value;
  REQUIRE(f.target->mem().write(0xE000'E018, 4, 0));
  run_to_instruction(f, written + 10 * period + period / 4);
  CHECK(ticks(f) == 15);
}

TEST_CASE("wfe sleeps to the next tick, passes after an sev, and sleeps once after sev wfe wfe")
{
  for(int round = 0; round < 2; ++round)
  {
    CAPTURE(round);
    {
      auto f = booted();
      REQUIRE(start(f, "wfe_basic").reason == stop_reason::user);
      CHECK(log_of(f) == std::vector<std::uint32_t>{0, 1});
    }
    {
      auto       f     = booted();
      auto const begin = f.target->virtual_clock().now().value;
      auto const done  = start(f, "wfe_sev");
      REQUIRE(done.reason == stop_reason::user);
      CHECK(log_of(f) == std::vector<std::uint32_t>{0});
      CHECK(done.instructions.value - begin < period);
    }
    {
      auto f = booted();
      REQUIRE(start(f, "wfe_sev_twice").reason == stop_reason::user);
      CHECK(log_of(f) == std::vector<std::uint32_t>{1});
    }
  }
}

TEST_CASE("yield runs as a nop")
{
  auto f = booted();
  REQUIRE(start(f, "yield_loop").reason == stop_reason::user);
  auto const log = log_of(f);
  REQUIRE(log.size() == 100);
  for(std::uint32_t index = 0; index < 100; ++index)
  {
    CHECK(log[index] == index);
  }
}

TEST_CASE("with SEVONPEND a disabled IRQ pended by the host wakes wfe without a handler")
{
  auto f = booted();
  REQUIRE(start(f, "wfe_sevonpend").reason == stop_reason::user);
  REQUIRE(f.target->mem().write(0xE000'E200, 4, 1u << 6));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(log_of(f) == std::vector<std::uint32_t>{0x5E});
  CHECK((f.target->mem().read(0xE000'E200, 4).value_or(0) & (1u << 6)) != 0);
}

TEST_CASE("sleep-on-exit keeps Thread from running and costs the handler only per tick")
{
  auto f = booted();
  f.set("pc", fn("sleep_on_exit"));
  auto const begin = f.target->virtual_clock().now().value;
  run_to_instruction(f, begin + 5 * period);
  auto const first = ticks(f);
  auto const b     = run_to_instruction(f, begin + 10 * period);
  CHECK(ticks(f) == first + 5);
  auto const c = run_to_instruction(f, begin + 15 * period);
  CHECK(ticks(f) == first + 10);
  CHECK(b.executed.value == c.executed.value);
  CHECK(f.word(fn("thread_counter")) == 0);
  CHECK(f.module->idle());
}

TEST_CASE("wfe respects PRIMASK: a masked IRQ leaves it asleep unless SEVONPEND sets the event")
{
  {
    auto f = booted();
    REQUIRE(start(f, "wfe_primask").reason == stop_reason::user);
    REQUIRE(f.target->mem().write(0xE000'E200, 4, 1u << 7));
    CHECK(f.run_to_stop().reason == stop_reason::idle);
    CHECK(log_of(f).empty());
  }
  {
    auto f = booted();
    REQUIRE(start(f, "wfe_primask_sevonpend").reason == stop_reason::user);
    REQUIRE(f.target->mem().write(0xE000'E200, 4, 1u << 7));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    CHECK(log_of(f) == std::vector<std::uint32_t>{0x7E});
  }
}
