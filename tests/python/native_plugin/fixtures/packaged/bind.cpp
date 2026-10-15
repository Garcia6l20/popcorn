#include "fixture.hpp"

#include <popcorn/plugin/bindings.hpp>

#include <nanobind/stl/shared_ptr.h>

#include <memory>

namespace nb = nanobind;

namespace
{
class nested_target final : public fixture::target
{};

class made_target final : public fixture::target
{};
}

NB_MODULE(_native, m)
{
  popcorn::plugin::require_core(m);
  nb::class_<fixture::target, popcorn::periph::spi_target>(m, "FixtureTargetBase")
      .def(nb::init<>());
  nb::class_<nested_target, fixture::target>(m.def_submodule("spi"), "NestedTargetBase")
      .def(nb::init<>());
  nb::class_<made_target, popcorn::periph::spi_target>(m, "MadeTargetBase").def(nb::new_([] {
    return std::make_shared<made_target>();
  }));
}
