#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/scheduler.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>
#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace
{
namespace core  = popcorn::core;
namespace image = popcorn::image;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;

using popcorn::testing::elf;
using popcorn::testing::m33;
using popcorn::testing::m33_code_alias;
using popcorn::testing::sym;

template <typename Arch = popcorn::testing::test_arch>
struct fixture : popcorn::testing::machine_fixture<Arch>
{
  explicit fixture(popcorn::mcu_description desc, std::string_view firmware, auto... extra)
      : popcorn::testing::machine_fixture<Arch>{
            std::move(desc), firmware, core::machine_options{}, extra...}
  {}
};

auto alu_count(std::uint64_t loops) -> std::uint64_t
{
  return 3 + 5 * loops + 1;
}

struct alu_regs
{
  std::uint32_t r0, r1, r2, r3;
};

auto alu_after(std::uint32_t loops, std::uint64_t executed) -> alu_regs
{
  alu_regs                                 regs{loops, 0, 0, 0};
  std::array<void (*)(alu_regs&), 3> const prologue{
      [](alu_regs& r) { r.r1 = 1; }, [](alu_regs& r) { r.r2 = 0; }, [](alu_regs& r) { r.r3 = 0; }};
  std::array<void (*)(alu_regs&), 4> const body{
      [](alu_regs& r) { r.r2 += r.r1; }, [](alu_regs& r) { r.r3 ^= r.r2; },
      [](alu_regs& r) { r.r1 += 3; }, [](alu_regs& r) { --r.r0; }};
  for(std::uint64_t k = 0; k < executed; ++k)
  {
    if(k < 3)
    {
      prologue[k](regs);
    }
    else if(auto const step = (k - 3) % 5; step < 4)
    {
      body[step](regs);
    }
  }
  return regs;
}

auto alu_pc(std::uint64_t executed) -> address
{
  auto const entry = sym("alu", "alu_entry");
  if(executed < 3)
  {
    return entry + 2 * static_cast<address>(executed);
  }
  return sym("alu", "alu_body") + 2 * static_cast<address>((executed - 3) % 5);
}

auto alu(std::uint32_t loops) -> fixture<>
{
  fixture<> f{m33(), "alu"};
  f.set("r0", loops);
  return f;
}

auto boot_run(popcorn::mcu_description desc) -> fixture<>
{
  fixture<>  f{std::move(desc), "run"};
  auto const booted = f.run_to_stop();
  INFO(
      std::format(
          "{} at {:#x} fault {:#x}", booted.reason, booted.where.pc,
          booted.fault ? booted.fault->fault_address : 0));
  REQUIRE(booted.reason == stop_reason::user);
  return f;
}

template <typename Arch> void enter(fixture<Arch> const& f, std::string_view function)
{
  f.set("pc", sym("run", function));
}
}

TEST_CASE("a budget stops at the first block boundary at or past it")
{
  auto       f      = alu(1'000'000);
  auto const before = f.target->virtual_clock().now();
  auto const result =
      f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{1'000'000}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == stop_reason::budget);
  CHECK(result.executed.value >= 1'000'000);
  CHECK(result.executed.value <= 1'000'005);
  CHECK(f.target->virtual_clock().now().value - before.value == result.executed.value);
  CHECK(result.stopped->instructions == f.target->virtual_clock().now());
}

TEST_CASE("a budget spans slices when the caller subtracts what each executed")
{
  auto                f = alu(1'000'000);
  core::slice_request request{
      .max_instructions = icount{10'000},
      .wall_budget      = std::chrono::seconds{60},
      .budget_left      = icount{1'000'000}};
  std::uint64_t total  = 0;
  int           slices = 0;
  for(;;)
  {
    auto const slice = f.run(request);
    total += slice.executed.value;
    ++slices;
    if(slice.stopped)
    {
      CHECK(slice.stopped->reason == stop_reason::budget);
      break;
    }
    request.budget_left = icount{request.budget_left->value - slice.executed.value};
    REQUIRE(slices < 1000);
  }
  CHECK(total >= 1'000'000);
  CHECK(total <= 1'000'005);
  CHECK(slices >= 100);
}

TEST_CASE("the executed count equals the instructions the loop runs")
{
  for(std::uint32_t const loops : {1u, 1000u, 1'000'000u})
  {
    CAPTURE(loops);
    auto       f = alu(loops);
    auto const stop =
        f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("alu", "alu_done")});
    CHECK(stop.reason == stop_reason::address);
    CHECK(stop.where.pc == sym("alu", "alu_done"));
    CHECK(stop.instructions.value == alu_count(loops));
    auto const expected = alu_after(loops, alu_count(loops));
    CHECK(f.reg("r3") == expected.r3);
  }
}

TEST_CASE("exact_instructions stops after exactly that many instructions")
{
  for(std::uint64_t const count : {1u, 2u, 7u, 1000u})
  {
    CAPTURE(count);
    auto       f     = alu(1'000'000);
    auto const start = std::chrono::steady_clock::now();
    auto const result =
        f.run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{count}});
    auto const took = std::chrono::steady_clock::now() - start;
    REQUIRE(result.stopped.has_value());
    CHECK(result.stopped->reason == stop_reason::budget);
    CHECK(result.executed.value == count);
    CHECK(f.target->regs().pc() == alu_pc(count));
    auto const expected = alu_after(1'000'000, count);
    CHECK(f.reg("r0") == expected.r0);
    CHECK(f.reg("r1") == expected.r1);
    CHECK(f.reg("r2") == expected.r2);
    CHECK(f.reg("r3") == expected.r3);
    CHECK(took < std::chrono::milliseconds{100});
  }
}

TEST_CASE("single steps reach the registers of one exact run")
{
  for(std::uint64_t const count : {2u, 7u})
  {
    CAPTURE(count);
    auto stepped = alu(1'000'000);
    for(std::uint64_t i = 0; i < count; ++i)
    {
      REQUIRE(
          stepped.run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{1}})
              .executed.value
          == 1);
    }
    auto whole = alu(1'000'000);
    std::ignore =
        whole.run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{count}});
    for(auto const* name : {"r0", "r1", "r2", "r3", "pc"})
    {
      CHECK(stepped.reg(name) == whole.reg(name));
    }
  }
}

TEST_CASE("the slice bound yields and the next slice continues")
{
  auto one = alu(100'000);
  std::ignore =
      one.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("alu", "alu_done")});

  auto sliced = alu(100'000);
  int  slices = 0;
  for(;;)
  {
    auto const slice = sliced.run(
        {.max_instructions = icount{10'000},
         .wall_budget      = std::chrono::seconds{60},
         .until            = sym("alu", "alu_done")});
    ++slices;
    if(slice.stopped)
    {
      CHECK(slice.stopped->reason == stop_reason::address);
      break;
    }
    CHECK(slice.executed.value >= 10'000);
    REQUIRE(slices < 1000);
  }
  CHECK(slices > 40);
  CHECK(sliced.reg("r3") == one.reg("r3"));
  CHECK(sliced.target->virtual_clock().now() == one.target->virtual_clock().now());
}

TEST_CASE("the wall budget ends a long slice without a stop")
{
  auto       f     = alu(0xffff'ffff);
  auto const start = std::chrono::steady_clock::now();
  auto const slice = f.run(
      {.max_instructions = icount{1'000'000'000}, .wall_budget = std::chrono::microseconds{1}});
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{50});
  CHECK_FALSE(slice.stopped.has_value());
  CHECK(slice.executed.value > 0);
}

TEST_CASE("until stops before the address, also inside a block translated without it")
{
  auto       f   = boot_run(m33());
  auto const mid = sym("run", "straight_mid");

  enter(f, "straight");
  auto const first = f.run_to_stop({.until = sym("run", "straight")});
  CHECK(first.reason == stop_reason::address);
  CHECK(
      f.run({.wall_budget = std::chrono::seconds{60}, .until = sym("run", "straight")})
          .executed.value
      == 0);

  auto const fresh = f.run_to_stop({.until = mid});
  CHECK(fresh.reason == stop_reason::address);
  CHECK(fresh.where.pc == mid);
  CHECK(f.reg("r3") == 4);

  enter(f, "straight");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r0") == 11);

  enter(f, "straight");
  auto const stale = f.run_to_stop({.until = mid});
  CHECK(stale.reason == stop_reason::address);
  CHECK(stale.where.pc == mid);
  CHECK(f.reg("r0") == 1);

  enter(f, "straight");
  auto const through = f.run_to_stop();
  CHECK(through.reason == stop_reason::user);
  CHECK(f.reg("r0") == 11);
}

TEST_CASE("code running from the alias at address 0 never stops there without until")
{
  auto f = boot_run(m33());
  f.set("pc", sym("run", "straight") - 0x0800'0000);
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::user);
  CHECK(f.reg("r0") == 11);
  CHECK(stop.where.pc < 0x0800'0000);
}

TEST_CASE("until at the current pc stops at once with nothing executed")
{
  auto       f     = alu(10);
  auto const entry = sym("alu", "alu_entry");
  auto const slice = f.run({.wall_budget = std::chrono::seconds{60}, .until = entry});
  REQUIRE(slice.stopped.has_value());
  CHECK(slice.stopped->reason == stop_reason::address);
  CHECK(slice.executed.value == 0);
  CHECK(slice.stopped->where.pc == entry);
}

TEST_CASE("debugger patches of flash reach the next run through both addresses")
{
  fixture<>  f{m33(), "basic"};
  auto const patched = sym("basic", "patched");
  auto const call    = [&] {
    f.set("pc", patched);
    f.set("lr", sym("basic", "patched_ret") | 1);
    CHECK(f.run_to_stop().reason == stop_reason::user);
    return f.reg("r0");
  };
  CHECK(call() == 1);
  REQUIRE(f.target->mem().write(patched, 2, 0x2002));
  CHECK(call() == 2);
  REQUIRE(f.target->mem().write(patched - 0x0800'0000, 2, 0x2003));
  CHECK(call() == 3);
}

TEST_CASE("a debugger patch through rw SRAM reaches code running at its rx alias")
{
  auto       f      = boot_run(m33_code_alias());
  auto const ram_fn = sym("run", "ram_fn");
  auto const alias  = ram_fn - 0x2000'0000 + 0x0a00'0000;
  enter(f, "ram_calls");
  f.set("r0", alias | 1);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.target->mem().write(ram_fn, 2, 0x2005));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.word(sym("run", "ram_results")) == 1);
  CHECK(f.word(sym("run", "ram_results") + 4) == 5);
}

namespace
{
class dma_patch_arch final : public popcorn::testing::test_arch
{
public:
  dma_patch_arch(popcorn::core_desc const& core, address target) : test_arch{core}, target_{target}
  {}

  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override
  {
    if(intno == 7 and bkpt_immediate() == 1)
    {
      REQUIRE(attached_services().write(target_, 2, 0x2007, popcorn::initiator::dma));
      write(UC_ARM_REG_PC, (pc() + 2) | 1);
      return popcorn::arch::trap_result::resume();
    }
    return test_arch::on_interrupt(intno);
  }

private:
  address target_;
};

enum class request_kind
{
  stop,
  reset,
  end,
};

class stop_request_arch final : public popcorn::testing::test_arch
{
public:
  stop_request_arch(popcorn::core_desc const& core, request_kind kind)
      : test_arch{core}, kind_{kind}
  {}

  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override
  {
    if(intno == 7 and bkpt_immediate() == 1)
    {
      auto& services = attached_services();
      switch(kind_)
      {
        case request_kind::stop:
          services.request_stop(stop_reason::user, "why");
          break;
        case request_kind::reset:
          services.request_reset(popcorn::reset_kind::software);
          break;
        case request_kind::end:
          services.end_slice();
          break;
      }
      write(UC_ARM_REG_PC, (pc() + 2) | 1);
      return popcorn::arch::trap_result::resume();
    }
    return test_arch::on_interrupt(intno);
  }

private:
  request_kind kind_;
};

struct test_failure : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

class throwing_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  bool           throw_on_deliverable = false;
  bool           throw_on_invalid     = false;
  core::machine* nested               = nullptr;

  [[nodiscard]] auto deliverable_now() const -> bool override
  {
    if(throw_on_deliverable)
    {
      throw test_failure{"deliverable_now"};
    }
    return test_arch::deliverable_now();
  }

  auto on_invalid_insn() -> popcorn::arch::trap_result override
  {
    if(throw_on_invalid)
    {
      throw test_failure{"on_invalid_insn"};
    }
    return test_arch::on_invalid_insn();
  }

  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override
  {
    if(nested != nullptr)
    {
      std::ignore = nested->run_slice({});
    }
    return test_arch::on_interrupt(intno);
  }
};

class stuck_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  bool armed = false;

  auto deliver_pending() -> popcorn::arch::trap_result override
  {
    if(armed)
    {
      std::ignore = attached_events().schedule(attached_clock().now(), 1, 0);
    }
    return test_arch::deliver_pending();
  }
};

class abort_pend_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  static constexpr std::uint32_t prefetch_abort = 3;
  static constexpr std::uint32_t exception_exit = 8;

  std::uint32_t aborts = 0;
  std::uint32_t other  = 0;

  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override
  {
    if(intno == prefetch_abort)
    {
      ++aborts;
      set_irq_line(0, true);
      return popcorn::arch::trap_result::resume();
    }
    if(intno != exception_exit)
    {
      other = intno;
    }
    return test_arch::on_interrupt(intno);
  }
};

template <typename Arch>
auto boot_with(popcorn::mcu_description desc, auto... extra) -> fixture<Arch>
{
  fixture<Arch> f{std::move(desc), "run", extra...};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}
}

TEST_CASE("a DMA patch from inside a slice takes effect at the next block boundary")
{
  auto const ram_fn = sym("run", "ram_fn");
  auto       f      = boot_with<dma_patch_arch>(m33(), ram_fn);
  f.set("pc", sym("run", "ram_calls"));
  f.set("r0", ram_fn | 1);
  auto const slice = f.run({.wall_budget = std::chrono::seconds{60}});
  REQUIRE(slice.stopped.has_value());
  CHECK(slice.stopped->reason == stop_reason::user);
  CHECK(f.word(sym("run", "ram_results")) == 1);
  CHECK(f.word(sym("run", "ram_results") + 4) == 7);
}

TEST_CASE("a guest store through rw SRAM reaches code running at its rx alias")
{
  auto desc = m33_code_alias();
  SUBCASE("rw window mapped first")
  {}
  SUBCASE("rx window mapped first")
  {
    std::ranges::rotate(desc.regions.begin() + 2, desc.regions.begin() + 3, desc.regions.end());
    REQUIRE(desc.regions[2].name == "SRAM_CODE");
  }
  auto       f      = boot_run(desc);
  auto const ram_fn = sym("run", "ram_fn");
  enter(f, "guest_patch");
  f.set("r0", (ram_fn - 0x2000'0000 + 0x0a00'0000) | 1);
  f.set("r1", ram_fn);
  f.set("r2", 0x2009);
  auto const patched = f.run_to_stop();
  INFO(
      std::format(
          "{} at {:#x} fault {:#x}", patched.reason, patched.where.pc,
          patched.fault ? patched.fault->fault_address : 0));
  REQUIRE(patched.reason == stop_reason::user);
  CHECK(f.word(sym("run", "ram_results")) == 1);
  CHECK(f.word(sym("run", "ram_results") + 4) == 9);
}

TEST_CASE("a guest store through one rwx window reaches code running at the other")
{
  auto desc = m33();
  desc.regions.push_back({
      .name     = "SRAM_ALIAS",
      .start    = 0x1000'0000,
      .size     = 768 * 1024,
      .kind     = popcorn::region_kind::ram,
      .acc      = {.read = true, .write = true, .execute = true},
      .secure   = false,
      .physical = 0x2000'0000
  });
  auto const ram_fn   = sym("run", "ram_fn");
  auto const aliased  = ram_fn - 0x2000'0000 + 0x1000'0000;
  address    run_at   = ram_fn;
  address    store_at = aliased;
  SUBCASE("code at the target, store through the alias")
  {}
  SUBCASE("code at the alias, store through the target")
  {
    std::swap(run_at, store_at);
  }
  auto f = boot_run(desc);
  enter(f, "guest_patch");
  f.set("r0", run_at | 1);
  f.set("r1", store_at);
  f.set("r2", 0x2009);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.word(sym("run", "ram_results")) == 1);
  CHECK(f.word(sym("run", "ram_results") + 4) == 9);
}

TEST_CASE("a stop requested from a handler is taken at the next block with its detail")
{
  auto f = boot_with<stop_request_arch>(m33(), request_kind::stop);
  f.set("pc", sym("run", "ram_calls"));
  f.set("r0", sym("run", "ram_fn") | 1);
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::user);
  CHECK(stop.detail == "why");
  CHECK(stop.where.pc == sym("run", "ram_calls") + 10);
}

TEST_CASE("a reset request from a handler stops with reset_request at the next block")
{
  auto f = boot_with<stop_request_arch>(m33(), request_kind::reset);
  f.set("pc", sym("run", "ram_calls"));
  f.set("r0", sym("run", "ram_fn") | 1);
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::reset_request);
  CHECK(stop.where.pc == sym("run", "ram_calls") + 10);
}

TEST_CASE("a slice end request from a handler ends the slice without a stop")
{
  auto f = boot_with<stop_request_arch>(m33(), request_kind::end);
  f.set("pc", sym("run", "ram_calls"));
  f.set("r0", sym("run", "ram_fn") | 1);
  auto const slice = f.run({.wall_budget = std::chrono::seconds{60}});
  CHECK_FALSE(slice.stopped.has_value());
  CHECK(f.target->regs().pc() == sym("run", "ram_calls") + 10);
  CHECK(f.run_to_stop().reason == stop_reason::user);
}

TEST_CASE("an exception from a callback reaches the caller and the machine stays usable")
{
  auto f = boot_with<throwing_arch>(m33());
  SUBCASE("from the block hook")
  {
    f.module->set_irq_line(0, true);
    f.set("primask", 1);
    f.set("pc", sym("run", "straight"));
    f.module->throw_on_deliverable = true;
    CHECK_THROWS_AS(std::ignore = f.run(), test_failure);
    CHECK_FALSE(f.target->running());
    f.module->throw_on_deliverable = false;
    CHECK(f.run_to_stop().reason == stop_reason::user);
  }
  SUBCASE("before a fault of the same slice")
  {
    f.set("pc", sym("run", "undefined"));
    f.module->throw_on_invalid = true;
    CHECK_THROWS_AS(std::ignore = f.run(), test_failure);
    f.module->throw_on_invalid = false;
    f.set("pc", sym("run", "undefined"));
    CHECK(f.run_to_stop().reason == stop_reason::fault);
  }
  SUBCASE("run_slice from a handler is machine_busy")
  {
    f.set("pc", sym("run", "straight"));
    f.module->nested = f.target.get();
    CHECK_THROWS_AS(std::ignore = f.run(), popcorn::machine_busy);
    f.module->nested = nullptr;
    CHECK_FALSE(f.target->running());
  }
}

TEST_CASE("an IRQ pended while masked is taken right after cpsie")
{
  auto f = boot_run(m33());
  enter(f, "irq_order");
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.module->set_irq_line(0, true);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const log = sym("run", "order_log");
  CHECK(f.word(sym("run", "order_count")) == 4);
  CHECK(f.word(log) == 1);
  CHECK(f.word(log + 4) == 2);
  CHECK(f.word(log + 8) == 0x10);
  CHECK(f.word(log + 12) == 3);
}

TEST_CASE("traps stop with their fault or reason")
{
  auto f = boot_run(m33());
  SUBCASE("unmapped load")
  {
    enter(f, "load_unmapped");
    auto const stop = f.run_to_stop();
    CHECK(stop.reason == stop_reason::fault);
    REQUIRE(stop.fault.has_value());
    CHECK(stop.fault->kind == popcorn::fault_kind::unmapped);
    CHECK(stop.fault->fault_address == 0x6000'0000);
    CHECK(f.module->last_refused() == popcorn::arch::memory_op::read);
  }
  SUBCASE("store to flash")
  {
    enter(f, "store_flash");
    auto const stop = f.run_to_stop();
    CHECK(stop.reason == stop_reason::fault);
    REQUIRE(stop.fault.has_value());
    CHECK(stop.fault->fault_address == 0x0800'0000);
    CHECK(f.module->last_refused() == popcorn::arch::memory_op::write);
  }
  SUBCASE("undefined instruction")
  {
    enter(f, "undefined");
    auto const stop = f.run_to_stop();
    CHECK(stop.reason == stop_reason::fault);
    REQUIRE(stop.fault.has_value());
    CHECK(stop.fault->kind == popcorn::fault_kind::usage);
    CHECK(stop.where.pc == sym("run", "undefined"));
  }
  SUBCASE("breakpoint")
  {
    enter(f, "straight");
    auto const stop = f.run_to_stop();
    CHECK(stop.reason == stop_reason::user);
    CHECK_FALSE(stop.fault.has_value());
  }
}

TEST_CASE("a reset after a run restarts the clock at 0")
{
  auto f      = alu(1'000'000);
  std::ignore = f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{10'000}});
  CHECK(f.target->virtual_clock().now().value >= 10'000);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(f.target->virtual_clock().now() == icount{0});
  CHECK(f.target->virtual_clock().time() == popcorn::vtime{0});
}

TEST_CASE("stop_when is evaluated at the slice end")
{
  auto       f     = alu(1'000'000);
  bool       asked = false;
  auto const slice = f.run(
      {.max_instructions = icount{1000}, .wall_budget = std::chrono::seconds{60}, .stop_when = [&] {
         asked = true;
         return true;
       }});
  CHECK(asked);
  REQUIRE(slice.stopped.has_value());
  CHECK(slice.stopped->reason == stop_reason::user);
}

TEST_CASE("the wall budget ends a slice whose engine runs stop at every first block")
{
  auto f = boot_with<stuck_arch>(m33());
  f.set("pc", sym("run", "straight"));
  f.module->armed  = true;
  auto const start = std::chrono::steady_clock::now();
  auto const slice = f.run(
      {.max_instructions = icount{1'000'000'000}, .wall_budget = std::chrono::milliseconds{5}});
  auto const took = std::chrono::steady_clock::now() - start;
  CHECK_FALSE(slice.stopped.has_value());
  CHECK(slice.executed.value == 0);
  CHECK(took >= std::chrono::milliseconds{5});
  CHECK(took < std::chrono::seconds{2});
}

TEST_CASE("a resumed trap that pends an interrupt enters its handler in the same slice")
{
  auto       f       = boot_with<abort_pend_arch>(m33());
  auto const handler = sym("run", "count_irq");
  REQUIRE(f.target->mem().write(0x0800'0000 + 4 * 16, 4, handler | 1));
  enter(f, "jump_xn");
  f.module->other = 0;
  auto const slice =
      f.run({.max_instructions = icount{10'000}, .wall_budget = std::chrono::seconds{60}});
  CHECK(f.module->other == 0);
  CHECK_FALSE(slice.stopped.has_value());
  CHECK(slice.executed.value >= 10'000);
  REQUIRE(f.module->aborts > 1);
  auto const entries = f.word(sym("run", "irq_count"));
  CHECK((entries == f.module->aborts or entries + 1 == f.module->aborts));
}
