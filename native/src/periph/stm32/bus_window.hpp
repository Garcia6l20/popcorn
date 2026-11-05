#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief A bus address window, both ends included, and the clock domain of the bus. */
struct bus_window
{
  /** @brief First address. */
  std::uint32_t first;
  /** @brief Last address. */
  std::uint32_t last;
  /** @brief Clock domain of the bus, as the RCC model names it, such as `pclk1`. */
  std::string_view clock = {};
};

/**
 * @brief The window of @p windows holding @p address.
 * @return The window, or null when none holds it.
 */
[[nodiscard]] constexpr auto window_of(std::span<bus_window const> windows, std::uint32_t address)
    -> bus_window const*
{
  for(auto const& window : windows)
  {
    if(address >= window.first and address <= window.last)
    {
      return &window;
    }
  }
  return nullptr;
}

/** @brief STM32L1 APB1 and APB2, RM0038 Table 5. */
inline constexpr std::array<bus_window, 2> l1_apb_windows{
    {
     {.first = 0x4000'0000, .last = 0x4000'7FFF, .clock = "pclk1"},
     {.first = 0x4001'0000, .last = 0x4001'3FFF, .clock = "pclk2"},
     }
};

/** @brief STM32L47x/L48x APB1 and APB2, RM0351 2.2.2 Table 1. */
inline constexpr std::array<bus_window, 2> l4_apb_windows{
    {
     {.first = 0x4000'0000, .last = 0x4000'97FF, .clock = "pclk1"},
     {.first = 0x4001'0000, .last = 0x4001'63FF, .clock = "pclk2"},
     }
};
}
