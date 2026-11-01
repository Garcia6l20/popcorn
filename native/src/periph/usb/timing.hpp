#pragma once

#include <cstdint>

namespace popcorn::periph::usb
{
/** @brief One USB full-speed frame, 1 ms, in picoseconds. */
inline constexpr std::uint64_t usb_frame_ps = 1'000'000'000;
}
