#include <popcorn/core/engine.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/smoke.hpp>
#include <popcorn/core/version.hpp>

#include <reflex/py.hpp>

#include <nanobind/stl/string.h>

#include <cstdint>
#include <string>

namespace py = reflex::py;

namespace popcorn::bind
{
[[= py::doc{"Version of popcorn and of the linked Unicorn commit."}]] auto version() -> std::string
{
  return std::string{core::version()};
}

class[[= py::doc{"A Cortex-M33 Unicorn engine, open from construction until close()."}]] Engine
{
public:
  Engine() : handle_{core::open_m33()}
  {}

  [[= py::doc{"Close the engine. Idempotent."}]] void close()
  {
    handle_.reset();
  }

  [[= py::doc{"Load an ELF into flash and SRAM, run it to the first bkpt and return r0."}]] auto
      run_to_bkpt(std::string const& elf) -> std::uint32_t
  {
    if(!handle_)
    {
      throw emulation_error{"engine is closed"};
    }
    return core::run_to_bkpt(handle_, elf);
  }

  [[= py::doc{"Whether the engine is open."}]] auto is_open() const -> bool
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
