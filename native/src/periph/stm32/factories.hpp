#pragma once

#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/caches.hpp>
#include <popcorn/periph/stm32/flash.hpp>
#include <popcorn/periph/stm32/pwr.hpp>
#include <popcorn/periph/stm32/rcc.hpp>

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
 * @brief Factory of the FLASH model, RM0456 section 7.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto flash_factory(flash_options options) -> model_factory;

/** @brief Factory of the GPIO port model, RM0456 section 13. Returns no model for LPGPIO1. */
[[nodiscard]] auto gpio_factory() -> model_factory;

/** @brief Factory of the EXTI model, RM0456 section 23. */
[[nodiscard]] auto exti_factory() -> model_factory;

/** @brief Factory of the GPDMA1 model, RM0456 section 17. */
[[nodiscard]] auto gpdma_factory() -> model_factory;

/**
 * @brief Factory of the USART, UART and LPUART model, RM0456 sections 66 and 67.
 * @param b Board whose `uarts` give each instance its far end, copied: the endpoints stay
 *        shared.
 */
[[nodiscard]] auto usart_factory(board const& b) -> model_factory;

/** @brief Factory of the TIM model, RM0456 sections 54 to 57. */
[[nodiscard]] auto tim_factory() -> model_factory;

/** @brief Factory of the IWDG model, RM0456 section 61. */
[[nodiscard]] auto iwdg_factory() -> model_factory;

/**
 * @brief Factory of the RTC model, RM0456 section 63.
 * @param initial Calendar at power-on, the board's `rtc_initial`, copied.
 */
[[nodiscard]] auto rtc_factory(std::optional<std::chrono::sys_seconds> initial) -> model_factory;

/** @brief Factory of the TAMP backup register model, RM0456 section 64. */
[[nodiscard]] auto tamp_factory() -> model_factory;

/**
 * @brief Factory of the SPI model, RM0456 section 68.
 * @param b Board whose `spi` lists each instance's devices, copied when a model is made.
 */
[[nodiscard]] auto spi_factory(board const& b) -> model_factory;

/**
 * @brief Factory of the I2C model, RM0456 section 65.
 * @param b Board whose `i2c` lists each instance's targets, copied when a model is made.
 */
[[nodiscard]] auto i2c_factory(board const& b) -> model_factory;

/**
 * @brief Factory of the SAI model, RM0456 section 69.
 * @param b Board whose `audio` wires each audio block, copied: the endpoints stay shared.
 */
[[nodiscard]] auto sai_factory(board const& b) -> model_factory;

/** @brief Factory of the USB OTG_FS core reset model, RM0456 section 72. */
[[nodiscard]] auto otg_fs_factory() -> model_factory;

/** @brief Factory of the ADC1 model, RM0456 section 33. */
[[nodiscard]] auto adc_factory() -> model_factory;

/** @brief Factory of the ADC12 common register model, RM0456 section 33.7. */
[[nodiscard]] auto adc_common_factory() -> model_factory;
}
