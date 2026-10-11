#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace popcorn::core
{
/** @brief How a party of a window_barrier waits for the others. */
struct wait_policy
{
  /** @brief Wall time spent spinning before sleeping on the futex, 0, the default, to sleep at
   * once. Spinning gained nothing measurable on a three-member linked system with free cores and
   * lost up to 2.6x under load or with fewer cores than members. */
  std::chrono::nanoseconds spin{0};
};

/** @brief CPUs the calling thread may run on, from its affinity mask, at least 1. */
[[nodiscard]] auto usable_cpus() noexcept -> std::size_t;

/**
 * @brief The wait policy for @p parties threads: @p base, or a spin of 0 when the parties
 *        outnumber usable_cpus(), since a spinning waiter then keeps a late party off a core.
 * @param parties Threads that arrive each round.
 * @param base Policy used while every party can have its own CPU.
 */
[[nodiscard]] auto default_wait_policy(std::size_t parties, wait_policy base = {}) noexcept
    -> wait_policy;

/**
 * @brief Reusable barrier for a fixed number of threads. The last party to arrive runs the
 *        completion before releasing the others, so the completion sees every write the parties
 *        made before arriving and every party sees the completion's writes after returning.
 *        Waits spin then sleep on a raw futex, never yield. The barrier owns no mutex. It must
 *        outlive the return of every arrive_and_wait() and abort() call: a released party may
 *        return before the last arriver or the aborting thread is done with the barrier.
 */
class window_barrier
{
public:
  /**
   * @param parties Threads that arrive each round.
   * @param policy How they wait.
   */
  window_barrier(std::size_t parties, wait_policy policy) pre(parties > 0)
      pre(parties <= std::numeric_limits<std::uint32_t>::max());

  /** @brief Not copyable. */
  window_barrier(window_barrier const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(window_barrier const&) -> window_barrier& = delete;

  /**
   * @brief Arrives and waits for the others. The last arriver runs @p complete with no lock
   *        held, then releases every party. Every party of a round passes the same completion.
   * @return False when abort() was called, before or during the wait. An arrival after abort()
   *         returns at once and runs no completion.
   */
  auto arrive_and_wait(std::function_ref<void() noexcept> complete) -> bool;

  /** @brief Releases every waiter now and makes every later arrival return false. Any thread. */
  void abort() noexcept;

  /** @brief Whether abort() was called. Any thread. */
  [[nodiscard]] auto aborted() const noexcept -> bool;

  /** @brief Policy used from the next wait. Any thread. */
  void set_policy(wait_policy policy) noexcept;

  /** @brief The policy of the next wait. */
  [[nodiscard]] auto policy() const noexcept -> wait_policy;

  /** @brief Threads that arrive each round. */
  [[nodiscard]] auto parties() const noexcept -> std::size_t;

  /** @brief Futex sleeps of every party so far, for tests and benches. */
  [[nodiscard]] auto sleeps() const noexcept -> std::uint64_t;

private:
  void wait_past(std::uint32_t seen) noexcept;

  alignas(64) std::atomic<std::uint32_t> generation_{0};
  alignas(64) std::atomic<std::uint32_t> arrived_{0};
  alignas(64) std::atomic<std::uint32_t> sleepers_{0};
  std::atomic<std::uint64_t> sleeps_{0};
  alignas(64) std::atomic<bool> aborted_{false};
  std::uint32_t             parties_;
  std::atomic<std::int64_t> spin_ns_;
};
}
