#pragma once

#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/rcc.hpp>

namespace popcorn::periph::stm32
{
/**
 * @brief Factory of the RCC model, RM0456 section 11.
 * @param options Board configuration, kept by reference: it must outlive every model made.
 */
[[nodiscard]] auto rcc_factory(rcc_options const& options) -> model_factory;
}
