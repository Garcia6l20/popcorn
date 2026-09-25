#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <tuple>
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
constexpr address boots         = 0x2008'0000;

auto booted(core::machine_options options = {}) -> fixture
{
  fixture f{m33(), "calls", std::move(options)};
  REQUIRE(f.run_to_stop({.budget_left = icount{1000}}).reason == stop_reason::budget);
  return f;
}

auto registers(fixture const& f) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> values;
  for(auto const& reg : f.target->regs().list())
  {
    values.push_back(f.target->regs().read(reg));
  }
  return values;
}

auto frame(std::string_view function, std::vector<arch::call_arg> args, address sentinel)
    -> arch::call_frame
{
  return {
      .entry           = sym("calls", function),
      .args            = std::move(args),
      .returns         = arch::value_kind::i32,
      .abi             = arch::float_abi::soft,
      .return_sentinel = sentinel,
  };
}

auto i32(std::uint32_t value) -> arch::call_arg
{
  return {.kind = arch::value_kind::i32, .bits = value};
}

auto counter(fixture const& f) -> std::uint32_t
{
  return f.word(sym("calls", "counter"));
}
}

TEST_CASE("a call returns its value and leaves the booted context as it was")
{
  auto       f       = booted();
  auto const context = registers(f);
  auto const count   = counter(f);
  SUBCASE("sentinel in unmapped space")
  {
    auto const result = f.target->call(frame("add", {i32(2), i32(3)}, unmapped_page), icount{1000});
    CHECK(result.stopped.reason == stop_reason::returned);
    CHECK(result.value == 5);
  }
  SUBCASE("sentinel in a page the booted firmware already runs")
  {
    auto const result =
        f.target->call(frame("add", {i32(2), i32(3)}, sym("calls", "counter_loop")), icount{1000});
    CHECK(result.stopped.reason == stop_reason::returned);
    CHECK(result.stopped.where.pc == sym("calls", "counter_loop"));
    CHECK(result.value == 5);
  }
  CHECK_FALSE(f.target->in_call());
  CHECK(registers(f) == context);
  CHECK(f.run_to_stop({.budget_left = icount{1000}}).reason == stop_reason::budget);
  CHECK(counter(f) > count);
}

TEST_CASE("a 64-bit call places its arguments in register pairs")
{
  auto f    = booted();
  auto call = frame(
      "mul64",
      {
          {.kind = arch::value_kind::i64, .bits = 0x1'0000'0003},
          {.kind = arch::value_kind::i64, .bits = 0x5'0000'0007}
  },
      unmapped_page);
  call.returns      = arch::value_kind::i64;
  auto const result = f.target->call(call, icount{1000});
  CHECK(result.stopped.reason == stop_reason::returned);
  CHECK(result.value == std::uint64_t{0x1'0000'0003} * 0x5'0000'0007);
}

TEST_CASE("a call that does not return stops and still restores the context")
{
  auto       f       = booted();
  auto const context = registers(f);
  SUBCASE("budget")
  {
    auto const result = f.target->call(frame("spin_forever", {}, unmapped_page), icount{100'000});
    CHECK(result.stopped.reason == stop_reason::budget);
    CHECK(result.value == 0);
  }
  SUBCASE("fault")
  {
    auto const result = f.target->call(frame("fault_now", {}, unmapped_page), icount{1000});
    CHECK(result.stopped.reason == stop_reason::fault);
  }
  CHECK(registers(f) == context);
  auto const count = counter(f);
  CHECK(f.run_to_stop({.budget_left = icount{1000}}).reason == stop_reason::budget);
  CHECK(counter(f) > count);
}

TEST_CASE("a reset request inside a call stops whatever the policy")
{
  core::machine_options options;
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  auto       f             = booted(options);
  auto const context       = registers(f);
  auto const result        = f.target->call(frame("reset_now", {}, unmapped_page), icount{1000});
  CHECK(result.stopped.reason == stop_reason::reset_request);
  CHECK(f.word(boots) == 1);
  CHECK(registers(f) == context);
  auto const count = counter(f);
  CHECK(f.run_to_stop({.budget_left = icount{1000}}).reason == stop_reason::budget);
  CHECK(counter(f) > count);
}

TEST_CASE("begin_call and end_call split a call over slices")
{
  auto f = booted();
  f.target->begin_call(frame("add", {i32(40), i32(2)}, unmapped_page));
  CHECK(f.target->in_call());
  CHECK_THROWS_AS(f.target->begin_call(frame("add", {}, unmapped_page)), popcorn::machine_busy);
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::returned);
  CHECK(f.target->end_call() == 42);
  CHECK_FALSE(f.target->in_call());
}

TEST_CASE("reset during a call is machine_busy")
{
  auto f = booted();
  f.target->begin_call(frame("spin_forever", {}, unmapped_page));
  CHECK_THROWS_AS(f.target->reset(), popcorn::machine_busy);
  std::ignore = f.target->end_call();
  CHECK_NOTHROW(f.target->reset());
}

namespace
{
struct call_failure : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

class failing_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  bool fail = false;

  auto on_unmapped(address addr, popcorn::arch::memory_op op) -> popcorn::arch::trap_result override
  {
    if(fail)
    {
      throw call_failure{"on_unmapped"};
    }
    return test_arch::on_unmapped(addr, op);
  }
};
}

TEST_CASE("call ends the call when a slice throws")
{
  popcorn::testing::machine_fixture<failing_arch> f{m33(), "calls", core::machine_options{}};
  REQUIRE(f.run_to_stop({.budget_left = icount{1000}}).reason == stop_reason::budget);
  std::vector<std::uint64_t> context;
  for(auto const& reg : f.target->regs().list())
  {
    context.push_back(f.target->regs().read(reg));
  }
  f.module->fail = true;
  CHECK_THROWS_AS(
      std::ignore = f.target->call(frame("fault_now", {}, unmapped_page), icount{1000}),
      call_failure);
  CHECK_FALSE(f.target->in_call());
  std::vector<std::uint64_t> after;
  for(auto const& reg : f.target->regs().list())
  {
    after.push_back(f.target->regs().read(reg));
  }
  CHECK(after == context);
}
