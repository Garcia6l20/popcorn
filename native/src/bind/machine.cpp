#include "machine.hpp"
#include "devices.hpp"
#include "errors.hpp"
#include "models.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ranges>
#include <span>

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
  options.config_digest      = config.config_digest;
  for(auto const& [name, maker] : config.python_models)
  {
    options.models.emplace(name, detail::python_model_factory(maker));
  }
  if(not config.python_devices.empty())
  {
    for(auto& [name, factory] : detail::device_factories(description, config.python_devices))
    {
      if(not options.models.emplace(name, std::move(factory)).second)
      {
        throw config_error(
            "devices", 0, std::format("devices.{}", name), "a peripheral without a Python model");
      }
    }
  }
  if(config.arena_start)
  {
    options.scratch =
        address_range{.name = "arena", .start = *config.arena_start, .size = config.arena_size};
  }
  return options;
}
}

machine_handle::machine_handle(mcu_description const& description, machine_config const& config)
    : alive_{std::make_shared<bool>(true)}
{
  detail::building_scope const building{alive_};
  machine_ = std::make_unique<core::machine>(
      description, factory_for(description), options_of(description, config));
}

machine_handle::~machine_handle()
{
  *alive_ = false;
}

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

void machine_handle::write_memory(address at, reflex::py::nb::bytes const& bytes)
{
  if(std::uint64_t{at} + bytes.size() > std::uint64_t{1} << 32)
  {
    throw config_error(
        "memory", 0, std::format("{:#010x}", at),
        std::format("{} bytes within the 32-bit address space", bytes.size()));
  }
  auto const written = machine_->mem().write_block(
      at, std::span{static_cast<std::byte const*>(bytes.data()), bytes.size()});
  if(not written)
  {
    throw detail::bus_access_error(
        std::format(
            "bus error at {:#010x}: {}", written.error().at,
            written.error().error == bus_error::unmapped ? "unmapped" : "denied"),
        written.error().at);
  }
}

auto machine_handle::snapshot() const -> snapshot_handle
{
  return snapshot_handle{machine_->snapshot()};
}

void machine_handle::restore(snapshot_handle const& saved)
{
  reflex::py::nb::gil_scoped_release unlocked;
  machine_->restore(saved.native());
}

void machine_handle::begin_call(arch::call_frame const& frame)
{
  machine_->begin_call(frame);
}

auto machine_handle::end_call() -> std::uint64_t
{
  if(not machine_->in_call())
  {
    throw config_error("call", 0, "end_call", "a call begun by begin_call");
  }
  return machine_->end_call();
}

auto machine_handle::in_call() const noexcept -> bool
{
  return machine_->in_call();
}

auto machine_handle::model(std::string const& peripheral) const -> reflex::py::nb::object
{
  return detail::python_object(machine_->model(peripheral));
}

auto machine_handle::id() const noexcept -> std::uint64_t
{
  return machine_->id();
}

auto machine_handle::native() const noexcept -> core::machine&
{
  return *machine_;
}
}
