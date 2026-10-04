#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief The analog inputs of an ADC as a board sees them, implemented by the ADC1 model. Reach
 *        it with `dynamic_cast<analog_port*>(machine.model("ADC1"))`.
 */
class analog_port
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
   * @param channel External channel, 1 to 17 on ADC1.
   * @param microvolts Input voltage. Above VDDA the code saturates.
   * @throws config_error for an internal or absent channel.
   */
  virtual void drive(std::uint8_t channel, std::uint32_t microvolts) = 0;

  /**
   * @brief The voltage driven on an external channel in microvolts, 0 when never driven.
   * @param channel External channel, 1 to 17 on ADC1.
   * @throws config_error for an internal or absent channel.
   */
  [[nodiscard]] virtual auto input(std::uint8_t channel) const -> std::uint32_t = 0;

  /** @brief VDDA in millivolts, the reference of every conversion. */
  [[nodiscard]] virtual auto vdda_mv() const -> std::uint32_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~analog_port() = default;
};
}
