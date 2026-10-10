#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/scheduler.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture                         = popcorn::testing::machine_fixture<>;
using logged                          = popcorn::testing::test_arch::logged_event;
constexpr auto          long_wall     = std::chrono::seconds{60};
constexpr address       boots         = 0x2008'0000;
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;

auto booted(std::string_view firmware, core::machine_options options = {}) -> fixture
{
  fixture f{m33(), firmware, std::move(options)};
  if(firmware == "idle")
  {
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
  }
  else
  {
    REQUIRE(f.run_to_stop({.budget_left = icount{1000}}).reason == stop_reason::budget);
  }
  return f;
}

auto event_log() -> std::vector<logged>
{
  auto       f      = booted("calls");
  auto&      events = f.module->attached_events();
  auto const now    = f.target->virtual_clock().now().value;
  std::ignore       = events.schedule(icount{now + 10}, 1, 1);
  std::ignore       = events.schedule(icount{now + 10}, 1, 2);
  std::ignore = events.schedule(icount{now + 20}, popcorn::testing::test_arch::chain_event, 10);
  events.cancel(events.schedule(icount{now + 30}, 1, 99));
  auto const stop = f.run_to_stop({.budget_left = icount{200}});
  CHECK(stop.reason == stop_reason::budget);
  return f.module->event_log;
}

auto ticks(fixture const& f) -> std::uint32_t
{
  return f.word(sym("idle", "ticks"));
}

class stamping_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  std::vector<std::uint64_t> stamps;
  bool                       reschedule_now = false;
  std::uint64_t              next_left      = 0;
  std::uint64_t              now_left       = 0;
  std::uint64_t              handled        = 0;

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    ++handled;
    if(reschedule_now)
    {
      std::ignore = attached_events().schedule(attached_clock().now(), 3, 0);
      return;
    }
    if(now_left != 0)
    {
      --now_left;
      std::ignore = attached_events().schedule(attached_clock().now(), 5, 0);
      return;
    }
    if(next_left != 0)
    {
      --next_left;
      std::ignore = attached_events().schedule(icount{attached_clock().now().value + 1}, 4, 0);
      return;
    }
    auto const before = timer_expiries;
    test_arch::on_event(kind, payload);
    if(timer_expiries != before)
    {
      stamps.push_back(attached_clock().now().value);
    }
  }
};

using stamping = popcorn::testing::machine_fixture<stamping_arch>;

auto booted_stamping() -> stamping
{
  stamping f{m33(), "idle", core::machine_options{}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

constexpr std::uint64_t period = 4000;

auto call_idle(
    stamping const&                      f,
    std::string_view                     function,
    std::vector<popcorn::arch::call_arg> args,
    icount                               budget) -> core::call_result
{
  return f.target->call(
      {.entry           = sym("idle", function),
       .args            = std::move(args),
       .returns         = popcorn::arch::value_kind::i32,
       .abi             = popcorn::arch::float_abi::soft,
       .return_sentinel = 0x3000'0000},
      budget);
}
}

TEST_CASE(
    "events fire in count then sequence order, chained ones in the same pass, cancelled never")
{
  auto const log = event_log();
  REQUIRE(log.size() == 4);
  CHECK(log[0].kind == 1);
  CHECK(log[0].payload == 1);
  CHECK(log[1].payload == 2);
  CHECK(log[0].at == log[1].at);
  CHECK(log[2].kind == popcorn::testing::test_arch::chain_event);
  CHECK(log[3].kind == 1);
  CHECK(log[3].payload == 11);
  CHECK(log[3].at == log[2].at);
  CHECK(log == event_log());
}

TEST_CASE("idle firmware runs 10 virtual seconds in a fraction of the instructions")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_entry"));
  core::slice_request request{.wall_budget = long_wall, .until_time = vtime{10 * ps_per_second}};
  std::uint64_t       executed = 0;
  auto const          start    = std::chrono::steady_clock::now();
  for(;;)
  {
    auto const slice = f.run(request);
    executed += slice.executed.value;
    if(slice.stopped)
    {
      INFO(
          std::format(
              "{} pc {:#x} fault {:#x} kind {}", slice.stopped->reason, slice.stopped->where.pc,
              slice.stopped->fault ? slice.stopped->fault->fault_address : 0,
              slice.stopped->fault ? static_cast<int>(slice.stopped->fault->kind) : -1));
      CHECK(slice.stopped->reason == stop_reason::time);
      CHECK(slice.stopped->time == vtime{10 * ps_per_second});
      break;
    }
  }
  auto const took = std::chrono::steady_clock::now() - start;
  MESSAGE(
      std::format(
          "10 virtual seconds idle: {} instructions, {} ms wall", executed,
          std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
  CHECK(f.target->virtual_clock().now() == icount{40'000'000});
  CHECK(executed < 400'000);
  CHECK(ticks(f) >= 9999);
  CHECK(ticks(f) <= 10'000);
}

TEST_CASE("a budget counts executed instructions only, idle jumps aside")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_budget"));
  auto const          before = f.target->virtual_clock().now();
  core::slice_request request{.wall_budget = long_wall, .budget_left = icount{100'000}};
  std::uint64_t       executed = 0;
  for(;;)
  {
    auto const slice = f.run(request);
    executed += slice.executed.value;
    if(slice.stopped)
    {
      CHECK(slice.stopped->reason == stop_reason::budget);
      break;
    }
    request.budget_left = icount{request.budget_left->value - slice.executed.value};
  }
  CHECK(executed >= 100'000);
  CHECK(executed <= 100'512);
  CHECK(ticks(f) >= 500);
  CHECK(ticks(f) <= 1000);
  CHECK(f.target->virtual_clock().now().value - before.value > 1'000'000);
}

TEST_CASE("the slice bound caps executed plus idle-jumped instructions")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_entry"));
  for(int i = 0; i < 20; ++i)
  {
    auto const before = f.target->virtual_clock().now();
    auto const slice  = f.run({.max_instructions = icount{10'000}, .wall_budget = long_wall});
    CHECK_FALSE(slice.stopped.has_value());
    auto const moved = f.target->virtual_clock().now().value - before.value;
    CHECK(moved >= 10'000);
    CHECK(moved <= 10'512);
  }
}

TEST_CASE("idle with no event pending stops as idle after the wfi")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  auto const stop = f.run_to_stop();
  INFO(
      std::format(
          "{} pc {:#x} fault {:#x} kind {}", stop.reason, stop.where.pc,
          stop.fault ? stop.fault->fault_address : 0,
          stop.fault ? static_cast<int>(stop.fault->kind) : -1));
  CHECK(stop.reason == stop_reason::idle);
  CHECK(stop.where.pc == sym("idle", "idle_forever") + 2);
}

TEST_CASE("idle with no event pending reaches until_time and the next slice continues there")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const start = f.target->virtual_clock().time();
  auto const first = vtime{start.ps + ps_per_second / 1000};
  auto const stop  = f.run_to_stop({.until_time = first});
  CHECK(stop.reason == stop_reason::time);
  CHECK(stop.time == first);
  CHECK(stop.instructions == f.target->virtual_clock().to_icount(first));
  CHECK(stop.where.pc == sym("idle", "idle_forever") + 2);
  auto const second = vtime{first.ps + ps_per_second / 1000};
  auto const again  = f.run(
      {.max_instructions = icount{1'000'000'000}, .wall_budget = long_wall, .until_time = second});
  REQUIRE(again.stopped.has_value());
  CHECK(again.stopped->reason == stop_reason::time);
  CHECK(again.stopped->time == second);
  CHECK(again.executed == icount{0});
  CHECK(f.run_to_stop().reason == stop_reason::idle);
  CHECK(f.target->virtual_clock().time() == second);
}

TEST_CASE("an event due exactly at until_time while asleep is dispatched before the time stop")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const& clock  = f.target->virtual_clock();
  auto const  target = vtime{clock.time().ps + ps_per_second / 1000};
  auto const  at     = clock.to_icount(target);
  std::ignore        = f.module->attached_events().schedule(at, 1, 7);
  f.module->event_log.clear();
  auto const stop = f.run_to_stop({.until_time = target});
  CHECK(stop.reason == stop_reason::time);
  CHECK(stop.time == target);
  CHECK(
      f.module->event_log
      == std::vector<logged>{
          {.kind = 1, .payload = 7, .at = at}
  });
}

TEST_CASE("an idle jump toward until_time stops at the slice bound first and resumes")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const clock  = &f.target->virtual_clock();
  auto const start  = clock->now();
  auto const target = vtime{clock->time().ps + ps_per_second};
  auto const capped =
      f.run({.max_instructions = icount{1000}, .wall_budget = long_wall, .until_time = target});
  CHECK_FALSE(capped.stopped.has_value());
  CHECK(clock->now() == icount{start.value + 1000});
  auto const rest =
      f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = target});
  CHECK(rest.reason == stop_reason::time);
  CHECK(clock->time() == target);
}

TEST_CASE("time since power-on runs on across every reset but a power-on reset")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  auto const& clock    = f.target->virtual_clock();
  auto const  sleep_to = [&f, &clock](std::uint64_t ms) {
    auto const target = vtime{clock.time().ps + ms * (ps_per_second / 1000)};
    REQUIRE(f.run_to_stop({.until_time = target}).reason == stop_reason::time);
    return target;
  };
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const first = sleep_to(2);
  CHECK(clock.since_power_on() == first);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(clock.time() == vtime{0});
  CHECK(clock.since_power_on() == first);
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const second = sleep_to(3);
  CHECK(clock.since_power_on() == vtime{first.ps + second.ps});
  auto const saved = f.target->snapshot();
  f.target->reset(popcorn::reset_kind::watchdog);
  CHECK(clock.since_power_on() == vtime{first.ps + second.ps});
  f.target->reset(popcorn::reset_kind::brown_out);
  CHECK(clock.since_power_on() == vtime{first.ps + second.ps});
  f.target->restore(saved);
  CHECK(clock.time() == second);
  CHECK(clock.since_power_on() == vtime{first.ps + second.ps});
  f.target->reset(popcorn::reset_kind::power_on);
  CHECK(clock.since_power_on() == vtime{0});
  f.target->restore(saved);
  CHECK(clock.since_power_on() == vtime{first.ps + second.ps});
}

TEST_CASE("stop_when is asked after every dispatched event")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_entry"));
  auto const stop = f.run_to_stop({.stop_when = [&f] { return f.module->timer_expiries == 3; }});
  CHECK(stop.reason == stop_reason::user);
  CHECK(f.module->timer_expiries == 3);
  CHECK(ticks(f) == 2);
}

TEST_CASE("stop_when after a dispatched event runs as a handler")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_entry"));
  auto       calls = 0;
  auto const stop  = f.run_to_stop({.stop_when = [&] {
    ++calls;
    CHECK(f.target->regs().read(*f.target->regs().find("sp")) != 0);
    CHECK(f.target->mem().read(sym("idle", "ticks"), 4).has_value());
    CHECK_THROWS_AS(f.target->reset(), popcorn::machine_running);
    CHECK_THROWS_AS(std::ignore = f.target->run_slice({}), popcorn::machine_busy);
    return f.module->timer_expiries == 1;
  }});
  CHECK(stop.reason == stop_reason::user);
  CHECK(calls >= 1);
}

TEST_CASE("a reset request stops under the stop policy and names the kind")
{
  auto f = booted("calls");
  f.set("pc", sym("calls", "reset_now"));
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::reset_request);
  CHECK(stop.detail == "software");
  CHECK(f.word(boots) == 1);
}

TEST_CASE("a reset request under reset_and_continue reboots and the slice continues")
{
  core::machine_options options;
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  auto f                   = booted("calls", options);
  f.set("pc", sym("calls", "reset_now"));
  auto const& clock = f.target->virtual_clock();
  auto const  start = clock.since_power_on();
  auto const  slice = f.run({.wall_budget = long_wall, .budget_left = icount{10'000}});
  REQUIRE(slice.stopped.has_value());
  CHECK(slice.stopped->reason == stop_reason::budget);
  CHECK(f.word(boots) == 2);
  CHECK(clock.now().value < slice.executed.value);
  CHECK(f.word(sym("calls", "counter")) > 0);
  CHECK(
      clock.since_power_on()
      == vtime{
          start.ps
          + popcorn::vtime_from_instructions(slice.executed.value, clock.instruction_hz()).ps});
}

TEST_CASE("a reset request in the block of a refused access wins over the fault")
{
  SUBCASE("reset_and_continue resets and continues")
  {
    core::machine_options options;
    options.on_reset_request = core::reset_request_policy::reset_and_continue;
    auto f                   = booted("calls", options);
    f.set("pc", sym("calls", "reset_then_fault"));
    auto const slice = f.run({.wall_budget = long_wall, .budget_left = icount{10'000}});
    REQUIRE(slice.stopped.has_value());
    CHECK(slice.stopped->reason == stop_reason::budget);
    CHECK(f.word(boots) == 2);
  }
  SUBCASE("stop policy reports the reset request")
  {
    auto f = booted("calls");
    f.set("pc", sym("calls", "reset_then_fault"));
    CHECK(f.run_to_stop().reason == stop_reason::reset_request);
  }
}

TEST_CASE("a timer enabled from inside the engine expires within one block of its period")
{
  auto f = booted_stamping();
  f.set("pc", sym("idle", "timer_spin"));
  f.set("primask", 1);
  auto const before = f.target->virtual_clock().now().value;
  auto const slice  = f.run({.wall_budget = long_wall, .budget_left = icount{4 * period + 100}});
  REQUIRE(slice.stopped.has_value());
  REQUIRE(f.module->stamps.size() >= 4);
  CHECK(f.module->stamps[0] - before >= period);
  CHECK(f.module->stamps[0] - before <= period + 16);
  for(std::size_t i = 1; i < f.module->stamps.size(); ++i)
  {
    CHECK(f.module->stamps[i] - f.module->stamps[i - 1] == period);
  }
}

TEST_CASE("a call spanning timer ticks leaves the timer on its period, then disabling stops it")
{
  auto f = booted_stamping();
  f.set("pc", sym("idle", "idle_entry"));
  REQUIRE(
      f.run_to_stop({.stop_when = [&f] { return f.module->timer_expiries == 5; }}).reason
      == stop_reason::user);
  auto const result = call_idle(
      f, "spin",
      {
          {.kind = popcorn::arch::value_kind::i32, .bits = 20'000}
  },
      icount{1'000'000});
  REQUIRE(result.stopped.reason == stop_reason::returned);
  auto const now = f.target->virtual_clock().time();
  REQUIRE(
      f.run_to_stop({.until_time = popcorn::vtime{now.ps + ps_per_second / 100}}).reason
      == stop_reason::time);
  auto const& stamps = f.module->stamps;
  REQUIRE(stamps.size() > 30);
  for(std::size_t i = 1; i < stamps.size(); ++i)
  {
    CAPTURE(i);
    CHECK(stamps[i] - stamps[i - 1] >= period - 16);
    CHECK(stamps[i] - stamps[i - 1] <= period + 16);
  }
  auto const counted = stamps.size();
  f.set("pc", sym("idle", "stop_timer"));
  f.set("lr", sym("idle", "idle_forever") | 1);
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::idle);
  CHECK(stamps.size() == counted);
}

TEST_CASE("a call into a core idling in wfi runs and returns")
{
  auto f = booted_stamping();
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const result = call_idle(f, "tick_handler", {}, icount{1000});
  CHECK(result.stopped.reason == stop_reason::returned);
  CHECK(f.word(sym("idle", "ticks")) == 1);
  CHECK(f.target->regs().pc() == sym("idle", "idle_forever") + 2);
}

TEST_CASE("wfi.w idles like wfi")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_wide"));
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::idle);
  CHECK(stop.where.pc == sym("idle", "idle_wide") + 4);
}

TEST_CASE("an engine exit right after a wfi is not taken for the wfi")
{
  auto       f   = booted("idle");
  auto const wfi = sym("idle", "idle_forever");
  f.set("pc", sym("idle", "tick_handler"));
  f.set("lr", (wfi + 2) | 1);
  auto const stop = f.run_to_stop({.until = wfi + 2});
  REQUIRE(stop.reason == stop_reason::address);
  CHECK(stop.where.pc == wfi + 2);
  auto const before = f.target->virtual_clock().now();
  auto const slice  = f.run({.wall_budget = long_wall, .exact_instructions = icount{1}});
  CHECK(slice.executed.value == 1);
  CHECK(f.target->virtual_clock().now().value - before.value == 1);
}

TEST_CASE("until right after an executed wfi stops with the core asleep")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_budget"));
  address found = sym("idle", "idle_budget");
  while(f.target->mem().read(found, 2) != 0xbf30)
  {
    found += 2;
  }
  auto const stop = f.run_to_stop({.until = found + 2});
  REQUIRE(stop.reason == stop_reason::address);
  CHECK(f.target->regs().pc() == found + 2);
  auto const before = f.target->virtual_clock().now();
  auto const slice  = f.run({.wall_budget = long_wall, .exact_instructions = icount{1}});
  CHECK(slice.executed.value == 1);
  CHECK(f.target->virtual_clock().now().value - before.value > 1);
}

TEST_CASE("events rescheduled at now forever stop as a spin naming their owner")
{
  auto f = booted_stamping();
  f.set("pc", sym("idle", "idle_forever"));
  f.module->reschedule_now = true;
  std::ignore = f.module->attached_events().schedule(f.target->virtual_clock().now(), 3, 0);
  auto const                   start = f.target->virtual_clock().now();
  std::optional<popcorn::stop> stopped;
  auto                         slices = 0;
  for(; not stopped and slices < 100'000; ++slices)
  {
    stopped = f.run({.wall_budget = std::chrono::microseconds{50}}).stopped;
  }
  REQUIRE(stopped.has_value());
  CHECK(stopped->reason == stop_reason::spin);
  CHECK(stopped->detail == "events repeat without time passing: arch kind 3");
  CHECK(f.target->virtual_clock().now() == start);
  CHECK(slices > 1);
}

TEST_CASE("a peripheral model's events repeating at now stop as a spin naming the peripheral")
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  auto f = booted("idle", std::move(options));
  f.set("pc", sym("idle", "idle_forever"));
  auto& m          = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
  m.reschedule_now = true;
  std::ignore      = m.context().events.schedule(f.target->virtual_clock().now(), 1, 0);
  auto const stop  = f.run_to_stop();
  CHECK(stop.reason == stop_reason::spin);
  CHECK(stop.detail == "events repeat without time passing: TESTDEV kind 1");
}

namespace
{
constexpr std::uint64_t cap = 65536;

auto storm(stamping const& f) -> void
{
  f.set("pc", sym("idle", "idle_forever"));
  f.module->reschedule_now = true;
  std::ignore = f.module->attached_events().schedule(f.target->virtual_clock().now(), 3, 0);
}

auto partial(stamping const& f) -> std::uint64_t
{
  auto const before = f.module->handled;
  auto const slice  = f.run({.wall_budget = std::chrono::microseconds{50}});
  REQUIRE_FALSE(slice.stopped.has_value());
  auto const done = f.module->handled - before;
  REQUIRE(done > 0);
  REQUIRE(done < cap);
  return done;
}

auto to_spin(stamping const& f) -> std::uint64_t
{
  auto const before = f.module->handled;
  auto const stop   = f.run_to_stop();
  CHECK(stop.reason == stop_reason::spin);
  return f.module->handled - before;
}
}

TEST_CASE("the cap lets 65535 events at one instant run and stops at the 65536th")
{
  {
    auto f = booted_stamping();
    f.set("pc", sym("idle", "idle_forever"));
    f.module->now_left = cap - 2;
    std::ignore = f.module->attached_events().schedule(f.target->virtual_clock().now(), 5, 0);
    CHECK(f.run_to_stop().reason == stop_reason::idle);
    CHECK(f.module->handled == cap - 1);
  }
  {
    auto f = booted_stamping();
    f.set("pc", sym("idle", "idle_forever"));
    f.module->now_left = cap - 1;
    std::ignore = f.module->attached_events().schedule(f.target->virtual_clock().now(), 5, 0);
    CHECK(f.run_to_stop().reason == stop_reason::spin);
    CHECK(f.module->handled == cap);
  }
}

TEST_CASE("resuming after an event spin stop runs 65536 more events before the next")
{
  auto f = booted_stamping();
  storm(f);
  CHECK(to_spin(f) == cap);
  CHECK(to_spin(f) == cap);
}

TEST_CASE("a restore in the middle of an event storm restarts the count")
{
  auto f = booted_stamping();
  storm(f);
  auto const saved = f.target->snapshot();
  std::ignore      = partial(f);
  f.target->restore(saved);
  CHECK(to_spin(f) == cap);
}

TEST_CASE("a reset in the middle of an event storm restarts the count")
{
  auto f = booted_stamping();
  f.target->reset(popcorn::reset_kind::software);
  storm(f);
  std::ignore = partial(f);
  f.target->reset(popcorn::reset_kind::software);
  REQUIRE(f.target->virtual_clock().now() == icount{0});
  storm(f);
  CHECK(to_spin(f) == cap);
}

TEST_CASE("events one instruction apart never count toward the per-instant cap")
{
  auto f = booted_stamping();
  f.set("pc", sym("idle", "idle_forever"));
  f.module->next_left = 100'000;
  auto const start    = f.target->virtual_clock().now();
  std::ignore         = f.module->attached_events().schedule(start, 4, 0);
  auto const stop     = f.run_to_stop({.max_instructions = icount{1'000'000}});
  CHECK(stop.reason == stop_reason::idle);
  CHECK(f.module->next_left == 0);
  CHECK(f.target->virtual_clock().now().value == start.value + 100'000);
}

TEST_CASE("a chain of events at one instant runs to its end")
{
  auto       f      = booted("calls");
  auto&      events = f.module->attached_events();
  auto const now    = f.target->virtual_clock().now();
  f.module->event_log.clear();
  for(std::uint64_t link = 0; link < 10; ++link)
  {
    std::ignore = events.schedule(now, popcorn::testing::test_arch::chain_event, link);
  }
  auto const stop = f.run_to_stop({.budget_left = icount{100}});
  CHECK(stop.reason == stop_reason::budget);
  CHECK(f.module->event_log.size() == 20);
}

namespace
{
constexpr address scratch_code = 0x2004'0000;

void write_code(fixture const& f, std::span<std::uint16_t const> words)
{
  for(std::size_t i = 0; i < words.size(); ++i)
  {
    REQUIRE(f.target->mem().write(scratch_code + 2 * i, 2, words[i]));
  }
}

auto call_scratch(fixture const& f, icount budget) -> core::call_result
{
  return f.target->call(
      {.entry           = scratch_code,
       .returns         = popcorn::arch::value_kind::i32,
       .abi             = popcorn::arch::float_abi::soft,
       .return_sentinel = 0x3000'0000},
      budget);
}

auto expiries_in_10_ms(fixture const& f) -> std::uint64_t
{
  auto const before = f.module->timer_expiries;
  auto const now    = f.target->virtual_clock().time();
  auto const stop   = f.run_to_stop({.until_time = vtime{now.ps + ps_per_second / 100}});
  CHECK(stop.reason == stop_reason::time);
  return f.module->timer_expiries - before;
}
}

TEST_CASE("a call that disables the timer and outlives an expiry leaves the caller's timer on")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_entry"));
  REQUIRE(
      f.run_to_stop({.stop_when = [&f] { return f.module->timer_expiries == 2; }}).reason
      == stop_reason::user);
  constexpr std::array<std::uint16_t, 6> disable_and_spin{0x4901, 0x2000, 0x6008,
                                                          0xe7fe, 0xe010, 0xe000};
  write_code(f, disable_and_spin);
  REQUIRE(call_scratch(f, icount{10'000}).stopped.reason == stop_reason::budget);
  CHECK(f.module->read_system(popcorn::testing::test_arch::timer_control_offset) == 1);
  auto const counted = expiries_in_10_ms(f);
  CHECK(counted >= 9);
  CHECK(counted <= 11);
}

TEST_CASE("a call that enables a timer the caller had off leaves no duplicate expiries")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_entry"));
  REQUIRE(
      f.run_to_stop({.stop_when = [&f] { return f.module->timer_expiries == 1; }}).reason
      == stop_reason::user);
  f.module->write_system(popcorn::testing::test_arch::timer_control_offset, 0);
  constexpr std::array<std::uint16_t, 6> enable_and_return{0x4901, 0x2001, 0x6008,
                                                           0x4770, 0xe010, 0xe000};
  write_code(f, enable_and_return);
  REQUIRE(call_scratch(f, icount{1000}).stopped.reason == stop_reason::returned);
  CHECK(f.module->read_system(popcorn::testing::test_arch::timer_control_offset) == 0);
  f.module->write_system(popcorn::testing::test_arch::timer_control_offset, 1);
  auto const counted = expiries_in_10_ms(f);
  CHECK(counted >= 9);
  CHECK(counted <= 11);
}

TEST_CASE("pending_events counts scheduled events until they are cancelled or dispatched")
{
  auto       f       = booted("calls");
  auto&      events  = f.module->attached_events();
  auto const before  = f.target->pending_events();
  auto const now     = f.target->virtual_clock().now().value;
  std::ignore        = events.schedule(icount{now + 10}, 1, 1);
  auto const dropped = events.schedule(icount{now + 20}, 1, 2);
  CHECK(f.target->pending_events() == before + 2);
  events.cancel(dropped);
  CHECK(f.target->pending_events() == before + 1);
  REQUIRE(f.run_to_stop({.budget_left = icount{200}}).reason == stop_reason::budget);
  CHECK(f.target->pending_events() == before);
}

namespace
{
constexpr std::uint16_t thumb_nop       = 0xbf00;
constexpr std::uint16_t branch_self     = 0xe7fe;
constexpr std::uint16_t store_ctrl      = 0x65d1;
constexpr address       testdev         = 0x4001'0000;
constexpr std::uint32_t logged_kind     = 1;
constexpr std::uint64_t long_block      = 401;
constexpr std::uint64_t bound_in_block  = 200;
constexpr std::uint64_t rate_change_nth = 300;

struct lanes
{
  fixture       f;
  std::uint64_t start;

  explicit lanes(bool rate_change = false, bool exact_epoch = false)
      : f{booted("calls", options())}, start{}
  {
    std::vector<std::uint16_t> code(long_block - 1, thumb_nop);
    if(rate_change)
    {
      code[rate_change_nth]    = store_ctrl;
      model().halve_rate_on_go = true;
      model().drives_clock     = exact_epoch;
      model().go_delay         = 1'000'000'000;
    }
    code.push_back(branch_self);
    write_code(f, code);
    f.set("pc", scratch_code);
    f.set("r1", 1);
    f.set("r2", testdev);
    f.module->event_log.clear();
    start = clock().now().value;
  }

  static auto options() -> core::machine_options
  {
    core::machine_options made;
    made.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
    return made;
  }

  [[nodiscard]] auto model() const -> popcorn::testing::test_model&
  {
    return dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
  }

  [[nodiscard]] auto clock() const -> core::clock const&
  {
    return f.target->virtual_clock();
  }

  [[nodiscard]] auto time_of(std::uint64_t offset) const -> vtime
  {
    auto const ps = ps_per_second / clock().instruction_hz();
    REQUIRE(ps * clock().instruction_hz() == ps_per_second);
    return vtime{clock().time().ps + (start + offset - clock().now().value) * ps};
  }

  void local(std::uint64_t offset, std::uint64_t payload) const
  {
    std::ignore =
        f.module->attached_events().schedule(icount{start + offset}, logged_kind, payload);
  }

  void local_at(std::uint64_t offset, std::uint64_t payload) const
  {
    std::ignore = f.module->attached_events().schedule_at(
        time_of(offset), logged_kind, payload, f.target->virtual_clock());
  }

  void lane(std::uint64_t offset, std::uint64_t payload, std::uint32_t which = 1) const
  {
    lane_at(time_of(offset), payload, which);
  }

  void lane_at(vtime due, std::uint64_t payload, std::uint32_t which = 1) const
  {
    std::ignore = f.module->attached_events().schedule_at(
        due, logged_kind, payload, which, f.target->virtual_clock());
  }

  auto slice(vtime until, bool hold) const -> core::slice_result
  {
    return f.run(
        {.max_instructions = icount{1'000'000},
         .wall_budget      = long_wall,
         .until_time       = until,
         .hold_from_until  = hold});
  }

  [[nodiscard]] auto payloads() const -> std::vector<std::uint64_t>
  {
    std::vector<std::uint64_t> out;
    for(auto const& one : f.module->event_log)
    {
      out.push_back(one.payload);
    }
    return out;
  }
};

auto stopped_on_time(core::slice_result const& result) -> bool
{
  return result.stopped and result.stopped->reason == stop_reason::time;
}
}

TEST_CASE("same-count events fire local ones first, then lanes in order, then in post order")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto&       events = f.module->attached_events();
  auto const& clock  = f.target->virtual_clock();
  auto const  due    = vtime{clock.time().ps + ps_per_second / 1000};
  std::ignore        = events.schedule_at(due, logged_kind, 30, 2, clock);
  std::ignore        = events.schedule_at(due, logged_kind, 20, 1, clock);
  std::ignore        = events.schedule_at(due, logged_kind, 10, clock);
  std::ignore        = events.schedule(clock.to_icount(due), logged_kind, 11);
  std::ignore        = events.schedule_at(due, logged_kind, 21, 1, clock);
  f.module->event_log.clear();
  REQUIRE(f.run_to_stop({.until_time = vtime{due.ps + 1}}).reason == stop_reason::time);
  std::vector<std::uint64_t> order;
  for(auto const& one : f.module->event_log)
  {
    CHECK(one.at == clock.to_icount(due));
    order.push_back(one.payload);
  }
  CHECK(order == std::vector<std::uint64_t>{10, 11, 20, 21, 30});
}

TEST_CASE("a lane event due ahead fires at its own count, not at once")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const& clock = f.target->virtual_clock();
  auto const  now   = clock.now().value;
  auto const  due   = vtime{clock.time().ps + 1000 * (ps_per_second / clock.instruction_hz())};
  std::ignore       = f.module->attached_events().schedule_at(due, logged_kind, 5, 1, clock);
  f.module->event_log.clear();
  REQUIRE(
      f.run_to_stop({.until_time = vtime{due.ps + ps_per_second / 1000}}).reason
      == stop_reason::time);
  CHECK(
      f.module->event_log
      == std::vector<logged>{
          {.kind = logged_kind, .payload = 5, .at = icount{now + 1000}}
  });
}

TEST_CASE("a slice held at until_time dispatches only the events due before it")
{
  lanes      l;
  auto const h = l.time_of(bound_in_block);
  l.local(bound_in_block - 1, 1);
  l.local(bound_in_block, 2);
  l.local(bound_in_block + 100, 3);
  auto const first = l.slice(h, true);
  REQUIRE(stopped_on_time(first));
  auto const b1 = l.clock().now().value;
  CHECK(b1 == l.start + long_block);
  CHECK(l.payloads() == std::vector<std::uint64_t>{1});
  CHECK(l.f.target->pending_events() >= 2);
  REQUIRE(stopped_on_time(l.slice(l.time_of(long_block + 1000), true)));
  CHECK(l.payloads() == std::vector<std::uint64_t>{1, 2, 3});
  CHECK(l.f.module->event_log[1].at == icount{b1});
  CHECK(l.f.module->event_log[2].at == icount{b1});
}

TEST_CASE("a held slice ended by its budget past until_time still holds")
{
  lanes      l;
  auto const h = l.time_of(bound_in_block);
  l.local(bound_in_block - 1, 1);
  l.local(bound_in_block, 2);
  l.local(bound_in_block + 100, 3);
  auto const first = l.f.run(
      {.max_instructions = icount{1'000'000},
       .wall_budget      = long_wall,
       .budget_left      = icount{bound_in_block + 1},
       .until_time       = h,
       .hold_from_until  = true});
  REQUIRE(first.stopped.has_value());
  CHECK(first.stopped->reason == stop_reason::budget);
  CHECK(l.payloads() == std::vector<std::uint64_t>{1});
  REQUIRE(stopped_on_time(l.slice(l.time_of(long_block + 1000), false)));
  CHECK(l.payloads() == std::vector<std::uint64_t>{1, 2, 3});
}

TEST_CASE("a slice to a stamp bound without the hold dispatches every event due at its end")
{
  lanes      l;
  auto const h = l.time_of(bound_in_block);
  l.local(bound_in_block - 1, 1);
  l.local(bound_in_block, 2);
  l.local(bound_in_block + 100, 3);
  auto const first = l.f.run(
      {.max_instructions = icount{1'000'000},
       .wall_budget      = long_wall,
       .until_time       = h,
       .internal_stop    = true});
  REQUIRE(stopped_on_time(first));
  CHECK(l.clock().now().value == l.start + long_block);
  CHECK(l.payloads() == std::vector<std::uint64_t>{1, 2, 3});
}

TEST_CASE("a lane event posted after a held slice fires as if posted before it")
{
  auto const reference = [] {
    lanes l;
    l.local(bound_in_block - 1, 1);
    l.local(bound_in_block, 2);
    l.local(bound_in_block + 100, 3);
    l.lane_at(l.time_of(bound_in_block + 20), 50);
    REQUIRE(stopped_on_time(l.slice(l.time_of(long_block + 1000), false)));
    return l.f.module->event_log;
  };
  auto const run = [](bool late) {
    lanes      l;
    auto const h = l.time_of(bound_in_block);
    auto const a = l.time_of(bound_in_block + 20);
    l.local(bound_in_block - 1, 1);
    l.local(bound_in_block, 2);
    l.local(bound_in_block + 100, 3);
    if(not late)
    {
      l.lane_at(a, 50);
    }
    REQUIRE(stopped_on_time(l.slice(h, true)));
    CHECK(l.payloads() == std::vector<std::uint64_t>{1});
    if(late)
    {
      REQUIRE(l.clock().time() > a);
      l.lane_at(a, 50);
    }
    REQUIRE(stopped_on_time(l.slice(l.time_of(long_block + 1000), true)));
    return l.f.module->event_log;
  };
  auto const early = run(false);
  auto const late  = run(true);
  CHECK(late == early);
  CHECK(reference() == early);
  std::vector<std::uint64_t> order;
  for(auto const& one : late)
  {
    order.push_back(one.payload);
  }
  CHECK(order == std::vector<std::uint64_t>{1, 2, 50, 3});
}

TEST_CASE("a rate change in the held block gives the same order to early and late lane posts")
{
  for(auto const exact_epoch : {false, true})
  {
    CAPTURE(exact_epoch);
    auto const run = [exact_epoch](bool late) {
      lanes      l{true, exact_epoch};
      auto const h = l.time_of(bound_in_block);
      auto const a = l.time_of(bound_in_block + 30);
      l.local(bound_in_block - 1, 1);
      l.local_at(bound_in_block + 50, 2);
      if(not late)
      {
        l.lane_at(a, 50);
      }
      REQUIRE(stopped_on_time(l.slice(h, true)));
      REQUIRE(l.model().go_time.has_value());
      auto const clock = l.f.target->snapshot().data().clock;
      if(exact_epoch)
      {
        CHECK(clock.epoch_icount < clock.now);
      }
      else
      {
        CHECK(clock.epoch_icount == clock.now);
        CHECK(l.clock().epoch_time() > h);
        CHECK(l.payloads() == std::vector<std::uint64_t>{1});
      }
      if(late)
      {
        l.lane_at(a, 50);
      }
      REQUIRE(stopped_on_time(l.slice(vtime{l.clock().time().ps + ps_per_second / 1000}, true)));
      return l.f.module->event_log;
    };
    auto const early = run(false);
    CHECK(run(true) == early);
    lanes reference{true, exact_epoch};
    reference.local(bound_in_block - 1, 1);
    reference.local_at(bound_in_block + 50, 2);
    reference.lane_at(reference.time_of(bound_in_block + 30), 50);
    REQUIRE(stopped_on_time(reference.slice(
        vtime{reference.time_of(bound_in_block).ps + ps_per_second / 1000}, false)));
    CHECK(reference.f.module->event_log == early);
    if(not exact_epoch)
    {
      std::vector<std::uint64_t> order;
      for(auto const& one : early)
      {
        order.push_back(one.payload);
      }
      CHECK(order == std::vector<std::uint64_t>{1, 2, 50});
    }
  }
}

TEST_CASE("a pending lane event survives a snapshot, then fires or cancels by its seq")
{
  for(auto const cancel : {false, true})
  {
    CAPTURE(cancel);
    auto f = booted("idle");
    f.set("pc", sym("idle", "idle_forever"));
    REQUIRE(f.run_to_stop().reason == stop_reason::idle);
    auto&       events = f.module->attached_events();
    auto const& clock  = f.target->virtual_clock();
    auto const  due    = vtime{clock.time().ps + ps_per_second / 1000};
    auto const  seq    = events.schedule_at(due, logged_kind, 50, 1, clock);
    REQUIRE(seq >= core::scheduler::lane_seq_base);
    auto const saved =
        core::snapshot{core::decode_snapshot(core::encode_snapshot(f.target->snapshot().data()))};
    std::ignore = events.schedule_at(due, logged_kind, 51, 1, clock);
    f.target->restore(saved);
    REQUIRE(f.target->pending_events() == 1);
    if(cancel)
    {
      events.cancel(seq);
      CHECK(f.target->pending_events() == 0);
    }
    f.module->event_log.clear();
    REQUIRE(
        f.run_to_stop({.until_time = vtime{due.ps + ps_per_second / 1000}}).reason
        == stop_reason::time);
    if(cancel)
    {
      CHECK(f.module->event_log.empty());
    }
    else
    {
      CHECK(
          f.module->event_log
          == std::vector<logged>{
              {.kind = logged_kind, .payload = 50, .at = clock.to_icount(due)}
      });
    }
    CHECK(events.schedule_at(due, logged_kind, 52, 1, clock) == seq + 1);
  }
}

TEST_CASE("local sequence numbers do not depend on lane posts")
{
  auto const local_seqs = [](bool with_lanes) {
    auto f = booted("idle");
    f.set("pc", sym("idle", "idle_forever"));
    REQUIRE(f.run_to_stop().reason == stop_reason::idle);
    auto&                      events = f.module->attached_events();
    auto const&                clock  = f.target->virtual_clock();
    auto const                 due    = vtime{clock.time().ps + ps_per_second / 1000};
    std::vector<std::uint64_t> seqs;
    seqs.push_back(events.schedule_at(due, logged_kind, 1, clock));
    if(with_lanes)
    {
      auto const lane_seq = events.schedule_at(due, logged_kind, 50, 1, clock);
      CHECK(lane_seq >= core::scheduler::lane_seq_base);
    }
    seqs.push_back(events.schedule_at(due, logged_kind, 2, clock));
    if(with_lanes)
    {
      auto const lane_seq = events.schedule_at(due, logged_kind, 51, 2, clock);
      CHECK(lane_seq >= core::scheduler::lane_seq_base);
      events.cancel(lane_seq);
    }
    seqs.push_back(events.schedule(clock.now(), logged_kind, 3));
    auto const saved = f.target->snapshot().data().events;
    seqs.push_back(saved.next_seq);
    CHECK(saved.next_lane_seq == (with_lanes ? 2u : 0u));
    return seqs;
  };
  CHECK(local_seqs(true) == local_seqs(false));
}

TEST_CASE("an idle jump landing on a held bound holds the event due there")
{
  auto f = booted("idle");
  f.set("pc", sym("idle", "idle_forever"));
  REQUIRE(f.run_to_stop().reason == stop_reason::idle);
  auto const& clock = f.target->virtual_clock();
  auto const  h     = vtime{clock.time().ps + ps_per_second / 1000};
  auto const  at    = clock.to_icount(h);
  std::ignore       = f.module->attached_events().schedule(icount{at.value - 1}, logged_kind, 1);
  std::ignore       = f.module->attached_events().schedule(at, logged_kind, 2);
  f.module->event_log.clear();
  auto const first = f.run({.wall_budget = long_wall, .until_time = h, .hold_from_until = true});
  REQUIRE(stopped_on_time(first));
  CHECK(clock.now() == at);
  CHECK(
      f.module->event_log
      == std::vector<logged>{
          {.kind = logged_kind, .payload = 1, .at = icount{at.value - 1}}
  });
  auto const saved = f.target->snapshot();
  auto const next  = vtime{h.ps + ps_per_second / 1000};
  REQUIRE(stopped_on_time(
      f.run({.wall_budget = long_wall, .until_time = next, .hold_from_until = true})));
  auto const after = f.module->event_log;
  CHECK(after.size() == 2);
  CHECK(after.back() == logged{.kind = logged_kind, .payload = 2, .at = at});
  f.target->restore(saved);
  f.module->event_log.resize(1);
  REQUIRE(stopped_on_time(
      f.run({.wall_budget = long_wall, .until_time = next, .hold_from_until = true})));
  CHECK(f.module->event_log == after);
}

TEST_CASE("a hold at count 0 holds the events at count 0")
{
  auto f = booted("idle");
  f.target->reset();
  REQUIRE(f.target->virtual_clock().now() == icount{0});
  std::ignore = f.module->attached_events().schedule(icount{0}, logged_kind, 4);
  f.module->event_log.clear();
  auto const held =
      f.run({.wall_budget = long_wall, .until_time = vtime{0}, .hold_from_until = true});
  REQUIRE(stopped_on_time(held));
  CHECK(f.module->event_log.empty());
  REQUIRE(stopped_on_time(f.run({.wall_budget = long_wall, .until_time = vtime{1}})));
  CHECK(
      f.module->event_log
      == std::vector<logged>{
          {.kind = logged_kind, .payload = 4, .at = icount{0}}
  });
}
