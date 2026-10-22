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

protected:
  /** @brief Destroys the interface, only through its model. */
  ~kernel_gate() = default;
};
}
