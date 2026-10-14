#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
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

TEST_CASE("an exception return completes the handler and the function it tail-called")
{
  using popcorn::testing::test_arch;
  auto f = booted();
  REQUIRE(f.target->mem().write(0x0800'0000 + 4 * 16, 4, fn("isr_tail") | 1));
  std::vector<core::returned_call> handlers;
  std::vector<core::returned_call> exits;
  std::vector<std::uint32_t>       exits_at_handler_return;
  auto const                       handler_hook = f.target->add_return_hook(
      fn("isr_tail"), {}, arch::value_kind::i32, [&](core::returned_call const& r) {
        handlers.push_back(r);
        exits_at_handler_return.push_back(*f.target->mem().read(fn("isr_exits"), 4));
      });
  auto const exit_hook = f.target->add_return_hook(
      fn("isr_exit"), {arch::value_kind::i32}, arch::value_kind::i32,
      [&](core::returned_call const& r) { exits.push_back(r); });
  f.set("pc", fn("loop_nop"));
  f.set("lr", fn("halt") | 1);
  f.set("r4", 3000);
  f.set("primask", 0);
  f.module->write_system(test_arch::timer_period_offset, 300);
  f.module->write_system(test_arch::timer_control_offset, 1);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.module->write_system(test_arch::timer_control_offset, 0);
  auto const entries = f.word(fn("isr_entries"));
  REQUIRE(entries >= 10);
  CHECK(f.word(fn("isr_exits")) == entries);
  REQUIRE(handlers.size() == entries);
  REQUIRE(exits.size() == entries);
  for(std::uint32_t index = 0; index < entries; ++index)
  {
    CAPTURE(index);
    CHECK(handlers[index].occurrence == index);
    CHECK(handlers[index].function == fn("isr_tail"));
    CHECK(exits_at_handler_return[index] == index + 1);
    CHECK(exits[index].occurrence == index);
    CHECK(exits[index].args.front() == 2);
    CHECK(exits[index].value == (index + 1) * 10 + 2);
    CHECK(handlers[index].instructions.value > exits[index].instructions.value);
  }
}

namespace
{
auto frame_here(fixture const& f) -> core::frame_return
{
  return {.ret = static_cast<address>(f.reg("lr") & ~std::uint64_t{1}), .sp = f.reg("sp")};
}

void stopped_at(fixture const& f, std::string_view name)
{
  core::slice_request request{.until = fn(name)};
  request.leave_until = true;
  REQUIRE(f.run_to_stop(request).reason == stop_reason::address);
  REQUIRE(f.reg("pc") == fn(name));
}

auto in_delay(core::machine_options options = {}) -> std::pair<fixture, core::frame_return>
{
  auto f = booted(std::move(options));
  f.set("pc", fn("use_delay"));
  f.set("lr", fn("halt") | 1);
  stopped_at(f, "delay_ms");
  auto const          target = frame_here(f);
  core::slice_request step{.exact_instructions = icount{40}};
  step.wall_budget = std::chrono::seconds{60};
  REQUIRE(f.run(step).stopped->reason == stop_reason::budget);
  return {std::move(f), target};
}

auto run_out(fixture const& f, core::slice_request request) -> std::pair<popcorn::stop, icount>
{
  request.wall_budget = std::chrono::seconds{60};
  icount total{};
  for(;;)
  {
    auto const result = f.run(request);
    total.value += result.executed.value;
    request.leave_until = false;
    if(result.stopped)
    {
      return {*result.stopped, total};
    }
  }
}
}

TEST_CASE("a frame-return goal stops at the return address with the entry stack pointer")
{
  auto [f, target] = in_delay();
  auto [g, same]   = in_delay();
  REQUIRE(same == target);
  auto const [stop, executed] = run_out(f, {.until_return = target});
  CHECK(stop.reason == stop_reason::returned);
  CHECK(f.reg("pc") == target.ret);
  CHECK(f.reg("sp") == target.sp);
  CHECK(f.reg("r0") == 1000);
  auto const [plain, plain_executed] = run_out(g, {.until = target.ret});
  CHECK(plain.reason == stop_reason::address);
  CHECK(executed == plain_executed);
  CHECK(f.reg("r0") == g.reg("r0"));
}

TEST_CASE("a frame-return goal stops the same whatever the slice length")
{
  auto [f, target]          = in_delay();
  auto const [whole, count] = run_out(f, {.until_return = target});
  REQUIRE(whole.reason == stop_reason::returned);
  for(auto const length : {std::uint64_t{1}, std::uint64_t{7}, count.value - 1, count.value})
  {
    CAPTURE(length);
    auto [g, again] = in_delay();
    auto const [stop, total] =
        run_out(g, {.max_instructions = icount{length}, .until_return = again});
    CHECK(stop.reason == stop_reason::returned);
    CHECK(total == count);
    CHECK(g.reg("pc") == target.ret);
  }
}

TEST_CASE("a frame-return goal runs an on_return hook of the same function first")
{
  auto       f        = booted();
  bool       returned = false;
  auto const hook     = f.target->add_return_hook(
      fn("delay_ms"), {arch::value_kind::i32}, arch::value_kind::i32,
      [&](core::returned_call const&) { returned = true; });
  f.set("pc", fn("use_delay"));
  f.set("lr", fn("halt") | 1);
  stopped_at(f, "delay_ms");
  auto const target           = frame_here(f);
  auto const [stop, executed] = run_out(f, {.until_return = target});
  CHECK(stop.reason == stop_reason::returned);
  CHECK(returned);
}

TEST_CASE("a frame-return goal with a budget below the return stops on the budget")
{
  auto [f, target]            = in_delay();
  auto const [stop, executed] = run_out(f, {.budget_left = icount{50}, .until_return = target});
  CHECK(stop.reason == stop_reason::budget);
  CHECK(f.reg("pc") != target.ret);
}

TEST_CASE("a frame-return goal in a recursion stops at the targeted frame's return only")
{
  auto f = booted();
  f.set("pc", fn("fib"));
  f.set("r0", 5);
  f.set("lr", fn("halt") | 1);
  core::slice_request deeper{.until = fn("fib")};
  deeper.leave_until = true;
  REQUIRE(f.run_to_stop(deeper).reason == stop_reason::address);
  REQUIRE(f.reg("r0") == 4);
  auto const target = frame_here(f);
  REQUIRE(target.ret > fn("fib"));
  REQUIRE(target.ret < fn("fib") + 64);
  auto const [stop, executed] = run_out(f, {.until_return = target});
  CHECK(stop.reason == stop_reason::returned);
  CHECK(f.reg("pc") == target.ret);
  CHECK(f.reg("sp") == target.sp);
  CHECK(f.reg("r0") == 3);
}

TEST_CASE("a frame-return goal through a tail call stops where the caller returns")
{
  auto f = booted();
  f.set("pc", fn("call_tail"));
  f.set("r0", 4);
  f.set("lr", fn("halt") | 1);
  stopped_at(f, "tail_f");
  auto const target           = frame_here(f);
  auto const [stop, executed] = run_out(f, {.until_return = target});
  CHECK(stop.reason == stop_reason::returned);
  CHECK(f.reg("pc") == target.ret);
  CHECK(f.reg("r0") == 15);
}

namespace
{
auto in_handler() -> std::pair<fixture, core::frame_return>
{
  using popcorn::testing::test_arch;
  auto f = booted();
  REQUIRE(f.target->mem().write(0x0800'0000 + 4 * 16, 4, fn("isr_tail") | 1));
  f.set("pc", fn("loop_nop"));
  f.set("lr", fn("halt") | 1);
  f.set("r4", 3000);
  f.set("primask", 0);
  f.module->write_system(test_arch::timer_period_offset, 300);
  f.module->write_system(test_arch::timer_control_offset, 1);
  stopped_at(f, "isr_tail");
  REQUIRE((f.reg("xpsr") & 0x1ffu) != 0);
  core::frame_return const target{.ret = static_cast<address>(f.reg("lr")), .sp = f.reg("sp")};
  REQUIRE((target.ret >> 24) == 0xffu);
  return {std::move(f), target};
}
}

TEST_CASE("a frame-return goal in an exception handler stops after its exception return")
{
  using popcorn::testing::test_arch;
  for(auto const length : {std::uint64_t{1'000'000}, std::uint64_t{1}, std::uint64_t{7}})
  {
    CAPTURE(length);
    auto [f, target] = in_handler();
    auto const [stop, executed] =
        run_out(f, {.max_instructions = icount{length}, .until_return = target});
    f.module->write_system(test_arch::timer_control_offset, 0);
    CHECK(stop.reason == stop_reason::returned);
    CHECK((f.reg("xpsr") & 0x1ffu) == 0);
    CHECK(f.reg("pc") >= fn("loop_nop"));
    CHECK(f.reg("pc") < fn("loop_nop") + 16);
    CHECK(f.word(fn("isr_exits")) == 1);
  }
}

TEST_CASE("an exception return met in a slice ended by its budget stops the next slice at once")
{
  auto [f, target]          = in_handler();
  auto const [whole, count] = run_out(f, {.until_return = target});
  REQUIRE(whole.reason == stop_reason::returned);
  auto [g, again] = in_handler();
  auto const [stop, executed] =
      run_out(g, {.budget_left = icount{count.value - 1}, .until_return = again});
  CHECK(stop.reason == stop_reason::budget);
  CHECK(executed == count);
  CHECK((g.reg("xpsr") & 0x1ffu) == 0);
  auto const          pc = g.reg("pc");
  core::slice_request next{.budget_left = icount{100'000}, .until_return = again};
  next.wall_budget  = std::chrono::seconds{60};
  auto const result = g.run(next);
  REQUIRE(result.stopped);
  CHECK(result.stopped->reason == stop_reason::returned);
  CHECK(result.executed.value == 0);
  CHECK(g.reg("pc") == pc);
}

namespace
{
constexpr std::uint64_t six_calls = 100;

auto in_loop_six() -> fixture
{
  auto f = booted();
  f.set("pc", fn("loop_six"));
  f.set("r4", six_calls);
  return f;
}

auto loop_to_end(fixture const& f, core::slice_request request = {}) -> popcorn::stop
{
  auto const [stop, executed] = run_out(f, request);
  REQUIRE(stop.reason == stop_reason::user);
  return stop;
}

auto stepped_to_end(fixture const& f) -> popcorn::stop
{
  for(;;)
  {
    auto const stop = f.run_to_stop({.exact_instructions = icount{1}});
    if(stop.reason != stop_reason::budget)
    {
      REQUIRE(stop.reason == stop_reason::user);
      return stop;
    }
  }
}

auto reached_at(address at) -> popcorn::vtime
{
  auto f                      = in_loop_six();
  auto const [stop, executed] = run_out(f, {.until = at});
  REQUIRE(stop.reason == stop_reason::address);
  return stop.time;
}
}

TEST_CASE("a probe counts every arrival and keeps the time before the first, however run")
{
  for(auto const at : {fn("six"), fn("six") + 2})
  {
    CAPTURE(at);
    auto const first = reached_at(at);
    for(auto const length : {std::uint64_t{1'000'000}, std::uint64_t{1}, std::uint64_t{7}})
    {
      CAPTURE(length);
      auto       f     = in_loop_six();
      auto const probe = f.target->add_probe(at);
      CHECK(probe.hits->count() == 0);
      CHECK_FALSE(probe.hits->first().has_value());
      std::ignore = loop_to_end(f, {.max_instructions = icount{length}});
      CHECK(probe.hits->count() == six_calls);
      CHECK(probe.hits->first() == first);
    }
    auto       f     = in_loop_six();
    auto const probe = f.target->add_probe(at);
    std::ignore      = stepped_to_end(f);
    CHECK(probe.hits->count() == six_calls);
    CHECK(probe.hits->first() == first);
  }
}

TEST_CASE("a probe added after its code was translated counts from then on, and never stops")
{
  auto f = in_loop_six();
  f.set("r4", 3);
  std::ignore      = loop_to_end(f);
  auto const probe = f.target->add_probe(fn("six") + 2);
  f.set("pc", fn("loop_six"));
  f.set("r4", six_calls);
  auto const [stop, executed] = run_out(f, {.budget_left = icount{200}});
  CHECK(stop.reason == stop_reason::budget);
  auto const counted = probe.hits->count();
  CHECK(counted > 0);
  CHECK(counted < six_calls);
  std::ignore = loop_to_end(f);
  CHECK(probe.hits->count() == six_calls);
}

TEST_CASE("a probe counts each arrival once beside intercepts, whatever the add order")
{
  for(auto const probe_first : {true, false})
  {
    CAPTURE(probe_first);
    auto                              f = in_loop_six();
    std::optional<core::probe_handle> probe;
    if(probe_first)
    {
      probe = f.target->add_probe(fn("six"));
    }
    int        calls = 0;
    auto const skip  = f.target->add_intercept(fn("six"), [&](core::intercept_call const&) {
      ++calls;
      return core::intercept_result::returns(i32(0));
    });
    if(not probe_first)
    {
      probe = f.target->add_probe(fn("six"));
    }
    std::ignore = loop_to_end(f);
    CHECK(calls == six_calls);
    CHECK(probe->hits->count() == six_calls);
  }
  auto       f       = in_loop_six();
  auto const probe   = f.target->add_probe(fn("six"));
  int        stops   = 0;
  auto const stopper = f.target->add_intercept(fn("six"), [&](core::intercept_call const&) {
    ++stops;
    return core::intercept_result::stop(stop_reason::user);
  });
  auto const after   = f.target->add_probe(fn("six"));
  auto const stop    = f.run_to_stop();
  REQUIRE(stop.reason == stop_reason::user);
  CHECK(f.reg("pc") == fn("six"));
  CHECK(probe.hits->count() == 1);
  CHECK(after.hits->count() == 1);
  core::slice_request step{.exact_instructions = icount{1}};
  CHECK(f.run_to_stop(step).reason == stop_reason::budget);
  CHECK(stops == 1);
  CHECK(probe.hits->count() == 1);
  CHECK(after.hits->count() == 1);
}

TEST_CASE("a removed probe counts no more and keeps its counts, a restore keeps it installed")
{
  auto       f     = in_loop_six();
  auto const saved = f.target->snapshot();
  auto       probe = f.target->add_probe(fn("six"));
  std::ignore      = loop_to_end(f);
  REQUIRE(probe.hits->count() == six_calls);
  auto const first = probe.hits->first();
  f.target->restore(saved);
  std::ignore = loop_to_end(f);
  CHECK(probe.hits->count() == 2 * six_calls);
  CHECK(probe.hits->first() == first);
  probe.hook.reset();
  CHECK_FALSE(probe.hook.active());
  f.set("pc", fn("loop_six"));
  f.set("r4", six_calls);
  std::ignore = loop_to_end(f);
  CHECK(probe.hits->count() == 2 * six_calls);
  f.target->restore(saved);
  std::ignore = loop_to_end(f);
  CHECK(probe.hits->count() == 2 * six_calls);
}

TEST_CASE("a probe counts an arrival once when the intercept that stopped there is removed")
{
  auto       f       = in_loop_six();
  auto const probe   = f.target->add_probe(fn("six"));
  auto       stopper = f.target->add_intercept(fn("six"), [](core::intercept_call const&) {
    return core::intercept_result::stop(stop_reason::user);
  });
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(probe.hits->count() == 1);
  stopper.reset();
  core::slice_request step{.exact_instructions = icount{1}};
  CHECK(f.run_to_stop(step).reason == stop_reason::budget);
  CHECK(probe.hits->count() == 1);
  std::ignore = loop_to_end(f);
  CHECK(probe.hits->count() == six_calls);
}

TEST_CASE("an intercept and a return hook run once per arrival when stepped")
{
  for(auto const at : {fn("six"), fn("six") + 2})
  {
    CAPTURE(at);
    auto          f       = in_loop_six();
    std::uint64_t calls   = 0;
    auto const    counter = f.target->add_intercept(at, [&](core::intercept_call const&) {
      ++calls;
      return core::intercept_result::proceed();
    });
    std::ignore           = stepped_to_end(f);
    CHECK(calls == six_calls);
  }
  auto          f       = in_loop_six();
  std::uint64_t returns = 0;
  auto const    hook    = f.target->add_return_hook(
      fn("six"), {}, arch::value_kind::i32, [&](core::returned_call const& r) {
        CHECK(r.value == 5);
        ++returns;
      });
  std::ignore = stepped_to_end(f);
  CHECK(returns == six_calls);
}
