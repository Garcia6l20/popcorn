#include <popcorn/core/version.hpp>

#include <reflex/py.hpp>

#include <nanobind/stl/string.h>

#include <string>

namespace py = reflex::py;

namespace popcorn::bind
{
[[= py::doc{"Version of popcorn and of the linked Unicorn commit."}]] auto version() -> std::string
{
  return std::string{core::version()};
}
}

REFLEX_PY_MODULE(_core, m)
{
  m.bind<^^popcorn::bind>();
}
