#pragma once

#include <chrono>
#include <cstdint>

namespace popcorn::periph::stm32
{
/** @brief Board configuration of the RCC model, RM0456 section 11. */
struct rcc_options
{
  /** @brief HSE crystal or bypass clock frequency in Hz. */
  std::uint64_t hse_hz = 16'000'000;
  /** @brief LSE crystal or bypass clock frequency in Hz. */
  std::uint64_t lse_hz = 32'768;
  /**
   * @brief Virtual time from an oscillator enable, a PLL enable or a system clock switch to its
   *        ready flag. Shorter than the datasheet start-up times, LSE 2 s typical, so that a
   *        boot polling at 4 MHz stays fast.
   */
  std::chrono::nanoseconds ready_delay = std::chrono::microseconds{100};
  /** @brief Whether the core clock stays at the machine's rate instead of following HCLK. */
  bool fixed_core_clock = false;
};
}
