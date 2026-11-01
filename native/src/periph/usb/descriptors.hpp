#pragma once

#include <cstddef>
#include <cstdint>
#include <generator>
#include <span>

namespace popcorn::periph::usb
{
/**
 * @brief The descriptors packed in @p extra, in order, up to the first malformed length.
 *
 * Each element spans one descriptor, its bLength byte first. Each span views @p extra, which
 * must outlive it. The walk ends at a length below 2 or
 * past the end of @p extra.
 */
[[nodiscard]] inline auto descriptors(std::span<std::uint8_t const> extra)
    -> std::generator<std::span<std::uint8_t const>>
{
  for(std::size_t at = 0; at + 2 <= extra.size();)
  {
    auto const length = std::size_t{extra[at]};
    if(length < 2 or at + length > extra.size())
    {
      co_return;
    }
    co_yield extra.subspan(at, length);
    at += length;
  }
}
}
