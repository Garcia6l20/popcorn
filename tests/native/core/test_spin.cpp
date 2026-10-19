#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address flag = 0x4001'0008;

auto booted(std::uint32_t threshold) -> fixture
{
  core::machine_options options;
  options.spin_threshold = threshold;
  fixture f{m33(), "spin", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto start(fixture const& f, std::string_view entry) -> void
{
  f.set("pc", sym("spin", entry));
}

auto long_request() -> core::slice_request
{
  core::slice_request request;
  request.max_instructions = icount{10'000'000};
  request.wall_budget      = std::chrono::seconds{60};
  return request;
}
}

TEST_CASE("a poll of an unmodelled status register stops as a spin naming it")
{
  auto f = booted(1000);
  start(f, "wait_ready");
  auto const before  = f.target->virtual_clock().now();
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in wait_ready"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT = 0x00000001"));
  CHECK(stopped.detail.contains("fields READY"));
  auto const executed = f.target->virtual_clock().now().value - before.value;
  CHECK(executed == 4 + 3 * 1004);
}

TEST_CASE("a delay loop makes progress and never stops as a spin")
{
  auto f = booted(1000);
  start(f, "delay");
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
}

TEST_CASE("a poll of a flag set later stops only when the threshold is below the wait")
{
  constexpr std::uint64_t iterations = 40'000;
  auto const              wait       = [&](std::uint32_t threshold) {
    auto f = booted(threshold);
    start(f, "wait_flag");
    core::slice_request request = long_request();
    request.max_instructions    = icount{3 * iterations};
    auto const first            = f.run(request);
    if(first.stopped)
    {
      return first.stopped->reason;
    }
    REQUIRE(
        core::machine_internals::services(*f.target).write(
            flag, 4, 1, popcorn::initiator::debugger));
    return f.run_to_stop(long_request()).reason;
  };
  CHECK(wait(50'000) == stop_reason::user);
  CHECK(wait(10'000) == stop_reason::spin);
}

TEST_CASE("a loop storing a constant to RAM counts as no progress")
{
  auto f = booted(1000);
  start(f, "store_forever");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.contains("in store_forever"));
  CHECK_FALSE(stopped.detail.contains("polling"));
}

TEST_CASE("a reset forgets the loop being tracked")
{
  auto f = booted(1000);
  start(f, "wait_ready");
  core::slice_request request = long_request();
  request.max_instructions    = icount{3 * 900};
  REQUIRE_FALSE(f.run(request).stopped.has_value());
  f.target->reset();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  start(f, "wait_ready");
  request.max_instructions = icount{3 * 900};
  CHECK_FALSE(f.run(request).stopped.has_value());
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::spin);
}

TEST_CASE("threshold 0 never tracks")
{
  auto f = booted(0);
  start(f, "store_forever");
  core::slice_request request = long_request();
  request.max_instructions    = icount{100'000};
  CHECK_FALSE(f.run(request).stopped.has_value());
}

namespace
{
auto resident_bytes() -> std::uint64_t
{
  std::ifstream statm{"/proc/self/statm"};
  std::uint64_t size     = 0;
  std::uint64_t resident = 0;
  statm >> size >> resident;
  return resident * 4096;
}
}

TEST_CASE("a loop longer than the window keeps its MMIO read list bounded")
{
  auto f = booted(1000);
  start(f, "long_poll");
  auto const before        = resident_bytes();
  auto       request       = long_request();
  request.max_instructions = icount{15'000'000};
  REQUIRE_FALSE(f.run(request).stopped.has_value());
  CHECK(resident_bytes() - before < 16u << 20);
}

TEST_CASE("an idle wait with a tick handler is not a spin")
{
  core::machine_options options;
  options.spin_threshold = 10;
  fixture f{m33(), "idle", options};
  f.set("pc", sym("idle", "idle_entry"));
  core::slice_request request{
      .wall_budget = std::chrono::seconds{60}, .until_time = popcorn::vtime{1'000'000'000'000}};
  for(;;)
  {
    auto const slice = f.run(request);
    if(slice.stopped)
    {
      CHECK(slice.stopped->reason == stop_reason::time);
      break;
    }
  }
}

TEST_CASE("a poll of a register whose model completes later is not a spin")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  fixture f{m33(), "spin", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto& model         = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
  model.go_delay      = 100'000;
  model.data_on_event = true;
  start(f, "wait_model");
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
  CHECK(model.fired_at.has_value());
}

TEST_CASE("a hung poll still spins while an interrupt handler reads MMIO")
{
  auto f = booted(1000);
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("reads made by a helper the loop calls belong to the iteration")
{
  SUBCASE("the helper's model completes later: no spin")
  {
    core::machine_options options;
    options.spin_threshold = 1000;
    options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
    fixture f{m33(), "spin", options};
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    auto& model         = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
    model.go_delay      = 100'000;
    model.data_on_event = true;
    start(f, "wait_helper");
    CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
  }
  SUBCASE("a hung helper poll names the register")
  {
    for(auto const* const entry : {"wait_helper_hung", "wait_near_helper"})
    {
      CAPTURE(entry);
      auto f = booted(1000);
      start(f, entry);
      auto const stopped = f.run_to_stop(long_request());
      CHECK(stopped.reason == stop_reason::spin);
      CAPTURE(stopped.detail);
      CHECK(stopped.detail.contains(std::string{"in "} + entry));
      CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
    }
  }
}

namespace
{
struct raw_pc_arch : popcorn::testing::test_arch
{
  using test_arch::test_arch;

  auto deliver_pending() -> popcorn::arch::trap_result override
  {
    inside            = true;
    auto const result = test_arch::deliver_pending();
    inside            = false;
    return result;
  }

  [[nodiscard]] auto pc() const -> address override
  {
    return inside ? test_arch::pc() : read(UC_ARM_REG_PC) & ~address{1};
  }

  bool inside = false;
};
}

TEST_CASE("plain WFI wakes inside a polling loop keep its MMIO reads counting")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  SUBCASE("a hung poll names its register")
  {
    popcorn::testing::machine_fixture<raw_pc_arch> f{m33(), "spin", options};
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    f.set("pc", sym("spin", "wake_then_poll_hung"));
    auto const stopped = f.run_to_stop(long_request());
    CHECK(stopped.reason == stop_reason::spin);
    CAPTURE(stopped.detail);
    CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
  }
  SUBCASE("a model completing later restarts the count")
  {
    options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
    popcorn::testing::machine_fixture<raw_pc_arch> f{m33(), "spin", options};
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    auto& model         = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
    model.go_delay      = 100'000;
    model.data_on_event = true;
    f.set("pc", sym("spin", "wake_then_wait_model"));
    CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
  }
}

TEST_CASE("an inner poll that hangs inside an outer loop takes over as the loop")
{
  auto f = booted(1000);
  start(f, "nested_poll");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in nested_poll"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

namespace
{
auto install_tick(fixture const& f, std::string_view handler) -> void
{
  REQUIRE(f.target->mem().write(0x0800'0000 + 4 * 16, 4, sym("spin", handler) | 1));
}
}

TEST_CASE("a loop in the tick handler does not restart the interrupted poll")
{
  auto f = booted(1000);
  install_tick(f, "tick_walk");
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  REQUIRE(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in wait_ready_ticking"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("a handler that hangs after an interrupted loop is a spin in the handler")
{
  auto f = booted(1000);
  install_tick(f, "tick_hang");
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  REQUIRE(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in tick_hang"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("calls stopped inside a handler leave the spin depth of their caller")
{
  auto f = booted(1000);
  install_tick(f, "tick_hang");
  popcorn::arch::call_frame const frame{
      .entry           = sym("spin", "wait_ready_ticking"),
      .return_sentinel = 0x3000'0000,
      .interrupts      = true,
  };
  auto const hang = sym("spin", "tick_hang") & ~address{1};
  for(std::uint32_t call = 0; call < popcorn::core::spin_state::max_levels + 1; ++call)
  {
    CAPTURE(call);
    auto const called = f.target->call(frame, icount{2000});
    REQUIRE(called.stopped.reason == stop_reason::budget);
    REQUIRE(called.stopped.where.pc >= hang);
    REQUIRE(called.stopped.where.pc < hang + 16);
  }
  install_tick(f, "tick_walk");
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  REQUIRE(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("a masked call running another loop does not restart the paused poll")
{
  auto const uninterrupted = [] {
    auto f = booted(1000);
    start(f, "wait_ready");
    auto const slice = f.run(long_request());
    REQUIRE(slice.stopped.has_value());
    REQUIRE(slice.stopped->reason == stop_reason::spin);
    return slice.executed.value;
  }();
  auto f = booted(1000);
  start(f, "wait_ready");
  auto const first = f.run({.max_instructions = icount{1500}});
  REQUIRE_FALSE(first.stopped.has_value());
  popcorn::arch::call_frame const frame{
      .entry = sym("spin", "delay"), .return_sentinel = 0x3000'0000};
  REQUIRE(f.target->call(frame, icount{5000}).stopped.reason == stop_reason::budget);
  auto const rest = f.run(long_request());
  REQUIRE(rest.stopped.has_value());
  CHECK(rest.stopped->reason == stop_reason::spin);
  CHECK(first.executed.value + rest.executed.value == uninterrupted);
}
