#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/**
 * @brief Factory of the MDF and ADF model, RM0456 sections 39 and 40, its layout read from the
 *        description of each instance.
 */
[[nodiscard]] auto mdf_factory() -> model_factory;

/** @brief The MDF model rows. */
[[nodiscard]] auto mdf_rows() -> std::span<builtin const>;
}
