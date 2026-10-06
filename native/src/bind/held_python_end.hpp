#pragma once

#include "gil_locks.hpp"

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

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
      live.attr("held")[reflex::py::nb::int_(token)] = error;
      return held_python_end{std::make_shared<entry>(std::move(cell), token)};
    }
    catch(reflex::py::nb::python_error const&)
    {
      return std::nullopt;
    }
  }

  /**
   * @brief Sets the Python error indicator to the exception the cell keeps for this end, or to
   *        EmulationError once the machine's Python side is gone. The GIL held.
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
                            .attr("EmulationError")(
                                "a handler exception ended the free run of a machine collected "
                                "since, the exception with it");
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
    entry(std::shared_ptr<python_cell const> held, std::uint64_t key)
        : cell{std::move(held)}, token{key}
    {}

    entry(entry const&)                    = delete;
    auto operator=(entry const&) -> entry& = delete;

    ~entry()
    {
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
  };

  explicit held_python_end(std::shared_ptr<entry> kept) : entry_{std::move(kept)}
  {}

  std::shared_ptr<entry> entry_;
};
}
