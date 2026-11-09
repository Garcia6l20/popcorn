#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

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
   * @brief LSI frequency in Hz before the U5 `LSIPREDIV` division. The family nominal when empty.
   *        The family's datasheet full-range limits bound it: STM32U5 30400 to 33600 Hz, STM32H5
   *        29400 to 33600, STM32L4 29500 to 34000, STM32L1 26000 to 56000.
   */
  std::optional<std::uint64_t> lsi_hz{};
  /**
   * @brief Virtual time from an oscillator or PLL enable to its ready flag. Shorter than the
   *        datasheet start-up times, LSE 2 s typical, so that a boot polling at 4 MHz stays fast.
   */
  std::chrono::nanoseconds ready_delay = std::chrono::microseconds{100};
  /** @brief Whether the core clock stays at the machine's rate instead of following HCLK. */
  bool fixed_core_clock = false;
};
}
