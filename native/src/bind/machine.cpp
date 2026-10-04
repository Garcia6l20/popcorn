#include "machine.hpp"
#include "devices.hpp"
#include "errors.hpp"
#include "models.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>

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

constexpr std::uint64_t hse_min_hz = 4'000'000;
constexpr std::uint64_t hse_max_hz = 50'000'000;
constexpr std::uint64_t lse_max_hz = 1'000'000;

auto rcc_options_of(machine_config const& config) -> std::optional<periph::stm32::rcc_options>
{
  if(not config.rcc_hse_hz and not config.rcc_lse_hz)
  {
    return std::nullopt;
  }
  periph::stm32::rcc_options options;
  if(config.rcc_hse_hz)
  {
    if(*config.rcc_hse_hz < hse_min_hz or *config.rcc_hse_hz > hse_max_hz)
    {
      throw config_error(
          "rcc", 0, "rcc.hse_hz",
          std::format("an HSE frequency from 4 to 50 MHz, not {} Hz", *config.rcc_hse_hz));
    }
    options.hse_hz = *config.rcc_hse_hz;
  }
  if(config.rcc_lse_hz)
  {
    if(*config.rcc_lse_hz == 0 or *config.rcc_lse_hz > lse_max_hz)
    {
      throw config_error(
          "rcc", 0, "rcc.lse_hz",
          std::format(
              "an LSE frequency above 0 Hz and at most 1 MHz, not {} Hz", *config.rcc_lse_hz));
    }
    options.lse_hz = *config.rcc_lse_hz;
  }
  return options;
}

auto supply_factories(mcu_description const& description, std::uint32_t vdda_mv)
    -> std::map<std::string, periph::model_factory, std::less<>>
{
  using periph::stm32::vdda_max_mv;
  using periph::stm32::vdda_min_mv;
  if(vdda_mv < vdda_min_mv or vdda_mv > vdda_max_mv)
  {
    throw config_error(
        "supply", 0, "supply.vdda_mv",
        std::format("a VDDA from {} to {} mV, not {} mV", vdda_min_mv, vdda_max_mv, vdda_mv));
  }
  periph::stm32::board board;
  board.supply.vdda_mv = vdda_mv;
  auto all             = periph::stm32::factories(board, description);
  std::map<std::string, periph::model_factory, std::less<>> out;
  for(auto const& unit : description.peripherals)
  {
    if(unit.ip != "ADC" or unit.alias_of)
    {
      continue;
    }
    if(auto found = all.find(unit.name); found != all.end())
    {
      out.emplace(unit.name, std::move(found->second));
    }
  }
  return out;
}

auto key_of(std::string const& usb) -> std::string
{
  return std::format("usb.{}", usb);
}

using usb_hub_map = std::map<std::string, detail::usb_hubs>;

auto usb_factories(
    mcu_description const& description,
    machine_config const&  config,
    usb_hub_map const&     hubs) -> std::map<std::string, periph::model_factory, std::less<>>
{
  periph::stm32::board board;
  for(auto const& [name, host] : config.usb_hosts)
  {
    if(host.reset_ns == 0 or host.timeout_ns == 0)
    {
      throw config_error("usb", 0, key_of(name), "a bus reset and a request timeout above 0 ns");
    }
    periph::stm32::usb_host_options options{.attached = host.attached};
    options.host.debounce   = std::chrono::nanoseconds{static_cast<std::int64_t>(host.debounce_ns)};
    options.host.reset_time = std::chrono::nanoseconds{static_cast<std::int64_t>(host.reset_ns)};
    options.host.request_timeout =
        std::chrono::nanoseconds{static_cast<std::int64_t>(host.timeout_ns)};
    auto const& channels = hubs.at(name);
    options.host.drivers.push_back(
        periph::usb::cdc_acm_factory(channels.cdc, {.dtr = host.dtr, .rts = host.rts}));
    options.host.drivers.push_back(periph::usb::hid_factory(channels.hid));
    options.host.drivers.push_back(periph::usb::uac2_factory(channels.uac2));
    board.usb_hosts.emplace(name, std::move(options));
  }
  auto all = periph::stm32::factories(board, description);
  std::map<std::string, periph::model_factory, std::less<>> out;
  for(auto const& name : config.usb_hosts | std::views::keys)
  {
    auto const unit  = std::ranges::find_if(description.peripherals, [&](peripheral_desc const& p) {
      return p.name == name and not p.alias_of;
    });
    auto       found = all.find(name);
    if(unit == description.peripherals.end() or unit->ip != "USB_OTG_FS" or found == all.end())
    {
      throw config_error("usb", 0, key_of(name), "a USB device controller a built-in model serves");
    }
    out.emplace(name, std::move(found->second));
  }
  return out;
}

auto options_of(
    mcu_description const& description,
    machine_config const&  config,
    usb_hub_map const&     hubs) -> core::machine_options
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
  auto const rcc = rcc_options_of(config);
  if(not config.python_devices.empty() or rcc)
  {
    for(auto& [name, factory] : detail::device_factories(description, config.python_devices, rcc))
    {
      if(not options.models.emplace(name, std::move(factory)).second)
      {
        if(not config.python_devices.contains(name))
        {
          throw config_error("rcc", 0, "rcc", "an RCC without a Python model");
        }
        throw config_error(
            "devices", 0, std::format("devices.{}", name), "a peripheral without a Python model");
      }
    }
  }
  for(auto& [name, factory] : usb_factories(description, config, hubs))
  {
    if(not options.models.emplace(name, std::move(factory)).second)
    {
      throw config_error("usb", 0, key_of(name), "a USB device controller without a Python model");
    }
  }
  if(config.vdda_mv)
  {
    for(auto& [name, factory] : supply_factories(description, *config.vdda_mv))
    {
      options.models.try_emplace(name, std::move(factory));
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
  for(auto const& name : config.usb_hosts | std::views::keys)
  {
    hubs_.emplace(name, detail::usb_hubs{});
  }
  machine_ = std::make_unique<core::machine>(
      description, factory_for(description), options_of(description, config, hubs_));
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
  for(auto const& hubs : hubs_ | std::views::values)
  {
    hubs.cdc->discard();
    hubs.hid->discard();
    hubs.uac2->discard();
  }
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

auto machine_handle::pending_events() const -> std::size_t
{
  return machine_->pending_events();
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
