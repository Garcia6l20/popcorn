#include "echo.hpp"

#include <popcorn/plugin/bindings.hpp>

namespace nb = nanobind;

NB_MODULE(_native, m)
{
  popcorn::plugin::require_core(m);
  nb::class_<echo::echo_target, popcorn::periph::spi_target>(m, "EchoBase").def(nb::init<>());
}
