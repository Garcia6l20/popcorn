#include <popcorn/core/machine.hpp>

#include <popcorn/core/errors.hpp>

#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace
{
namespace core = popcorn::core;

auto toy() -> popcorn::mcu_description
{
  char const* const path = std::getenv("POPCORN_TOY_DESC");
  REQUIRE(path != nullptr);
  return popcorn::load_description(path);
}
}

static_assert(not std::is_copy_constructible_v<core::machine>);
static_assert(not std::is_move_constructible_v<core::machine>);
static_assert(not std::is_convertible_v<core::machine&, core::machine_services&>);
static_assert(not std::is_copy_constructible_v<core::hook_handle>);
static_assert(std::is_nothrow_move_constructible_v<core::hook_handle>);
static_assert(std::is_same_v<core::intercept_handle, core::hook_handle>);
static_assert(std::is_same_v<core::watch_handle, core::hook_handle>);

TEST_CASE("machine stub keeps its description, clock and trace ring")
{
  core::machine_options options;
  options.trace_capacity = 8;
  core::machine first{toy(), popcorn::testing::test_arch_factory(), options};
  CHECK(first.description().part == toy().part);
  CHECK(first.id() != 0);
  CHECK(first.generation() == 0);
  CHECK_FALSE(first.running());
  CHECK_FALSE(first.in_call());
  CHECK(first.virtual_clock().core_hz() == toy().core.reset_clock_hz);
  CHECK(first.virtual_clock().now() == popcorn::icount{0});
  CHECK(first.trace().capacity() == 8);
  CHECK(first.loaded_images().empty());

  options.core_hz = 160'000'000;
  options.cpi     = 2.0;
  core::machine second{toy(), popcorn::testing::test_arch_factory(), options};
  CHECK(second.id() != first.id());
  CHECK(second.virtual_clock().core_hz() == 160'000'000);
  CHECK(second.virtual_clock().instruction_hz() == 80'000'000);
}

TEST_CASE("every machine entry point answers on a toy description")
{
  core::machine                   target{toy(), popcorn::testing::test_arch_factory(), {}};
  popcorn::arch::call_frame const frame{.entry = 0x0800'0100, .return_sentinel = 0xffff'fffe};

  CHECK_NOTHROW(target.load({}));
  CHECK_NOTHROW(target.reset());
  auto const slice = target.run_slice({});
  REQUIRE(slice.stopped.has_value());
  CHECK(slice.stopped->reason == popcorn::stop_reason::fault);
  CHECK_NOTHROW(target.begin_call(frame));
  CHECK_THROWS_AS(target.call(frame, popcorn::icount{1000}), popcorn::machine_busy);
  CHECK(target.end_call() == 0);
  CHECK(target.call(frame, popcorn::icount{1000}).stopped.reason == popcorn::stop_reason::fault);
  CHECK_NOTHROW(target.restore(target.snapshot()));
  CHECK_THROWS_AS(target.restore(core::snapshot{core::snapshot_data{}}), popcorn::config_error);
  CHECK(target.add_intercept(0x0800'0100, {}).active());
  CHECK(target.add_return_hook(0x0800'0100, {}, popcorn::arch::value_kind::i32, {}).active());
  CHECK(target.add_watch(0x2000'0000, 4, core::watch_kind::write, {}).active());
  CHECK(target.add_trace_sink(core::trace_kinds::all(), {}).active());
  CHECK(target.peripheral("UART1").description().name == "UART1");
  CHECK(target.model("UART1") == nullptr);
  CHECK(target.add_log_sink({}).active());
  CHECK(target.add_intercept(0x0800'0100, {}, core::intercept_mode::once).active());
  CHECK_FALSE(target.coverage_enabled());
  CHECK_NOTHROW(target.enable_coverage({}));
  CHECK(target.coverage_enabled());
  CHECK_NOTHROW(target.disable_coverage());
  CHECK_NOTHROW(std::ignore = target.coverage());
  CHECK_NOTHROW(target.clear_coverage());

  auto const memory = target.mem();
  CHECK(memory.write(0x2000'0000, 2, 0xffff).has_value());
  CHECK(memory.read(0x2000'0000, 4) == 0xffffu);
  CHECK(memory.store<std::uint64_t>(0x2000'0001, 1).has_value());
  CHECK(memory.load<double>(0x2000'0001).has_value());

  auto const registers = target.regs();
  CHECK_FALSE(registers.list().empty());
  auto const r0 = registers.find("r0");
  REQUIRE(r0.has_value());
  registers.write(*r0, 0xffff'ffff);
  CHECK(registers.read(*r0) == 0xffff'ffff);
  CHECK_NOTHROW(std::ignore = registers.pc());
}

TEST_CASE("slice request defaults are one million instructions and one millisecond")
{
  core::slice_request const request;
  CHECK(request.max_instructions == popcorn::icount{1'000'000});
  CHECK(request.wall_budget == std::chrono::milliseconds{1});
  CHECK_FALSE(request.until.has_value());
  CHECK_FALSE(request.budget_left.has_value());
  CHECK_FALSE(request.until_time.has_value());
  CHECK_FALSE(request.exact_instructions.has_value());
  CHECK_FALSE(static_cast<bool>(request.stop_when));

  core::slice_result const result;
  CHECK_FALSE(result.stopped.has_value());
  CHECK(result.executed == popcorn::icount{0});
}

TEST_CASE("a hook handle owning nothing is inactive and moves")
{
  core::hook_handle first;
  CHECK_FALSE(first.active());
  first.reset();
  core::hook_handle second{std::move(first)};
  CHECK_FALSE(second.active());
  first = std::move(second);
  CHECK_FALSE(first.active());
}

TEST_CASE("intercept results carry exactly their action's data")
{
  constexpr auto proceed = core::intercept_result::proceed();
  static_assert(proceed.action() == core::intercept_action::proceed);
  static_assert(not proceed.value().has_value() and not proceed.reason().has_value());

  constexpr auto returned = core::intercept_result::returns(
      popcorn::arch::call_arg{.kind = popcorn::arch::value_kind::i64, .bits = 0x1'0000'0000});
  static_assert(returned.action() == core::intercept_action::returns);
  static_assert(returned.value()->bits == 0x1'0000'0000);
  static_assert(not returned.reason().has_value());

  constexpr auto void_return = core::intercept_result::returns(std::nullopt);
  static_assert(not void_return.value().has_value());

  constexpr auto stopped = core::intercept_result::stop(popcorn::stop_reason::user);
  static_assert(stopped.action() == core::intercept_action::stop);
  static_assert(stopped.reason() == popcorn::stop_reason::user);
  CHECK(std::format("{}", stopped.action()) == "stop");
}

TEST_CASE("watch and intercept enums format by name")
{
  CHECK(std::format("{}", core::watch_kind::access) == "access");
  CHECK(std::format("{}", core::watch_action::stop) == "stop");
  CHECK(std::format("{}", core::intercept_action::returns) == "returns");
}

TEST_CASE("machine options default to stopping on faults and reset requests")
{
  core::machine_options const options;
  CHECK(options.faults == popcorn::arch::fault_policy::stop);
  CHECK(options.on_reset_request == core::reset_request_policy::stop);
  CHECK(options.spin_threshold == 0);
  CHECK_FALSE(options.strict_peripherals);
  CHECK(std::format("{}", core::reset_request_policy::reset_and_continue) == "reset_and_continue");
}

TEST_CASE("a machine without a core clock is a configuration error")
{
  auto description                = toy();
  description.core.reset_clock_hz = 0;
  CHECK_THROWS_AS(
      core::machine(description, popcorn::testing::test_arch_factory(), {}), popcorn::config_error);
  core::machine_options options;
  options.core_hz = 1'000'000;
  core::machine const clocked{description, popcorn::testing::test_arch_factory(), options};
  CHECK(clocked.virtual_clock().core_hz() == 1'000'000);
}
