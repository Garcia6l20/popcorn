#include "errors.hpp"
#include "held_python_end.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <meta>
#include <stdexcept>
#include <typeinfo>
#include <utility>

namespace popcorn::bind::detail
{
namespace
{
namespace nb = reflex::py::nb;

constexpr std::array translated{
    ^^config_error,    ^^image_error,     ^^symbol_error,      ^^fault_error,
    ^^budget_exceeded, ^^emulation_error, ^^unmodelled_config, ^^model_error,
    ^^machine_running, ^^machine_busy,    ^^not_implemented,   ^^error,
};

consteval auto untranslated_errors() -> std::size_t
{
  std::size_t missing = 0;
  for(auto const member : std::meta::members_of(^^popcorn, std::meta::access_context::unchecked()))
  {
    if(not std::meta::is_type(member) or not std::meta::is_class_type(std::meta::dealias(member)))
    {
      continue;
    }
    auto const type = std::meta::dealias(member);
    if(std::meta::is_complete_type(type)
       and std::meta::is_base_of_type(^^error, type)
       and std::ranges::find(translated, type) == translated.end())
    {
      ++missing;
    }
  }
  return missing;
}

static_assert(untranslated_errors() == 0, "every popcorn error class needs a translation below");

auto errors_module() -> nb::module_
{
  return nb::module_::import_("popcorn._errors");
}

auto enum_value(char const* name, auto value) -> nb::object
{
  return nb::module_::import_("popcorn._enums").attr(name)(std::to_underlying(value));
}

void raise(nb::handle type, nb::object const& instance)
{
  PyErr_SetObject(type.ptr(), instance.ptr());
}

template <typename... Fields>
void raise(char const* name, std::exception const& e, Fields&&... fields)
{
  auto const type = errors_module().attr(name);
  raise(type, type(e.what(), std::forward<Fields>(fields)...));
}

void raise_chained(core::chained_error const& chained) noexcept
{
  set_python_error(chained.during());
  auto* const during = PyErr_GetRaisedException();
  set_python_error(chained.raised());
  auto* const raised = PyErr_GetRaisedException();
  PyException_SetContext(raised, during);
  PyErr_SetRaisedException(raised);
}

void translate(std::exception_ptr const& thrown)
{
  try
  {
    std::rethrow_exception(thrown);
  }
  catch(core::chained_error const& chained)
  {
    raise_chained(chained);
  }
  catch(config_error const& e)
  {
    raise(
        "ConfigError", e, nb::arg("file") = e.file(), nb::arg("line") = e.line(),
        nb::arg("key") = e.key(), nb::arg("expected") = e.expected());
  }
  catch(image_error const& e)
  {
    raise(
        "ImageError", e, nb::arg("segment") = e.segment(), nb::arg("address") = e.addr(),
        nb::arg("region") = e.region());
  }
  catch(symbol_error const& e)
  {
    raise(
        "SymbolError", e, nb::arg("name") = e.name(), nb::arg("close_matches") = e.close_matches());
  }
  catch(fault_error const& e)
  {
    auto const& fault = e.fault();
    raise(
        "FaultError", e, nb::arg("kind") = enum_value("FaultKind", fault.kind),
        nb::arg("address") = fault.fault_address, nb::arg("cfsr") = fault.status,
        nb::arg("hfsr") = fault.detail, nb::arg("pc") = e.where().pc,
        nb::arg("symbol") = e.where().symbol);
  }
  catch(budget_exceeded const& e)
  {
    raise(
        "BudgetExceeded", e, nb::arg("budget") = e.budget().value, nb::arg("pc") = e.where().pc,
        nb::arg("symbol") = e.where().symbol);
  }
  catch(bus_access_error const& e)
  {
    raise("BusAccessError", e, nb::arg("address") = e.at(), nb::arg("denied") = e.denied());
  }
  catch(emulation_error const& e)
  {
    raise("EmulationError", e);
  }
  catch(unmodelled_config const& e)
  {
    raise(
        "UnmodelledConfig", e, nb::arg("peripheral") = e.peripheral(),
        nb::arg("register") = e.reg(), nb::arg("offset") = e.offset(), nb::arg("field") = e.field(),
        nb::arg("value") = e.value());
  }
  catch(model_error const& e)
  {
    raise(
        "ModelError", e, nb::arg("peripheral") = e.peripheral(), nb::arg("register") = e.reg(),
        nb::arg("offset") = e.offset());
  }
  catch(machine_running const& e)
  {
    raise("MachineRunning", e);
  }
  catch(machine_busy const& e)
  {
    raise("MachineBusy", e);
  }
  catch(not_implemented const& e)
  {
    PyErr_SetString(PyExc_NotImplementedError, e.what());
  }
  catch(core::machine_forked const& e)
  {
    raise("EmulationError", e);
  }
  catch(core::machine_closed const&)
  {
    auto const type = errors_module().attr("EmulationError");
    raise(type, type("the machine was closed"));
  }
  catch(core::operation_cancelled const& e)
  {
    auto const type = errors_module().attr("EmulationError");
    raise(type, type(e.what()));
  }
  catch(held_python_end const& end)
  {
    end.restore();
  }
  catch(error const& e)
  {
    if(typeid(e) != typeid(error))
    {
      throw;
    }
    raise("PopcornError", e);
  }
}
}

void register_error_translator()
{
  nb::register_exception_translator([](std::exception_ptr const& thrown, void*) {
    try
    {
      translate(thrown);
    }
    catch(nb::python_error& failed)
    {
      failed.restore();
    }
  });
}

void set_python_error(std::exception_ptr const& thrown) noexcept
{
  try
  {
    try
    {
      std::rethrow_exception(thrown);
    }
    catch(nb::python_error const& raised)
    {
      nb::python_error{raised}.restore();
      return;
    }
    catch(held_python_end const& end)
    {
      end.restore();
      return;
    }
    catch(core::chained_error const& chained)
    {
      raise_chained(chained);
      return;
    }
    catch(std::invalid_argument const& invalid)
    {
      PyErr_SetString(PyExc_ValueError, invalid.what());
      return;
    }
    catch(...)
    {}
    translate(thrown);
  }
  catch(nb::python_error& failed)
  {
    failed.restore();
  }
  catch(std::exception const& other)
  {
    PyErr_SetString(PyExc_RuntimeError, other.what());
  }
  catch(...)
  {
    PyErr_SetString(PyExc_RuntimeError, "unknown native exception");
  }
}
}
