#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/fwd.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/audio.hpp>
#include <popcorn/periph/stm32/caches.hpp>
#include <popcorn/periph/stm32/flash.hpp>
#include <popcorn/periph/stm32/i2c.hpp>
#include <popcorn/periph/stm32/pwr.hpp>
#include <popcorn/periph/stm32/rcc.hpp>
#include <popcorn/periph/stm32/spi.hpp>
#include <popcorn/periph/stm32/supply.hpp>
#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/uart.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief Board configuration of the built-in STM32 models: clock sources, and the devices and
 *        endpoints wired to the MCU. factories() copies what it needs, so the board may change or
 *        go away once the factories are made.
 */
struct board
{
  /** @brief Clock sources and timing of the RCC model. */
  rcc_options rcc{};
  /** @brief Timing of the PWR model. */
  pwr_options pwr{};
  /** @brief Timing of the ICACHE and DCACHE models. */
  cache_options caches{};
  /** @brief Busy times of the FLASH model. */
  flash_options flash{};
  /** @brief Supply voltages, VDDA for the ADC model. */
  supply_options supply{};
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
   * list when the machine is built, saves and loads each device with its own state, and resets it
   * with the machine. A device object is shared by every machine built from these factories:
   * give each machine its own board and devices. */
  std::map<std::string, std::vector<spi_attachment>, std::less<>> spi{};
  /** @brief The controller clocking each SPI instance in target mode, by instance name. An
   * instance with one takes no device in `spi`. Shared as the devices are. */
  std::map<std::string, std::shared_ptr<spi_controller_sink>, std::less<>> spi_controllers{};
  /** @brief Targets on each I2C instance by instance name, such as `I2C1`. The I2C model copies
   * the list when the machine is built, addresses the first target answering an address, saves
   * and loads each target with its own state, and resets it with the machine. A target object is
   * shared by every machine built from these factories: give each machine its own board and
   * targets. */
  std::map<std::string, std::vector<std::shared_ptr<i2c_target>>, std::less<>> i2c{};
  /** @brief What each SAI audio block is wired to, keyed by instance and block, such as
   * `SAI1_A`. The SAI model copies the attachments when the machine is built: the endpoints stay
   * shared. */
  std::map<std::string, audio_attachment, std::less<>> audio{};
  /** @brief The virtual host wired to each USB device controller by instance name, such as
   * `OTG_FS`. A controller without one sees no host, as with no cable. The model copies the
   * options when the machine is built and builds its host from them. */
  std::map<std::string, usb_host_options, std::less<>> usb_hosts{};
};

/**
 * @brief Factory per peripheral instance of @p part that a built-in model serves, for
 *        core::machine_options::models.
 * @param b Board the models read their configuration from, copied into the factories. UART
 *        endpoints, SPI devices, I2C targets and audio endpoints are held by `shared_ptr` and stay
 *        shared.
 * @param part Description the machine is built from.
 * @return Factories keyed by instance name, aliases excluded.
 */
[[nodiscard]] POPCORN_API auto factories(board const& b, part_description const& part)
    -> std::map<std::string, model_factory, std::less<>>;

/** @brief Why check_option_bytes() or configure_option_bytes() refuses option bytes. */
struct option_refusal
{
  /** @brief Dotted key of the refused field, such as `option_bytes.secwm1`. */
  std::string key;
  /** @brief What the field must hold instead. */
  std::string expected;
};

/**
 * @brief Runs every check the FLASH model of @p part makes on @p wanted before it encodes them,
 *        so a caller can refuse them before building anything.
 * @param part Description the machine is built from.
 * @param wanted Option bytes the machine boots with.
 * @return The address the core boots from: the secure boot address with TZEN set, the
 *         non-secure one otherwise. The refusal when the part has no built-in FLASH model that
 *         encodes option bytes, or a field breaks a rule of its family.
 */
[[nodiscard]] POPCORN_API auto
    check_option_bytes(part_description const& part, option_bytes const& wanted)
        -> std::expected<address, option_refusal>;

/**
 * @brief IDAU of the family of @p part, as data for core::machine_options::idau.
 * @param part Description the machine is built from.
 * @return The ranges, std::nullopt when the family has no IDAU table.
 */
[[nodiscard]] POPCORN_API auto idau_map(part_description const& part)
    -> std::optional<std::span<idau_region const>>;

/**
 * @brief Sets up a machine to boot with @p wanted, so the FLASH model, the boot address and
 *        TrustZone agree: the FLASH factory with the option bytes in `options.models`,
 *        `options.vector_table` to the boot address when it is empty, `options.trustzone` to
 *        TZEN and `options.idau` to the family's IDAU table under TZEN. Every refusal runs
 *        before @p options changes.
 * @param part Description the machine is built from.
 * @param wanted Option bytes the machine boots with.
 * @param options Options of the machine to build.
 * @return The refusal: `option_bytes.tzen` with TZEN on a core without a security extension or
 *         on a family without an IDAU table, `option_bytes` when `options.models` already holds
 *         a model of the FLASH or no built-in FLASH model serves it, and every refusal of
 *         check_option_bytes().
 */
[[nodiscard]] POPCORN_API auto configure_option_bytes(
    part_description const& part,
    option_bytes const&     wanted,
    core::machine_options&  options) -> std::expected<void, option_refusal>;
}
