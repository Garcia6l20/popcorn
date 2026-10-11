#include "../../../native/src/core/machine_impl.hpp"
#include "stm32_fixture.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string_view>
#include <tuple>

namespace
{
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::core::engine_end;
using popcorn::core::machine_internals;
using popcorn::core::run_counters;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::uint64_t ms_ps = 1'000'000'000;

auto options() -> popcorn::core::machine_options
{
  auto out           = popcorn::testing::stm32_options();
  out.spin_threshold = 0;
  return out;
}

struct irq_machine : stm32_fixture
{
  std::uint64_t origin{};

  irq_machine(std::string_view entry, std::uint32_t period, std::uint32_t stores)
      : stm32_fixture{popcorn::testing::u575(), "irq_ll", options()}
  {
    auto const booted = run_to_stop({.max_instructions = icount{100'000'000}, .until = at("main")});
    REQUIRE(booted.reason == stop_reason::address);
    REQUIRE(target->mem().write(at("irq_period_cycles"), 4, period));
    REQUIRE(target->mem().write(at("irq_stores"), 4, stores));
    set("pc", at(entry));
    origin = target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto at(std::string_view name) const -> address
  {
    return sym(firmware_name, name);
  }

  [[nodiscard]] auto word(std::string_view name) const -> std::uint32_t
  {
    auto const value = target->mem().read(at(name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void run_to(std::uint64_t ps) const
  {
    for(;;)
    {
      auto const result = run(
          {.max_instructions = icount{1'000'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{origin + ps}});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == stop_reason::time);
        return;
      }
    }
  }

  [[nodiscard]] auto counters() const -> run_counters
  {
    return machine_internals::counters(*target);
  }
};

auto ended(run_counters const& from, run_counters const& to, engine_end why) -> std::uint64_t
{
  return to.ended(why) - from.ended(why);
}

auto ends_sum(run_counters const& counters) -> std::uint64_t
{
  return std::accumulate(counters.ends.begin(), counters.ends.end(), std::uint64_t{0});
}
}

TEST_CASE("every TIM6 interrupt entry is counted once")
{
  irq_machine m{"irq_wfi", 1600, 0};
  m.run_to(2 * ms_ps + ms_ps / 3);
  auto const before = m.counters();
  auto const from   = m.word("irq_count");
  m.run_to(3 * ms_ps + ms_ps / 3);
  auto const after   = m.counters();
  auto const entries = m.word("irq_count") - from;
  CHECK(entries == 100);
  CHECK(after.entries - before.entries == entries);
  CHECK(after.removes_direct - before.removes_direct == entries);
  CHECK(after.remove_calls_direct - before.remove_calls_direct == 4 * entries);
  CHECK(ended(before, after, engine_end::exit) == entries);
  CHECK(ends_sum(after) == after.engine_runs);
}

TEST_CASE("a spin that only loads hooks no store and stops the engine only for its bound")
{
  irq_machine m{"irq_rate", 0, 0};
  m.run_to(2 * ms_ps);
  auto const before = m.counters();
  m.run_to(3 * ms_ps);
  auto const after = m.counters();
  CHECK(after.guest_stores_hooked == before.guest_stores_hooked);
  CHECK(after.removes_in_engine == before.removes_in_engine);
  CHECK(after.removes_direct == before.removes_direct);
  CHECK(after.entries == before.entries);
  CHECK(ended(before, after, engine_end::store) == 0);
  CHECK(ends_sum(after) == after.engine_runs);
}

TEST_CASE("each hooked SRAM store in a spin stops the engine and is removed by a drain")
{
  irq_machine m{"spin_store", 0, 0};
  m.run_to(2 * ms_ps);
  auto const before     = m.counters();
  auto const spins_from = m.word("spins");
  m.run_to(2 * ms_ps + ms_ps / 10);
  auto const after        = m.counters();
  auto const spins        = m.word("spins") - spins_from;
  auto const stores       = after.guest_stores_hooked - before.guest_stores_hooked;
  auto const removes      = after.removes_in_engine - before.removes_in_engine;
  auto const remove_calls = after.remove_calls_in_engine - before.remove_calls_in_engine;
  CHECK(spins >= 1000);
  CHECK(stores == spins);
  CHECK(removes == spins);
  CHECK(remove_calls == 4 * removes);
  CHECK(ended(before, after, engine_end::store) == spins);
  CHECK(ends_sum(after) == after.engine_runs);
}

TEST_CASE("an interrupt unmasked inside the engine ends the run at the next block as deliverable")
{
  irq_machine m{"irq_unmask", 1600, 0};
  m.run_to(2 * ms_ps);
  auto const before = m.counters();
  auto const from   = m.word("irq_count");
  m.run_to(3 * ms_ps);
  auto const after   = m.counters();
  auto const entries = m.word("irq_count") - from;
  CHECK(entries == 100);
  CHECK(ended(before, after, engine_end::interrupt) == entries);
  CHECK(ended(before, after, engine_end::wall) == 0);
}

TEST_CASE("TIM6 events end engine runs as events")
{
  irq_machine m{"irq_rate", 1600, 0};
  m.run_to(2 * ms_ps);
  auto const before = m.counters();
  auto const from   = m.word("irq_count");
  m.run_to(3 * ms_ps);
  auto const after   = m.counters();
  auto const entries = m.word("irq_count") - from;
  CHECK(entries == 100);
  CHECK(after.entries - before.entries == entries);
  CHECK(ended(before, after, engine_end::event) >= entries);
  CHECK(ended(before, after, engine_end::wall) == 0);
  CHECK(ended(before, after, engine_end::other) == 0);
  CHECK(after.engine_runs - before.engine_runs == ends_sum(after) - ends_sum(before));
}

TEST_CASE(
    "enabling a USART TXE interrupt ends engine runs as precise exits and deliverable entries")
{
  irq_machine m{"uart_tx", 0, 0};
  m.run_to(8 * ms_ps);
  auto const before = m.counters();
  m.run_to(168 * ms_ps);
  auto const after = m.counters();
  CHECK(ended(before, after, engine_end::precise) == 20);
  CHECK(ended(before, after, engine_end::interrupt) == 20);
  CHECK(ended(before, after, engine_end::request) == 0);
  CHECK(ended(before, after, engine_end::exit) == 0);
  CHECK(ended(before, after, engine_end::other) == 0);
  CHECK(after.entries - before.entries == 10 * 2 * 512 + 160);
  CHECK(ends_sum(after) == after.engine_runs);
}

TEST_CASE("a zero wall budget ends the engine run as wall")
{
  irq_machine m{"irq_rate", 0, 0};
  m.run_to(2 * ms_ps);
  auto const before = m.counters();
  auto const result = m.run(
      {.max_instructions = icount{1'000'000'000},
       .wall_budget      = std::chrono::seconds{0},
       .until_time       = vtime{m.origin + 100 * ms_ps}});
  auto const after = m.counters();
  CHECK_FALSE(result.stopped.has_value());
  CHECK(after.engine_runs - before.engine_runs == 1);
  CHECK(ended(before, after, engine_end::wall) == 1);
}

TEST_CASE("an interrupt() inside the engine ends the run as a request, at a block and stepping")
{
  for(bool const stepping : {false, true})
  {
    CAPTURE(stepping);
    irq_machine m{"irq_rate", 0, 0};
    m.run_to(2 * ms_ps);
    auto const spin = static_cast<address>(m.target->regs().read(*m.target->regs().find("pc")));
    auto const hook = m.target->add_intercept(
        spin,
        [&m](auto const&) {
          m.target->interrupt();
          return popcorn::core::intercept_result::proceed();
        },
        popcorn::core::intercept_mode::once);
    auto const before = m.counters();
    auto const result = m.run(
        {.max_instructions   = icount{1'000'000'000},
         .wall_budget        = std::chrono::seconds{60},
         .exact_instructions = stepping ? std::optional{icount{5}} : std::nullopt});
    auto const after = m.counters();
    CHECK_FALSE(result.stopped.has_value());
    CHECK(after.engine_runs - before.engine_runs == 1);
    CHECK(ended(before, after, engine_end::request) == 1);
    CHECK(ended(before, after, engine_end::store) == 0);
  }
}

TEST_CASE("a stepped slice ends its last engine run at the deadline as an event")
{
  irq_machine m{"irq_rate", 0, 0};
  m.run_to(2 * ms_ps);
  auto const before = m.counters();
  auto const result = m.run(
      {.max_instructions   = icount{1'000'000'000},
       .wall_budget        = std::chrono::seconds{60},
       .exact_instructions = icount{5}});
  auto const after = m.counters();
  CHECK(result.executed == icount{5});
  CHECK(ended(before, after, engine_end::event) >= 1);
  CHECK(ended(before, after, engine_end::exit) == 0);
  CHECK(ended(before, after, engine_end::request) == 0);
  CHECK(after.engine_runs - before.engine_runs == ends_sum(after) - ends_sum(before));
}

TEST_CASE("a stopping intercept and an engine fault end the run as other")
{
  irq_machine m{"irq_wfi", 1600, 0};
  auto const  hook    = m.target->add_intercept(m.at("TIM6_IRQHandler"), [](auto const&) {
    return popcorn::core::intercept_result::stop(stop_reason::user);
  });
  auto const  before  = m.counters();
  auto const  stopped = m.run_to_stop({.max_instructions = icount{1'000'000'000}});
  auto const  after   = m.counters();
  CHECK(stopped.reason == stop_reason::user);
  CHECK(ended(before, after, engine_end::other) == 1);

  irq_machine faulting{"irq_rate", 0, 0};
  faulting.set("pc", 0x5fff'0000);
  auto const from  = faulting.counters();
  auto const fault = faulting.run_to_stop({.max_instructions = icount{1'000'000}});
  auto const to    = faulting.counters();
  CHECK(fault.reason == stop_reason::fault);
  CHECK(to.engine_runs - from.engine_runs == 1);
  CHECK(ended(from, to, engine_end::other) == 1);

  irq_machine       refused{"irq_rate", 0, 0};
  constexpr address code_at = 0x2003'f000;
  REQUIRE(refused.target->mem().write(code_at, 4, 0xbf00'6808u));
  refused.set("r1", 0x1000'0000);
  refused.set("pc", code_at);
  auto const ahead    = refused.counters();
  auto const unmapped = refused.run_to_stop({.max_instructions = icount{1'000'000}});
  auto const behind   = refused.counters();
  CHECK(unmapped.reason == stop_reason::fault);
  CHECK(behind.engine_runs - ahead.engine_runs == 1);
  CHECK(ended(ahead, behind, engine_end::other) == 1);
}
