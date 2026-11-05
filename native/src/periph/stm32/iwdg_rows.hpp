#pragma once

#include "family_names.hpp"
#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the IWDG peripherals the model rows serve. */
struct iwdg_variant
{
  /** @brief The peripheral has the window: `WINR` and `SR.WVU`. Without it a refresh reloads at
   *         any counter value. */
  bool window = true;
  /** @brief The peripheral has the early wake-up interrupt: `EWCR`, `SR.EWU`, `SR.EWIF` and the
   *         `IWDG_IRQn` line. */
  bool early_wakeup = true;
  /** @brief Largest `PR` value with its own divider, 4 << `PR`. A larger value divides by
   *         4 << `prescaler_top`. */
  std::uint8_t prescaler_top = 8;
  /** @brief LSI cycles from a `PR`, `RLR`, `WINR` or `EWCR` write to the fall of its update
   *         flag. */
  std::uint8_t update_cycles = 6;
  /**
   * @brief `FLASH` register and field of the hardware watchdog option. At every reset the
   *        watchdog starts when the field reads 0. Empty: it starts only at `KR` 0xCCCC.
   */
  field_name hardware_option = {};
};

/** @brief STM32U5 and STM32H5 IWDG, RM0456 61.7.2 and Table 623, RM0481 49.7.2. */
inline constexpr iwdg_variant u5_iwdg{};
/** @brief STM32L1 IWDG, RM0038 21.4: KR, PR, RLR and SR with PVU and RVU only, PR 110 and 111
 *         both divide by 256, an update takes 5 LSI cycles. */
inline constexpr iwdg_variant l1_iwdg{
    .window        = false,
    .early_wakeup  = false,
    .prescaler_top = 6,
    .update_cycles = 5};
/**
 * @brief STM32L4 IWDG, RM0351 36.4: KR, PR, RLR, SR with PVU, RVU and WVU, and WINR, no early
 *        wake-up, PR 110 and 111 both divide by 256, an update takes up to 5 LSI cycles, the
 *        hardware watchdog started by FLASH_OPTR.IWDG_SW 0, RM0351 36.3.3.
 */
inline constexpr iwdg_variant l4_iwdg{
    .window          = true,
    .early_wakeup    = false,
    .prescaler_top   = 6,
    .update_cycles   = 5,
    .hardware_option = {"OPTR", "IWDG_SW"}
};

/**
 * @brief Factory of the IWDG model, RM0456 section 61, RM0038 section 21.
 * @param variant What differs between the IWDG of the family the row serves.
 */
[[nodiscard]] auto iwdg_factory(iwdg_variant variant) -> model_factory;

/** @brief The IWDG model rows. */
[[nodiscard]] auto iwdg_rows() -> std::span<builtin const>;
}
