#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <span>

namespace popcorn::periph::stm32
{
/** @brief Factory of the WWDG model, RM0351 section 37 and RM0038 section 22. */
[[nodiscard]] auto wwdg_factory() -> model_factory;

/** @brief The WWDG model rows. */
[[nodiscard]] auto wwdg_rows() -> std::span<builtin const>;
}
