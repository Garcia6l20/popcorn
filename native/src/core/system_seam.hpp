#pragma once

#include <popcorn/export.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace popcorn::core::detail
{
/**
 * @brief Test seam: called on a member's host thread right before its sync_mode::clocks runner
 *        waits on its epoch, with the member's index, the epoch and the value it waits past, and
 *        again with a null epoch once the wait returns.
 */
using clock_wait_hook =
    void (*)(std::size_t member, std::atomic<std::uint32_t> const* epoch, std::uint32_t seen);

/** @brief The process's clock_wait_hook, empty unless a test sets it. */
POPCORN_API extern std::atomic<clock_wait_hook> on_clock_wait;
}
