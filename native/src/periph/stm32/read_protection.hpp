#pragma once

namespace popcorn::periph::stm32
{
/**
 * @brief A model the STM32L4 FLASH model calls, found by `dynamic_cast` over the machine's peers,
 *        when an option byte programming lowers the read protection from level 1 to level 0,
 *        RM0351 3.5.1: the model erases what that regression erases besides the flash, the SRAM2
 *        and the RTC backup registers.
 */
class read_protection_listener
{
public:
  /** @brief Creates the interface. */
  read_protection_listener() = default;
  /** @brief Not copyable. */
  read_protection_listener(read_protection_listener const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(read_protection_listener const&) -> read_protection_listener& = delete;
  /** @brief Not movable. */
  read_protection_listener(read_protection_listener&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(read_protection_listener&&) -> read_protection_listener& = delete;

  /** @brief Called once per level 1 to level 0 regression, after the flash erase, on the
   *         machine thread. */
  virtual void on_read_protection_regression() = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~read_protection_listener() = default;
};
}
