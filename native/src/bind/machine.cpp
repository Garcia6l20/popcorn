#include "machine.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ranges>

namespace popcorn::bind
{
namespace
{
struct arch_module
{
  auto (*implements)(core_desc const& core) -> bool;
  auto (*factory)() -> arch::arch_factory;
};

constexpr std::array arch_modules{
    arch_module{.implements = &arch::armv8m::implements, .factory = &arch::armv8m::factory},
};

auto factory_for(mcu_description const& description) -> arch::arch_factory
{
  auto const found = std::ranges::find_if(
      arch_modules, [&](arch_module const& module) { return module.implements(description.core); });
  if(found == arch_modules.end())
  {
    throw config_error(
        description.part, 0, "core.cpu",
        std::format("a core an arch module implements, not {}", description.core.cpu));
  }
  return found->factory();
}

auto options_of(mcu_description const& description, machine_config const& config)
    -> core::machine_options
{
  if(not std::isfinite(config.cpi) or not(config.cpi > 0.0))
  {
    throw config_error(description.part, 0, "cpi", "finite cycles per instruction above 0");
  }
  auto const hz = config.core_hz != 0 ? config.core_hz : description.core.reset_clock_hz;
  if(hz != 0 and static_cast<double>(hz) / config.cpi < 1.0)
  {
    throw config_error(
        description.part, 0, "core_hz",
        std::format("a core clock of at least one instruction per second at cpi {}", config.cpi));
  }
  if(config.trace_capacity == 0)
  {
    throw config_error(description.part, 0, "trace_capacity", "a trace capacity above 0");
  }
  core::machine_options options;
  options.core_hz            = config.core_hz;
  options.cpi                = config.cpi;
  options.trace_capacity     = config.trace_capacity;
  options.spin_threshold     = config.spin_threshold;
  options.strict_peripherals = config.strict_peripherals;
  options.vector_table       = config.vector_table;
  return options;
}
}

machine_handle::machine_handle(mcu_description const& description, machine_config const& config)
    : machine_{std::make_unique<core::machine>(
          description,
          factory_for(description),
          options_of(description, config))}
{}

void machine_handle::load(image const& loaded)
{
  machine_->load(loaded.native());
}

void machine_handle::reset(reset_kind kind)
{
  if(not is_enumerator(kind))
  {
    throw config_error("reset", 0, "kind", "a reset kind");
  }
  machine_->reset(kind);
}

auto machine_handle::image_digests() const -> std::vector<std::string>
{
  return machine_->loaded_images()
       | std::views::transform(&popcorn::image::image::digest)
       | std::ranges::to<std::vector>();
}

auto machine_handle::locate(address pc) const -> location_info
{
  return owned(machine_->locate(pc));
}

auto machine_handle::native() const noexcept -> core::machine&
{
  return *machine_;
}
}
