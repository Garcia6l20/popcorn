#include "smoke.hpp"

#include <reflex/py.hpp>

#include <nanobind/stl/string.h>

#include <cstdint>
#include <string>

namespace py = reflex::py;

namespace popcorn::testing::bind
{
[[= py::doc{"Load an ELF into flash and SRAM of a fresh Cortex-M33 engine, run it to the first "
            "bkpt and return r0."}]] auto run_to_bkpt(std::string const& elf) -> std::uint32_t
{
  return testing::run_to_bkpt(open_m33(), elf);
}
}

REFLEX_PY_MODULE(_smoke, m)
{
  m.bind<^^popcorn::testing::bind>();
}
