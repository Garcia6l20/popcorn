#pragma once

#include <cstdint>
#include <format>
#include <string>

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
}
