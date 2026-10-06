#pragma once

#include "gil_locks.hpp"

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
/**
 * @brief A copy of the current `contextvars` context, PyContext_CopyCurrent(), called with the
 *        GIL held.
 */
inline auto capture_context() -> reflex::py::nb::object
{
  return reflex::py::nb::steal(PyContext_CopyCurrent());
}

/**
 * @brief Enters a captured context for a callback, unless the current thread already entered
 *        it: a model whose access reaches itself nests the same context. Called with the GIL
 *        held.
 */
class context_entry
{
public:
  /**
   * @brief Enters @p context.
   * @param context A context from capture_context(), or None to stay in the current one.
   * @throws reflex::py::nb::python_error when the context is entered on another thread.
   */
  explicit context_entry(reflex::py::nb::handle context)
  {
    if(context.is_none() or std::ranges::contains(entered(), context.ptr()))
    {
      return;
    }
    if(PyContext_Enter(context.ptr()) != 0)
    {
      throw reflex::py::nb::python_error();
    }
    context_ = context.ptr();
    entered().push_back(context_);
  }
  /** @brief Not copyable. */
  context_entry(context_entry const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(context_entry const&) -> context_entry& = delete;
  /** @brief Leaves the context if this entry entered it. */
  ~context_entry()
  {
    if(context_ != nullptr)
    {
      entered().pop_back();
      PyContext_Exit(context_);
    }
  }

private:
  static auto entered() -> std::vector<PyObject*>&
  {
    thread_local std::vector<PyObject*> contexts;
    return contexts;
  }

  PyObject* context_ = nullptr;
};

/**
 * @brief Enters, for a callback, the `context` attribute of a Python cell native code holds only
 *        weakly, so a cycle through the context stays visible to the cycle collector. A dead
 *        cell, after its machine's Python side was collected, leaves the current context.
 *        Called with the GIL held.
 */
class cell_entry
{
public:
  /**
   * @brief Enters the cell's context.
   * @param weak_cell A weak reference to the cell, or None to stay in the current context.
   * @throws reflex::py::nb::python_error when the context is entered on another thread.
   */
  explicit cell_entry(reflex::py::nb::handle weak_cell)
  {
    if(weak_cell.is_none())
    {
      return;
    }
    PyObject* cell = nullptr;
    if(PyWeakref_GetRef(weak_cell.ptr(), &cell) < 0)
    {
      throw reflex::py::nb::python_error();
    }
    if(cell == nullptr)
    {
      return;
    }
    cell_    = reflex::py::nb::steal(cell);
    context_ = cell_.attr("context");
    entered_.emplace(context_);
  }

private:
  reflex::py::nb::object       cell_;
  reflex::py::nb::object       context_;
  std::optional<context_entry> entered_;
};

/**
 * @brief A Python callable native code keeps, with the context it runs in, both released under
 *        the GIL wherever they are dropped.
 */
struct python_callable
{
  /**
   * @brief Keeps a callable.
   * @param fn The callable, a reference taken with the GIL held.
   * @param context Its context from capture_context(), or None for the caller's.
   */
  python_callable(reflex::py::nb::object fn, reflex::py::nb::object context)
      : fn{std::move(fn)}, context{std::move(context)}
  {}
  /** @brief Not copyable. */
  python_callable(python_callable const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(python_callable const&) -> python_callable& = delete;
  /** @brief Drops both references with the GIL held. */
  ~python_callable()
  {
    gil_entry const held;
    fn.reset();
    context.reset();
  }

  /** @brief Calls the callable in its context, the GIL held by the caller. */
  template <typename... Args> auto operator()(Args&&... args) const -> reflex::py::nb::object
  {
    context_entry const entered{context};
    return fn(std::forward<Args>(args)...);
  }

  /** @brief The callable. */
  reflex::py::nb::object fn;
  /** @brief The context it runs in. */
  reflex::py::nb::object context;
};

/**
 * @brief Shares a Python callable with native callbacks, in a copy of the current context.
 * @param fn The callable, the GIL held.
 */
inline auto held(reflex::py::nb::object fn) -> std::shared_ptr<python_callable>
{
  return std::make_shared<python_callable>(std::move(fn), capture_context());
}

/**
 * @brief Shares a Python callable with native callbacks, run in whatever context the calling
 *        thread is in. For a callable that enters its own context on the Python side, where the
 *        cycle collector sees it: the `Relay` of a hook or log sink.
 * @param fn The callable, the GIL held.
 */
inline auto held_bare(reflex::py::nb::object fn) -> std::shared_ptr<python_callable>
{
  return std::make_shared<python_callable>(std::move(fn), reflex::py::nb::none());
}
}
