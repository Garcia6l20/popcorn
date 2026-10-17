#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::initiator;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;

constexpr address testdev    = 0x4001'0000;
constexpr address unmodelled = 0x4002'0000;

auto options(bool strict = false) -> core::machine_options
{
  core::machine_options made;
  made.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  made.strict_peripherals = strict;
  return made;
}

using fixture = popcorn::testing::machine_fixture<>;

auto booted(bool strict = false) -> fixture
{
  fixture f{m33(), "periph", options(strict)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto model(fixture const& f) -> test_model&
{
  auto* const found = dynamic_cast<test_model*>(f.target->model("TESTDEV"));
  REQUIRE(found != nullptr);
  return *found;
}

void enter(fixture const& f, std::string_view function)
{
  f.set("pc", sym("periph", function));
}

auto result(fixture const& f, std::size_t index) -> std::uint32_t
{
  return f.word(sym("periph", "results") + 4 * static_cast<address>(index));
}
}

TEST_CASE("guest loads and stores reach the register layer through every access size")
{
  auto f = booted();
  enter(f, "cpu_accesses");
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(result(f, 0) == 0xa5a5'a5a5);
  CHECK(result(f, 1) == 0xa5a5'3ca5);
  CHECK(result(f, 2) == 0x77);
  CHECK(result(f, 3) == 0xa5);
  CHECK(result(f, 4) == 0);
  CHECK(result(f, 5) == 0x42);
  CHECK(f.target->mem().read(testdev + 8, 4) == 0x42u);
  CHECK(f.target->mem().read(0x5001'0008, 4) == 0x42u);
}

TEST_CASE("claimed registers reach the model with offset, size, values and context")
{
  auto  f = booted();
  auto& m = model(f);
  m.accesses.clear();
  enter(f, "model_access");
  auto const stop = f.run_to_stop();
  REQUIRE(stop.reason == stop_reason::user);
  REQUIRE(m.accesses.size() == 2);
  CHECK(m.accesses[0].offset == test_model::data_offset);
  CHECK(m.accesses[0].size == 4);
  CHECK_FALSE(m.accesses[0].write);
  CHECK(m.accesses[0].ctx.by == initiator::cpu);
  CHECK(m.accesses[0].ctx.now == stop.instructions);
  CHECK(m.accesses[1].offset == test_model::ctrl_offset);
  CHECK(m.accesses[1].write);
  CHECK(m.accesses[1].old == 0);
  CHECK(m.accesses[1].value == 1);
  SUBCASE("without an exact pc consumer the pc is the start of the translated block")
  {
    CHECK(m.accesses[0].ctx.pc == sym("periph", "model_access"));
    CHECK(m.accesses[1].ctx.pc == sym("periph", "model_access"));
  }
}

TEST_CASE("a model bus error on a CPU read faults at the register, not at a later unmapped load")
{
  auto f              = booted();
  model(f).fail_reads = true;
  enter(f, "fault_then_unmapped");
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::fault);
  REQUIRE(stop.fault.has_value());
  CHECK(stop.fault->fault_address == testdev + test_model::data_offset);
  CHECK(f.module->last_refused() == popcorn::arch::memory_op::read);
}

TEST_CASE("a model bus error on a DMA read returns to the caller without a stop")
{
  auto f              = booted();
  model(f).fail_reads = true;
  auto& services      = f.module->attached_services();
  CHECK(
      services.read(testdev + test_model::data_offset, 4, initiator::dma).error()
      == popcorn::bus_error::denied);
  CHECK(
      f.target->mem().read(testdev + test_model::data_offset, 4).error()
      == popcorn::bus_error::denied);
}

TEST_CASE("of two exceptions in one block, the first leaves run_slice")
{
  auto f               = booted();
  model(f).throw_reads = true;
  model(f).accesses.clear();
  enter(f, "two_reads");
  try
  {
    std::ignore = f.run();
    FAIL("no exception");
  }
  catch(std::runtime_error const& e)
  {
    CHECK(std::string{e.what()} == "read 1");
  }
  CHECK(model(f).accesses.size() == 2);
  CHECK_FALSE(f.target->running());
}

TEST_CASE("an unmodelled register is logged once, and fails the run under strict")
{
  SUBCASE("logged")
  {
    auto                          f = booted();
    std::vector<core::log_record> records;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
    enter(f, "unmodelled_reads");
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(records.size() == 1);
    CHECK(records[0].kind == core::log_kind::unmodelled_access);
    CHECK(records[0].peripheral == "UNMODELLED");
    CHECK(records[0].reg == "STAT");
    CHECK(records[0].offset == 4);
    CHECK_FALSE(records[0].write);
    CHECK(records[0].pc == sym("periph", "unmodelled_reads"));
  }
  SUBCASE("strict")
  {
    auto f = booted(true);
    enter(f, "unmodelled_reads");
    CHECK_THROWS_AS(std::ignore = f.run(), popcorn::model_error);
    CHECK_FALSE(f.target->running());
    CHECK(f.run_to_stop().reason == stop_reason::user);
  }
}

TEST_CASE("under strict every unmodelled access raises, and the log stays once per register")
{
  for(auto const strict : {false, true})
  {
    for(auto const restore : {false, true})
    {
      CAPTURE(strict);
      CAPTURE(restore);
      auto                          f = booted(strict);
      std::vector<core::log_record> records;
      auto const                    sink =
          f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
      auto const saved = f.target->snapshot();
      for(auto round = 0; round < 3; ++round)
      {
        CAPTURE(round);
        if(restore)
        {
          f.target->restore(saved);
        }
        enter(f, "unmodelled_reads");
        if(strict)
        {
          CHECK_THROWS_AS(std::ignore = f.run(), popcorn::model_error);
        }
        else
        {
          CHECK(f.run_to_stop().reason == stop_reason::user);
        }
      }
      CHECK(records.size() == 1);
    }
  }
}

TEST_CASE("an access to an offset without register reads 0 and warns once")
{
  auto                          f = booted();
  std::vector<core::log_record> records;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
  auto& bus = core::machine_internals::services(*f.target);
  CHECK(bus.read(testdev + 0x100, 4, popcorn::initiator::debugger) == 0u);
  CHECK(bus.read(testdev + 0x100, 4, popcorn::initiator::debugger) == 0u);
  REQUIRE(records.size() == 1);
  CHECK(records[0].kind == core::log_kind::model_warning);
}

TEST_CASE("a stop a model requests from a write is taken at the next block, the write not replayed")
{
  auto  f         = booted();
  auto& m         = model(f);
  m.stop_on_write = true;
  m.accesses.clear();
  enter(f, "data_write");
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::user);
  CHECK(stop.detail == "model stop");
  CHECK(m.accesses.size() == 1);
}

TEST_CASE("a block access through services splits into bus accesses in address order")
{
  auto  f = booted();
  auto& m = model(f);
  m.accesses.clear();
  std::array<std::byte, 8> bytes{};
  REQUIRE(f.module->attached_services().read_block(testdev + 0x56, bytes, initiator::dma));
  REQUIRE(m.accesses.size() == 2);
  CHECK(m.accesses[0].offset == 0x58);
  CHECK(m.accesses[0].size == 4);
  CHECK(m.accesses[1].offset == 0x5c);
  CHECK(m.accesses[1].size == 2);
  CHECK(m.accesses[1].ctx.by == initiator::dma);
}

TEST_CASE("the debugger reaches the arch's own MMIO page")
{
  auto f = booted();
  CHECK(
      f.target->mem().read(popcorn::testing::test_arch::system_base + 0x40, 4)
      == popcorn::testing::test_arch::system_pattern);
}

TEST_CASE("unaligned guest accesses reach MMIO as aligned pieces")
{
  auto  f = booted();
  auto& m = model(f);
  m.accesses.clear();
  enter(f, "unaligned");
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  std::vector<std::array<std::uint32_t, 3>> seen;
  for(auto const& access : m.accesses)
  {
    seen.push_back({access.offset, access.size, access.write ? 1u : 0u});
  }
  CHECK(
      seen
      == std::vector<std::array<std::uint32_t, 3>>{
          {0x58, 4, 0},
          {0x5c, 4, 0},
          {0x59, 1, 1},
          {0x5a, 1, 1},
          {0x5b, 1, 1},
          {0x5c, 1, 1}
  });
}

TEST_CASE("a DMA patch of code from a model write takes effect in the same slice")
{
  auto       f      = booted();
  auto const ram_fn = sym("periph", "ram_fn");
  auto&      m      = model(f);
  m.patch_on_go     = ram_fn;
  m.patch_value     = 0x2007;
  enter(f, "dma_patch");
  f.set("r0", ram_fn | 1);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(result(f, 0) == 1);
  CHECK(result(f, 1) == 7);
}

TEST_CASE("a DMA write between slices reaches the next slice's code")
{
  auto       f      = booted();
  auto const ram_fn = sym("periph", "ram_fn");
  REQUIRE(f.module->attached_services().write(ram_fn, 2, 0x2009, initiator::dma));
  enter(f, "dma_patch");
  f.set("r0", ram_fn | 1);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(result(f, 0) == 9);
}

TEST_CASE("a clock rate change inside a model call keeps until_time exact in one slice")
{
  auto f                    = booted();
  model(f).halve_rate_on_go = true;
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const start  = f.target->virtual_clock().time();
  auto const target = popcorn::vtime{start.ps + 1'000'000'000};
  auto const slice  = f.run(
      {.max_instructions = icount{100'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until_time       = target});
  REQUIRE(slice.stopped.has_value());
  CHECK(slice.stopped->reason == stop_reason::time);
  CHECK(f.target->virtual_clock().core_hz() == 2'000'000);
  CHECK(slice.stopped->time.ps >= target.ps);
  CHECK(slice.stopped->time.ps < target.ps + 500'000);
  CHECK(slice.executed.value < 2'000 + 10);
}

TEST_CASE("an event a model schedules from an MMIO write fires on time inside the engine run")
{
  auto  f = booted();
  auto& m = model(f);
  m.accesses.clear();
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const slice = f.run(
      {.wall_budget = std::chrono::seconds{60}, .budget_left = icount{4 * test_model::delay}});
  REQUIRE(slice.stopped.has_value());
  REQUIRE(m.accesses.size() == 1);
  REQUIRE(m.fired_at.has_value());
  auto const due = m.accesses.front().ctx.now.value + test_model::delay;
  CHECK(m.fired_at->value >= due);
  CHECK(m.fired_at->value <= due + 2);
}

TEST_CASE(
    "an access through the SEC_ alias window is via_secure_alias, neither is secure without "
    "TrustZone")
{
  auto  f = booted();
  auto& m = model(f);
  m.accesses.clear();
  auto& services = f.module->attached_services();
  REQUIRE(services.read(testdev + test_model::data_offset, 4, initiator::cpu));
  REQUIRE(services.read(0x5001'0000 + test_model::data_offset, 4, initiator::cpu));
  REQUIRE(m.accesses.size() == 2);
  CHECK_FALSE(m.accesses[0].ctx.via_secure_alias);
  CHECK(m.accesses[1].ctx.via_secure_alias);
  CHECK_FALSE(m.accesses[0].ctx.secure);
  CHECK_FALSE(m.accesses[1].ctx.secure);
}

TEST_CASE("a model bus error reaches every services caller, the CPU initiator included")
{
  auto  f        = booted();
  auto& m        = model(f);
  m.fail_writes  = true;
  auto& services = f.module->attached_services();
  CHECK(
      services.write(testdev + test_model::data_offset, 4, 1, initiator::cpu).error()
      == popcorn::bus_error::denied);
  SUBCASE("the arch stacking an exception frame into the erroring register sees the error")
  {
    f.set("pc", sym("periph", "data_write"));
    f.set("msp", testdev + test_model::data_offset + 0x20);
    f.set("sp", testdev + test_model::data_offset + 0x20);
    f.module->set_irq_line(0, true);
    CHECK_THROWS_AS(std::ignore = f.run(), popcorn::emulation_error);
  }
}

TEST_CASE(
    "under strict_peripherals the arch and model debugger path reads unmodelled registers "
    "freely")
{
  auto                          f = booted(true);
  std::vector<core::log_record> records;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
  CHECK(
      core::machine_internals::services(*f.target).read(
          unmodelled + 4, 4, popcorn::initiator::debugger)
      == 1u);
  CHECK(records.empty());
  enter(f, "unmodelled_reads");
  CHECK_THROWS_AS(std::ignore = f.run(), popcorn::model_error);
  CHECK(records.size() == 1);
}

TEST_CASE(
    "a host view access to a peripheral without a model or to an offset without register is a "
    "model_error, strict or not, logs nothing and leaves the firmware's first access logged")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    auto                          f = booted(strict);
    std::vector<core::log_record> records;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
    auto const refused = [](auto&& access, std::string_view peripheral, std::string_view reg,
                            address offset) {
      try
      {
        std::ignore = access();
        FAIL("no model_error");
      }
      catch(popcorn::model_error const& error)
      {
        CHECK(error.peripheral() == peripheral);
        CHECK(error.reg() == reg);
        CHECK(error.offset() == offset);
      }
    };
    auto const               view = f.target->mem();
    std::array<std::byte, 8> bytes{};
    refused([&] { return view.read(unmodelled + 4, 4); }, "UNMODELLED", "STAT", 4);
    refused([&] { return view.write(unmodelled, 4, 1); }, "UNMODELLED", "DATA", 0);
    refused([&] { return view.read_block(unmodelled, bytes); }, "UNMODELLED", "DATA", 0);
    refused([&] { return view.write_block(unmodelled + 2, bytes); }, "UNMODELLED", "DATA", 0);
    refused([&] { return view.read(unmodelled + 0x100, 4); }, "UNMODELLED", "", 0x100);
    refused([&] { return view.read_block(unmodelled + 2, bytes); }, "UNMODELLED", "DATA", 0);
    refused([&] { return view.read(testdev + 0x100, 4); }, "TESTDEV", "", 0x100);
    refused([&] { return view.read_block(testdev + 0x5c, bytes); }, "TESTDEV", "", 0x60);
    CHECK(view.read(testdev + 8, 4).has_value());
    CHECK(records.empty());
    enter(f, "unmodelled_reads");
    if(strict)
    {
      CHECK_THROWS_AS(std::ignore = f.run(), popcorn::model_error);
    }
    else
    {
      std::ignore = f.run();
    }
    CHECK(
        std::ranges::count(records, core::log_kind::unmodelled_access, &core::log_record::kind)
        >= 1);
  }
}

TEST_CASE("a model logs through its services to the machine's log sinks")
{
  auto                          f = booted();
  auto&                         m = model(f);
  std::vector<core::log_record> records;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
  m.log_messages  = true;
  m.data_on_event = true;
  m.accesses.clear();
  enter(f, "go_and_wait");
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const write_pc = std::ranges::find_if(m.accesses, [](auto const& a) { return a.write; });
  REQUIRE(write_pc != m.accesses.end());
  REQUIRE(records.size() == 2);
  CHECK(records[0].kind == core::log_kind::model_warning);
  CHECK(records[0].peripheral == "TESTDEV");
  CHECK(records[0].message == "write 0x5c");
  CHECK(records[0].pc == write_pc->ctx.pc);
  CHECK(records[1].message == "event");
  CHECK(records[1].peripheral == "TESTDEV");
  CHECK(records[1].pc == test_model::event_pc);
  f.target->reset();
  REQUIRE(records.size() == 3);
  CHECK(records[2].message == "reset");
  CHECK(records[2].pc == f.reg("pc"));
}

TEST_CASE("a time-keyed event follows a core rate change made in the same MMIO write")
{
  auto  f            = booted();
  auto& m            = model(f);
  m.halve_rate_on_go = true;
  m.time_delay_ps    = 25'000'000;
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const slice = f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{1000}});
  REQUIRE(slice.stopped.has_value());
  CHECK(f.target->virtual_clock().core_hz() == 2'000'000);
  REQUIRE(m.go_time.has_value());
  REQUIRE(m.fired_time.has_value());
  auto const due = m.go_time->ps + *m.time_delay_ps;
  CHECK(m.fired_time->ps >= due);
  CHECK(m.fired_time->ps <= due + 2 * 500'000);
}

TEST_CASE("a time-keyed event follows a core speed-up made in the same MMIO write")
{
  auto  f             = booted();
  auto& m             = model(f);
  m.double_rate_on_go = true;
  m.time_delay_ps     = 25'000'000;
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const slice = f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{1000}});
  REQUIRE(slice.stopped.has_value());
  CHECK(f.target->virtual_clock().core_hz() == 8'000'000);
  REQUIRE(m.go_time.has_value());
  REQUIRE(m.fired_time.has_value());
  auto const due = m.go_time->ps + *m.time_delay_ps;
  CHECK(m.fired_time->ps >= due);
  CHECK(m.fired_time->ps <= due + 2 * 125'000);
}

TEST_CASE("a core slow-down in an MMIO write ends the engine run so until_time stays exact")
{
  auto  f            = booted();
  auto& m            = model(f);
  m.halve_rate_on_go = true;
  m.time_delay_ps    = 1'000'000'000'000;
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const start  = f.target->virtual_clock().time();
  auto const target = popcorn::vtime{start.ps + 1'000'000'000};
  auto const slice  = f.run(
      {.max_instructions = icount{100'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until_time       = target});
  REQUIRE(slice.stopped.has_value());
  CHECK(slice.stopped->reason == stop_reason::time);
  CHECK_FALSE(m.fired_time.has_value());
  CHECK(slice.stopped->time.ps >= target.ps);
  CHECK(slice.stopped->time.ps < target.ps + 2 * 500'000);
}

TEST_CASE("an instruction-keyed event keeps its count across a core rate change")
{
  auto  f            = booted();
  auto& m            = model(f);
  m.halve_rate_on_go = true;
  m.accesses.clear();
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const slice = f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{1000}});
  REQUIRE(slice.stopped.has_value());
  REQUIRE(m.fired_at.has_value());
  auto const due = m.accesses.front().ctx.now.value + test_model::delay;
  CHECK(m.fired_at->value >= due);
  CHECK(m.fired_at->value <= due + 2);
}

TEST_CASE("a time-keyed event follows a core rate change the host makes between slices")
{
  auto  f         = booted();
  auto& m         = model(f);
  m.time_delay_ps = 250'000'000;
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const first = f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{10}});
  REQUIRE(first.stopped.has_value());
  REQUIRE(m.go_time.has_value());
  REQUIRE_FALSE(m.fired_time.has_value());
  m.context().clock.set_core_hz(16'000'000);
  auto const rest = f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{10'000}});
  REQUIRE(rest.stopped.has_value());
  REQUIRE(m.fired_time.has_value());
  auto const due = m.go_time->ps + *m.time_delay_ps;
  CHECK(m.fired_time->ps >= due);
  CHECK(m.fired_time->ps <= due + 2 * 62'500);
}

TEST_CASE("a restore replays a time-keyed event across a rate change at the same count")
{
  auto  f            = booted();
  auto& m            = model(f);
  m.halve_rate_on_go = true;
  m.time_delay_ps    = 25'000'000;
  enter(f, "rate_write");
  f.set("primask", 1);
  auto const saved = f.target->snapshot();
  auto const fire  = [&] {
    m.fired_at.reset();
    auto const slice =
        f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{1000}});
    REQUIRE(slice.stopped.has_value());
    REQUIRE(m.fired_at.has_value());
    return *m.fired_at;
  };
  auto const first = fire();
  f.target->restore(saved);
  CHECK(fire() == first);
}
