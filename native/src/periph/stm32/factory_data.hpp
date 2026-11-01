#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace popcorn::periph::stm32
{
/**
 * @brief Little-endian halfword of a part's region content, the factory calibration words.
 * @param part Part whose regions hold the content.
 * @param at Address of the halfword.
 * @return The halfword, or nothing when no content piece covers both bytes.
 */
[[nodiscard]] inline auto factory_halfword(part_description const& part, address at)
    -> std::optional<std::uint16_t>
{
  for(auto const& region : part.regions)
  {
    if(not region.content)
    {
      continue;
    }
    for(auto const& piece : *region.content)
    {
      auto const first = region.start + piece.offset;
      if(at >= first and at + 2 <= first + piece.data.size())
      {
        auto const index = static_cast<std::size_t>(at - first);
        return static_cast<std::uint16_t>(piece.data[index] | (piece.data[index + 1] << 8));
      }
    }
  }
  return std::nullopt;
}
}
