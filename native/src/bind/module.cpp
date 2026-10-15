// false positive on std::optional<std::string> move-assign in nanobind def_rw, PR 80635
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#include "description.hpp"
#include "devices.hpp"
#include "errors.hpp"
#include "fault_bits.hpp"
#include "image.hpp"
#include "machine.hpp"
#include "models.hpp"
#include "plugins.hpp"
#include "system.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/core/version.hpp>
#include <popcorn/periph/i2c.hpp>

#include <reflex/py.hpp>

#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <format>
#include <string>

namespace nb = nanobind;
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
  popcorn::bind::detail::load_builtin_plugins();
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
  m.bind_enum<popcorn::core::condition::kind>("ConditionKind");
  m.bind_enum<popcorn::core::condition::op>("ConditionOp");
  m.bind<popcorn::core::condition>("MemoryCondition");
  m.bind_enum<popcorn::core::condition_set::mode>("ConditionMode");
  m.bind<popcorn::core::condition_set>("ConditionSet");
  m.bind_enum<popcorn::periph::stm32::pin_mode>("PinMode");
  m.bind_enum<popcorn::periph::i2c_error>("I2CError");
  m.bind_enum<popcorn::periph::uart_error>("UartError");
  m.bind_enum<popcorn::periph::usb::device_state>("UsbDeviceState");
  m.bind_enum<popcorn::periph::usb::transfer_status>("UsbTransferStatus");
  m.bind_enum<popcorn::core::log_kind>("LogKind");
  m.bind_enum<popcorn::core::pause_reason>("PauseReason");
  m.bind<popcorn::core::log_record>("LogRecord");
  m.bind_enum<popcorn::access>("Access");
  m.bind_enum<popcorn::write_effect>("WriteEffect");
  m.bind_enum<popcorn::read_effect>("ReadEffect");
  m.bind_enum<popcorn::cross_effect>("CrossEffect");
  m.bind_enum<popcorn::region_kind>("RegionKind");
  m.bind<popcorn::field_desc>("FieldDesc");
  m.bind<popcorn::register_desc>("RegisterDesc");
  m.bind<popcorn::cross_rule>("CrossRule");
  m.bind<popcorn::peripheral_desc>("PeripheralDesc")
      .def(
          "register",
          [](popcorn::peripheral_desc const& self,
             std::string const&              name) -> popcorn::register_desc {
            auto const* const found = popcorn::find_register(self, name);
            if(found == nullptr)
            {
              throw nb::key_error(std::format("{} has no register {}", self.name, name).c_str());
            }
            return *found;
          },
          nb::arg("name"),
          "The register with this bare name, such as \"CR1\", or this SVD alias, such as "
          "\"TIM1_CR1\". KeyError when no register has either.");
  m.bind<popcorn::region_access>("RegionAccess");
  m.bind<popcorn::region_content>("RegionContent");
  m.bind<popcorn::region_desc>("RegionDesc");
  m.bind<popcorn::irq_desc>("IrqDesc");
  m.bind<popcorn::core_desc>("CoreDesc");
  m.bind<popcorn::part_description>("PartDescription");
  popcorn::bind::detail::bind_model_base(m.handle);
  popcorn::bind::detail::bind_device_bases(m.handle);
  popcorn::bind::detail::bind_plugins(m.handle);
  m.bind<^^popcorn::bind>();
}
