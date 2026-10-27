#pragma once

#include <popcorn/export.hpp>

#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief The analog inputs of an ADC as a board sees them, implemented by the ADC model of each
 *        ADC instance, ADC1 on the STM32U5, ADC1 and ADC2 on the STM32H5. Reach it with
 *        `dynamic_cast<analog_port*>(machine.model("ADC1"))`.
 */
class POPCORN_API analog_port
{
public:
  /** @brief Creates the interface. */
  analog_port() = default;
  /** @brief Not copyable. */
  analog_port(analog_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(analog_port const&) -> analog_port& = delete;
  /** @brief Not movable. */
  analog_port(analog_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(analog_port&&) -> analog_port& = delete;

  /**
   * @brief Drives an external channel. The drive is board state: it survives an MCU reset and a
   *        snapshot restores it. A conversion reads the value in force when it ends.
   * @param channel External channel, one of external_channels().
   * @param microvolts Input voltage. Above VDDA the code saturates.
   * @throws config_error for an internal or absent channel.
   */
  virtual void drive(std::uint8_t channel, std::uint32_t microvolts) = 0;

  /**
   * @brief The voltage driven on an external channel in microvolts, 0 when never driven.
   * @param channel External channel, one of external_channels().
   * @throws config_error for an internal or absent channel.
   */
  [[nodiscard]] virtual auto input(std::uint8_t channel) const -> std::uint32_t = 0;

  /**
   * @brief The external channels: bit n is set when channel n takes a drive. Fixed for the life
   *        of the model, so any thread may read it.
   */
  [[nodiscard]] virtual auto external_channels() const -> std::uint32_t = 0;

  /** @brief VDDA in millivolts, the reference of every conversion. */
  [[nodiscard]] virtual auto vdda_mv() const -> std::uint32_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~analog_port();
};
}
