#pragma once

#include "gil_locks.hpp"

#include <popcorn/core/host.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace popcorn::bind::detail
{
/**
 * @brief A weak reference to a machine's Python cell, released under the GIL wherever it is
 *        dropped.
 */
class python_cell
{
public:
  /**
   * @brief Holds @p weak_cell.
   * @param weak_cell A weak reference to a cell with a `held` dict attribute, the GIL held.
   */
  explicit python_cell(reflex::py::nb::object weak_cell) : weak_{std::move(weak_cell)}
  {}

  python_cell(python_cell const&)                    = delete;
  auto operator=(python_cell const&) -> python_cell& = delete;

  ~python_cell()
  {
    if(not gil_entry::available())
    {
      std::ignore = weak_.release();
      return;
    }
    gil_entry const held;
    weak_.reset();
  }

  /**
   * @brief The cell, invalid once it was collected. The GIL held.
   * @throws reflex::py::nb::python_error when the weak reference cannot be read.
   */
  [[nodiscard]] auto live() const -> reflex::py::nb::object
  {
    PyObject* cell = nullptr;
    if(PyWeakref_GetRef(weak_.ptr(), &cell) < 0)
    {
      throw reflex::py::nb::python_error();
    }
    return reflex::py::nb::steal(cell);
  }

private:
  reflex::py::nb::object weak_;
};

/**
 * @brief The Python exception that ended one free run, as the host holds it: a token into the
 *        `held` dict of the machine's Python cell, which keeps the exception until the last copy
 *        of this end is destroyed.
 *
 * The host holds an end natively, out of the cycle collector's sight. The exception's traceback
 * reaches the machine through its frames, so holding the exception itself would keep the machine
 * and its host thread alive forever. Through the cell the cycle stays collectable.
 */
class held_python_end
{
public:
  /**
   * @brief Stores @p error in the cell under @p token, the GIL held.
   * @return The end holding the token, none when the cell is gone or refused it.
   */
  [[nodiscard]] static auto keep(
      std::shared_ptr<python_cell const> cell,
      std::uint64_t                      token,
      reflex::py::nb::handle             error) noexcept -> std::optional<held_python_end>
  {
    try
    {
      auto const live = cell->live();
      if(not live.is_valid())
      {
        return std::nullopt;
      }
      auto text                                      = text_of(error);
      live.attr("held")[reflex::py::nb::int_(token)] = error;
      return held_python_end{std::make_shared<entry>(std::move(cell), token, std::move(text))};
    }
    catch(reflex::py::nb::python_error const&)
    {
      return std::nullopt;
    }
  }

  /**
   * @brief Sets the Python error indicator to the exception the cell keeps for this end, or to
   *        EmulationError naming the exception's type and message once the machine's Python
   *        side is gone. The GIL held.
   */
  void restore() const noexcept
  {
    try
    {
      auto const live = entry_->cell->live();
      if(live.is_valid())
      {
        auto const error = live.attr("held").attr("get")(entry_->token);
        if(not error.is_none())
        {
          PyErr_SetRaisedException(error.inc_ref().ptr());
          return;
        }
      }
      auto const gone = reflex::py::nb::module_::import_("popcorn._errors")
                            .attr("EmulationError")(std::format(
                                "a handler exception ended the free run of a machine collected "
                                "since: {}",
                                entry_->text));
      PyErr_SetRaisedException(gone.inc_ref().ptr());
    }
    catch(reflex::py::nb::python_error& failed)
    {
      failed.restore();
    }
  }

private:
  struct entry
  {
    entry(std::shared_ptr<python_cell const> held, std::uint64_t key, std::string what)
        : cell{std::move(held)}, token{key}, text{std::move(what)}
    {}

    entry(entry const&)                    = delete;
    auto operator=(entry const&) -> entry& = delete;

    ~entry()
    {
      if(not gil_entry::available())
      {
        return;
      }
      gil_entry const held;
      auto* const     pending = PyErr_GetRaisedException();
      try
      {
        if(auto const live = cell->live(); live.is_valid())
        {
          live.attr("held").attr("pop")(token, reflex::py::nb::none());
        }
      }
      catch(reflex::py::nb::python_error const&)
      {}
      PyErr_SetRaisedException(pending);
    }

    std::shared_ptr<python_cell const> cell;
    std::uint64_t                      token;
    std::string                        text;
  };

  static auto text_of(reflex::py::nb::handle error) -> std::string
  {
    auto const  kind = reflex::py::nb::handle{reinterpret_cast<PyObject*>(Py_TYPE(error.ptr()))};
    std::string name;
    try
    {
      name = reflex::py::nb::cast<std::string>(kind.attr("__qualname__"));
      return std::format(
          "{}: {}", name, reflex::py::nb::cast<std::string>(reflex::py::nb::str(error)));
    }
    catch(...)
    {
      PyErr_Clear();
      return name.empty() ? std::string{"an exception"} : name;
    }
  }

  explicit held_python_end(std::shared_ptr<entry> kept) : entry_{std::move(kept)}
  {}

  std::shared_ptr<entry> entry_;
};

/**
 * @brief Whether a wait of the current free run delivered the free run's own end to Python, so
 *        it is not reported again. Only a wait the end completed counts: a wait that saw a
 *        boundary records nothing, even when its stop equals the end. Each wait carries the
 *        generation of the free run it was made in, and a result read after a later start()
 *        records nothing. Its mutex is a leaf.
 */
class delivered_end
{
public:
  /** @brief The generation of the current free run, for a wait made now. */
  [[nodiscard]] auto generation() const -> std::uint64_t
  {
    std::scoped_lock const lock{mutex_};
    return generation_;
  }

  /**
   * @brief Records a stop a wait of free run @p made returned.
   * @param made The wait's generation.
   * @param ended Whether the free run's end completed the wait, core::waiter_handle::ended().
   */
  void returned(std::uint64_t made, bool ended)
  {
    std::scoped_lock const lock{mutex_};
    stopped_ = stopped_ or (ended and made == generation_);
  }

  /**
   * @brief Records the exception a wait of free run @p made raised, the end's when it is a
   *        held_python_end.
   */
  void raised(std::uint64_t made, std::exception_ptr const& error) noexcept
  {
    try
    {
      std::rethrow_exception(error);
    }
    catch(held_python_end const&)
    {
      std::scoped_lock const lock{mutex_};
      error_ = error_ or made == generation_;
    }
    catch(...)
    {}
  }

  /** @brief Forgets what was delivered and begins a generation, when a free run starts. */
  void reset() noexcept
  {
    std::scoped_lock const lock{mutex_};
    ++generation_;
    stopped_ = false;
    error_   = false;
  }

  /** @brief Whether a wait of the current free run delivered @p end. */
  [[nodiscard]] auto delivered(core::held_end const& end) const -> bool
  {
    std::scoped_lock const lock{mutex_};
    return std::holds_alternative<stop>(end) ? stopped_ : error_;
  }

private:
  mutable std::mutex mutex_;
  std::uint64_t      generation_ = 0;
  bool               stopped_    = false;
  bool               error_      = false;
};

/**
 * @brief The exception reporting the end of a free run no caller received. The GIL held.
 * @param end The end.
 * @param delivered What the waits of the free run delivered.
 * @return None when a wait delivered @p end or the machine's collection caused it, a
 *         stop_reason::user stop with collected_detail or a `popcorn._hooks.HookGone`.
 *         Otherwise EmulationError naming the stop, or the exception a handler raised.
 * @throws reflex::py::nb::python_error when the exception cannot be built.
 */
auto report_of(core::held_end const& end, delivered_end const& delivered) -> reflex::py::nb::object;
}
