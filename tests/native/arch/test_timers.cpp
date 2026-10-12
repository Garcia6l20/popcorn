#include "arch_fixture.hpp"

#include "cortex_m.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/trace.hpp>

#include <doctest/doctest.h>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
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
constexpr address       syst_rvr     = 0xE000'E014;
constexpr address       syst_cvr     = 0xE000'E018;
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

void put(arch_fixture const& f, address at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value));
}

auto get(arch_fixture const& f, address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto sleeping() -> arch_fixture
{
  auto f = booted();
  f.set("pc", fn("tick_1khz"));
  run_to_instruction(f, f.target->virtual_clock().now().value + 100);
  return f;
}

auto now(arch_fixture const& f) -> std::uint64_t
{
  return f.target->virtual_clock().now().value;
}

void tickless_exit(arch_fixture const& f, std::uint32_t remainder, std::uint32_t control)
{
  put(f, syst_csr, control & ~1u);
  put(f, syst_rvr, remainder);
  put(f, syst_cvr, 0);
  put(f, syst_csr, control);
  put(f, syst_rvr, 3999);
}

struct tickless_idle
{
  static constexpr std::uint64_t seconds   = 5;
  static constexpr std::uint64_t busy      = 200;
  static constexpr std::uint64_t latency   = 20;
  static constexpr std::uint32_t countflag = 1u << 16;
  static constexpr std::uint32_t pendstclr = 1u << 25;
  static constexpr std::uint32_t processor = 7;

  arch_fixture const& f;
  bool                core_clock{true};
  std::uint64_t       stepped{};
  std::uint64_t       tick_wakes{};
  std::uint64_t       event_wakes{};
  std::uint64_t       sleeps{};

  [[nodiscard]] auto divisor() const -> std::uint32_t
  {
    return core_clock ? 1 : 8;
  }

  [[nodiscard]] auto one() const -> std::uint32_t
  {
    return static_cast<std::uint32_t>(period) / divisor();
  }

  [[nodiscard]] auto stopped() const -> std::uint32_t
  {
    return core_clock ? 6 : 2;
  }

  void start() const
  {
    put(f, syst_csr, stopped());
    put(f, syst_rvr, one() - 1);
    put(f, syst_cvr, 0);
    put(f, syst_csr, stopped() | 1u);
  }

  [[nodiscard]] auto sleep(std::uint32_t expected) -> std::uint32_t
  {
    ++sleeps;
    put(f, syst_csr, stopped());
    auto const left   = get(f, syst_cvr);
    auto       reload = (left == 0 ? one() : left) + one() * (expected - 1);
    if((get(f, icsr) & pendstset) != 0)
    {
      put(f, icsr, pendstclr);
      reload -= one();
    }
    put(f, syst_rvr, reload);
    put(f, syst_cvr, 0);
    put(f, syst_csr, stopped() | 1u);
    return reload;
  }

  void wake(std::uint32_t expected, std::uint32_t reload)
  {
    put(f, syst_csr, stopped());
    std::uint32_t complete{};
    if((get(f, syst_csr) & countflag) != 0)
    {
      auto load = (one() - 1) - (reload - get(f, syst_cvr));
      if(load == 0 or load > one())
      {
        load = one() - 1;
      }
      put(f, syst_rvr, load);
      complete = expected - 1;
      ++tick_wakes;
    }
    else
    {
      auto left = get(f, syst_cvr);
      if(not core_clock and left == 0)
      {
        left = reload;
      }
      auto const done = expected * one() - left;
      complete        = done / one();
      put(f, syst_rvr, (complete + 1) * one() - done);
      ++event_wakes;
    }
    put(f, syst_cvr, 0);
    put(f, syst_csr, processor);
    if(not core_clock)
    {
      put(f, syst_csr, processor & ~1u);
      if((get(f, syst_csr) & countflag) != 0)
      {
        put(f, syst_cvr, 0);
      }
    }
    put(f, syst_rvr, one() - 1);
    if(not core_clock)
    {
      put(f, syst_csr, stopped() | 1u);
    }
    stepped += complete;
  }

  /**
   * @brief Ticks, rounded up, lost to the reload edges the port does not compensate: on the
   * processor clock one count at each of the two restarts per sleep, on the external reference one
   * count at the restart on entry, the restart on wake being latched from the processor clock.
   */
  [[nodiscard]] auto edge_loss() const -> std::int64_t
  {
    return static_cast<std::int64_t>((sleeps * (core_clock ? 2 : divisor()) + period - 1) / period);
  }

  /**
   * @brief Sleeps @p expected ticks at a time for #seconds, woken by events at the instruction
   *        counts @p next returns or by SysTick, as the FreeRTOS ARMv8-M port does.
   * @return Ticks the counted tick is behind the ticks of virtual time.
   */
  auto drift(std::uint32_t expected, auto next) -> std::int64_t
  {
    start();
    auto const begin  = now(f);
    auto const before = ticks(f);
    auto const end    = begin + seconds * 1000 * period;
    auto       event  = next(begin);
    while(now(f) < end)
    {
      run_to_instruction(f, now(f) + busy);
      event              = std::max(event, now(f) + busy);
      auto const reload  = sleep(expected);
      auto const crossed = now(f) + std::uint64_t{1 + reload} * divisor() + latency;
      run_to_instruction(f, std::min({event, crossed, end}));
      if(now(f) >= event)
      {
        event = next(event);
      }
      wake(expected, reload);
    }
    auto const counted = static_cast<std::int64_t>(ticks(f) - before + stepped);
    return static_cast<std::int64_t>((now(f) - begin) / period) - counted;
  }
};
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

TEST_CASE("a tickless exit reloads the remainder at the edge and the full tick at the next wrap")
{
  auto f = sleeping();
  tickless_exit(f, 999, 7);
  auto const written = now(f);
  auto const before  = ticks(f);
  CHECK(get(f, syst_rvr) == 3999);
  run_to_instruction(f, written + 1000 - 100);
  CHECK(ticks(f) == before);
  run_to_instruction(f, written + 1000 + 100);
  CHECK(ticks(f) == before + 1);
  run_to_instruction(f, written + 1000 + period - 100);
  CHECK(ticks(f) == before + 1);
  run_to_instruction(f, written + 1000 + period + 100);
  CHECK(ticks(f) == before + 2);
}

TEST_CASE("on the external reference an RVR write before the edge wins, one after it waits")
{
  constexpr std::uint64_t divisor = 8;
  {
    auto f = sleeping();
    tickless_exit(f, 999, 3);
    auto const written = now(f);
    auto const before  = ticks(f);
    run_to_instruction(f, written + divisor * 1000 + 100);
    CHECK(ticks(f) == before);
    run_to_instruction(f, written + divisor * period - 100);
    CHECK(ticks(f) == before);
    run_to_instruction(f, written + divisor * period + 100);
    CHECK(ticks(f) == before + 1);
  }
  {
    auto f = sleeping();
    put(f, syst_csr, 2);
    put(f, syst_rvr, 999);
    put(f, syst_cvr, 0);
    put(f, syst_csr, 3);
    auto const written = now(f);
    auto const before  = ticks(f);
    run_to_instruction(f, written + 2 * divisor);
    CHECK(get(f, syst_cvr) == 998);
    put(f, syst_rvr, 3999);
    run_to_instruction(f, written + divisor * 1000 + 100);
    CHECK(ticks(f) == before + 1);
    run_to_instruction(f, written + divisor * (1000 + period) - 100);
    CHECK(ticks(f) == before + 1);
    run_to_instruction(f, written + divisor * (1000 + period) + 100);
    CHECK(ticks(f) == before + 2);
  }
}

TEST_CASE("a CVR write while enabled reloads RVR as it is at the next processor clock")
{
  auto f = sleeping();
  put(f, syst_rvr, 999);
  put(f, syst_cvr, 0);
  CHECK(get(f, syst_cvr) == 999);
  put(f, syst_rvr, 3999);
  auto const written = now(f);
  auto const before  = ticks(f);
  CHECK(get(f, syst_cvr) == 999);
  run_to_instruction(f, written + 1000 + 100);
  CHECK(ticks(f) == before + 1);
  run_to_instruction(f, written + 1000 + period - 100);
  CHECK(ticks(f) == before + 1);
  run_to_instruction(f, written + 1000 + period + 100);
  CHECK(ticks(f) == before + 2);
}

TEST_CASE("a switch to the external reference after the reload edge counts on from the reload")
{
  auto f = sleeping();
  put(f, syst_rvr, 999);
  put(f, syst_cvr, 0);
  put(f, syst_csr, 3);
  auto const written = now(f);
  auto const before  = ticks(f);
  CHECK(get(f, syst_cvr) == 999);
  run_to_instruction(f, written + 8 * 999 - 4);
  CHECK(ticks(f) == before);
  run_to_instruction(f, written + 8 * 999 + 4);
  CHECK(ticks(f) == before + 1);
}

TEST_CASE("a snapshot inside a tickless exit restores the reload it had latched or not")
{
  auto f = sleeping();
  put(f, syst_csr, 6);
  put(f, syst_rvr, 999);
  put(f, syst_cvr, 0);
  put(f, syst_csr, 7);
  auto const enabled = core::encode_snapshot(f.target->snapshot().data());
  put(f, syst_rvr, 3999);
  auto const latched = core::encode_snapshot(f.target->snapshot().data());
  auto const written = now(f);
  auto const before  = ticks(f);
  for(auto const* bytes : {&enabled, &latched})
  {
    CAPTURE(bytes == &latched);
    auto other = booted();
    other.target->reset();
    other.target->restore(core::snapshot{core::decode_snapshot(*bytes)});
    if(bytes == &enabled)
    {
      put(other, syst_rvr, 3999);
    }
    REQUIRE(get(other, syst_rvr) == 3999);
    run_to_instruction(other, written + 1000 + 100);
    CHECK(ticks(other) == before + 1);
    run_to_instruction(other, written + 1000 + period - 100);
    CHECK(ticks(other) == before + 1);
    run_to_instruction(other, written + 1000 + period + 100);
    CHECK(ticks(other) == before + 2);
  }
}

namespace
{
auto every_ms(std::uint64_t at) -> std::uint64_t
{
  return at + period;
}

auto irregular()
{
  return [seed = std::uint32_t{12345}](std::uint64_t at) mutable {
    seed = seed * 1'103'515'245u + 12'345u;
    return at + 1200 + (seed >> 8) % 60'000;
  };
}
}

TEST_CASE("a tickless idle loses only the uncompensated reload edges against virtual time")
{
  for(auto const core_clock : {true, false})
  {
    CAPTURE(core_clock);
    {
      auto          f = sleeping();
      tickless_idle idle{.f = f, .core_clock = core_clock};
      auto const    drift = idle.drift(10, every_ms);
      MESSAGE("woken every 1 ms, ticks behind: " << drift << ", edge loss: " << idle.edge_loss());
      CHECK(std::abs(drift - idle.edge_loss()) <= 1);
      if(core_clock)
      {
        CHECK(drift <= static_cast<std::int64_t>(tickless_idle::seconds));
      }
    }
    {
      auto          f = sleeping();
      tickless_idle idle{.f = f, .core_clock = core_clock};
      auto const    drift = idle.drift(8, irregular());
      MESSAGE("woken irregularly, ticks behind: " << drift << ", edge loss: " << idle.edge_loss());
      CHECK(idle.tick_wakes > 0);
      CHECK(idle.event_wakes > 0);
      CHECK(std::abs(drift - idle.edge_loss()) <= 1);
      CHECK(drift <= static_cast<std::int64_t>(tickless_idle::seconds));
    }
  }
}

namespace
{
constexpr address       demcr      = 0xE000'EDFC;
constexpr address       dwt_ctrl   = 0xE000'1000;
constexpr address       dwt_cyccnt = 0xE000'1004;
constexpr std::uint64_t held_span  = 20'000;

auto counting_sleeper() -> arch_fixture
{
  auto f = sleeping();
  put(f, demcr, get(f, demcr) | 1u << 24);
  put(f, dwt_ctrl, get(f, dwt_ctrl) | 1u);
  return f;
}

auto saved_state(arch_fixture const& f) -> popcorn::arch::armv8m::state
{
  std::vector<std::byte> bytes;
  f.module->save(bytes);
  return reflex::serde::bson::deserializer{bytes}.load<popcorn::arch::armv8m::state>();
}

auto last_ticks(arch_fixture const& f, std::size_t count) -> std::vector<std::uint64_t>
{
  auto const entries = tick_entries(f);
  REQUIRE(entries.size() >= count);
  return {entries.end() - static_cast<std::ptrdiff_t>(count), entries.end()};
}
}

TEST_CASE("a held core keeps SysTick and CYCCNT, then ticks on its phase shifted by the hold")
{
  auto       f     = counting_sleeper();
  auto       twin  = counting_sleeper();
  auto const held  = now(f);
  auto const cvr   = get(f, syst_cvr);
  auto const cyc   = get(f, dwt_cyccnt);
  auto const count = ticks(f);
  f.target->hold_core(true);
  CHECK(saved_state(f).held_at == held);
  auto const span = run_to_instruction(f, held + held_span);
  CHECK(span.executed.value == 0);
  CHECK(get(f, syst_cvr) == cvr);
  CHECK(get(f, dwt_cyccnt) == cyc);
  CHECK(ticks(f) == count);
  f.target->hold_core(false);
  CHECK_FALSE(saved_state(f).held_at.has_value());
  run_to_instruction(f, held + held_span + 3 * period + period / 2);
  run_to_instruction(twin, held + 3 * period + period / 2);
  CHECK(ticks(f) == ticks(twin));
  CHECK(get(f, syst_cvr) == get(twin, syst_cvr));
  CHECK(get(f, dwt_cyccnt) == get(twin, dwt_cyccnt));
  auto shifted = last_ticks(twin, 3);
  for(auto& at : shifted)
  {
    at += held_span;
  }
  CHECK(last_ticks(f, 3) == shifted);
}

TEST_CASE("a SysTick pended while the core is held is taken at the release, not during the hold")
{
  auto       f     = sleeping();
  auto const held  = now(f);
  auto const count = ticks(f);
  f.target->hold_core(true);
  put(f, icsr, pendstset);
  run_to_instruction(f, held + held_span);
  CHECK(ticks(f) == count);
  f.target->hold_core(false);
  run_to_instruction(f, held + held_span + 10);
  CHECK(ticks(f) == count + 1);
  CHECK(last_ticks(f, 1) == std::vector<std::uint64_t>{held + held_span});
}

TEST_CASE("a hold released with no time between leaves SysTick and the core as never held")
{
  auto       f    = counting_sleeper();
  auto       twin = counting_sleeper();
  auto const held = now(f);
  f.target->hold_core(true);
  f.target->hold_core(false);
  run_to_instruction(f, held + 3 * period + period / 2);
  run_to_instruction(twin, held + 3 * period + period / 2);
  CHECK(last_ticks(f, 3) == last_ticks(twin, 3));
  CHECK(get(f, syst_cvr) == get(twin, syst_cvr));
  CHECK(get(f, dwt_cyccnt) == get(twin, dwt_cyccnt));
  CHECK(f.reg("pc") == twin.reg("pc"));
  CHECK(f.reg("sp") == twin.reg("sp"));
}

TEST_CASE("a reset of a held core freezes SysTick again from the reset")
{
  auto f = sleeping();
  f.target->hold_core(true);
  f.target->reset();
  CHECK(f.target->core_held());
  CHECK(saved_state(f).held_at == std::optional<std::uint64_t>{0});
}

TEST_CASE("a held core restored from its saved form into another machine stays held and frozen")
{
  auto       f    = counting_sleeper();
  auto       twin = counting_sleeper();
  auto const held = now(f);
  f.target->hold_core(true);
  run_to_instruction(f, held + held_span / 2);
  auto const bytes = core::encode_snapshot(f.target->snapshot().data());

  auto other = booted();
  other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(other.target->core_held());
  CHECK(saved_state(other).held_at == held);
  run_to_instruction(other, held + held_span);
  other.target->hold_core(false);
  run_to_instruction(other, held + held_span + 3 * period + period / 2);
  run_to_instruction(twin, held + 3 * period + period / 2);
  CHECK(ticks(other) == ticks(twin));
  CHECK(get(other, dwt_cyccnt) == get(twin, dwt_cyccnt));
  CHECK(get(other, syst_cvr) == get(twin, syst_cvr));
}
