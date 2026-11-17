#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief The RCC model as the PWR model sees it, found by `dynamic_cast` over the machine's
 *        peers: the clock the EPOD booster counts, RM0456 section 10.5.4.
 */
class booster_clock_source
{
public:
  /** @brief Creates the interface. */
  booster_clock_source() = default;
  /** @brief Not copyable. */
  booster_clock_source(booster_clock_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(booster_clock_source const&) -> booster_clock_source& = delete;
  /** @brief Not movable. */
  booster_clock_source(booster_clock_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(booster_clock_source&&) -> booster_clock_source& = delete;

  /** @brief Hz of the PLL1 input clock divided by PLL1MBOOST, 0 when PLL1SRC selects no clock or
   *         the oscillator it selects is not ready. */
  [[nodiscard]] virtual auto booster_input_hz() const -> std::uint64_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~booster_clock_source() = default;
};

/**
 * @brief Model told when booster_input_hz() changes. Found by `dynamic_cast` over the machine's
 *        peers. Never called from a reset.
 */
class booster_clock_listener
{
public:
  /** @brief Creates the listener. */
  booster_clock_listener() = default;
  /** @brief Not copyable. */
  booster_clock_listener(booster_clock_listener const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(booster_clock_listener const&) -> booster_clock_listener& = delete;
  /** @brief Not movable. */
  booster_clock_listener(booster_clock_listener&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(booster_clock_listener&&) -> booster_clock_listener& = delete;

  /** @brief The value booster_input_hz() returns changed. */
  virtual void on_booster_input_change() = 0;

protected:
  /** @brief Destroys the listener, only through its model. */
  ~booster_clock_listener() = default;
};
}
