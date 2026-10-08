#pragma once

#include "alive.hpp"
#include "gil_locks.hpp"

#include <popcorn/core/services.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <frameobject.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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
 * @brief Clears the locals of every frame of @p traceback, then copies it into entries that keep
 *        their code, globals, `tb_lasti` and `tb_lineno` in new frames, made by PyFrame_New()
 *        with no closure, no locals and no `f_back`. The GIL held.
 * @param traceback A traceback.
 * @return The copy, or nothing when a frame of it still executes or is suspended: the traceback
 *         then belongs to a call still on a stack, and stays as it is, the frames after the
 *         running one with their locals.
 * @throws reflex::py::nb::python_error when an entry cannot be read, cleared or made.
 */
inline auto detached_traceback(reflex::py::nb::handle traceback)
    -> std::optional<reflex::py::nb::object>
{
  namespace nb = reflex::py::nb;
  struct entry
  {
    nb::object code;
    nb::object globals;
    nb::object lasti;
    nb::object lineno;
  };
  std::vector<entry> entries;
  for(auto at = nb::borrow(traceback); not at.is_none(); at = at.attr("tb_next"))
  {
    auto const frame = at.attr("tb_frame");
    if(auto const cleared = nb::steal(PyObject_CallMethod(frame.ptr(), "clear", nullptr));
       not cleared.is_valid())
    {
      if(PyErr_ExceptionMatches(PyExc_RuntimeError) == 0)
      {
        throw nb::python_error();
      }
      PyErr_Clear();
      return std::nullopt;
    }
    entries.push_back({
        .code    = frame.attr("f_code"),
        .globals = frame.attr("f_globals"),
        .lasti   = at.attr("tb_lasti"),
        .lineno  = at.attr("tb_lineno"),
    });
  }
  auto const kind = nb::steal(PyObject_Type(traceback.ptr()));
  nb::object next = nb::none();
  for(auto const& found : entries | std::views::reverse)
  {
    auto const frame = nb::steal(
        reinterpret_cast<PyObject*>(PyFrame_New(
            PyThreadState_Get(), reinterpret_cast<PyCodeObject*>(found.code.ptr()),
            found.globals.ptr(), nullptr)));
    if(not frame.is_valid())
    {
      throw nb::python_error();
    }
    next = kind(next, frame, found.lasti, found.lineno);
  }
  return next;
}

/**
 * @brief Detaches @p error, the exceptions chained to it through `__cause__` and `__context__`
 *        and the members of an exception group from the frames that raised them: each frame's
 *        locals are cleared, and each traceback is replaced by detached_traceback(), unless a
 *        frame of it still executes or is suspended, which leaves that traceback as it is. Type,
 *        message, notes and formatted traceback lines stay. A replaced `tb_frame` is a new frame
 *        with no locals and no `f_back`, whose `f_lineno` is not the raise line. A failure ends
 *        the walk. Leaves the error indicator as it found it and never throws. The GIL held.
 * @param error An exception.
 */
inline void release_frames(reflex::py::nb::handle error) noexcept
{
  namespace nb                    = reflex::py::nb;
  PyObject* const         pending = PyErr_GetRaisedException();
  std::vector<PyObject*>  visited;
  std::vector<nb::object> todo{nb::borrow(error)};
  auto const              chain = [&todo](PyObject* next) {
    if(next != nullptr)
    {
      todo.push_back(nb::steal(next));
    }
  };
  try
  {
    while(not todo.empty())
    {
      auto const exception = std::move(todo.back());
      todo.pop_back();
      if(PyExceptionInstance_Check(exception.ptr()) == 0
         or std::ranges::contains(visited, exception.ptr()))
      {
        continue;
      }
      visited.push_back(exception.ptr());
      chain(PyException_GetCause(exception.ptr()));
      chain(PyException_GetContext(exception.ptr()));
      if(PyObject_IsInstance(exception.ptr(), PyExc_BaseExceptionGroup) == 1)
      {
        for(auto const member : nb::tuple(exception.attr("exceptions")))
        {
          todo.push_back(nb::borrow(member));
        }
      }
      if(auto const original = nb::steal(PyException_GetTraceback(exception.ptr()));
         original.is_valid())
      {
        if(auto const detached = detached_traceback(original))
        {
          PyException_SetTraceback(exception.ptr(), detached->ptr());
        }
      }
    }
  }
  catch(...)
  {}
  PyErr_Clear();
  PyErr_SetRaisedException(pending);
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
