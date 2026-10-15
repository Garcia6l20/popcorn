#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

namespace popcorn::periph::stm32
{
/**
 * @brief The bus side of a built-in I2C model, which a host drives as another controller would.
 *        Called on the machine's thread, as engine owner.
 */
class POPCORN_API i2c_bus
{
public:
  /**
   * @brief Another controller owns the bus from @p from to @p until, machine times since reset.
   *        BUSY reads 1 meanwhile, while PE is set, and a START the firmware requests waits for
   *        the release. A transfer of the firmware running at @p from keeps the bus, and the
   *        hold starts at that transfer's STOP, at the end of a lost arbitration, or when PE is
   *        cleared, its end unchanged. A hold not over yet, pending or
   *        owning the bus, is replaced: the one owning the bus keeps it until its own end or
   *        the replacement's start, whichever is first. A reset drops it.
   * @param from Start, not before the current time.
   * @param until End, after @p from.
   */
  virtual void hold(vtime from, vtime until) = 0;

protected:
  /** @brief Destroyed through its model only. */
  virtual ~i2c_bus();
};
}
