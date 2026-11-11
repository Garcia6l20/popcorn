#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace popcorn::periph::stm32
{
/** @brief Differences between the ADC4 units the model rows serve, `aditf4_v4_U5_Cube`. */
struct adc4_variant
{
  /** @brief ADC_CCR.PRESC dividers of the codes 0000 to 1011, RM0456 34.7.18. The codes above
   *         are reserved. */
  std::array<std::uint16_t, 12> prescalers{1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256};
  /** @brief tADCVREG_STUP, the LDO startup ready flag time, in nanoseconds, the maximum of
   *         DS13737 Table 106 and DS13086 Table 108. */
  std::uint64_t regulator_setup_ns = 25'000;
  /** @brief tOFF_CAL, the offset calibration time, in ADC clock cycles, the typical value of
   *         DS13737 Table 106 and DS13086 Table 108. */
  std::uint32_t calibration_cycles = 123;
  /** @brief tSTAB in ADC clock cycles, 3 cycles and one conversion of DS13737 Table 106 and
   *         DS13086 Table 108, the conversion taken as 12 bits at SMP 000, 1.5 + 12.5 cycles. */
  std::uint32_t stabilisation_cycles = 17;
  /** @brief ADC clock cycles from the ADDIS write to ADEN and ADDIS clearing, ES0499 2.9.2. */
  std::uint32_t disable_cycles = 6;
  /** @brief ADC clock cycles after ADCAL clears during which an ADEN write is lost, RM0456
   *         34.4.5. */
  std::uint32_t enable_lockout_cycles = 4;
  /** @brief Lowest ADC clock in Hz, fADC of DS13737 Table 106 and DS13086 Table 108. */
  std::uint64_t min_hz = 140'000;
  /** @brief Highest ADC clock in Hz, fADC of DS13737 Table 106 and DS13086 Table 108. */
  std::uint64_t max_hz = 55'000'000;
};

/** @brief The ADC4 of the STM32U575 and STM32U585, RM0456 section 34. */
inline constexpr adc4_variant u5_adc4{};

/**
 * @brief Factory of the ADC4 model, RM0456 section 34.
 * @param variant What differs between the ADC4 units the row serves.
 */
[[nodiscard]] auto adc4_factory(adc4_variant variant) -> model_factory;

/** @brief The ADC4 model rows. */
[[nodiscard]] auto adc4_rows() -> std::span<builtin const>;
}
