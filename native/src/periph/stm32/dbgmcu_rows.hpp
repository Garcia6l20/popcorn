#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Factory of the DBGMCU model, RM0456 section 75.12 and RM0038 section 30.16. */
[[nodiscard]] auto dbgmcu_factory() -> model_factory;

/** @brief The DBGMCU and DBG model rows. */
[[nodiscard]] auto dbgmcu_rows() -> std::span<builtin const>;
}
