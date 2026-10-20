#pragma once

#include <algorithm>
#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief Integrators of the popcorn-defined 2nd-order single-bit sigma-delta modulator that
 *        turns test PCM into a PDM bitstream. Zero is the reset state.
 */
struct modulator_state
{
  std::int64_t i1{};
  std::int64_t i2{};

  friend auto operator==(modulator_state const&, modulator_state const&) -> bool = default;
};

/**
 * @brief Produces the next PDM bit for a 16-bit PCM input held at the input. Integer
 *        arithmetic, no dither, feedback +-2^15, integrators clamped at +-2^20 and +-2^22.
 * @param state Modulator integrators.
 * @param pcm16 Input sample, -32768 to 32767.
 * @return the bit, true for 1.
 */
[[nodiscard]] constexpr auto modulate(modulator_state& state, std::int32_t pcm16) -> bool
{
  constexpr std::int64_t full     = std::int64_t{1} << 15;
  constexpr std::int64_t i1_limit = std::int64_t{1} << 20;
  constexpr std::int64_t i2_limit = std::int64_t{1} << 22;
  auto const             bit      = state.i2 >= 0;
  auto const             feedback = bit ? full : -full;
  state.i2 = std::clamp(state.i2 + state.i1 - 2 * feedback, -i2_limit, i2_limit);
  state.i1 = std::clamp(state.i1 + pcm16 - feedback, -i1_limit, i1_limit);
  return bit;
}
}
