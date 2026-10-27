#pragma once

#include <cstdint>
#include <string_view>

namespace popcorn::periph::stm32
{
/**
 * @brief Differences between the GPIO ports of the families a model row serves.
 *        The model throws at attach when the description disagrees.
 */
struct gpio_variant
{
  /** @brief Port with fewer than 16 pins, 0 for none. */
  char short_port = 0;
  /** @brief Pins of that port. Its registers carry the per-pin fields of these pins only. */
  std::uint8_t short_port_pins = 16;
};

/** @brief STM32U5 ports with 16 pins, RM0456 13.4.10. */
inline constexpr gpio_variant u5_gpio{};
/** @brief STM32U575 and STM32U585 ports, port I has pins 0 to 7, read back on a NUCLEO-U575ZI-Q:
 *         `GPIOI_MODER` keeps 0x0000FFFF of 0xFFFFFFFF and `GPIOI_AFRH` reads 0, RM0456 13.4.1. */
inline constexpr gpio_variant u575_gpio{.short_port = 'I', .short_port_pins = 8};
/** @brief STM32H56x and H57x ports, port I has pins 0 to 11, read back on an STM32H573:
 *         `GPIOI_MODER` keeps 0x00FFFFFF of 0xFFFFFFFF and `GPIOI_AFRH` holds AFSEL8 to AFSEL11,
 *         as DS14121 Table 17 lists. */
inline constexpr gpio_variant h56x_gpio{.short_port = 'I', .short_port_pins = 12};

/** @brief Differences between the I2C peripherals the model rows serve. */
struct i2c_variant
{
  /** @brief The peripheral has the `I2C_AUTOCR` register and its trigger and DMA fields. */
  bool autocr = true;
};

/** @brief STM32U5 I2C, RM0456 65.7.14. */
inline constexpr i2c_variant u5_i2c{};
/** @brief STM32H5 I2C, RM0481 has no `I2C_AUTOCR`. */
inline constexpr i2c_variant h5_i2c{.autocr = false};

/** @brief Differences between the USART, UART and LPUART peripherals the model rows serve. */
struct usart_variant
{
  /** @brief The peripheral has the `CR1_enabled` and `ISR_enabled` register views and no `CR1`
   *         and `ISR`. */
  bool mode_views = true;
  /** @brief The peripheral has the `FIFOEN` control bit, otherwise it holds one data word. */
  bool fifo = true;
  /** @brief Name of the IRQ line, empty for `<instance>_IRQn`. */
  std::string_view irq_name = {};
};

/** @brief STM32U5 USART and UART, RM0456 66.8, and every STM32H5 USART and UART, RM0481 56.8. */
inline constexpr usart_variant with_views_usart{};
/** @brief STM32U5 LPUART, RM0456 67.7, one `CR1` and one `ISR`. */
inline constexpr usart_variant u5_lpuart{.mode_views = false};
/** @brief STM32H5 LPUART, RM0481 57.7: the register views of the USART, and the CMSIS header
 *         names its line `LPUART1_IRQn` for the peripheral `LPUART`. */
inline constexpr usart_variant h5_lpuart{.irq_name = "LPUART1_IRQn"};

/** @brief Differences between the RTC and TAMP peripherals the model rows serve. */
struct rtc_variant
{
  /** @brief Name of the privilege register the security write handler binds. */
  std::string_view privilege_register = "PRIVCR";
};

/** @brief STM32U5 RTC and TAMP, RM0456 63.6.14 and 64.6.7. */
inline constexpr rtc_variant u5_rtc{};
/** @brief STM32H5 RTC and TAMP, RM0481 51.6.8 and 52.7.10. */
inline constexpr rtc_variant h5_rtc{.privilege_register = "PRIVCFGR"};

/** @brief Differences between the SAI peripherals the model rows serve. */
struct sai_variant
{
  /** @brief Name of the block enable field of `xCR1`. */
  std::string_view enable = "SAIAEN";
};

/** @brief STM32U5 SAI, RM0456 69.6.2. */
inline constexpr sai_variant u5_sai{};
/** @brief STM32H5 SAI, RM0481 59.6.2. */
inline constexpr sai_variant h5_sai{.enable = "SAIEN"};
}
