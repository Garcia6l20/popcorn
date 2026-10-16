#include "arch_fixture.hpp"

#include "cortex_m.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/clock.hpp>
#include <popcorn/core/coverage.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/scheduler.hpp>

#include "../fake_services.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;

constexpr address       testdev        = 0x4001'0000;
constexpr address       iser0          = 0xE000'E100;
constexpr address       ispr0          = 0xE000'E200;
constexpr address       icsr           = 0xE000'ED04;
constexpr address       stir           = 0xE000'EF00;
constexpr address       shpr3          = 0xE000'ED20;
constexpr address       aircr          = 0xE000'ED0C;
constexpr std::uint32_t aircr_key      = 0x05FAu << 16;
constexpr address       ipr0           = 0xE000'E400;
constexpr std::uint32_t pendsvset      = 1u << 28;
constexpr std::uint32_t testdev_irq    = 3;
constexpr std::uint32_t handler_value  = 0xaa;
constexpr std::uint64_t handler_length = 4;

auto booted(bool enabled = true) -> arch_fixture
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  arch_fixture f{m33(), "irq", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  if(enabled)
  {
    REQUIRE(f.target->mem().write(iser0, 4, 1u << testdev_irq));
  }
  return f;
}

void enter(arch_fixture const& f, address target, std::uint32_t value)
{
  f.set("pc", sym("irq", "scs_raise") | 1u);
  f.set("r1", 1);
  f.set("r2", testdev);
  f.set("r3", handler_value);
  f.set("r9", value);
  f.set("r10", target);
  for(auto const name : {"r4", "r5", "r6", "r7"})
  {
    f.set(name, 0);
  }
}

auto data_writes(arch_fixture const& f) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> values;
  for(auto const& access : dynamic_cast<test_model&>(*f.target->model("TESTDEV")).accesses)
  {
    if(access.write and access.offset == test_model::data_offset)
    {
      values.push_back(access.value);
    }
  }
  return values;
}

auto run(arch_fixture const& f) -> core::slice_result
{
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::user);
  return result;
}
}

TEST_CASE("a store that pends an exception through the SCS takes it before the next instruction")
{
  auto f = booted();
  SUBCASE("STIR")
  {
    enter(f, stir, testdev_irq);
  }
  SUBCASE("ISPR")
  {
    enter(f, ispr0, 1u << testdev_irq);
  }
  SUBCASE("ICSR PENDSVSET")
  {
    enter(f, icsr, pendsvset);
  }
  std::ignore = run(f);
  CHECK(f.reg("r5") == 0);
  CHECK(data_writes(f) == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("a store that enables an already pending interrupt takes it before the next instruction")
{
  auto f = booted(false);
  REQUIRE(f.target->mem().write(ispr0, 4, 1u << testdev_irq));
  enter(f, iser0, 1u << testdev_irq);
  std::ignore = run(f);
  CHECK(f.reg("r5") == 0);
  CHECK(data_writes(f) == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("a STIR store with PRIMASK set leaves the interrupt pending and runs on")
{
  auto f = booted();
  f.set("primask", 1);
  enter(f, stir, testdev_irq);
  std::ignore = run(f);
  CHECK(f.reg("r4") == 1);
  CHECK(data_writes(f) == std::vector<std::uint32_t>{1});
}

TEST_CASE("instructions counted across an SCS precise exit are the instructions run")
{
  auto plain = booted(false);
  enter(plain, stir, testdev_irq);
  auto const without = run(plain).executed.value;

  auto f = booted();
  enter(f, stir, testdev_irq);
  CHECK(run(f).executed.value == without + handler_length);
}

TEST_CASE("a debugger store that pends an interrupt between slices cuts no block")
{
  auto f = booted();
  REQUIRE(f.target->mem().write(ispr0, 4, 1u << testdev_irq));
  enter(f, 0x2000'0000, 0);
  auto const hook =
      f.target->add_intercept(sym("irq", "scs_raise_next") + 4, [](core::intercept_call const&) {
        return core::intercept_result::stop(stop_reason::user);
      });
  f.target->enable_coverage({});
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->where.pc == sym("irq", "scs_raise_next") + 4);
  CHECK(f.target->coverage().cuts.size() == 1);
  CHECK(result.executed.value == handler_length + 14);
}

TEST_CASE("an SHPR store that raises a pended exception above BASEPRI takes it at once")
{
  auto f = booted();
  REQUIRE(f.target->mem().write(shpr3, 4, 0x0080'0000u));
  REQUIRE(f.target->mem().write(icsr, 4, pendsvset));
  f.set("basepri", 0x40);
  enter(f, shpr3, 0);
  std::ignore = run(f);
  CHECK(f.reg("r5") == 0);
  CHECK(data_writes(f) == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("an AIRCR store to a finer PRIGROUP that unmasks a pending interrupt takes it at once")
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  arch_fixture f{
      popcorn::testing::m33_with([](popcorn::part_description& d) { d.core.nvic_prio_bits = 4; }),
      "irq", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.target->mem().write(iser0, 4, 1u << testdev_irq));
  REQUIRE(f.target->mem().write(ipr0, 4, 0x40u << 24));
  REQUIRE(f.target->mem().write(aircr, 4, aircr_key | 5u << 8));
  f.set("basepri", 0x50);
  REQUIRE(f.target->mem().write(ispr0, 4, 1u << testdev_irq));
  enter(f, aircr, aircr_key | 3u << 8);
  std::ignore = run(f);
  CHECK(f.reg("r5") == 0);
  CHECK(data_writes(f) == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("the SCS asks for an exit only when a store makes an exception deliverable")
{
  struct engine_close
  {
    void operator()(uc_engine* engine) const noexcept
    {
      uc_close(engine);
    }
  };

  auto desc                = m33();
  desc.core.nvic_prio_bits = 4;
  popcorn::core::clock                     clock{1'000'000};
  popcorn::core::scheduler                 queue;
  popcorn::core::event_port                events{queue, popcorn::arch::arch_event_owner};
  popcorn::testing::fake_services          services;
  std::unique_ptr<uc_engine, engine_close> engine;
  popcorn::arch::armv8m::cortex_m          core{desc.core};
  auto const                               spec = core.engine();
  uc_engine*                               raw  = nullptr;
  REQUIRE(
      uc_open(static_cast<uc_arch>(spec.arch), static_cast<uc_mode>(spec.mode), &raw) == UC_ERR_OK);
  engine.reset(raw);
  if(spec.cpu_model)
  {
    REQUIRE(uc_ctl_set_cpu_model(raw, *spec.cpu_model) == UC_ERR_OK);
  }
  core.attach({raw, desc, clock, events, services});
  services.poke_word(0, 0x2000'1000);
  services.poke_word(4, 0x0000'0101);
  core.reset(0);

  auto const store = [raw](address at, std::uint32_t value) {
    REQUIRE(uc_mem_write(raw, at, &value, 4) == UC_ERR_OK);
  };
  store(iser0, 1u << testdev_irq);
  CHECK(services.instruction_ends == 0);
  store(ipr0, 0x40u << 24);
  std::uint32_t const basepri = 0x50;
  REQUIRE(uc_reg_write(raw, UC_ARM_REG_BASEPRI, &basepri) == UC_ERR_OK);
  store(aircr, aircr_key | 5u << 8);
  CHECK(services.instruction_ends == 0);
  store(ispr0, 1u << testdev_irq);
  CHECK(services.instruction_ends == 0);
  store(aircr, aircr_key | 3u << 8);
  CHECK(services.instruction_ends == 1);
  store(stir, testdev_irq);
  CHECK(services.instruction_ends == 1);
  store(shpr3, 0);
  CHECK(services.instruction_ends == 1);
}

TEST_CASE("an even pc written by the host keeps the Thumb state when an IRQ is taken first")
{
  auto const f = booted();
  REQUIRE(f.target->mem().write(ispr0, 4, 1u << testdev_irq));
  auto const resume = sym("irq", "scs_raise_next");
  REQUIRE(resume % 2 == 0);
  f.set("pc", resume);
  f.set("r1", 1);
  f.set("r2", testdev);
  f.set("r3", handler_value);
  for(auto const name : {"r4", "r5", "r6", "r7"})
  {
    f.set(name, 0);
  }
  CHECK(f.reg("pc") == resume);
  CHECK((f.reg("xpsr") & 0x0100'0000u) != 0);
  auto const stop = f.run_to_stop({.max_instructions = popcorn::icount{100}});
  CHECK(stop.reason == stop_reason::user);
  CHECK(f.reg("r4") == 1);
  CHECK(f.reg("r5") == 0);
  CHECK(data_writes(f) == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("a host pc write leaves a clear Thumb state clear, whatever bit 0")
{
  auto const f      = booted();
  auto const resume = sym("irq", "scs_raise_next");
  f.set("xpsr", 0);
  f.set("pc", resume | 1u);
  CHECK((f.reg("xpsr") & 0x0100'0000u) == 0);
  CHECK(f.reg("pc") == resume);
}

TEST_CASE("a run resumed with the Thumb state cleared by the host faults at once")
{
  auto const f      = booted();
  auto const resume = sym("irq", "scs_raise_next");
  f.set("pc", resume);
  f.set("r1", 1);
  f.set("r2", testdev);
  f.set("r4", 0);
  f.set("xpsr", 0);
  auto const stop = f.run_to_stop({.max_instructions = popcorn::icount{100}});
  REQUIRE(stop.reason == stop_reason::fault);
  CHECK(stop.fault->kind == popcorn::fault_kind::usage);
  CHECK(f.reg("r4") == 0);
}
