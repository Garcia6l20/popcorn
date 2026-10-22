#pragma once

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/bus_reset.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

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
   * @brief The reset line of @p instance, its bit in an RCC_xxxRSTR register.
   * @param instance Instance name as the description spells it.
   * @return A handle for held_in_reset(), nullopt for an instance with no reset bit.
   */
  [[nodiscard]] virtual auto reset_line(std::string_view instance) const
      -> std::optional<std::size_t> = 0;

  /**
   * @brief Whether RCC holds a peripheral in reset, its RCC_xxxRSTR bit 1.
   * @param line Handle from reset_line().
   * @return The reset bit.
   */
  [[nodiscard]] virtual auto held_in_reset(std::size_t line) const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~kernel_gate() = default;
};
}
