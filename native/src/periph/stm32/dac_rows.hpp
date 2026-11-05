#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the DAC peripherals the model rows serve. */
struct dac_variant
{
  /** @brief DAC_CR.TSELx value that selects the software trigger SWTRIGx. */
  std::uint32_t software_trigger = 0b111;
  /** @brief Trigger signal of each DAC_CR.TSELx value below @ref software_trigger. */
  std::array<std::string_view, 7> hardware_triggers{
      "tim6_trgo", "tim8_trgo", "tim7_trgo", "tim5_trgo", "tim2_trgo", "tim4_trgo", "exti9"};
  /** @brief Factory OTRIMx of each channel, the DAC_CCR reset value. RM0351 19.7.15 gives
   *         0x00XX 00XX, so the value is a mid-range code, inferred. */
  std::array<std::uint32_t, 2> factory_trim{16, 16};
  /** @brief GPIO port of the channel output pads, RM0351 19.3. */
  std::string_view pad_port = "GPIOA";
  /** @brief Pin of each channel's DAC_OUTx pad on @ref pad_port, PA4 and PA5, RM0351 19.3. */
  std::array<std::uint8_t, 2> pads{4, 5};
};

/** @brief STM32L4 DAC1, RM0351 section 19, the triggers and SWTRIG at TSELx 111 of RM0351
 *         Table 128. */
inline constexpr dac_variant l4_dac{};

/**
 * @brief Factory of the DAC model, RM0351 section 19.
 * @param supply VDDA, the VREF+ the channel outputs scale to.
 * @param variant What differs between the DAC of the family the row serves.
 */
[[nodiscard]] auto dac_factory(supply_options supply, dac_variant variant) -> model_factory;

/** @brief The DAC model rows. */
[[nodiscard]] auto dac_rows() -> std::span<builtin const>;
}
