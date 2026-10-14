#pragma once

#include <popcorn/export.hpp>

namespace popcorn::periph::stm32
{
/**
 * @brief Registers every built-in STM32 model with periph::register_model, each over a default
 *        board. Only the first call registers, later calls do nothing, so a registration a
 *        caller replaced afterwards stays replaced.
 */
POPCORN_API void register_builtin_models();
}
