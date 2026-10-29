#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace popcorn::periph::stm32
{
/**
 * @brief Text of the config_error a call raises for a pin the port lacks.
 * @param pins Number of pins of the port.
 */
[[nodiscard]] inline auto pin_range_text(std::uint8_t pins) -> std::string
{
  return std::format("a pin of the port, 0 to {}", pins - 1);
}

/**
 * @brief Text of the config_error a board-side call raises for a pin the package does not bond.
 * @param part Part name, such as `STM32U575ZGTx`.
 * @param port GPIO port name, such as `GPIOI`.
 * @param pin Pin number in the port.
 * @param bonded Bonded pins of the port, bit n for pin n.
 */
[[nodiscard]] inline auto unbonded_pin_text(
    std::string_view part,
    std::string_view port,
    std::uint8_t     pin,
    std::uint16_t    bonded) -> std::string
{
  std::string pins;
  for(std::uint8_t n = 0; n < 16; ++n)
  {
    if((bonded >> n & 1U) != 0)
    {
      pins += std::format("{}{}", pins.empty() ? "" : " ", n);
    }
  }
  return std::format(
      "a bonded pin of {}, P{}{} reaches no package pin, bonded: {}", part, port.back(), pin,
      pins.empty() ? "none" : pins);
}
}
