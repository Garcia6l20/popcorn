#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <cstdint>
#include <functional>
#include <string_view>

namespace popcorn::periph::stm32
{
/**
 * @brief The clocks the RCC drives onto pins, as a board sees them, implemented by the RCC model.
 *        Reach it with `dynamic_cast<clock_output_port*>(machine.model("RCC"))`. A pin that no
 *        clock output uses reads 0 Hz. The rate does not depend on the GPIO mode of the pin: the
 *        pin carries the clock as an additional function.
 */
class POPCORN_API clock_output_port
{
public:
  /** @brief Creates the interface. */
  clock_output_port() = default;
  /** @brief Not copyable. */
  clock_output_port(clock_output_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(clock_output_port const&) -> clock_output_port& = delete;
  /** @brief Not movable. */
  clock_output_port(clock_output_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(clock_output_port&&) -> clock_output_port& = delete;

  /**
   * @brief Frequency the RCC drives onto a pin now.
   * @param port GPIO port instance name, such as `GPIOA`.
   * @param pin Pin number.
   * @return The frequency in Hz, 0 when no clock output is enabled on the pin.
   */
  [[nodiscard]] virtual auto clock_hz(std::string_view port, std::uint8_t pin) const
      -> std::uint64_t = 0;

  /**
   * @brief Adds a watcher called once for every change of the frequency on a pin, with the new
   *        frequency and the time of the change, the first instruction boundary at or after it: a
   *        register write stamps at the end of its block, a source still starting at the boundary
   *        after its ready instant. The call is a model event on core::host_lane at that time, so
   *        it runs on the machine's thread at the next event dispatch, never inside the write that
   *        changed the clock. A machine reset drops the calls still pending, then calls once with
   *        the new frequency at the reset time when it changed the frequency. A restore changes
   *        the frequency with no call and brings back the calls its snapshot holds. A pin that no
   *        clock output uses never calls.
   * @param port GPIO port instance name.
   * @param pin Pin number.
   * @param watcher Called with the new frequency in Hz and the change time, machine time since
   *        reset.
   * @return Handle whose destruction removes the watcher, which then receives no call posted
   *         before.
   */
  [[nodiscard]] virtual auto watch_clock(
      std::string_view                                          port,
      std::uint8_t                                              pin,
      std::move_only_function<void(std::uint64_t hz, vtime at)> watcher) -> subscription = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~clock_output_port() = default;
};
}
