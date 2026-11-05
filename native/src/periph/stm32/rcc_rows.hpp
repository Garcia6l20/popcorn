#pragma once

#include <popcorn/periph/stm32/rcc.hpp>

#include "row_of.hpp"

#include <span>

namespace popcorn::periph::stm32
{
/**
 * @brief Factory of the RCC model, RM0456 section 11.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto rcc_factory(rcc_options options) -> model_factory;

/**
 * @brief Factory of the STM32H5 RCC model, RM0481 section 11.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto rcc_h5_factory(rcc_options options) -> model_factory;

/**
 * @brief Factory of the STM32L1 RCC model, RM0038 section 6.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto rcc_l1_factory(rcc_options options) -> model_factory;

/**
 * @brief Factory of the STM32L47x/L48x RCC model, RM0351 section 6.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto rcc_l4_factory(rcc_options options) -> model_factory;

/** @brief The RCC model rows. */
[[nodiscard]] auto rcc_rows() -> std::span<builtin const>;
}
