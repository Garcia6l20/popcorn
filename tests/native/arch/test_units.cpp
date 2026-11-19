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

TEST_CASE("SysTick ticks and expiries at a cpi of 1, 2 and 1.5 on the processor clock")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{
      .csr = systick::enable | systick::tickint | systick::clksource, .rvr = 3999, .anchor = 100};
  CHECK(systick::expiry(s, 4000, 1.0, 0) == 4100u);
  CHECK(systick::expiry(s, 4000, 2.0, 0) == 2100u);
  CHECK(systick::ticks(s, 2100, 2.0, 0) == 4000u);
  CHECK(systick::expiry(s, 4000, 1.0, 37) == 4100u);
  for(std::uint64_t k = 1; k <= 1000; ++k)
  {
    auto const z = *systick::crossing(s, k);
    auto const n = systick::expiry(s, z, 1.5, 0);
    CAPTURE(k);
    CHECK(systick::ticks(s, n, 1.5, 0) >= z);
    CHECK(systick::ticks(s, n - 1, 1.5, 0) < z);
  }
}

TEST_CASE(
    "SysTick on the external reference counts the edges of a divider free-running from the reset, "
    "inferred offset 0")
{
  namespace systick = armv8m::systick;
  armv8m::systick_state s{.csr = systick::enable | systick::tickint, .rvr = 3999, .anchor = 100};
  CHECK(systick::expiry(s, 4000, 1.0, 0) == 32096u);
  CHECK(systick::ticks(s, 32095, 1.0, 0) == 3999u);
  CHECK(systick::ticks(s, 32096, 1.0, 0) == 4000u);
  for(std::uint64_t delay = 0; delay < 8; ++delay)
  {
    s.anchor = 96 + delay;
    CAPTURE(delay);
    CHECK(systick::expiry(s, 1, 1.0, 0) == 104u);
    CHECK(systick::ticks(s, 103, 1.0, 0) == 0u);
    CHECK(systick::ticks(s, 104, 1.0, 0) == 1u);
    CHECK(systick::ticks(s, 111, 1.0, 0) == 1u);
    CHECK(systick::ticks(s, 112, 1.0, 0) == 2u);
  }
  s.anchor = 104;
  CHECK(systick::expiry(s, 1, 1.0, 0) == 112u);
  s.anchor = 100;
  CHECK(systick::expiry(s, 1, 1.0, 5) == 101u);
  CHECK(systick::ticks(s, 100, 1.0, 5) == 0u);
  CHECK(systick::ticks(s, 101, 1.0, 5) == 1u);
  CHECK(systick::expiry(s, 2, 1.0, 5) == 109u);
}

TEST_CASE("SysTick expiry on the external reference is exact and bounded far from the origin")
{
  namespace systick = armv8m::systick;
  for(auto const cpi : {1.0, 2.0, 1.5})
  {
    for(std::uint64_t const anchor : {5'000'000'000u, 1'000'000'000'000u})
    {
      for(std::uint64_t const origin : {0u, 3u})
      {
        armv8m::systick_state s{.csr = systick::enable, .rvr = 3999, .anchor = anchor + 5};
        for(std::uint64_t const z : {1u, 2u, 4000u})
        {
          CAPTURE(cpi);
          CAPTURE(anchor);
          CAPTURE(origin);
          CAPTURE(z);
          auto const n = systick::expiry(s, z, cpi, origin);
          CHECK(systick::ticks(s, n, cpi, origin) >= z);
          CHECK(systick::ticks(s, n - 1, cpi, origin) < z);
          CHECK(n - s.anchor <= static_cast<std::uint64_t>(z * 8 / cpi) + 8);
        }
      }
    }
  }
}

TEST_CASE("SysTick on the external reference at a cpi of 2 and 1.5 is exact at every edge")
{
  namespace systick = armv8m::systick;
  for(auto const cpi : {2.0, 1.5})
  {
    for(std::uint64_t const origin : {0u, 3u, 1000u})
    {
      armv8m::systick_state s{.csr = systick::enable, .rvr = 3999, .anchor = origin + 777};
      for(std::uint64_t z = 1; z <= 400; ++z)
      {
        auto const n = systick::expiry(s, z, cpi, origin);
        CAPTURE(cpi);
        CAPTURE(origin);
        CAPTURE(z);
        CHECK(systick::ticks(s, n, cpi, origin) >= z);
        CHECK(systick::ticks(s, n - 1, cpi, origin) < z);
      }
    }
  }
  armv8m::systick_state s{.csr = systick::enable, .rvr = 3999, .anchor = 800};
  CHECK(systick::expiry(s, 1, 2.0, 0) == 804u);
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
