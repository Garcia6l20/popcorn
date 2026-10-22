#pragma once

/**
 * @file
 * @brief The interface for a model that must finish work due before a reset.
 */

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

namespace popcorn::periph
{
/**
 * @brief A model that finishes work due before a reset while the clock still holds the reset's
 *        time. The machine finds it among its models by dynamic_cast and calls it in description
 *        order, for every reset kind, a reset the firmware or a watchdog requests included,
 *        before the scheduler and the clock are reset and before any model's reset(). In a
 *        power cycle it runs first, before any model's power_cut() and before RAM is zeroed,
 *        and once.
 */
class POPCORN_API reset_listener
{
public:
  /**
   * @brief Called at the start of every reset, with the model's state, its registers and the
   *        clock as they are at the reset's time, so `model_context::clock` gives that time.
   *        Lazily computed state brought up to that time here is what reset() then discards or
   *        keeps. A throw leaves the machine unreset.
   * @param kind Cause of the coming reset.
   * @throws Any exception, such as model_error under strict_peripherals.
   */
  virtual void before_reset(reset_kind kind) = 0;

protected:
  virtual ~reset_listener();
};
}
