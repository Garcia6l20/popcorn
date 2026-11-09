#pragma once

#include "family_names.hpp"
#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <span>

namespace popcorn::periph::stm32
{
/** @brief Differences between the WWDG peripherals the model rows serve. */
struct wwdg_variant
{
  /**
   * @brief `FLASH` register and field of the hardware watchdog option. At every reset the
   *        watchdog starts when the field reads 0. Empty: it starts only at the `CR` write that
   *        sets WDGA.
   */
  field_name hardware_option = {};
};

/** @brief STM32L1 WWDG, RM0038 22: no hardware option. */
inline constexpr wwdg_variant l1_wwdg{};
/** @brief STM32L4 WWDG, RM0351 37 and `FLASH_OPTR.WWDG_SW`. */
inline constexpr wwdg_variant l4_wwdg{
    .hardware_option = {"OPTR", "WWDG_SW"}
};
/** @brief STM32U5 WWDG, RM0456 62 and `FLASH_OPTR.WWDG_SW`. */
inline constexpr wwdg_variant u5_wwdg{
    .hardware_option = {"OPTR", "WWDG_SW"}
};
/** @brief STM32H5 WWDG, RM0481 50 and `FLASH_OPTSR_CUR.WWDG_SW`. */
inline constexpr wwdg_variant h5_wwdg{
    .hardware_option = {"OPTSR_CUR", "WWDG_SW"}
};

/**
 * @brief Factory of the WWDG model, RM0456 section 62, RM0481 section 50, RM0351 section 37 and
 *        RM0038 section 22.
 * @param variant What differs between the WWDG of the family the row serves.
 */
[[nodiscard]] auto wwdg_factory(wwdg_variant variant) -> model_factory;

/** @brief The WWDG model rows. */
[[nodiscard]] auto wwdg_rows() -> std::span<builtin const>;
}
