#pragma once

#include <chrono>

namespace popcorn::periph::stm32
{
/** @brief Board configuration of the PWR model, RM0456 section 10 and RM0481 section 10. */
struct pwr_options
{
  /**
   * @brief Virtual time from a PWR_VOSR.VOS or BOOSTEN change to VOSRDY or BOOSTRDY on the
   *        STM32U5, RM0456 gives no single figure. The STM32H5 times PWR_VOSCR.VOS changes by
   *        its measured ACTVOS steps.
   */
  std::chrono::nanoseconds ready_delay = std::chrono::microseconds{50};
};
}
