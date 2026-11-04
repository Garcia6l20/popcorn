#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief The PWR model as the GPIO model sees it, found by `dynamic_cast` over the machine's
 *        peers: whether the independent I/O supply VDDIO2 is declared valid, RM0351 5.4.2.
 */
class io_supply_source
{
public:
  /** @brief Creates the interface. */
  io_supply_source() = default;
  /** @brief Not copyable. */
  io_supply_source(io_supply_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(io_supply_source const&) -> io_supply_source& = delete;
  /** @brief Not movable. */
  io_supply_source(io_supply_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(io_supply_source&&) -> io_supply_source& = delete;

  /** @brief `PWR_CR2.IOSV` on the STM32L4, true on a family without it. */
  [[nodiscard]] virtual auto io_supply_valid() const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~io_supply_source() = default;
};

/**
 * @brief A GPIO port as the ADC model sees it, found by `dynamic_cast` over the machine's peers:
 *        whether the analog switch between a pin and its ADC input is closed, RM0351 8.5.12.
 */
class analog_switch_source
{
public:
  /** @brief Creates the interface. */
  analog_switch_source() = default;
  /** @brief Not copyable. */
  analog_switch_source(analog_switch_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(analog_switch_source const&) -> analog_switch_source& = delete;
  /** @brief Not movable. */
  analog_switch_source(analog_switch_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(analog_switch_source&&) -> analog_switch_source& = delete;

  /**
   * @brief Whether @p pin reaches its ADC input.
   * @param pin Pin of the port, 0 to 15.
   * @return `GPIOx_ASCR.ASCy` on a port with the register, true on a port without it.
   */
  [[nodiscard]] virtual auto switch_closed(std::uint8_t pin) const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~analog_switch_source() = default;
};
}
