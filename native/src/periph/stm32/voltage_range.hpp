#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief The PWR model as the FLASH model sees it, found by `dynamic_cast` over the machine's
 *        peers: the voltage range VCORE runs at, RM0456 section 10.5.4.
 */
class voltage_range_source
{
public:
  /** @brief Creates the interface. */
  voltage_range_source() = default;
  /** @brief Not copyable. */
  voltage_range_source(voltage_range_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(voltage_range_source const&) -> voltage_range_source& = delete;
  /** @brief Not movable. */
  voltage_range_source(voltage_range_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(voltage_range_source&&) -> voltage_range_source& = delete;

  /** @brief Range applied to VCORE, PWR_SVMSR.ACTVOS as a range number, 1 to 4. */
  [[nodiscard]] virtual auto active_range() const -> std::uint32_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~voltage_range_source() = default;
};

/**
 * @brief Model told when the PWR model applies a new voltage range, at the end of its ready
 *        delay. Found by `dynamic_cast` over the machine's peers. Never called from a reset.
 */
class voltage_range_listener
{
public:
  /** @brief Creates the listener. */
  voltage_range_listener() = default;
  /** @brief Not copyable. */
  voltage_range_listener(voltage_range_listener const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(voltage_range_listener const&) -> voltage_range_listener& = delete;
  /** @brief Not movable. */
  voltage_range_listener(voltage_range_listener&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(voltage_range_listener&&) -> voltage_range_listener& = delete;

  /** @brief PWR_SVMSR.ACTVOS took a new value. */
  virtual void on_voltage_range_change() = 0;

protected:
  /** @brief Destroys the listener, only through its model. */
  ~voltage_range_listener() = default;
};
}
