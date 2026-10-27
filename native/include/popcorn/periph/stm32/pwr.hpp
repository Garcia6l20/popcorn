#pragma once

#include <chrono>

namespace popcorn::periph::stm32
{
/** @brief Board configuration of the PWR model, RM0456 section 10 and RM0481 section 10. */
struct pwr_options
{
  /**
   * @brief Virtual time from a PWR_VOSR.VOS or BOOSTEN change to VOSRDY or BOOSTRDY, or from a
   *        PWR_VOSCR.VOS change to VOSRDY. RM0456 and RM0481 give no single figure.
   */
  std::chrono::nanoseconds ready_delay = std::chrono::microseconds{50};
};
}
