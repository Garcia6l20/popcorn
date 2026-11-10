#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <span>

namespace popcorn::periph::stm32
{
/**
 * @brief Factory of the STM32U5 LPGPIO model, RM0456 section 14. The pad of each LPGPIO pin is the
 *        GPIO pin whose description alternates carry the signal `lpgpio1_p<y>` on AF11.
 */
[[nodiscard]] auto lpgpio_factory() -> model_factory;

/** @brief The LPGPIO model rows. */
[[nodiscard]] auto lpgpio_rows() -> std::span<builtin const>;
}
