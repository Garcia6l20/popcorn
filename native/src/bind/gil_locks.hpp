#pragma once

#include "casters.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/link_guard.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
/**
 * @brief Runs @p body under the link lock of @p queue, called with the GIL held. Tries the lock
 *        first. On contention it releases the GIL, blocks on the lock, runs @p body and unlocks
 *        before it takes the GIL back, so no thread waits on a link lock with the GIL held and
 *        none takes the GIL under one.
 * @param queue A link endpoint or USB hub, with lock() and try_lock() returning a
 *        core::link_guard.
 * @param body Called with the held core::link_guard. Copies native data out, creates and
 *        destroys no Python object.
 * @return What @p body returns.
 */
template <typename Queue, typename Body> auto link_locked(Queue const& queue, Body&& body)
{
  if(auto const held = queue.try_lock(); held.owns_lock())
  {
    return std::invoke(std::forward<Body>(body), held);
  }
  reflex::py::nb::gil_scoped_release const released;
  auto const                               held = queue.lock();
  return std::invoke(std::forward<Body>(body), held);
}

/**
 * @brief Takes the GIL on entry to a callback, refusing a thread that holds a link lock: a link
 *        lock is a leaf, its holder waits for nothing.
 */
class gil_entry
{
public:
  /** @brief Takes the GIL, nested or not. */
  gil_entry() pre(core::detail::link_locks_held == 0)
  {}

private:
  reflex::py::nb::gil_scoped_acquire held_;
};

/**
 * @brief Takes the mutex of @p on, never blocking on it with the GIL held. Tries it first. On
 *        contention it releases the GIL, blocks, and takes the GIL back with the mutex held, the
 *        allowed order. A thread that already holds a host mutex only tries.
 * @return The lock, unowned when the thread holds another host's mutex and this one is taken.
 */
inline auto lock_releasing_gil(core::host& on) -> core::held_lock
{
  if(auto held = on.try_lock(); held.owns_lock() or core::detail::host_locks_held > 0)
  {
    return held;
  }
  if(PyGILState_Check() == 0)
  {
    return on.lock();
  }
  reflex::py::nb::gil_scoped_release const released;
  return on.lock();
}

/**
 * @brief Passes an owned lock through.
 * @throws popcorn::machine_running when @p held owns nothing: the thread holds another host's
 * mutex.
 */
inline auto require_owned(core::held_lock held) -> core::held_lock
{
  if(not held.owns_lock())
  {
    throw popcorn::machine_running("the machine is in use by another machine's callback");
  }
  return held;
}

/** @brief The host of a machine, empty until its handle hands the machine over. */
struct host_slot
{
  /** @brief The host, expired once the handle destroyed it. */
  std::weak_ptr<core::host> host;
  /** @brief Guards retired, a leaf: nothing is taken under it. */
  mutable std::mutex retired_mutex{};
  /** @brief Removals retire() could not run at once, run by the engine owner at the next
   * boundary or once the host is closed. */
  mutable std::vector<std::move_only_function<void()>> retired{};
  /** @brief Whether retired may hold a removal, read without the mutex. */
  mutable std::atomic<bool> retiring{false};
};

/**
 * @brief Runs the removals queued on @p slot, on the engine owner or after the host closed.
 *        Never throws.
 */
inline void run_retired(host_slot const& slot) noexcept
{
  if(not slot.retiring.load(std::memory_order_acquire))
  {
    return;
  }
  std::vector<std::move_only_function<void()>> bodies;
  {
    std::scoped_lock const lock{slot.retired_mutex};
    bodies = std::exchange(slot.retired, {});
    slot.retiring.store(false, std::memory_order_release);
  }
  for(auto& body : bodies)
  {
    try
    {
      body();
    }
    catch(...)
    {}
  }
}

/** @brief Shared reference to a machine's host_slot, held by its views and links. */
using host_ref = std::shared_ptr<host_slot const>;

/**
 * @brief Runs @p body on the only member of @p on synchronously: inline for the engine owner,
 *        directly under the host mutex while parked, taken with lock_releasing_gil().
 * @throws popcorn::machine_running while the member is busy, or when the thread holds another
 *         host's mutex and this one is taken.
 */
template <typename Body>
auto host_access(core::host& on, Body&& body) -> std::invoke_result_t<Body, core::machine&>
{
  if(on.owns_engine())
  {
    return std::invoke(std::forward<Body>(body), on.member_inline(core::member_id{}));
  }
  auto const held = require_owned(lock_releasing_gil(on));
  if(on.busy(held, core::member_id{}) or not on.parked(held, core::member_id{}))
  {
    throw popcorn::machine_running(
        "the machine is running: await m.read() or m.write(), or halt() it first");
  }
  core::owner_scope const owning{on, held};
  return std::invoke(std::forward<Body>(body), on.member(held, core::member_id{}));
}

/**
 * @brief Whether the host of @p ref was never set: its machine is still being built.
 * @param ref The slot.
 */
inline auto host_never_set(host_ref const& ref) noexcept -> bool
{
  std::weak_ptr<core::host> const never;
  return not ref->host.owner_before(never) and not never.owner_before(ref->host);
}

/**
 * @brief host_access() through a machine's host_slot, for a view or link that reaches the
 *        engine through its own pointers. Before the host exists, while its machine is built on
 *        this thread, @p body runs directly.
 * @param ref The slot.
 * @param body Called with no argument.
 * @throws config_error once the machine's handle destroyed the host.
 */
template <typename Body>
auto host_access(host_ref const& ref, Body&& body) -> std::invoke_result_t<Body>
{
  if(auto const on = ref->host.lock())
  {
    return host_access(*on, [&body](core::machine&) { return std::invoke(body); });
  }
  if(not host_never_set(ref))
  {
    throw config_error("machine", 0, "machine", "a machine that still exists");
  }
  return std::invoke(std::forward<Body>(body));
}

/**
 * @brief Destroys what @p body owns on the engine's side: a hook handle, and what its hook
 *        feeds. Inline for the engine owner. Otherwise under the host mutex, taken with
 *        lock_releasing_gil(), as a command: at once while parked, at the next boundary on the
 *        host thread while running. When the thread holds another host's mutex and this one is
 *        taken, queued on the slot for the next boundary. Never throws, for destructors on any
 *        thread.
 * @param ref The machine's host slot.
 * @param body Destroys or releases what it captured.
 */
inline void retire(host_ref const& ref, std::move_only_function<void()> body) noexcept
{
  try
  {
    auto const on = ref->host.lock();
    if(on == nullptr or on->owns_engine())
    {
      body();
      return;
    }
    auto const held = lock_releasing_gil(*on);
    if(not held.owns_lock())
    {
      std::scoped_lock const lock{ref->retired_mutex};
      ref->retired.push_back(std::move(body));
      ref->retiring.store(true, std::memory_order_release);
      return;
    }
    std::ignore = on->post(
        held, core::member_id{}, [owned = std::move(body)](core::machine&) mutable { owned(); });
  }
  catch(...)
  {}
}
}
