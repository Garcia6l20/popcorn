#pragma once

#include "row_of.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the LPTIM peripherals of the STM32U5 and STM32H5 IP the model rows
 *         serve, RM0456 58.3 Table 599. */
struct lptim_v2_variant
{
  /** @brief Name of the `LPTIMx_ISR` view the model claims, as the description spells it. */
  std::string_view status = "ISR_output";
  /** @brief Name of the `LPTIMx_ICR` view the model claims. */
  std::string_view clear = "ICR_output";
  /** @brief Name of the `LPTIMx_DIER` view the model claims. */
  std::string_view interrupts = "DIER_output";
  /** @brief Capture/compare channels: 2, or 0 on a reduced instance, which has no CCMR1 and
   *         CCR2. */
  std::uint8_t channels = 2;
  /** @brief The instance has the encoder mode, CFGR.ENC. */
  bool encoder = true;
  /** @brief The instance has the input capture mode, CCMR1.CC1SEL and CC2SEL. */
  bool capture = true;
};

/** @brief STM32U5 LPTIM1 and LPTIM2, RM0456 58.3 Table 599: encoder, input capture, two
 *         channels. */
inline constexpr lptim_v2_variant u5_lptim_full{};
/** @brief STM32U5 LPTIM3: LPTIM1 without the encoder mode, RM0456 58.3 Table 599. */
inline constexpr lptim_v2_variant u5_lptim3{.encoder = false};
/** @brief STM32U5 LPTIM4, RM0456 58.3 Table 599 and 58.7.1: no channel, no encoder, no input
 *         capture, the `ISR`, `ICR` and `DIER` layout of 58.7.1, 58.7.4 and 58.7.7. */
inline constexpr lptim_v2_variant u5_lptim4{
    .status     = "ISR",
    .clear      = "ICR",
    .interrupts = "DIER",
    .channels   = 0,
    .encoder    = false,
    .capture    = false};

/**
 * @brief Factory of the LPTIM model of the STM32U5 and STM32H5 IP, RM0456 section 58.
 * @param variant What differs between the LPTIM instances the row serves.
 */
[[nodiscard]] auto lptim_v2_factory(lptim_v2_variant variant) -> model_factory;

/** @brief The LPTIM model rows of the STM32U5 and STM32H5 IP. */
[[nodiscard]] auto lptim_v2_rows() -> std::span<builtin const>;
}
