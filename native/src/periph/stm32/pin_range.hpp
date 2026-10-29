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
 * @brief The set bits of @p mask as decimal numbers joined by spaces, `none` when it has none.
 * @param mask Bit mask.
 */
[[nodiscard]] inline auto bit_list_text(std::uint32_t mask) -> std::string
{
  std::string bits;
  for(std::uint32_t n = 0; n < 32; ++n)
  {
    if((mask >> n & 1U) != 0)
    {
      bits += std::format("{}{}", bits.empty() ? "" : " ", n);
    }
  }
  return bits.empty() ? "none" : bits;
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
  return std::format(
      "a bonded pin of {}, P{}{} reaches no package pin, bonded: {}", part, port.back(), pin,
      bit_list_text(bonded));
}

/**
 * @brief Text of the config_error a board-side call raises for an ADC external channel whose pad
 *        the package does not bond.
 * @param part Part name, such as `STM32U575CGTx`.
 * @param adc ADC instance name, such as `ADC1`.
 * @param channel External channel number.
 * @param bonded Bonded external channels of the ADC, bit n for channel n.
 */
[[nodiscard]] inline auto unbonded_channel_text(
    std::string_view part,
    std::string_view adc,
    std::uint8_t     channel,
    std::uint32_t    bonded) -> std::string
{
  return std::format(
      "a bonded channel of {}, {} channel {} reaches no package pin, bonded: {}", part, adc,
      channel, bit_list_text(bonded));
}
}
