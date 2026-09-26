#include "arch_fixture.hpp"

#include "../support/test_model.hpp"
#include "aapcs.hpp"
#include "cortex_m.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace arch   = popcorn::arch;
namespace periph = popcorn::periph;
using arch::call_arg;
using arch::call_frame;
using arch::call_stack;
using arch::float_abi;
using arch::value_kind;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::m33_with;
using popcorn::testing::sym;

constexpr address       sentinel     = 0x3000'0000;
constexpr std::uint64_t ps_per_instr = 250'000;
constexpr std::uint64_t period       = 4000;

auto fn(std::string_view name) -> address
{
  return sym("abi", name);
}

auto i32(std::int32_t value) -> call_arg
{
  return {.kind = value_kind::i32, .bits = static_cast<std::uint32_t>(value)};
}

auto i64(std::int64_t value) -> call_arg
{
  return {.kind = value_kind::i64, .bits = static_cast<std::uint64_t>(value)};
}

auto f32(float value) -> call_arg
{
  return {.kind = value_kind::f32, .bits = std::bit_cast<std::uint32_t>(value)};
}

auto f64(double value) -> call_arg
{
  return {.kind = value_kind::f64, .bits = std::bit_cast<std::uint64_t>(value)};
}

auto frame(
    std::string_view      name,
    std::vector<call_arg> args,
    value_kind            returns = value_kind::i32,
    float_abi             abi     = float_abi::hard_double) -> call_frame
{
  return {
      .entry           = fn(name),
      .args            = std::move(args),
      .returns         = returns,
      .abi             = abi,
      .return_sentinel = sentinel};
}

auto booted(core::machine_options options = {}, popcorn::mcu_description desc = m33())
    -> arch_fixture
{
  arch_fixture f{std::move(desc), "abi", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto call(arch_fixture const& f, call_frame const& one, icount budget = icount{1'000'000})
    -> core::call_result
{
  return f.target->call(one, budget);
}

auto registers_by_name(arch_fixture const& f) -> std::map<std::string, std::uint64_t>
{
  std::map<std::string, std::uint64_t> out;
  for(auto const& reg : f.module->registers())
  {
    out.emplace(reg.name, f.reg(reg.name));
  }
  return out;
}

auto run_for(arch_fixture const& f, std::uint64_t instructions) -> core::slice_result
{
  auto const& clock  = f.target->virtual_clock();
  auto const  at     = vtime{clock.time().ps + instructions * ps_per_instr};
  auto const  result = f.run(
      {.max_instructions = icount{400'000'000},
       .wall_budget      = std::chrono::seconds{30},
       .until_time       = at});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::time);
  return result;
}

auto booted_main(core::machine_options options = {}) -> arch_fixture
{
  auto f = booted(std::move(options));
  f.set("pc", fn("main_loop"));
  run_for(f, 5 * period + period / 2);
  return f;
}

auto ticks(arch_fixture const& f) -> std::uint32_t
{
  return f.word(fn("ticks"));
}

auto slots(std::vector<call_arg> const& args, bool vfp)
    -> std::vector<popcorn::arch::armv8m::aapcs::slot>
{
  return popcorn::arch::armv8m::aapcs::allocate(args, vfp).args;
}
}

TEST_CASE("the AAPCS allocator places arguments in core, VFP and stack slots")
{
  using popcorn::arch::armv8m::aapcs::place;
  using popcorn::arch::armv8m::aapcs::slot;
  auto const core = [](std::uint32_t at, std::uint32_t words) {
    return slot{.where = place::core, .index = at, .words = words};
  };
  auto const vfp = [](std::uint32_t at, std::uint32_t words) {
    return slot{.where = place::vfp, .index = at, .words = words};
  };
  auto const stack = [](std::uint32_t at, std::uint32_t words) {
    return slot{.where = place::stack, .index = at, .words = words};
  };
  CHECK(slots({i32(0), i32(0)}, true) == std::vector{core(0, 1), core(1, 1)});
  CHECK(slots({i32(0), i64(0)}, true) == std::vector{core(0, 1), core(2, 2)});
  CHECK(
      slots({i32(0), i32(0), i32(0), i64(0), i32(0)}, true)
      == std::vector{core(0, 1), core(1, 1), core(2, 1), stack(0, 2), stack(8, 1)});
  CHECK(slots({f32(0), f64(0), f32(0)}, true) == std::vector{vfp(0, 1), vfp(2, 2), vfp(1, 1)});
  CHECK(slots({f32(0), f64(0), f32(0)}, false) == std::vector{core(0, 1), core(2, 2), stack(0, 1)});
  std::vector<call_arg> nine(9, f64(0));
  auto const            placed = slots(nine, true);
  for(std::uint32_t index = 0; index < 8; ++index)
  {
    CHECK(placed[index] == vfp(2 * index, 2));
  }
  CHECK(placed[8] == stack(0, 2));
  std::vector<call_arg> const mixed{i32(0), f64(0), i32(0), f32(0), i64(0)};
  CHECK(
      slots(mixed, true) == std::vector{core(0, 1), vfp(0, 2), core(1, 1), vfp(2, 1), core(2, 2)});
  CHECK(
      slots(mixed, false)
      == std::vector{core(0, 1), core(2, 2), stack(0, 1), stack(4, 1), stack(8, 2)});
}

TEST_CASE("calls return values under hard_double and soft")
{
  auto f = booted_main();
  struct row
  {
    call_frame    one;
    std::uint64_t expected;
  };
  std::vector<row> const rows{
      {frame("add", {i32(2), i32(3)}), 5},
      {frame("mul64", {i64(0x1'0000'0001), i64(3)}, value_kind::i64), 0x3'0000'0003},
      {frame("add64_after3", {i32(1), i32(2), i32(3), i64(0x1'0000'0000)}, value_kind::i64),
       0x1'0000'0006},
      {frame("fadd", {f32(1.5f), f32(2.25f)}, value_kind::f32), 0x40700000},
      {frame("dmul", {f64(1.5), f64(-2.0)}, value_kind::f64), 0xC008000000000000},
      {frame("backfill", {f32(1.0f), f64(2.0), f32(3.0f)}, value_kind::f32), 0x42F60000},
      {frame(
           "sum10", {i32(1), i32(2), i32(3), i32(4), i32(5), i32(6), i32(7), i32(8), i32(9), i32(10)}),
       385},
      {frame(
           "d9", {f64(1.0), f64(2.0), f64(3.0), f64(4.0), f64(5.0), f64(6.0), f64(7.0), f64(8.0),
            f64(9.0)},
       value_kind::f64),
       std::bit_cast<std::uint64_t>(126.0)},
      {frame("mixed", {i32(1), f64(2.0), i32(3), f32(4.0f), i64(5)}, value_kind::f64),
       std::bit_cast<std::uint64_t>(54321.0)},
      {frame("soft_fadd", {f32(1.5f), f32(2.25f)}, value_kind::f32, float_abi::soft), 0x40700000},
      {frame("soft_dmul", {f64(1.5), f64(-2.0)}, value_kind::f64, float_abi::soft),
       0xC008000000000000},
      {frame(
           "soft_mixed", {i32(1), f64(2.0), i32(3), f32(4.0f), i64(5)}, value_kind::f64,
       float_abi::soft),
       std::bit_cast<std::uint64_t>(54321.0)},
  };
  for(auto const& one : rows)
  {
    CAPTURE(one.one.entry);
    auto const result = call(f, one.one);
    REQUIRE(result.stopped.reason == stop_reason::returned);
    CHECK(result.value == one.expected);
  }
}

TEST_CASE("a struct argument passed by pointer reads guest memory")
{
  auto f = booted_main();
  REQUIRE(f.target->mem().write(0x200B'E000, 4, 7));
  REQUIRE(f.target->mem().write(0x200B'E004, 4, 9));
  auto const result = call(f, frame("struct_sum", {i32(0x200B'E000)}));
  REQUIRE(result.stopped.reason == stop_reason::returned);
  CHECK(result.value == 9007);
}

TEST_CASE("prepare_call refusals throw before any register changes")
{
  auto       f      = booted_main();
  auto const before = registers_by_name(f);
  auto single = frame("mixed", {i32(1), f64(2.0), i32(3), f32(4.0f), i64(5)}, value_kind::f64);
  single.abi  = float_abi::hard_single;
  CHECK_THROWS_AS(f.target->begin_call(single), popcorn::config_error);
  auto high            = frame("add", {i32(2), i32(3)});
  high.return_sentinel = 0xF000'0000;
  CHECK_THROWS_AS(f.target->begin_call(high), popcorn::config_error);
  auto stack  = frame("add", {i32(2), i32(3)});
  stack.stack = 0x3000'0000;
  CHECK_THROWS_AS(f.target->begin_call(stack), popcorn::config_error);
  stack.stack = 0x0801'0000;
  CHECK_THROWS_AS(f.target->begin_call(stack), popcorn::config_error);
  CHECK(registers_by_name(f) == before);
  CHECK_FALSE(f.target->in_call());

  auto fpu_less    = booted({}, m33_with([](popcorn::mcu_description& d) { d.core.fpu = false; }));
  auto const plain = registers_by_name(fpu_less);
  CHECK_THROWS_AS(
      fpu_less.target->begin_call(frame("add", {i32(2), i32(3)})), popcorn::config_error);
  CHECK(registers_by_name(fpu_less) == plain);
  CHECK_FALSE(fpu_less.target->in_call());
}

TEST_CASE("privilege, masks and SVC inside calls")
{
  auto f = booted_main();
  {
    auto one       = frame("read_control", {});
    one.privileged = false;
    auto const r   = call(f, one);
    REQUIRE(r.stopped.reason == stop_reason::returned);
    CHECK((r.value & 1u) == 1u);
  }
  {
    auto one       = frame("try_cpsid", {});
    one.privileged = false;
    auto const r   = call(f, one);
    REQUIRE(r.stopped.reason == stop_reason::returned);
    CHECK(r.value == 0);
  }
  {
    auto const r = call(f, frame("svc_in_call", {}));
    REQUIRE(r.stopped.reason == stop_reason::returned);
    CHECK(r.value == 0x5C3);
  }
  {
    auto const r = call(f, frame("spin_forever", {}), icount{100'000});
    CHECK(r.stopped.reason == stop_reason::budget);
  }
  CHECK_THROWS_AS(
      [&] {
        f.target->begin_call(frame("add", {i32(2), i32(3)}));
        f.target->begin_call(frame("add", {i32(2), i32(3)}));
      }(),
      popcorn::machine_busy);
  std::ignore = f.target->end_call();
}

TEST_CASE("a call on a core paused in unprivileged Thread runs privileged and restores CONTROL")
{
  auto f = booted_main();
  f.set("pc", fn("unpriv_loop"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.reg("control") == 3);
  auto const before = registers_by_name(f);
  auto const r      = call(f, frame("read_control", {}));
  REQUIRE(r.stopped.reason == stop_reason::returned);
  CHECK((r.value & 1u) == 0u);
  CHECK(registers_by_name(f) == before);
  auto const masked = call(f, frame("set_basepri", {i32(0x40)}));
  REQUIRE(masked.stopped.reason == stop_reason::returned);
  CHECK(f.reg("basepri") == 0);
  CHECK(f.reg("control") == 3);
  CHECK(f.run_to_stop().reason == stop_reason::user);
}

TEST_CASE("the MPU never denies the call sentinel, popcorn's MPU is storage only")
{
  auto f = booted();
  f.set("pc", fn("mpu_setup"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto one            = frame("add", {i32(2), i32(3)});
  one.return_sentinel = 0x200B'F000;
  auto const r        = call(f, one);
  CHECK(r.stopped.reason == stop_reason::returned);
  CHECK(r.value == 5);
}

TEST_CASE("a call keeps the paused context and SysTick phase")
{
  auto       f       = booted_main();
  auto const before  = registers_by_name(f);
  auto const t0      = ticks(f);
  auto const counter = f.word(fn("counter"));
  REQUIRE(call(f, frame("add", {i32(2), i32(3)})).value == 5);
  CHECK(registers_by_name(f) == before);
  CHECK(f.word(fn("counter")) == counter);
  run_for(f, 10 * period);
  CHECK(ticks(f) == t0 + 10);

  auto const t1 = ticks(f);
  REQUIRE(
      call(f, frame("spin_forever", {}), icount{100'000}).stopped.reason == stop_reason::budget);
  CHECK(ticks(f) == t1);
  run_for(f, 10 * period);
  CHECK(ticks(f) == t1 + 11);

  auto const t2  = ticks(f);
  auto       run = frame("spin_forever", {});
  run.interrupts = true;
  REQUIRE(call(f, run, icount{100'000}).stopped.reason == stop_reason::budget);
  auto const during = ticks(f) - t2;
  CHECK((during == 24 or during == 25));
  auto const t3 = ticks(f);
  run_for(f, 10 * period);
  CHECK(ticks(f) == t3 + 10);
}

TEST_CASE("a SysTick pended before an unmasked call is taken once")
{
  auto f  = booted_main();
  auto t0 = ticks(f);
  REQUIRE(f.target->mem().write(0xE000'ED04, 4, 1u << 26));
  auto one       = frame("add", {i32(2), i32(3)});
  one.interrupts = true;
  REQUIRE(call(f, one).value == 5);
  CHECK(ticks(f) == t0 + 1);
  run_for(f, 10 * period);
  CHECK(ticks(f) == t0 + 11);
}

TEST_CASE("a call that reprograms SysTick leaves the pre-call phase and generation intact")
{
  for(auto const rewrite : {false, true})
  {
    CAPTURE(rewrite);
    auto       f  = booted_main();
    auto const t0 = ticks(f);
    REQUIRE(
        call(f, frame("reprogram_systick", {}), icount{1000}).stopped.reason
        == stop_reason::returned);
    if(rewrite)
    {
      auto const csr = f.target->mem().read(0xE000'E010, 4);
      REQUIRE(csr.has_value());
      REQUIRE(f.target->mem().write(0xE000'E010, 4, *csr & 0x7u));
    }
    run_for(f, 10 * period);
    CHECK(ticks(f) == t0 + 10);
  }
}

TEST_CASE("a call on a core asleep in WFI runs, and the core sleeps again after it")
{
  auto f = booted();
  f.set("pc", fn("main_wfi"));
  run_for(f, 2 * period + period / 2);
  REQUIRE(f.module->idle());
  auto const wfi_next = f.module->pc();
  auto const r        = call(f, frame("add", {i32(2), i32(3)}));
  REQUIRE(r.stopped.reason == stop_reason::returned);
  CHECK(r.value == 5);
  CHECK(f.module->idle());
  CHECK(f.module->pc() == wfi_next);
  auto const t0     = ticks(f);
  auto const before = f.target->virtual_clock().now().value;
  auto const slice =
      f.run({.wall_budget = std::chrono::seconds{10}, .exact_instructions = icount{1}});
  CHECK(slice.executed.value == 1);
  CHECK(f.target->virtual_clock().now().value - before > 1);
  CHECK(ticks(f) == t0);
}

TEST_CASE("stack selection follows stack_select and comes back after the call")
{
  auto f = booted_main();
  struct row
  {
    call_stack             select;
    std::optional<address> stack;
    std::uint32_t          spsel;
  };
  auto const probe = [&](row const& one, bool from_process) {
    auto const paused = registers_by_name(f);
    auto       c      = frame("read_sp_state", {});
    c.stack_select    = one.select;
    c.stack           = one.stack;
    auto const r      = call(f, c);
    REQUIRE(r.stopped.reason == stop_reason::returned);
    CHECK(r.value == one.spsel);
    auto const msp = f.word(fn("sp_result"));
    auto const psp = f.word(fn("sp_result") + 4);
    if(one.spsel == 1)
    {
      CHECK(psp == one.stack.value_or(paused.at("psp")));
      CHECK(msp == paused.at("msp"));
    }
    else
    {
      CHECK(msp == one.stack.value_or(paused.at("msp")));
      CHECK(psp == paused.at("psp"));
    }
    std::ignore      = from_process;
    auto const after = registers_by_name(f);
    CHECK(after.at("control") == paused.at("control"));
    CHECK(after.at("msp") == paused.at("msp"));
    CHECK(after.at("psp") == paused.at("psp"));
    CHECK(after.at("xpsr") == paused.at("xpsr"));
  };
  probe({call_stack::current, std::nullopt, 0}, false);
  probe({call_stack::process, 0x200B'0000, 1}, false);
  {
    auto c = frame(
        "sum10", {i32(1), i32(2), i32(3), i32(4), i32(5), i32(6), i32(7), i32(8), i32(9), i32(10)});
    c.stack_select = call_stack::process;
    c.stack        = 0x200B'0000;
    CHECK(call(f, c).value == 385);
  }
  f.set("pc", fn("unpriv_loop"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  probe({call_stack::current, std::nullopt, 1}, true);
  probe({call_stack::main, std::nullopt, 0}, true);
  CHECK(f.run_to_stop().reason == stop_reason::user);

  auto h = booted_main();
  h.set("pc", fn("handler_pause"));
  REQUIRE(h.run_to_stop().reason == stop_reason::user);
  REQUIRE((h.reg("xpsr") & 0x1ff) == 16);
  auto const paused = registers_by_name(h);
  {
    auto const r = call(h, frame("read_sp_state", {}));
    REQUIRE(r.stopped.reason == stop_reason::returned);
    CHECK(r.value == 0);
    CHECK(h.word(fn("sp_result")) == paused.at("msp"));
  }
  {
    auto c         = frame("read_sp_state", {});
    c.stack_select = call_stack::process;
    c.stack        = 0x200B'0000;
    auto const r   = call(h, c);
    CHECK(r.value == 1);
    CHECK(h.word(fn("sp_result") + 4) == 0x200B'0000);
  }
  CHECK(registers_by_name(h) == paused);
  CHECK(h.run_to_stop().reason == stop_reason::user);
  CHECK((h.reg("xpsr") & 0x1ff) == 16);
}

TEST_CASE("an intercept reads arguments and returns a value to the firmware caller")
{
  for(auto const soft : {false, true})
  {
    CAPTURE(soft);
    auto                       f = booted();
    std::vector<std::uint64_t> seen;
    auto const                 kinds = std::vector<value_kind>{
        value_kind::i32, value_kind::f64, value_kind::i32, value_kind::f32, value_kind::i64};
    auto const hook = f.target->add_intercept(
        fn(soft ? "soft_mixed" : "mixed"), [&](core::intercept_call const& c) {
          if(soft)
          {
            seen = c.args(kinds);
            return core::intercept_result::returns(f64(99.0));
          }
          auto signature =
              frame("mixed", {i32(0), f64(0), i32(0), f32(0), i64(0)}, value_kind::f64);
          f.module->read_args(signature);
          for(auto const& arg : signature.args)
          {
            seen.push_back(arg.bits);
          }
          f.module->return_to_caller(f64(99.0), float_abi::hard_double);
          return core::intercept_result::proceed();
        });
    f.set("pc", fn(soft ? "call_soft_mixed" : "call_mixed"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(seen.size() == 5);
    CHECK(seen[0] == 1);
    CHECK(seen[1] == std::bit_cast<std::uint64_t>(2.0));
    CHECK(seen[2] == 3);
    CHECK(seen[3] == std::bit_cast<std::uint32_t>(4.0f));
    CHECK(seen[4] == 5);
    CHECK(f.target->mem().read(fn("mixed_seen"), 4) == 0u);
    CHECK(
        f.target->mem().read(fn("mixed_seen") + 4, 4)
        == static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(99.0) >> 32));
  }
}

TEST_CASE("an IRQ line raised during a masked call is taken after end_call")
{
  std::shared_ptr<popcorn::testing::test_model> model;
  core::machine_options                         options;
  options.models.emplace(
      "TESTDEV",
      [&model](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> {
        model = std::make_shared<popcorn::testing::test_model>(ctx);
        return model;
      });
  auto f = booted_main(std::move(options));
  REQUIRE(model != nullptr);
  model->go_delay = 1000;
  REQUIRE(f.target->mem().write(0x4001'005C, 4, 1));
  REQUIRE(
      call(f, frame("spin_forever", {}), icount{100'000}).stopped.reason == stop_reason::budget);
  CHECK(f.word(fn("irq3_count")) == 0);
  run_for(f, 10 * period);
  CHECK(f.word(fn("irq3_count")) == 1);
}

TEST_CASE("an intercept returning from the SysTick handler keeps Thread running and spin detection")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  auto       f           = booted(std::move(options));
  auto const hook = f.target->add_intercept(fn("SysTick_Handler"), [](core::intercept_call const&) {
    return core::intercept_result::returns(std::nullopt);
  });
  f.set("pc", fn("poll_stat"));
  auto const stopped = f.run_to_stop();
  CHECK(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.find("UNMODELLED.STAT") != std::string::npos);
  CHECK(f.word(fn("ticks")) == 0);
  CHECK((f.reg("xpsr") & 0x1ff) == 0);
}

TEST_CASE("an IRQ pended before an unmasked call runs once, during the call")
{
  auto f = booted_main();
  REQUIRE(f.word(fn("irq3_count")) == 0);
  REQUIRE(f.target->mem().write(0xE000'E200, 4, 1u << 3));
  auto one       = frame("add", {i32(2), i32(3)});
  one.interrupts = true;
  REQUIRE(call(f, one).value == 5);
  CHECK(f.word(fn("irq3_count")) == 1);
  run_for(f, 10 * period);
  CHECK(f.word(fn("irq3_count")) == 1);
}
