#include "exceptions.hpp"
#include "frame.hpp"
#include "systick.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace
{
namespace armv8m = popcorn::arch::armv8m;

auto exceptions_with(std::uint32_t irqs) -> armv8m::exception_state
{
  auto const count = 16 + irqs;
  auto const words = (count + 31) / 32;
  return {
      .priority = std::vector<std::uint8_t>(count, 0),
      .enabled  = std::vector<std::uint32_t>(words, 0),
      .pending  = std::vector<std::uint32_t>(words, 0),
      .active   = std::vector<std::uint32_t>(words, 0),
      .level    = std::vector<std::uint32_t>(words, 0),
  };
}
}

TEST_CASE("group_mask keeps the bits above PRIGROUP")
{
  CHECK(armv8m::group_mask(0) == 0xFE);
  CHECK(armv8m::group_mask(4) == 0xE0);
  CHECK(armv8m::group_mask(6) == 0x80);
  CHECK(armv8m::group_mask(7) == 0x00);
}

TEST_CASE("execution_priority follows the active set and the masks")
{
  auto state = exceptions_with(8);
  CHECK(armv8m::execution_priority(state, 0, {}) == 256);
  CHECK(armv8m::execution_priority(state, 0, {.basepri = 0x40}) == 0x40);
  CHECK(armv8m::execution_priority(state, 5, {.basepri = 0x50}) == 0x40);
  CHECK(armv8m::execution_priority(state, 0, {.primask = true}) == 0);
  CHECK(armv8m::execution_priority(state, 0, {.faultmask = true}) == -1);
  armv8m::put_bit(state.active, armv8m::exc_hardfault, true);
  CHECK(armv8m::execution_priority(state, 0, {}) == -1);
}

TEST_CASE("best_pending takes the lowest priority value, then the lowest number")
{
  auto state = exceptions_with(8);
  CHECK_FALSE(armv8m::best_pending(state).has_value());
  for(std::uint32_t number = 16; number < 24; ++number)
  {
    armv8m::put_bit(state.enabled, number, true);
  }
  state.priority[18] = 0x40;
  state.priority[17] = 0x40;
  armv8m::put_bit(state.pending, 18, true);
  armv8m::put_bit(state.pending, 17, true);
  CHECK(armv8m::best_pending(state) == 17u);
  state.priority[18] = 0x20;
  CHECK(armv8m::best_pending(state) == 18u);
  armv8m::put_bit(state.enabled, 18, false);
  CHECK(armv8m::best_pending(state) == 17u);
}

TEST_CASE("EXC_RETURN encodes the six entry cases and decodes Non-secure values")
{
  using armv8m::encode_exc_return;
  CHECK(
      encode_exc_return({.thread = true, .process_stack = false, .basic_frame = true})
      == 0xFFFFFFF9);
  CHECK(
      encode_exc_return({.thread = true, .process_stack = true, .basic_frame = true})
      == 0xFFFFFFFD);
  CHECK(
      encode_exc_return({.thread = false, .process_stack = false, .basic_frame = true})
      == 0xFFFFFFF1);
  CHECK(
      encode_exc_return({.thread = true, .process_stack = false, .basic_frame = false})
      == 0xFFFFFFE9);
  CHECK(
      encode_exc_return({.thread = true, .process_stack = true, .basic_frame = false})
      == 0xFFFFFFED);
  CHECK(
      encode_exc_return({.thread = false, .process_stack = false, .basic_frame = false})
      == 0xFFFFFFE1);
  CHECK(
      armv8m::decode_exc_return(0xFFFFFFBC)
      == armv8m::exc_return{.thread = true, .process_stack = true, .basic_frame = true});
  CHECK(armv8m::valid_exc_return(0xFFFFFFBC));
  CHECK_FALSE(armv8m::valid_exc_return(0xFFFFFFF5));
}

TEST_CASE("SysTick crossings, values and expiries follow the reload and anchor value")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{
      .csr = systick::enable | systick::tickint | systick::clksource, .rvr = 3999};
  CHECK(systick::crossing(s, 1) == 4000u);
  CHECK(systick::crossing(s, 2) == 8000u);
  CHECK(systick::crossing(s, 3) == 12000u);
  CHECK(systick::value_at(s, 4001) == 3999u);
  CHECK(systick::value_at(s, 4000) == 0u);
  CHECK(systick::value_at(s, 1) == 3999u);
  s.anchor_value = 10;
  CHECK(systick::crossing(s, 1) == 10u);
  CHECK(systick::crossing(s, 2) == 4010u);
  CHECK(systick::crossing(s, 3) == 8010u);
  CHECK(systick::value_at(s, 4) == 6u);
  s.csr          = systick::enable | systick::clksource;
  s.anchor_value = 25;
  CHECK(systick::value_at(s, 0) == 25u);
  s.csr = 0;
  CHECK(systick::value_at(s, 9999) == 25u);
}

TEST_CASE("SysTick with RVR 0 stops at 0 after its first crossing")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{
      .csr = systick::enable | systick::tickint | systick::clksource, .rvr = 0, .anchor_value = 5};
  CHECK(systick::crossing(s, 1) == 5u);
  CHECK_FALSE(systick::crossing(s, 2).has_value());
  CHECK(systick::value_at(s, 5) == 0u);
  CHECK(systick::value_at(s, 500) == 0u);
  CHECK(systick::crossings_through(s, 500) == 1u);
  s.anchor_value = 0;
  CHECK_FALSE(systick::crossing(s, 1).has_value());
}

TEST_CASE("SysTick with a latched reload counts it from tick 1, then RVR from the next wrap")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{
      .csr = systick::enable | systick::tickint | systick::clksource, .rvr = 3999, .reload = 999};
  CHECK(systick::start_tick(s) == 1u);
  CHECK(systick::start_value(s) == 999u);
  CHECK(systick::value_at(s, 0) == 999u);
  CHECK(systick::value_at(s, 1) == 999u);
  CHECK(systick::value_at(s, 2) == 998u);
  CHECK(systick::value_at(s, 1000) == 0u);
  CHECK(systick::value_at(s, 1001) == 3999u);
  CHECK(systick::value_at(s, 5000) == 0u);
  CHECK(systick::value_at(s, 5001) == 3999u);
  CHECK(systick::crossing(s, 1) == 1000u);
  CHECK(systick::crossing(s, 2) == 5000u);
  CHECK(systick::crossing(s, 3) == 9000u);
  CHECK(systick::crossings_through(s, 999) == 0u);
  CHECK(systick::crossings_through(s, 1000) == 1u);
  CHECK(systick::crossings_through(s, 4999) == 1u);
  CHECK(systick::crossings_through(s, 5000) == 2u);

  s.reload = 0;
  CHECK(systick::value_at(s, 0) == 0u);
  CHECK(systick::value_at(s, 1) == 0u);
  CHECK(systick::value_at(s, 2) == 3999u);
  CHECK(systick::crossing(s, 1) == 4001u);
  CHECK(systick::crossing(s, 2) == 8001u);
  CHECK(systick::crossings_through(s, 4000) == 0u);
  CHECK(systick::crossings_through(s, 4001) == 1u);

  s.rvr = 0;
  CHECK_FALSE(systick::crossing(s, 1).has_value());
  CHECK(systick::crossings_through(s, 10'000) == 0u);
  CHECK(systick::value_at(s, 10'000) == 0u);
  s.reload = 5;
  CHECK(systick::crossing(s, 1) == 6u);
  CHECK_FALSE(systick::crossing(s, 2).has_value());
  CHECK(systick::value_at(s, 6) == 0u);
  CHECK(systick::value_at(s, 100) == 0u);
  CHECK(systick::crossings_through(s, 5) == 0u);
  CHECK(systick::crossings_through(s, 100) == 1u);

  s.csr = 0;
  CHECK(systick::value_at(s, 3) == 0u);
}

TEST_CASE("a SysTick reload edge counts as passed at tick 0 only on the processor clock")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{.csr = systick::enable | systick::clksource, .rvr = 3999};
  CHECK(systick::reload_passed(s, 0));
  CHECK_FALSE(systick::reload_passed(s, 1));
  s.anchor_value = 1;
  CHECK_FALSE(systick::reload_passed(s, 0));
  s.anchor_value = 0;
  s.reload       = 999;
  CHECK_FALSE(systick::reload_passed(s, 0));
  s.reload = std::nullopt;
  s.csr    = systick::enable;
  CHECK_FALSE(systick::reload_passed(s, 0));
  s.csr = systick::clksource;
  CHECK_FALSE(systick::reload_passed(s, 0));
}

TEST_CASE("SysTick ticks and expiries at a cpi of 1, 2 and 1.5 and the external divisor")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{
      .csr = systick::enable | systick::tickint | systick::clksource, .rvr = 3999, .anchor = 100};
  CHECK(systick::expiry(s, 4000, 1.0) == 4100u);
  CHECK(systick::expiry(s, 4000, 2.0) == 2100u);
  CHECK(systick::ticks(s, 2100, 2.0) == 4000u);
  for(std::uint64_t k = 1; k <= 1000; ++k)
  {
    auto const z = *systick::crossing(s, k);
    auto const n = systick::expiry(s, z, 1.5);
    CAPTURE(k);
    CHECK(systick::ticks(s, n, 1.5) >= z);
    CHECK(systick::ticks(s, n - 1, 1.5) < z);
  }
  s.csr = systick::enable | systick::tickint;
  CHECK(systick::expiry(s, 4000, 1.0) == 100u + 32000u);
  CHECK(systick::ticks(s, 100 + 32007, 1.0) == 4000u);
}

TEST_CASE("SysTick COUNTFLAG and missed crossings coalesce")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{
      .csr = systick::enable | systick::tickint | systick::clksource, .rvr = 99, .fired = 3};
  CHECK(systick::crossings_through(s, 950) == 9u);
  CHECK(*systick::crossing(s, 10) == 1000u);
  CHECK_FALSE(systick::countflag(s, 99));
  CHECK(systick::countflag(s, 100));
  s.flag_since = 150;
  CHECK_FALSE(systick::countflag(s, 199));
  CHECK(systick::countflag(s, 200));
  s.flag = true;
  CHECK(systick::countflag(s, 150));
}

TEST_CASE("valid_v7_exc_return accepts the six ARMv7-M values only")
{
  for(std::uint32_t const value :
      {0xFFFF'FFE1u, 0xFFFF'FFE9u, 0xFFFF'FFEDu, 0xFFFF'FFF1u, 0xFFFF'FFF9u, 0xFFFF'FFFDu})
  {
    CAPTURE(value);
    CHECK(armv8m::valid_v7_exc_return(value));
  }
  for(std::uint32_t const value : {0xFFFF'FFBCu, 0xFFFF'FFE5u, 0xFFFF'FFF5u})
  {
    CAPTURE(value);
    CHECK_FALSE(armv8m::valid_v7_exc_return(value));
  }
  CHECK(armv8m::valid_exc_return(0xFFFF'FFBC));
}
