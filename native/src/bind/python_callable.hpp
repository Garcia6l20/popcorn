#pragma once

#include "alive.hpp"
#include "frames.hpp"
#include "gil_locks.hpp"

#include <popcorn/core/services.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
/** @brief Detail of the stop a callback_cell requests once its machine is collected. */
inline constexpr std::string_view collected_detail =
    "a model or device of a collected Machine was called";

/**
 * @brief A copy of the current `contextvars` context, PyContext_CopyCurrent(), called with the
 *        GIL held.
 */
inline auto capture_context() -> reflex::py::nb::object
{
  return reflex::py::nb::steal(PyContext_CopyCurrent());
}

/**
 * @brief Runs @p body, a call into Python with the GIL held. On a host thread, an exception it
 *        raises is detached from its frames through release_frames() before it leaves, so
 *        nothing the host thread carries past the GIL's release keeps a collectable machine
 *        reachable.
 * @return What @p body returns.
 * @throws reflex::py::nb::python_error, rethrown from @p body.
 */
template <typename Body> auto call_python(Body&& body) -> decltype(std::forward<Body>(body)())
{
  try
  {
    return std::forward<Body>(body)();
  }
  catch(reflex::py::nb::python_error const& raised)
  {
    if(core::detail::on_host_thread)
    {
      release_frames(raised.value());
    }
    throw;
  }
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
 *        cell, after its machine's Python side was collected, enters nothing and reports dead():
 *        the callback must not run. Called with the GIL held.
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
      dead_ = true;
      return;
    }
    cell_    = reflex::py::nb::steal(cell);
    context_ = cell_.attr("context");
    entered_.emplace(context_);
  }

  /** @brief Whether the cell was given and is collected, its machine's Python side gone. */
  [[nodiscard]] auto dead() const noexcept -> bool
  {
    return dead_;
  }

private:
  bool                         dead_ = false;
  reflex::py::nb::object       cell_;
  reflex::py::nb::object       context_;
  std::optional<context_entry> entered_;
};

/**
 * @brief Where a Python model or bus device runs its calls: in the context of its machine's cell,
 *        and only while that machine's Python side lives. Once the machine is destroyed, its
 *        lifetime flag clear, a call is refused silently: its services may be gone. Once its
 *        `Machine` is found garbage, the collected flag set by `Machine.__del__` or the cell dead
 *        before `__del__` runs, a call is refused and the run asked to stop with
 *        stop_reason::user and collected_detail, on a machine a finalizer resurrected too.
 */
class callback_cell
{
public:
  callback_cell() = default;
  /** @brief Not copyable. */
  callback_cell(callback_cell const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(callback_cell const&) -> callback_cell& = delete;
  /**
   * @brief Drops the cell's reference on any thread: nested under a GIL already held, taken on
   *        an abandoned machine's thread that destroys its attachments, leaked once the GIL is
   *        unavailable.
   */
  ~callback_cell()
  {
    if(not gil_entry::available())
    {
      std::ignore = cell_.release();
      return;
    }
    gil_entry const held;
    cell_.reset();
  }

  /**
   * @brief Attaches the calls to a machine being built, whose services are not known yet.
   * @param cell A weak reference to the machine's cell, or None, the GIL held.
   * @param alive The machine's lifetime flag.
   * @param collected The machine's collected flag.
   */
  void run_in(reflex::py::nb::object cell, alive_flag alive, alive_flag collected)
  {
    cell_      = std::move(cell);
    alive_     = std::move(alive);
    collected_ = std::move(collected);
    services_  = nullptr;
  }

  /** @brief Sets the services of the machine run_in() attached the calls to. */
  void stop_through(core::machine_services& services) noexcept
  {
    services_ = &services;
  }

  /**
   * @brief Runs @p body in the cell's context, the GIL held.
   * @return What @p body returns, empty when the call is refused.
   */
  template <typename Body> auto call(Body&& body) const -> std::optional<reflex::py::nb::object>
  {
    if(alive_ != nullptr and not is_alive(alive_))
    {
      return std::nullopt;
    }
    if(collected_ != nullptr and is_alive(collected_))
    {
      stop_collected();
      return std::nullopt;
    }
    cell_entry const entered{cell_};
    if(entered.dead())
    {
      stop_collected();
      return std::nullopt;
    }
    return call_python(std::forward<Body>(body));
  }

private:
  void stop_collected() const
  {
    if(services_ != nullptr)
    {
      services_->request_stop(stop_reason::user, std::string{collected_detail});
    }
  }

  reflex::py::nb::object  cell_      = reflex::py::nb::none();
  alive_flag              alive_     = nullptr;
  alive_flag              collected_ = nullptr;
  core::machine_services* services_  = nullptr;
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
  /** @brief Drops both references with the GIL held, taking it only when one is left. */
  ~python_callable()
  {
    if(not fn.is_valid() and not context.is_valid())
    {
      return;
    }
    if(not gil_entry::available())
    {
      std::ignore = fn.release();
      std::ignore = context.release();
      return;
    }
    gil_entry const held;
    fn.reset();
    context.reset();
  }

  /** @brief Drops both references, the GIL held by the caller, so the destructor takes no GIL. */
  void drop() noexcept
  {
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
