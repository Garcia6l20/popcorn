#pragma once

#include <popcorn/core/clock.hpp>

namespace popcorn::periph::stm32
{
/**
 * @brief The RCC model as a peripheral with a kernel clock sees it, found by `dynamic_cast` over
 *        the machine's peers. A kernel domain reads 0 Hz both when the peripheral's bus enable
 *        is clear and when its selected source is stopped. This tells the two apart.
 */
class kernel_gate
{
public:
  /** @brief Creates the interface. */
  kernel_gate() = default;
  /** @brief Not copyable. */
  kernel_gate(kernel_gate const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(kernel_gate const&) -> kernel_gate& = delete;
  /** @brief Not movable. */
  kernel_gate(kernel_gate&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(kernel_gate&&) -> kernel_gate& = delete;

  /**
   * @brief Whether the bus enable of the peripheral clocked by @p kernel is set, such as
   *        RCC_AHB2ENR1.RNGEN for the `rng` domain.
   * @param kernel Kernel clock domain of the peripheral.
   * @return The enable bit, true for a domain without a kernel clock mux, such as a timer.
   */
  [[nodiscard]] virtual auto bus_enabled(core::clock_domain kernel) const -> bool = 0;

  /**
   * @brief Whether RCC holds the peripheral clocked by @p kernel in reset, such as
   *        RCC_AHB2RSTR1.RNGRST 1 for the `rng` domain.
   * @param kernel Kernel clock domain of the peripheral.
   * @return The reset bit, false for a domain whose reset bit is not modelled.
   */
  [[nodiscard]] virtual auto held_in_reset(core::clock_domain kernel) const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~kernel_gate() = default;
};

/**
 * @brief Model told when RCC puts a peripheral in reset through its RCC_xxxRSTR bit. Found by
 *        `dynamic_cast` over the machine's peers. Never called from a model's reset.
 */
class bus_reset_listener
{
public:
  /** @brief Creates the listener. */
  bus_reset_listener() = default;
  /** @brief Not copyable. */
  bus_reset_listener(bus_reset_listener const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(bus_reset_listener const&) -> bus_reset_listener& = delete;
  /** @brief Not movable. */
  bus_reset_listener(bus_reset_listener&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(bus_reset_listener&&) -> bus_reset_listener& = delete;

  /**
   * @brief The reset bit of the peripheral clocked by @p kernel went from 0 to 1.
   * @param kernel Kernel clock domain of the peripheral put in reset.
   */
  virtual void on_bus_reset(core::clock_domain kernel) = 0;

protected:
  /** @brief Destroys the listener, only through its model. */
  ~bus_reset_listener() = default;
};
}
