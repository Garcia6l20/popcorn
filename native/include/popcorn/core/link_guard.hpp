#pragma once

#include <mutex>

namespace popcorn::core
{
namespace detail
{
/** @brief Link locks the current thread holds through a link_guard. */
inline thread_local int link_locks_held = 0;
}

/**
 * @brief The lock of one link endpoint or USB hub, held by the current thread. A link lock is a
 *        leaf: its holder blocks on nothing, takes no GIL, no host mutex and no other lock.
 *        Counted in detail::link_locks_held while owned. Neither copied nor moved: it is
 *        returned as a prvalue and dies on the thread that took it.
 */
class link_guard
{
public:
  /**
   * @brief Takes @p mutex, blocking until it is free.
   * @param mutex The endpoint's or hub's mutex.
   */
  explicit link_guard(std::mutex& mutex) : lock_{mutex}
  {
    ++detail::link_locks_held;
  }

  /**
   * @brief Takes @p mutex if it is free, else holds nothing.
   * @param mutex The endpoint's or hub's mutex.
   */
  link_guard(std::mutex& mutex, std::try_to_lock_t) : lock_{mutex, std::try_to_lock}
  {
    if(lock_.owns_lock())
    {
      ++detail::link_locks_held;
    }
  }

  /** @brief Not copyable. */
  link_guard(link_guard const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(link_guard const&) -> link_guard& = delete;
  /** @brief Not movable. */
  link_guard(link_guard&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(link_guard&&) -> link_guard& = delete;

  /** @brief Releases the mutex, if held. */
  ~link_guard()
  {
    if(lock_.owns_lock())
    {
      --detail::link_locks_held;
    }
  }

  /** @brief Whether the mutex is held. */
  [[nodiscard]] auto owns_lock() const noexcept -> bool
  {
    return lock_.owns_lock();
  }

  /**
   * @brief Whether this guard holds a given mutex.
   * @param mutex The mutex of the endpoint or hub about to be touched.
   */
  [[nodiscard]] auto guards(std::mutex const& mutex) const noexcept -> bool
  {
    return lock_.owns_lock() and lock_.mutex() == &mutex;
  }

private:
  std::unique_lock<std::mutex> lock_;
};
}
