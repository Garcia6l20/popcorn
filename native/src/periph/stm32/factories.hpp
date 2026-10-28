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
 * @brief Factory of the RCC model, RM0456 section 11.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto rcc_factory(rcc_options options) -> model_factory;

/**
 * @brief Factory of the STM32H5 RCC model, RM0481 section 11.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto rcc_h5_factory(rcc_options options) -> model_factory;

/**
 * @brief Factory of the ICACHE model, RM0456 section 8.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto icache_factory(cache_options options) -> model_factory;

/**
 * @brief Factory of the DCACHE model, RM0456 section 9.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto dcache_factory(cache_options options) -> model_factory;

/**
 * @brief Factory of the PWR model, RM0456 section 10.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto pwr_factory(pwr_options options) -> model_factory;

/**
 * @brief Factory of the STM32H5 PWR model, RM0481 section 10.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto pwr_h5_factory(pwr_options options) -> model_factory;

/**
 * @brief Factory of the FLASH model, RM0456 section 7.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto flash_factory(flash_options options) -> model_factory;

/**
 * @brief Factory of the STM32H5 FLASH model, RM0481 section 7.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto flash_h5_factory(flash_options options) -> model_factory;

/**
 * @brief Factory of the GPIO port model, RM0456 section 13. Returns no model for LPGPIO1.
 * @param variant What differs between the ports of the family the row serves.
 */
[[nodiscard]] auto gpio_factory(gpio_variant variant) -> model_factory;

/** @brief Factory of the EXTI model, RM0456 section 23. */
[[nodiscard]] auto exti_factory() -> model_factory;

/** @brief Factory of the GPDMA1 model, RM0456 section 17. */
[[nodiscard]] auto gpdma_factory() -> model_factory;

/**
 * @brief Factory of the GPDMA1 and GPDMA2 model of the STM32H5, RM0481 section 16: 8 channels,
 *        2D addressing on 6 and 7, requests from Table 142, frozen by `DBG_GPDMA<n>_<ch>_STOP`.
 */
[[nodiscard]] auto gpdma_h5_factory() -> model_factory;

/** @brief Factory of the LPDMA1 model, the GPDMA engine with 4 channels, RM0456 section 18. */
[[nodiscard]] auto lpdma_factory() -> model_factory;

/**
 * @brief Factory of the USART, UART and LPUART model, RM0456 sections 66 and 67.
 * @param b Board whose `uarts` give each instance its far end, copied: the endpoints stay
 *        shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto usart_factory(board const& b, usart_variant variant) -> model_factory;

/**
 * @brief Factory of the TIM model, RM0456 sections 54 to 57 and RM0481 sections 43 to 48.
 * @param variant What differs between the timers of the family the row serves.
 */
[[nodiscard]] auto tim_factory(tim_variant variant) -> model_factory;

/** @brief Factory of the IWDG model, RM0456 section 61. */
[[nodiscard]] auto iwdg_factory() -> model_factory;

/**
 * @brief Factory of the RTC model, RM0456 section 63.
 * @param initial Calendar at power-on, the board's `rtc_initial`, copied.
 * @param variant What differs between the RTC of the family the row serves.
 */
[[nodiscard]] auto rtc_factory(std::optional<std::chrono::sys_seconds> initial, rtc_variant variant)
    -> model_factory;

/**
 * @brief Factory of the TAMP backup register model, RM0456 section 64.
 * @param variant What differs between the TAMP of the family the row serves.
 */
[[nodiscard]] auto tamp_factory(rtc_variant variant) -> model_factory;

/**
 * @brief Factory of the SPI model, RM0456 section 68 and RM0481 section 58.
 * @param b Board whose `spi` lists each instance's devices, copied when a model is made.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto spi_factory(board const& b, spi_variant variant) -> model_factory;

/**
 * @brief Factory of the I2C model, RM0456 section 65.
 * @param b Board whose `i2c` lists each instance's targets, copied when a model is made.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto i2c_factory(board const& b, i2c_variant variant) -> model_factory;

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
 * @brief Factory of the USB OTG_FS device core model, RM0456 section 72.
 * @param b Board whose `usb_hosts` gives each instance its virtual host, copied.
 */
[[nodiscard]] auto otg_fs_factory(board const& b) -> model_factory;

/**
 * @brief Factory of the USB full-speed device controller model of the STM32H5, RM0481 section 61.
 * @param b Board whose `usb_hosts` gives each instance its virtual host, copied.
 */
[[nodiscard]] auto usb_drd_factory(board const& b) -> model_factory;

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

/** @brief Factory of the DBGMCU model, RM0456 section 75.12. */
[[nodiscard]] auto dbgmcu_factory() -> model_factory;

/** @brief Factory of the CRC model, RM0456 section 24. */
[[nodiscard]] auto crc_factory() -> model_factory;

/** @brief Factory of the CRS model, RM0456 and RM0481 section 12. */
[[nodiscard]] auto crs_factory() -> model_factory;

/** @brief Factory of the SYSCFG model, RM0456 section 15. */
[[nodiscard]] auto syscfg_factory() -> model_factory;

/**
 * @brief Factory of the RNG model, RM0456 section 48.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto rng_factory(rng_options options) -> model_factory;

/** @brief Factory of the HASH model, RM0456 section 51. */
[[nodiscard]] auto hash_factory() -> model_factory;

/** @brief Factory of the STM32H5 HASH model, RM0481 section 40, SHA-2, SHA-3 and SHAKE. */
[[nodiscard]] auto hash_h5_factory() -> model_factory;

/**
 * @brief Factory of the GTZC TZSC, TZIC and MPCBB models, RM0456 and RM0481 section 5. Returns
 *        no model without TrustZone: the GTZC registers then stay storage at their description
 *        reset values.
 * @param variant Field stems of the family the row serves.
 */
[[nodiscard]] auto gtzc_factory(gtzc_variant variant) -> model_factory;

/** @brief Factory of the STM32H5 SBS temporal isolation model, RM0481 section 14. Returns no
 * model without TrustZone: the SBS registers then stay storage at their description reset
 * values. */
[[nodiscard]] auto sbs_factory() -> model_factory;
}
