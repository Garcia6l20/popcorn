#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the STM32L1 ADC categories the model rows serve, RM0038 12.3.3. */
struct adc_l1_variant
{
  /** @brief The unit has the two channel banks of ADC_CR2.ADC_CFG, ADC_SMPR0 for channels 30
   *         and 31 and the 28th rank SQ28. Without them it has one bank and 27 ranks. */
  bool banks = true;
  /** @brief External channels of bank A, bit n for ADC_INn, RM0038 Table 58. */
  std::uint32_t external = 0xFBFC'FFFF;
  /** @brief Bank B channels on pads of their own, bit n for ADC_INnb, RM0038 Table 41. The other
   *         bank B channels share the pad of their bank A channel. */
  std::uint32_t bank_b_only = 0x0000'1FCF;
  /** @brief Address of the factory VREFINT_CAL halfword. */
  std::uint32_t vrefint_cal = 0x1FF8'00F8;
  /** @brief Address of the factory TS_CAL1 halfword, the sensor code at 30 C. */
  std::uint32_t ts_cal1 = 0x1FF8'00FA;
};

/** @brief STM32L1 Cat.3 to Cat.6 ADC, DS8890, DS10262, DS10764 and DS10002. */
inline constexpr adc_l1_variant l1_adc{};
/** @brief STM32L1 Cat.1 and Cat.2 ADC, `STM32L1_adc_cat12`: one bank of ADC_IN0 to ADC_IN25,
 *         27 ranks, DS6876 and DS9515. */
inline constexpr adc_l1_variant l1_cat12_adc{
    .banks       = false,
    .external    = 0x03FC'FFFF,
    .bank_b_only = 0,
    .vrefint_cal = 0x1FF8'0078,
    .ts_cal1     = 0x1FF8'007A};

/**
 * @brief Factory of the ADC1 model, RM0456 section 33.
 * @param supply Board supplies, VDDA the conversion reference, copied into every model made.
 */
[[nodiscard]] auto adc_factory(supply_options supply) -> model_factory;

/** @brief Factory of the ADC12 common register model, RM0456 section 33.7. */
[[nodiscard]] auto adc_common_factory() -> model_factory;

/**
 * @brief Factory of the STM32H5 ADC1 and ADC2 model, RM0481 section 28.
 * @param supply Board supplies, VDDA the conversion reference, copied into every model made.
 */
[[nodiscard]] auto adc_h5_factory(supply_options supply) -> model_factory;

/** @brief Factory of the STM32H5 ADCC common register model, RM0481 section 28.7. */
[[nodiscard]] auto adc_common_h5_factory() -> model_factory;

/**
 * @brief Factory of the STM32L47x and STM32L48x ADC1, ADC2 and ADC3 model, RM0351 section 18.
 * @param supply Board supplies, VDDA the conversion reference, copied into every model made.
 */
[[nodiscard]] auto adc_l4_factory(supply_options supply) -> model_factory;

/** @brief Factory of the STM32L4 ADC_Common register model, RM0351 section 18.8. */
[[nodiscard]] auto adc_common_l4_factory() -> model_factory;

/**
 * @brief Factory of the STM32L1 ADC model, RM0038 section 12.
 * @param supply Board supplies, VDDA the conversion reference, copied into every model made.
 * @param variant What differs between the ADC of the categories the row serves.
 */
[[nodiscard]] auto adc_l1_factory(supply_options supply, adc_l1_variant variant) -> model_factory;

/** @brief The ADC model rows. */
[[nodiscard]] auto adc_rows() -> std::span<builtin const>;

/** @brief The STM32L1 ADC model rows. */
[[nodiscard]] auto adc_l1_rows() -> std::span<builtin const>;
}
