#include "exceptions.hpp"
#include "frame.hpp"

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
