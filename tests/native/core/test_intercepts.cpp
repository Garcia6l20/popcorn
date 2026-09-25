#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace arch = popcorn::arch;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture                   = popcorn::testing::machine_fixture<>;
constexpr address unmapped_page = 0x3000'0000;

auto booted(core::machine_options options = {}) -> fixture
{
  fixture f{m33(), "intercepts", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::reset_request);
  return f;
}

auto fn(std::string_view name) -> address
{
  return sym("intercepts", name);
}

auto call(fixture const& f, std::string_view name, std::vector<arch::call_arg> args = {})
    -> core::call_result
{
  return f.target->call(
      {.entry           = fn(name),
       .args            = std::move(args),
       .returns         = arch::value_kind::i32,
       .abi             = arch::float_abi::soft,
       .return_sentinel = unmapped_page},
      icount{10'000'000});
}

auto i32(std::uint32_t value) -> arch::call_arg
{
  return {.kind = arch::value_kind::i32, .bits = value};
}

auto returning(std::uint32_t value) -> core::intercept_handler
{
  return
      [value](core::intercept_call const&) { return core::intercept_result::returns(i32(value)); };
}
}

TEST_CASE("an intercept returns at once and the caller continues with its value")
{
  auto       f      = booted();
  int        calls  = 0;
  auto const hook   = f.target->add_intercept(fn("delay_ms"), [&](core::intercept_call const& c) {
    ++calls;
    CHECK(c.function() == fn("delay_ms"));
    CHECK(c.return_address() > fn("use_delay"));
    return core::intercept_result::returns(i32(5));
  });
  auto const result = call(f, "use_delay");
  CHECK(result.stopped.reason == stop_reason::returned);
  CHECK(result.value == 6);
  CHECK(calls == 1);
}

TEST_CASE("an intercept added or removed between calls takes effect on the next call")
{
  auto f = booted();
  CHECK(call(f, "call_seven").value == 107);
  auto hook = f.target->add_intercept(fn("seven"), returning(1));
  CHECK(call(f, "call_seven").value == 101);
  hook.reset();
  CHECK(call(f, "call_seven").value == 107);
}

TEST_CASE("a once intercept fires on the first call only")
{
  auto       f    = booted();
  auto const hook = f.target->add_intercept(fn("seven"), returning(1), core::intercept_mode::once);
  CHECK(call(f, "call_seven").value == 101);
  CHECK(call(f, "call_seven").value == 107);
}

TEST_CASE("several intercepts on one address run in add order, the first non-proceed wins")
{
  auto             f = booted();
  std::vector<int> order;
  auto const       first  = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
    order.push_back(1);
    return core::intercept_result::proceed();
  });
  auto const       second = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
    order.push_back(2);
    return core::intercept_result::returns(i32(3));
  });
  auto const       third  = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
    order.push_back(3);
    return core::intercept_result::returns(i32(4));
  });
  CHECK(call(f, "call_seven").value == 103);
  CHECK(order == std::vector<int>{1, 2});
}

TEST_CASE("an intercept stop leaves pc at the function and the resumed run executes its body")
{
  auto       f     = booted();
  int        calls = 0;
  auto const hook  = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
    ++calls;
    return core::intercept_result::stop(stop_reason::user);
  });
  f.target->begin_call(
      {.entry           = fn("call_seven"),
       .returns         = arch::value_kind::i32,
       .abi             = arch::float_abi::soft,
       .return_sentinel = unmapped_page});
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::user);
  CHECK(stop.where.pc == fn("seven"));
  CHECK(f.run_to_stop().reason == stop_reason::returned);
  CHECK(f.target->end_call() == 107);
  CHECK(calls == 1);
}

TEST_CASE("a skipped function counts its first translated block in full")
{
  constexpr std::uint64_t n        = 6;
  auto const              run_loop = [](std::string_view entry, bool intercept) {
    auto                             f = booted();
    std::optional<core::hook_handle> hook;
    if(intercept)
    {
      hook = f.target->add_intercept(fn("six"), returning(0));
    }
    f.set("pc", fn(entry));
    f.set("r4", 100);
    auto const before = f.target->virtual_clock().now();
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    return f.target->virtual_clock().now().value - before.value;
  };
  auto const skipped   = run_loop("loop_six", true);
  auto const reference = run_loop("loop_nop", false);
  CHECK(skipped == reference + 100 * n);
  CHECK(run_loop("loop_six", true) == skipped);
}

TEST_CASE("an intercept reads the arguments of the call")
{
  auto                       f = booted();
  std::vector<std::uint64_t> seen;
  auto const hook = f.target->add_intercept(fn("add"), [&](core::intercept_call const& c) {
    std::array const kinds{arch::value_kind::i32, arch::value_kind::i32};
    seen = c.args(kinds);
    return core::intercept_result::proceed();
  });
  CHECK(call(f, "add", {i32(2), i32(3)}).value == 5);
  CHECK(seen == std::vector<std::uint64_t>{2, 3});
}

TEST_CASE("a return hook sees every return of a recursive function")
{
  auto                             f = booted();
  std::vector<core::returned_call> returns;
  auto const                       hook = f.target->add_return_hook(
      fn("fib"), {arch::value_kind::i32}, arch::value_kind::i32,
      [&](core::returned_call const& r) { returns.push_back(r); });
  CHECK(call(f, "fib", {i32(10)}).value == 55);
  REQUIRE(returns.size() == 177);
  auto const fib = [](this auto const& self, std::uint64_t k) -> std::uint64_t {
    return k < 2 ? k : self(k - 1) + self(k - 2);
  };
  for(auto const& r : returns)
  {
    CAPTURE(r.args.front());
    CHECK(r.value == fib(r.args.front()));
    CHECK(r.function == fn("fib"));
  }
  CHECK(returns.back().args.front() == 10);
  CHECK(returns.back().occurrence == 176);
  auto const longest = std::ranges::max(returns, {}, &core::returned_call::instructions);
  CHECK(longest.args.front() == 10);
}

TEST_CASE("handles refuse changes from inside a handler, and a dropped handle goes at slice end")
{
  auto f = booted();
  SUBCASE("adding a hook from a handler")
  {
    auto const hook = f.target->add_intercept(fn("seven"), [&](core::intercept_call const& c) {
      std::ignore = c.target().add_intercept(fn("add"), returning(0));
      return core::intercept_result::proceed();
    });
    CHECK_THROWS_AS(std::ignore = call(f, "call_seven"), popcorn::machine_running);
  }
  SUBCASE("resetting a handle from a handler")
  {
    std::optional<core::hook_handle> hook;
    hook = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
      hook->reset();
      return core::intercept_result::proceed();
    });
    CHECK_THROWS_AS(std::ignore = call(f, "call_seven"), popcorn::machine_running);
  }
  SUBCASE("dropping a handle in a handler")
  {
    int                              calls = 0;
    std::optional<core::hook_handle> hook;
    hook = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
      ++calls;
      hook.reset();
      return core::intercept_result::returns(i32(1));
    });
    CHECK(call(f, "call_seven").value == 101);
    CHECK(call(f, "call_seven").value == 107);
    CHECK(calls == 1);
  }
}

TEST_CASE("a reset with a call in flight drops its frame")
{
  core::machine_options options;
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  fixture                          f{m33(), "intercepts", options};
  std::vector<core::returned_call> returns;
  auto const                       hook = f.target->add_return_hook(
      fn("resetting"), {}, arch::value_kind::i32,
      [&](core::returned_call const& r) { returns.push_back(r); });
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.word(0x2008'0000) == 2);
  REQUIRE(returns.size() == 1);
  CHECK(returns.front().value == 7);
  CHECK(returns.front().occurrence == 0);
}

TEST_CASE("a tail call returns through every hooked frame, innermost first")
{
  auto                             f = booted();
  std::vector<core::returned_call> returns;
  auto const record = [&](core::returned_call const& r) { returns.push_back(r); };
  auto const outer  = f.target->add_return_hook(
      fn("tail_f"), {arch::value_kind::i32}, arch::value_kind::i32, record);
  auto const inner = f.target->add_return_hook(
      fn("tail_g"), {arch::value_kind::i32}, arch::value_kind::i32, record);
  CHECK(call(f, "call_tail", {i32(4)}).value == 1015);
  REQUIRE(returns.size() == 2);
  CHECK(returns[0].function == fn("tail_g"));
  CHECK(returns[0].args.front() == 5);
  CHECK(returns[0].value == 15);
  CHECK(returns[1].function == fn("tail_f"));
  CHECK(returns[1].args.front() == 4);
  CHECK(returns[1].value == 15);
}

TEST_CASE("a call ending on its budget drops its frames and keeps the caller's")
{
  auto                             f = booted();
  std::vector<core::returned_call> sevens;
  int                              delays        = 0;
  auto const                       seven_returns = f.target->add_return_hook(
      fn("seven"), {}, arch::value_kind::i32,
      [&](core::returned_call const& r) { sevens.push_back(r); });
  auto const pause = f.target->add_intercept(
      fn("seven"),
      [](core::intercept_call const&) { return core::intercept_result::stop(stop_reason::user); },
      core::intercept_mode::once);
  auto const delay_returns = f.target->add_return_hook(
      fn("delay_ms"), {}, arch::value_kind::i32, [&](core::returned_call const&) { ++delays; });
  f.set("pc", fn("call_seven"));
  f.set("lr", fn("halt") | 1);
  auto const paused = f.run_to_stop();
  REQUIRE(paused.reason == stop_reason::user);
  REQUIRE(paused.where.pc == fn("seven"));
  auto const aborted = f.target->call(
      {.entry           = fn("use_delay"),
       .returns         = arch::value_kind::i32,
       .abi             = arch::float_abi::soft,
       .return_sentinel = unmapped_page},
      icount{200});
  CHECK(aborted.stopped.reason == stop_reason::budget);
  auto const halted = f.run_to_stop();
  CHECK(halted.reason == stop_reason::user);
  CHECK(halted.where.pc == fn("halt") + 2);
  CHECK(delays == 0);
  REQUIRE(sevens.size() == 1);
  CHECK(sevens[0].value == 7);
}

TEST_CASE("a resumed stop runs the entries after the stopping intercept once")
{
  auto       f       = booted();
  int        stops   = 0;
  int        counted = 0;
  int        returns = 0;
  auto const stopper = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
    ++stops;
    return core::intercept_result::stop(stop_reason::user);
  });
  auto const counter = f.target->add_intercept(fn("seven"), [&](core::intercept_call const&) {
    ++counted;
    return core::intercept_result::proceed();
  });
  auto const hook    = f.target->add_return_hook(
      fn("seven"), {}, arch::value_kind::i32, [&](core::returned_call const& r) {
        ++returns;
        CHECK(r.value == 7);
      });
  f.target->begin_call(
      {.entry           = fn("call_seven"),
       .returns         = arch::value_kind::i32,
       .abi             = arch::float_abi::soft,
       .return_sentinel = unmapped_page});
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(counted == 0);
  CHECK(returns == 0);
  CHECK(f.run_to_stop().reason == stop_reason::returned);
  CHECK(f.target->end_call() == 107);
  CHECK(stops == 1);
  CHECK(counted == 1);
  CHECK(returns == 1);
}
