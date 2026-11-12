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

  /** @brief Performance level applied to VCORE, 0 the highest the family offers. The STM32U5
   *         range 1 to 4 of PWR_SVMSR.ACTVOS are the levels 0 to 3. */
  [[nodiscard]] virtual auto active_level() const -> std::uint32_t = 0;

  /** @brief Performance level the VOS field selects, equal to active_level() when no change is
   *         pending. A change in flight applies it at the end of its ready delay. */
  [[nodiscard]] virtual auto target_level() const -> std::uint32_t = 0;

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

/**
 * @brief Model asked by the PWR model before it starts a change to a lower performance level.
 *        Found by `dynamic_cast` over the machine's peers.
 */
class voltage_range_guard
{
public:
  /** @brief Creates the guard. */
  voltage_range_guard() = default;
  /** @brief Not copyable. */
  voltage_range_guard(voltage_range_guard const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(voltage_range_guard const&) -> voltage_range_guard& = delete;
  /** @brief Not movable. */
  voltage_range_guard(voltage_range_guard&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(voltage_range_guard&&) -> voltage_range_guard& = delete;

  /**
   * @brief A write selects a lower performance level.
   * @param level The level the write selects.
   * @return Whether the running configuration is outside its limits at @p level. The guard
   *         has logged it.
   */
  [[nodiscard]] virtual auto voltage_range_refused(std::uint32_t level) -> bool = 0;

protected:
  /** @brief Destroys the guard, only through its model. */
  ~voltage_range_guard() = default;
};
}
