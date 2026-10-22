#pragma once

/**
 * @file
 * @brief The interface for a model that a bus reset bit resets, such as an STM32 RCC_xxxRSTR
 *        bit.
 */

#include <popcorn/export.hpp>
#include <popcorn/periph/model.hpp>

#include <cstdint>

namespace popcorn::periph
{
/**
 * @brief A model told when its peripheral enters a reset that a bus reset bit holds, through
 *        core::machine_services::hold_in_reset(). The machine finds it by dynamic_cast. Never
 *        called from a machine reset.
 *
 *        Entering the hold, the machine brings every register the model does not claim to its
 *        reset value, then calls on_bus_reset() on the machine thread, inside the access that
 *        set the bit and at its virtual time. While the hold lasts, the machine serves reads as
 *        usual and passes no write to the model, from any initiator. It calls on_held_write()
 *        instead, after the bus firewall, so a model can still raise what its own bus filter
 *        raises, such as an illegal-access event.
 */
class POPCORN_API bus_reset_listener
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
   * @brief The peripheral entered the reset: bring the model to its reset state. Board state
   *        outside the peripheral, such as a device on its bus, is left alone.
   */
  virtual void on_bus_reset() = 0;

  /**
   * @brief A write to a register the model claims arrived while the reset holds. The write is
   *        ignored whatever this does. The default does nothing.
   * @param offset Byte offset of the access in the peripheral.
   * @param ctx Attributes of the access.
   */
  virtual void on_held_write(std::uint32_t offset, access_ctx ctx);

protected:
  /** @brief Destroys the listener, only through its model. */
  virtual ~bus_reset_listener();
};
}
