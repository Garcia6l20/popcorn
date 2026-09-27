#include "config.hpp"
#include "description.hpp"
#include "errors.hpp"
#include "image.hpp"
#include "machine.hpp"
#include "models.hpp"
#include "toml_caster.hpp"

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
  m.bind_enum<popcorn::arch::value_kind>("ValueKind");
  m.bind_enum<popcorn::arch::call_stack>("CallStack");
  m.bind<popcorn::arch::call_arg>("CallArg");
  m.bind<popcorn::arch::call_frame>("CallFrame");
  m.bind_enum<popcorn::initiator>("Initiator");
  m.bind_enum<popcorn::core::intercept_action>("InterceptAction");
  m.bind_enum<popcorn::core::watch_kind>("WatchKind");
  m.bind_enum<popcorn::core::watch_action>("WatchAction");
  m.bind<popcorn::core::intercept_result>("InterceptResult");
  m.bind<popcorn::core::watch_hit>("WatchHit");
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
  m.bind<popcorn::config::project_section>("ProjectSection");
  m.bind<popcorn::config::mcu_section>("McuSection");
  m.bind<popcorn::config::image_entry>("ImageEntry");
  m.bind<popcorn::config::memory_init_entry>("MemoryInit");
  m.bind<popcorn::config::boot_section>("BootSection");
  m.bind<popcorn::config::clock_section>("ClockSection");
  m.bind<popcorn::config::device_entry>("DeviceEntry");
  m.bind<popcorn::config::board_entry>("BoardEntry");
  m.bind<popcorn::config::models_section>("ModelsSection");
  m.bind<popcorn::config::project_file>("ProjectFile");
  m.bind<popcorn::config::loaded_project>("LoadedProject");
  popcorn::bind::detail::bind_model_base(m.handle);
  m.bind<^^popcorn::bind>();
}
