#pragma once

namespace popcorn::periph
{
/**
 * @brief A model that freezes part of its state while the machine holds its core, see
 *        core::machine::hold_core(). The machine finds it among its models by dynamic_cast and
 *        calls it in description order.
 */
class core_hold_listener
{
public:
  /**
   * @brief Called on every hold and release, and with true again after a reset of a held
   *        machine, once every model has reset. Not called by a restore: the frozen state comes
   *        back with the model's saved bytes. A hold may throw, only before it changed any
   *        state. A release never throws. Work a hold applies at the hold, such as a timer's
   *        updates due by then, an RTC event or a DMA burst, may fail as in an event: that
   *        failure propagates out of core::machine::hold_core() after its rollback.
   * @param held True on a hold, false on the release.
   * @throws Any exception, on a hold only, such as model_error under strict_peripherals.
   */
  virtual void on_core_hold(bool held) = 0;

protected:
  ~core_hold_listener() = default;
};
}
