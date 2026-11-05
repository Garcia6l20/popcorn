#pragma once

#include <popcorn/periph/stm32/pwr.hpp>

#include "row_of.hpp"

#include <span>

namespace popcorn::periph::stm32
{
/**
 * @brief Factory of the PWR model, RM0456 section 10.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto pwr_factory(pwr_options options) -> model_factory;

/**
 * @brief Factory of the PWR model of the STM32U5 packages with the SMPS, RM0456 section 10.5:
 *        PWR_SVMSR.REGS follows PWR_CR3.REGSEL.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto pwr_smps_factory(pwr_options options) -> model_factory;

/**
 * @brief Factory of the STM32H5 PWR model, RM0481 section 10.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto pwr_h5_factory(pwr_options options) -> model_factory;

/**
 * @brief Factory of the STM32L1 PWR model, RM0038 section 5.4.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto pwr_l1_factory(pwr_options options) -> model_factory;

/**
 * @brief Factory of the STM32L47x/L48x PWR model, RM0351 section 5.4.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto pwr_l4_factory(pwr_options options) -> model_factory;

/** @brief The PWR model rows. */
[[nodiscard]] auto pwr_rows() -> std::span<builtin const>;
}
