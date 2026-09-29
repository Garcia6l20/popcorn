#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/caches.hpp>
#include <popcorn/periph/stm32/pwr.hpp>
#include <popcorn/periph/stm32/rcc.hpp>
#include <popcorn/periph/stm32/spi.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief Board configuration of the built-in STM32 models: clock sources, and the devices and
 *        endpoints wired to the MCU. Owned by the caller, it outlives every machine built from
 *        factories() over it.
 */
struct board
{
  /** @brief Clock sources and timing of the RCC model. */
  rcc_options rcc{};
  /** @brief Timing of the PWR model. */
  pwr_options pwr{};
  /** @brief Timing of the ICACHE and DCACHE models. */
  cache_options caches{};
  /** @brief Far end of each UART by instance name, such as `USART1`. A UART without one drops
   * what it transmits. */
  std::map<std::string, std::shared_ptr<uart_endpoint>, std::less<>> uarts{};
  /**
   * @brief Calendar of the RTC model at power-on, as if a backup battery had kept it, weekday
   *        derived from the date, years 2000 to 2099. Empty for the RTC_TR and RTC_DR reset
   *        values, 2000-01-01 00:00:00, Monday, with INITS clear.
   */
  std::optional<std::chrono::sys_seconds> rtc_initial{};
  /** @brief Devices on each SPI instance by instance name, such as `SPI1`. The SPI model copies the
   * list when the machine is built and saves and loads each device with its own state. */
  std::map<std::string, std::vector<spi_attachment>, std::less<>> spi{};
};

/**
 * @brief Factory per peripheral instance of @p part that a built-in model serves, for
 *        core::machine_options::models.
 * @param b Board the models read their configuration from, kept by reference.
 * @param part Description the machine is built from.
 * @return Factories keyed by instance name, aliases excluded.
 */
[[nodiscard]] auto factories(board const& b, mcu_description const& part)
    -> std::map<std::string, model_factory, std::less<>>;
}
