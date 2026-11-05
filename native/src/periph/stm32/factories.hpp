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
 * @brief Factory of the SPI model, RM0456 section 68 and RM0481 section 58.
 * @param b Board whose `spi` lists each instance's devices, copied when a model is made.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto spi_factory(board const& b, spi_variant variant) -> model_factory;

/**
 * @brief Factory of the classic SPI model, RM0038 section 28.
 * @param b Board whose `spi` lists each instance's devices and `spi_controllers` its controller,
 *        copied when a model is made.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto spi_classic_factory(board const& b, spi_classic_variant variant)
    -> model_factory;

/**
 * @brief Factory of the I2C model, RM0456 section 65.
 * @param b Board whose `i2c` lists each instance's targets, copied when a model is made.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto i2c_factory(board const& b, i2c_variant variant) -> model_factory;

/**
 * @brief Factory of the STM32L1 I2C model, RM0038 section 26.
 * @param b Board whose `i2c` lists each instance's targets, copied when a model is made. An
 *        instance with a `i2c_controllers` entry is refused with config_error.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto i2c_v1_factory(board const& b, i2c_v1_variant variant) -> model_factory;

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
 * @brief Factory of the USB OTG_FS device core model, RM0456 section 72 on the STM32U5, RM0351
 *        section 47 on the STM32L4.
 * @param b Board whose `usb_hosts` gives each instance its virtual host, copied.
 * @param variant GRSTCTL reset field names of the core, copied.
 */
[[nodiscard]] auto otg_fs_factory(board const& b, otg_fs_variant const& variant) -> model_factory;

/**
 * @brief Factory of the USB full-speed device controller model, RM0481 section 61 on the STM32H5,
 *        RM0038 section 24 on the STM32L1.
 * @param b Board whose `usb_hosts` gives each instance its virtual host, copied.
 * @param variant Register names, packet memory and pull-up of the controller, copied.
 */
[[nodiscard]] auto usb_drd_factory(board const& b, usb_drd_variant const& variant) -> model_factory;

/**
 * @brief Factory of the packet memory window of a USB device controller held in a peripheral of
 *        its own, `USB_SRAM` on the STM32L1, served by the controller's model.
 * @param controller Peripheral name of the controller.
 */
[[nodiscard]] auto usb_packet_memory_factory(std::string_view controller) -> model_factory;

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
