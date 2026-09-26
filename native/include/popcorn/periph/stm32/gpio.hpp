#pragma once

#include <reflex/enum.hpp>

#include <cstdint>
#include <functional>
#include <optional>
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
  explicit subscription(std::function<void()> cancel) : cancel_{std::move(cancel)}
  {}

  /** @brief Not copyable. */
  subscription(subscription const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(subscription const&) -> subscription& = delete;

  /** @brief Takes over @p other's listener, leaving @p other empty. */
  subscription(subscription&& other) noexcept : cancel_{std::exchange(other.cancel_, {})}
  {}

  /** @brief Removes the current listener, then takes over @p other's. */
  auto operator=(subscription&& other) noexcept -> subscription&
  {
    if(this != &other)
    {
      reset();
      cancel_ = std::exchange(other.cancel_, {});
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
    if(auto cancel = std::exchange(cancel_, {}))
    {
      cancel();
    }
  }

private:
  std::function<void()> cancel_;
};

/**
 * @brief One GPIO port as a board sees it, implemented by the GPIO model. Reach it with
 *        `dynamic_cast<gpio_port*>(machine.model("GPIOA"))`.
 */
class gpio_port
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
  [[nodiscard]] virtual auto on_change(std::function<void(std::uint8_t pin, bool level)> listener)
      -> subscription = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~gpio_port() = default;
};

/** @brief Input of the EXTI model from the GPIO models of the same machine. */
class exti_input
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
  ~exti_input() = default;
};
}
