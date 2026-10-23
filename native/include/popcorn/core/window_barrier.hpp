#pragma once

#include <popcorn/export.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace popcorn::core
{
/** @brief CPUs the calling thread may run on, from its affinity mask, at least 1. */
[[nodiscard]] POPCORN_API auto usable_cpus() noexcept -> std::size_t;

/**
 * @brief Reusable barrier for a fixed number of threads. The last party to arrive runs the
 *        completion before releasing the others, so the completion sees every write the parties
 *        made before arriving and every party sees the completion's writes after returning.
 *        Waits sleep on a raw futex. The barrier owns no mutex. It must
 *        outlive the return of every arrive_and_wait() and abort() call: a released party may
 *        return before the last arriver or the aborting thread is done with the barrier.
 */
class POPCORN_API window_barrier
{
public:
  /** @param parties Threads that arrive each round. */
  explicit window_barrier(std::size_t parties) pre(parties > 0)
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
  std::uint32_t parties_;
};
}
