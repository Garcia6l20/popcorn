#pragma once

#include <chrono>

namespace popcorn::periph::stm32
{
/** @brief Board configuration of the PWR model, RM0456 section 10, RM0481 section 10, RM0038
 *         section 5 and RM0351 section 5. */
struct pwr_options
{
  /**
   * @brief Virtual time from a VOS change to its ready flag: PWR_VOSR VOSRDY and BOOSTRDY on the
   *        STM32U5, where RM0456 gives no single figure, PWR_CSR VOSF on the STM32L1, PWR_SR2
   *        VOSF on the STM32L47x/L48x, where DS10198 Table 53 gives tVOST 20 us typical, 40 us
   *        maximum. The STM32H5 times VOS changes by its measured ACTVOS steps.
   */
  std::chrono::nanoseconds ready_delay = std::chrono::microseconds{50};
};
}
