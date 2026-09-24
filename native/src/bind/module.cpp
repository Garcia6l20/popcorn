#include <popcorn/core/engine.hpp>
#include <popcorn/core/version.hpp>

#include <reflex/py.hpp>

#include <nanobind/stl/string.h>

#include <string>

namespace py = reflex::py;

namespace popcorn::bind
{
[[= py::doc{"Version of popcorn and of the linked Unicorn commit."}]]
auto version() -> std::string
{
  return std::string{core::version()};
}

class [[= py::doc{"A Cortex-M33 Unicorn engine, open from construction until close()."}]] Engine
{
public:
  Engine() : handle_{core::open_m33()}
  {
  }

  [[= py::doc{"Close the engine. Idempotent."}]]
  void close()
  {
    handle_.reset();
  }

  [[= py::doc{"Whether the engine is open."}]]
  auto is_open() const -> bool
  {
    return handle_ != nullptr;
  }

private:
  core::engine_handle handle_;
};
}

REFLEX_PY_MODULE(_core, m)
{
  m.bind<^^popcorn::bind>();
}
