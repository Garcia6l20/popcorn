#include "description.hpp"
#include "errors.hpp"
#include "image.hpp"
#include "machine.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/core/version.hpp>

#include <reflex/py.hpp>

#include <nanobind/stl/function.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#if __has_include(<popcorn/periph/stm32/builtin.hpp>)
#include <popcorn/periph/stm32/builtin.hpp>
#endif

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
  popcorn::bind::detail::register_error_translator();
#if __has_include(<popcorn/periph/stm32/builtin.hpp>)
  popcorn::periph::stm32::register_builtin_models();
#endif
  m.bind_enum<popcorn::stop_reason>("StopReason");
  m.bind_enum<popcorn::fault_kind>("FaultKind");
  m.bind_enum<popcorn::reset_kind>("ResetKind");
  m.bind_enum<popcorn::arch::float_abi>("FloatAbi");
  m.bind_enum<popcorn::access>("Access");
  m.bind_enum<popcorn::write_effect>("WriteEffect");
  m.bind_enum<popcorn::read_effect>("ReadEffect");
  m.bind_enum<popcorn::cross_effect>("CrossEffect");
  m.bind_enum<popcorn::region_kind>("RegionKind");
  m.bind<popcorn::field_desc>("FieldDesc");
  m.bind<popcorn::register_desc>("RegisterDesc");
  m.bind<popcorn::cross_rule>("CrossRule");
  m.bind<popcorn::peripheral_desc>("PeripheralDesc");
  m.bind<popcorn::region_access>("RegionAccess");
  m.bind<popcorn::region_desc>("RegionDesc");
  m.bind<popcorn::irq_desc>("IrqDesc");
  m.bind<popcorn::core_desc>("CoreDesc");
  m.bind<popcorn::mcu_description>("McuDescription");
  m.bind<^^popcorn::bind>();
}
