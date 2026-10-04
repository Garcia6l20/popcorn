#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/** @brief Lowest VDDA with the ADC on in millivolts, STM32U575 datasheet table 103. */
inline constexpr std::uint32_t vdda_min_mv = 1620;
/** @brief Highest VDDA with the ADC on in millivolts, STM32U575 datasheet table 103. */
inline constexpr std::uint32_t vdda_max_mv = 3600;

/**
 * @brief Supply voltages of the board. VDDA is one pin feeding ADC1, ADC4, DAC and COMP, so it is
 *        board-wide, not per ADC instance.
 */
struct supply_options
{
  /** @brief VDDA in millivolts, also VREF+: the ADC reference with VREFBUF off. 3000 is the
   * voltage the factory calibration words were taken at. */
  std::uint32_t vdda_mv = 3000;
};
}
