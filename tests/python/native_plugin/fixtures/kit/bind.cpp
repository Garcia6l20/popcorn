#include "kit.hpp"

#include <popcorn/plugin/bindings.hpp>

namespace nb = nanobind;

NB_MODULE(_native, m)
{
  popcorn::plugin::require_core(m);
  nb::class_<kit::target, popcorn::periph::spi_target>(m, "KitTargetBase").def(nb::init<>());
  auto spi = m.def_submodule("spi");
  nb::class_<kit::sub_target, popcorn::periph::spi_target>(spi, "KitSubBase").def(nb::init<>());
}
