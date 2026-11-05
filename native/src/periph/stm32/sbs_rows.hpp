#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Factory of the STM32H5 SBS temporal isolation model, RM0481 section 14. Returns no
 * model without TrustZone: the SBS registers then stay storage at their description reset
 * values. */
[[nodiscard]] auto sbs_factory() -> model_factory;

/** @brief The SBS model rows. */
[[nodiscard]] auto sbs_rows() -> std::span<builtin const>;
}
