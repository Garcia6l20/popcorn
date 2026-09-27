#pragma once

#include <reflex/py.hpp>

#include <memory>
#include <utility>

namespace popcorn::bind::detail
{
/** @brief A Python object native code keeps, released under the GIL wherever it is dropped. */
struct python_callable
{
  /**
   * @brief Keeps a Python object.
   * @param fn The object, a reference taken with the GIL held.
   */
  explicit python_callable(reflex::py::nb::object fn) : fn{std::move(fn)}
  {}
  /** @brief Not copyable. */
  python_callable(python_callable const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(python_callable const&) -> python_callable& = delete;
  /** @brief Drops the reference with the GIL held. */
  ~python_callable()
  {
    reflex::py::nb::gil_scoped_acquire held;
    fn.reset();
  }

  /** @brief The object. */
  reflex::py::nb::object fn;
};

/**
 * @brief Shares a Python object with native callbacks.
 * @param fn The object, the GIL held.
 */
inline auto held(reflex::py::nb::object fn) -> std::shared_ptr<python_callable>
{
  return std::make_shared<python_callable>(std::move(fn));
}
}
