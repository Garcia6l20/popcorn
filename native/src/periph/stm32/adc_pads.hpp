#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <format>

namespace popcorn::periph::stm32
{
/**
 * @brief Refuse a pad table that lacks a pad of @p role for a channel of @p channels.
 *
 * A description without `channel_pins`, as a hand-written one, passes. The shipped descriptions
 * carry a table with every external channel of each modelled ADC.
 *
 * @param desc The ADC's description.
 * @param channels Channels that need a pad, bit n for channel n.
 * @param role Input the pad carries.
 * @throws config_error The table has no pad of @p role for a channel of @p channels.
 */
inline void require_channel_pins(peripheral_desc const& desc, std::uint32_t channels, pad_role role)
{
  if(not desc.channel_pins)
  {
    return;
  }
  for(std::uint32_t channel = 0; channel < 32; ++channel)
  {
    auto const has = [&](adc_pad_desc const& pad) {
      return pad.channel == channel and pad.role == role;
    };
    if((channels >> channel & 1u) != 0 and std::ranges::none_of(*desc.channel_pins, has))
    {
      throw config_error(
          desc.name, 0, std::format("channel_pins.{}", channel),
          std::format("a {} pad of channel {}", role, channel));
    }
  }
}
}
