#pragma once

#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/caches.hpp>
#include <popcorn/periph/stm32/flash.hpp>
#include <popcorn/periph/stm32/pwr.hpp>
#include <popcorn/periph/stm32/rcc.hpp>
#include <popcorn/periph/stm32/rng.hpp>

#include "variants.hpp"

#include <chrono>
#include <optional>

namespace popcorn::periph::stm32
{
/**
 * @brief Factory of the IWDG model, RM0456 section 61, RM0038 section 21.
 * @param variant What differs between the IWDG of the family the row serves.
 */
[[nodiscard]] auto iwdg_factory(iwdg_variant variant) -> model_factory;

/**
 * @brief Factory of the DAC model, RM0351 section 19.
 * @param supply VDDA, the VREF+ the channel outputs scale to.
 * @param variant What differs between the DAC of the family the row serves.
 */
[[nodiscard]] auto dac_factory(supply_options supply, dac_variant variant) -> model_factory;

/**
 * @brief Factory of the RTC model, RM0456 section 63.
 * @param initial Calendar at power-on, the board's `rtc_initial`, copied.
 * @param variant What differs between the RTC of the family the row serves.
 */
[[nodiscard]] auto rtc_factory(std::optional<std::chrono::sys_seconds> initial, rtc_variant variant)
    -> model_factory;

/**
 * @brief Factory of the classic RTC model, RM0038 section 20.
 * @param initial Calendar at power-on, the board's `rtc_initial`, copied.
 * @param variant What differs between the RTC of the category the row serves.
 */
[[nodiscard]] auto rtc_classic_factory(
    std::optional<std::chrono::sys_seconds> initial,
    rtc_classic_variant                     variant) -> model_factory;

/**
 * @brief Factory of the TAMP backup register model, RM0456 section 64.
 * @param variant What differs between the TAMP of the family the row serves.
 */
[[nodiscard]] auto tamp_factory(rtc_variant variant) -> model_factory;

/**
 * @brief Factory of the SAI model, RM0456 section 69.
 * @param b Board whose `audio` wires each audio block, copied: the endpoints stay shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto sai_factory(board const& b, sai_variant variant) -> model_factory;

/**
 * @brief Factory of the MDF and ADF model, RM0456 sections 39 and 40, its layout read from the
 *        description of each instance.
 */
[[nodiscard]] auto mdf_factory() -> model_factory;

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

/** @brief Factory of the DBGMCU model, RM0456 section 75.12 and RM0038 section 30.16. */
[[nodiscard]] auto dbgmcu_factory() -> model_factory;

/**
 * @brief Factory of the CRC model, RM0456 section 24, RM0038 section 4.
 * @param variant What differs between the CRC of the family the row serves.
 */
[[nodiscard]] auto crc_factory(crc_variant variant) -> model_factory;

/**
 * @brief Factory of the RNG model, RM0456 section 48 and RM0351 section 27.
 * @param options Board configuration, copied into every model made.
 * @param variant What differs between the RNG of the family the row serves.
 */
[[nodiscard]] auto rng_factory(rng_options options, rng_variant variant) -> model_factory;

/** @brief Factory of the HASH model, RM0456 section 51. */
[[nodiscard]] auto hash_factory() -> model_factory;

/**
 * @brief Factory of the STM32H5 HASH model, RM0481 section 40, SHA-2, and SHA-3 and SHAKE where
 *        @p variant has them.
 */
[[nodiscard]] auto hash_h5_factory(hash_variant variant) -> model_factory;

/** @brief Factory of the STM32H5 SBS temporal isolation model, RM0481 section 14. Returns no
 * model without TrustZone: the SBS registers then stay storage at their description reset
 * values. */
[[nodiscard]] auto sbs_factory() -> model_factory;
}
