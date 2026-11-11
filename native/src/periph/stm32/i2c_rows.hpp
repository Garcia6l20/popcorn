#pragma once

#include "bus_window.hpp"
#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the I2C peripherals the model rows serve. */
struct i2c_variant
{
  /** @brief The peripheral has the `I2C_AUTOCR` register and its trigger and DMA fields. */
  bool autocr = true;
  /** @brief CR1 has ADDRACLR and STOPFACLR, which keep ADDR and STOPF clear. */
  bool clear_on_ack = true;
  /**
   * @brief A write of ICR.ADDRCF clears CR2.START, RM0351 39.9.2 and 39.9.8 as corrected by
   *        ES0250 2.20.7. Otherwise an address matched in target mode clears it, RM0456 65.9.2.
   */
  bool start_cleared_by_address_clear = false;
};

/** @brief STM32U5 I2C, RM0456 65.7.14. */
inline constexpr i2c_variant u5_i2c{};
/** @brief STM32H5 I2C, RM0481 has no `I2C_AUTOCR`. */
inline constexpr i2c_variant h5_i2c{.autocr = false};
/** @brief STM32L4 I2C1 to I2C3, RM0351 39.9: no `I2C_AUTOCR`, no ADDRACLR and STOPFACLR, the
 *         kernel clock the CCIPR I2CxSEL selection. */
inline constexpr i2c_variant l4_i2c{
    .autocr                         = false,
    .clear_on_ack                   = false,
    .start_cleared_by_address_clear = true};

/** @brief Differences between the I2C peripherals of RM0038 section 26 the model rows serve. */
struct i2c_l1_variant
{
  /** @brief Buses whose clock is the clock of an instance in their window. */
  std::span<bus_window const> kernel_buses;
};

/** @brief STM32L1 I2C1 and I2C2 on APB1, RM0038 Table 5. */
inline constexpr i2c_l1_variant l1_i2c{.kernel_buses = l1_apb_windows};

/**
 * @brief Factory of the I2C model, RM0456 section 65.
 * @param targets Board `i2c`, each instance's targets, copied when a model is made.
 * @param controllers Board `i2c_controllers`, each instance's controller, shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto i2c_factory(
    decltype(board::i2c) const&             targets,
    decltype(board::i2c_controllers) const& controllers,
    i2c_variant                             variant) -> model_factory;

/**
 * @brief Factory of the STM32L1 I2C model, RM0038 section 26.
 * @param targets Board `i2c`, each instance's targets, copied when a model is made.
 * @param controllers Board `i2c_controllers`, each instance's controller, shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto i2c_l1_factory(
    decltype(board::i2c) const&             targets,
    decltype(board::i2c_controllers) const& controllers,
    i2c_l1_variant                          variant) -> model_factory;

/** @brief The I2C model rows. */
[[nodiscard]] auto i2c_rows() -> std::span<builtin const>;

/** @brief The STM32L1 I2C model rows. */
[[nodiscard]] auto i2c_l1_rows() -> std::span<builtin const>;
}
