#pragma once

#include <popcorn/periph/stm32/board.hpp>

#include "row_of.hpp"

#include <span>

namespace popcorn::periph::stm32
{
/** @brief Factory of the CRS model, RM0456 and RM0481 section 12. */
[[nodiscard]] auto crs_factory() -> model_factory;

/** @brief The CRS model rows. */
[[nodiscard]] auto crs_rows() -> std::span<builtin const>;
}
