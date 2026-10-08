#pragma once

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <frameobject.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
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

}
