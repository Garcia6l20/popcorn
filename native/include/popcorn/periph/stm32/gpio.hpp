#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
/** @brief Mode of a GPIO pin, GPIOx_MODER, RM0456 section 13.4.1. */
enum class[[= reflex::derive{reflex::Format}]] pin_mode
{
  input,
  output,
  alternate,
  analog,
};

/**
 * @brief Registration of a listener. Destroying or resetting the handle removes the listener,
 *        and a handle that outlives what it listens to does nothing. Move-only.
 */
class subscription
{
public:
  /** @brief Creates an empty handle. */
  subscription() = default;

  /**
   * @brief Creates a handle that calls @p cancel once, when destroyed or reset.
   * @param cancel Removes the listener.
   */
  explicit subscription(std::move_only_function<void()> cancel) : cancel_{std::move(cancel)}
  {}

  /** @brief Not copyable. */
  subscription(subscription const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(subscription const&) -> subscription& = delete;

  /** @brief Takes over @p other's listener, leaving @p other empty. */
  subscription(subscription&& other) noexcept : cancel_{std::move(other.cancel_)}
  {
    other.cancel_ = nullptr;
  }

  /** @brief Removes the current listener, then takes over @p other's. */
  auto operator=(subscription&& other) noexcept -> subscription&
  {
    if(this != &other)
    {
      reset();
      cancel_       = std::move(other.cancel_);
      other.cancel_ = nullptr;
    }
    return *this;
  }

  /** @brief Removes the listener. */
  ~subscription()
  {
    reset();
  }

  /** @brief Removes the listener now, leaving the handle empty. */
  void reset()
  {
    if(not cancel_)
    {
      return;
    }
    auto cancel = std::move(cancel_);
    cancel_     = nullptr;
    cancel();
  }

private:
  std::move_only_function<void()> cancel_;
};

/** @brief Most edges a GPIO port keeps pending, gpio_port::drive_at. */
inline constexpr std::size_t max_pending_edges = 65536;

/**
 * @brief One GPIO port as a board sees it, implemented by the GPIO model. Reach it with
 *        `dynamic_cast<gpio_port*>(machine.model("GPIOA"))`.
 */
class POPCORN_API gpio_port
{
public:
  /** @brief Creates the interface. */
  gpio_port() = default;
  /** @brief Not copyable. */
  gpio_port(gpio_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(gpio_port const&) -> gpio_port& = delete;
  /** @brief Not movable. */
  gpio_port(gpio_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(gpio_port&&) -> gpio_port& = delete;

  /**
   * @brief Drives a pin from outside the MCU, or releases it. The drive is board state: it
   *        survives an MCU reset and a snapshot restores it.
   * @param pin Pin number, 0 to 15.
   * @param level Level driven, empty to release the pin to its pull.
   */
  virtual void drive(std::uint8_t pin, std::optional<bool> level) = 0;

  /**
   * @brief Drives a pin from outside the MCU, or releases it, at exactly a virtual time, through
   *        an event of the model on core::host_lane: after the model events due at that
   *        instant, edges of one instant in the order they were given. Pending edges are board
   *        state: snapshots save them, and an MCU reset of any kind keeps each at its instant,
   *        its remaining delay counted from the reset. After a reset, edges of one instant on
   *        different ports apply in port order. drive() leaves them in place.
   * @param pin Pin number, 0 to 15.
   * @param level Level driven, empty to release the pin to its pull.
   * @param at Time of the edge, machine time since reset, not before now.
   * @throws std::invalid_argument when @p at lies before the machine time.
   * @throws config_error when the port already keeps max_pending_edges pending edges.
   */
  virtual void drive_at(std::uint8_t pin, std::optional<bool> level, vtime at) = 0;

  /** @brief Number of edges drive_at() scheduled that have not applied yet. */
  [[nodiscard]] virtual auto pending_edges() const -> std::size_t = 0;

  /**
   * @brief Level a pin of this port gives to an alternate function input, such as `tim3_ch1`:
   *        high when a pin in alternate function mode whose AFSEL selects that signal reads
   *        high. Every change of it reaches the trigger_sink peers listening to the signal, at
   *        the time of the edge that caused it.
   * @param signal Signal name in lower case, as the description's alternates spell it.
   * @return The level, empty when no pin of the port carries the signal at any AF.
   */
  [[nodiscard]] virtual auto signal_level(std::string_view signal) const -> std::optional<bool> = 0;

  /**
   * @brief Level of a pin as GPIOx_IDR reads it: the output for a push-pull output, the external
   *        drive or else the pull for an input, an alternate function or a released open-drain
   *        output, 0 in analog mode.
   * @param pin Pin number, 0 to 15.
   */
  [[nodiscard]] virtual auto level(std::uint8_t pin) const -> bool = 0;

  /**
   * @brief Mode of a pin.
   * @param pin Pin number, 0 to 15.
   */
  [[nodiscard]] virtual auto mode(std::uint8_t pin) const -> pin_mode = 0;

  /**
   * @brief Alternate function number of a pin, GPIOx_AFRL or GPIOx_AFRH.
   * @param pin Pin number, 0 to 15.
   */
  [[nodiscard]] virtual auto alternate(std::uint8_t pin) const -> std::uint8_t = 0;

  /**
   * @brief Adds a listener called on every pin level change after the change took effect. A
   *        machine reset changes levels silently: a listener reads levels again after one.
   * @param listener Called with the pin and its new level.
   * @return Handle whose destruction removes the listener.
   */
  [[nodiscard]] virtual auto
      on_change(std::copyable_function<void(std::uint8_t pin, bool level)> listener)
          -> subscription = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~gpio_port();
};

/** @brief Input of the EXTI model from the GPIO models of the same machine. */
class POPCORN_API exti_input
{
public:
  /** @brief Creates the interface. */
  exti_input() = default;
  /** @brief Not copyable. */
  exti_input(exti_input const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(exti_input const&) -> exti_input& = delete;
  /** @brief Not movable. */
  exti_input(exti_input&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(exti_input&&) -> exti_input& = delete;

  /**
   * @brief A GPIO pin changed level.
   * @param port Port index, 0 for GPIOA to 8 for GPIOI, as EXTI_EXTICRn encodes it.
   * @param pin Pin number, 0 to 15, the EXTI line it can feed.
   * @param level New level.
   */
  virtual void pin_changed(std::uint8_t port, std::uint8_t pin, bool level) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~exti_input();
};
}
