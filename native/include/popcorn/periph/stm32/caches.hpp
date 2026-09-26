#pragma once

#include <chrono>

namespace popcorn::periph::stm32
{
/** @brief Board configuration of the ICACHE and DCACHE models, RM0456 sections 8 and 9. */
struct cache_options
{
  /**
   * @brief Virtual time of a full invalidate or a DCACHE maintenance command, BUSYF or BUSYCMDF
   *        set until then. RM0456 gives the invalidation time in cycles per cache size, not as
   *        one figure.
   */
  std::chrono::nanoseconds busy_delay = std::chrono::microseconds{10};
};
}
