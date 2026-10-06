#pragma once

#include <popcorn/core/link_guard.hpp>

#include <reflex/py.hpp>

#include <functional>
#include <utility>

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
}
