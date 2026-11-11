#pragma once

#include "row_of.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the LPTIM peripherals the model rows serve. */
struct lptim_l4_variant
{
  /** @brief The instance has the encoder mode: CFGR.ENC, ISR.UP and DOWN, IER.UPIE and DOWNIE,
   *         ICR.UPCF and DOWNCF. */
  bool encoder = true;
  /** @brief The instance has the option register `OR` with OR_0 and OR_1. */
  bool option_register = true;
  /** @brief Kernel clock cycles from a CR.CNTSTRT or SNGSTRT write to the counter start. */
  std::uint8_t start_cycles = 3;
  /** @brief Kernel clock cycles from a CMP or ARR write to its CMPOK or ARROK flag, the write
   *         taking effect then. */
  std::uint8_t ok_cycles = 3;
  /** @brief Kernel clock cycles from the counter reaching CMP or ARR to its CMPM or ARRM flag. */
  std::uint8_t event_delay_cycles = 0;
};

/** @brief STM32L4 LPTIM1, RM0351 section 34, with the ES0250 2.18.3 delay of one kernel clock
 *         on CMPM and ARRM. CMPOK and ARROK after 3 kernel clocks, inferred. */
inline constexpr lptim_l4_variant l4_lptim1{.event_delay_cycles = 1};
/** @brief STM32L4 LPTIM2: LPTIM1 without the encoder mode, RM0351 34.3 Table 216. */
inline constexpr lptim_l4_variant l4_lptim2{.encoder = false, .event_delay_cycles = 1};

/**
 * @brief Factory of the LPTIM model, RM0351 section 34.
 * @param variant What differs between the LPTIM instances the row serves.
 */
[[nodiscard]] auto lptim_l4_factory(lptim_l4_variant variant) -> model_factory;

/** @brief The LPTIM model rows. */
[[nodiscard]] auto lptim_l4_rows() -> std::span<builtin const>;
}
