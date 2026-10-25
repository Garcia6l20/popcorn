#pragma once

#include <popcorn/core/member_link.hpp>
#include <popcorn/export.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

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

/**
 * @brief Test seam: called on the thread that resumes a sync_mode::clocks system, with the
 *        system lock and no host mutex held, after the runners restarted and before the coupling
 *        tables are rebuilt.
 */
using resume_hook = void (*)();

/** @brief The process's resume_hook, empty unless a test sets it. */
POPCORN_API extern std::atomic<resume_hook> before_couple;

/**
 * @brief Test seam: called on a member's host thread with its index and true right before it
 *        arrives at a system barrier, and with false once the barrier released it.
 */
using barrier_hook = void (*)(std::size_t member, bool entering);

/** @brief The process's barrier_hook, empty unless a test sets it. */
POPCORN_API extern std::atomic<barrier_hook> on_barrier;

/**
 * @brief Test seam: called on a member's host thread inside a sync_mode::clocks slice right
 *        after the member published @p floor from inside it and woke its successors.
 */
using progress_hook = void (*)(std::size_t member, sys_time floor);

/** @brief The process's progress_hook, empty unless a test sets it. */
POPCORN_API extern std::atomic<progress_hook> on_progress;

/**
 * @brief Test seam: called on a member's host thread at each sync_mode::clocks publish where
 *        the member could promise past its floor, right before the record is written, with
 *        whether the publish evaluated the promise, and the floor, general promise and wire
 *        promises in picoseconds it writes.
 */
using promise_hook = void (*)(
    std::size_t                   member,
    bool                          evaluated,
    sys_time                      floor,
    sys_time                      general,
    std::span<std::int64_t const> wires);

/** @brief The process's promise_hook, empty unless a test sets it. */
POPCORN_API extern std::atomic<promise_hook> on_promise;
}
