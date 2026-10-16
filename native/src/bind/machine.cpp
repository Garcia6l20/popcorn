#include "machine.hpp"
#include "devices.hpp"
#include "errors.hpp"
#include "held_python_end.hpp"
#include "models.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace popcorn::bind
{
namespace
{
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

auto supply_factories(part_description const& description, std::uint32_t vdda_mv)
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

auto option_bytes_of(option_bytes_config const& config) -> periph::stm32::option_bytes
{
  using periph::stm32::no_area;
  using periph::stm32::page_range;
  using periph::stm32::watermark;
  using periph::stm32::whole_bank;
  auto const range = [](std::pair<std::uint32_t, std::uint32_t> const& pages) {
    return page_range{.first = pages.first, .last = pages.second};
  };
  auto const area =
      [&range](
          bool                                                          all,
          std::optional<std::pair<std::uint32_t, std::uint32_t>> const& pages) -> watermark {
    if(all)
    {
      return whole_bank{};
    }
    if(pages)
    {
      return range(*pages);
    }
    return no_area{};
  };
  return {
      .tzen      = config.tzen,
      .secwm1    = area(config.secwm1_all, config.secwm1),
      .secwm2    = area(config.secwm2_all, config.secwm2),
      .hdp1      = config.hdp1.transform(range),
      .hdp2      = config.hdp2.transform(range),
      .secboot   = config.secboot,
      .nsboot    = config.nsboot,
      .swap_bank = config.swap_bank,
      .dualbank  = config.dualbank,
  };
}

void refuse_replaced_flash(part_description const& description, machine_config const& config)
{
  auto const unit = std::ranges::find_if(description.peripherals, [](peripheral_desc const& p) {
    return p.ip == "flash" and not p.alias_of;
  });
  if(unit == description.peripherals.end())
  {
    return;
  }
  if(config.python_models.contains(unit->name) or config.plugin_models.contains(unit->name))
  {
    throw config_error(
        "option_bytes", 0, "option_bytes",
        std::format(
            "the built-in model of {}, not a {} model", unit->name,
            config.plugin_models.contains(unit->name) ? "plugin" : "Python"));
  }
}

auto key_of(std::string const& usb) -> std::string
{
  return std::format("usb.{}", usb);
}

using usb_hub_map = std::map<std::string, detail::usb_hubs>;

auto usb_factories(
    part_description const& description,
    machine_config const&   config,
    usb_hub_map const&      hubs) -> std::map<std::string, periph::model_factory, std::less<>>
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
    part_description const&              description,
    machine_config const&                config,
    usb_hub_map const&                   hubs,
    std::vector<reflex::py::nb::handle>& python_owned) -> core::machine_options
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
  auto const option_bytes = config.option_bytes.transform(option_bytes_of);
  if(option_bytes)
  {
    refuse_replaced_flash(description, config);
  }
  core::machine_options options;
  options.core_hz            = config.core_hz;
  options.cpi                = config.cpi;
  options.trace_capacity     = config.trace_capacity;
  options.spin_threshold     = config.spin_threshold;
  options.strict_peripherals = config.strict_peripherals;
  options.vector_table       = config.vector_table;
  options.config_digest      = config.config_digest;
  if(option_bytes)
  {
    auto const configured =
        periph::stm32::configure_option_bytes(description, *option_bytes, options);
    if(not configured)
    {
      throw config_error("option_bytes", 0, configured.error().key, configured.error().expected);
    }
  }
  for(auto const& [name, maker] : config.python_models)
  {
    options.models.emplace(name, detail::python_model_factory(maker));
  }
  for(auto const& [name, chosen] : config.plugin_models)
  {
    auto const& [record, model] = chosen;
    auto const key              = std::format("board.{}.model", name);
    if(record == nullptr)
    {
      throw config_error("plugin", 0, key, "a loaded plugin");
    }
    auto factory = plugin::find_model(*record, model);
    if(not factory)
    {
      throw config_error(
          record->path.string(), 0, key,
          std::format(
              "a model plugin {} registers, one of {}", record->name,
              record->models.empty() ? std::string{"none"}
                                     : record->models
                                           | std::views::transform(&plugin::model_entry::name)
                                           | std::views::join_with(std::string_view{", "})
                                           | std::ranges::to<std::string>()));
    }
    if(not options.models.emplace(name, std::move(factory)).second)
    {
      throw config_error("plugin", 0, key, "one board model per peripheral");
    }
  }
  auto const kind = [&config](std::string const& name) {
    return config.plugin_models.contains(name) ? "plugin" : "Python";
  };
  auto const rcc = rcc_options_of(config);
  if(not config.python_devices.empty() or rcc)
  {
    for(auto& [name, factory] :
        detail::device_factories(description, config.python_devices, rcc, python_owned))
    {
      if(not options.models.emplace(name, std::move(factory)).second)
      {
        if(not config.python_devices.contains(name))
        {
          throw config_error("rcc", 0, "rcc", std::format("an RCC without a {} model", kind(name)));
        }
        throw config_error(
            "devices", 0, std::format("devices.{}", name),
            std::format("a peripheral without a {} model", kind(name)));
      }
    }
  }
  for(auto& [name, factory] : usb_factories(description, config, hubs))
  {
    if(not options.models.emplace(name, std::move(factory)).second)
    {
      throw config_error(
          "usb", 0, key_of(name),
          std::format("a USB device controller without a {} model", kind(name)));
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

auto bounds_of(machine_config const& config) -> core::slice_bounds
{
  if(config.slice_instructions == 0 or config.slice_wall_ns <= 0)
  {
    throw config_error(
        "machine", 0, "slice_instructions", "a slice of at least one instruction and 1 ns");
  }
  return {
      .max_instructions = icount{config.slice_instructions},
      .wall_budget      = std::chrono::nanoseconds{config.slice_wall_ns},
  };
}

auto boundary_of(
    std::weak_ptr<detail::host_slot const> slot,
    std::weak_ptr<detail::pty_bridges>     ptys,
    core::machine&                         engine) -> std::move_only_function<void(core::member_id)>
{
  return [slot = std::move(slot), ptys = std::move(ptys), &engine](core::member_id) {
    if(auto const reached = slot.lock())
    {
      detail::run_retired(*reached);
    }
    auto const bridges = ptys.lock();
    if(bridges == nullptr or not bridges->any.load(std::memory_order_acquire))
    {
      return;
    }
    detail::pump_and_log(*bridges, engine);
  };
}

auto held_in(std::shared_ptr<detail::python_cell const> cell)
    -> std::move_only_function<void(core::member_id, std::exception_ptr&)>
{
  return [cell = std::move(cell),
          next = std::uint64_t{0}](core::member_id, std::exception_ptr& error) mutable {
    try
    {
      std::rethrow_exception(error);
    }
    catch(reflex::py::nb::python_error const&)
    {}
    catch(...)
    {
      return;
    }
    detail::gil_entry const                held;
    std::optional<detail::held_python_end> kept;
    try
    {
      std::rethrow_exception(error);
    }
    catch(reflex::py::nb::python_error const& raised)
    {
      kept = detail::held_python_end::keep(cell, next++, raised.value());
    }
    if(kept)
    {
      error = std::make_exception_ptr(*std::move(kept));
    }
  };
}

void report_unclaimed(core::held_end const& ended, detail::delivered_end const& delivered) noexcept
{
  try
  {
    if(auto const raised = detail::report_of(ended, delivered); not raised.is_none())
    {
      PyErr_SetRaisedException(raised.inc_ref().ptr());
      PyErr_WriteUnraisable(nullptr);
    }
  }
  catch(...)
  {
    PyErr_Clear();
  }
}
}

namespace detail
{
auto report_of(core::held_end const& end, delivered_end const& delivered) -> reflex::py::nb::object
{
  namespace nb = reflex::py::nb;
  if(delivered.delivered(end))
  {
    return nb::none();
  }
  if(auto const* const stopped = std::get_if<stop>(&end))
  {
    if(stopped->reason == stop_reason::user and stopped->detail == collected_detail)
    {
      return nb::none();
    }
    return nb::module_::import_("popcorn._errors")
        .attr("EmulationError")(std::format(
            "a machine was destroyed with the stop that ended its free run never returned to "
            "a caller: {} at {:#010x}",
            stopped->detail.empty() ? "a stop" : stopped->detail, stopped->where.pc));
  }
  auto       raised = python_exception(std::get<std::exception_ptr>(end));
  auto const gone   = nb::module_::import_("popcorn._hooks").attr("HookGone");
  if(PyObject_IsInstance(raised.ptr(), gone.ptr()) == 1)
  {
    return nb::none();
  }
  return raised;
}
}

machine_handle::machine_handle(part_description const& description, machine_config const& config)
    : alive_{detail::make_alive()}, slot_{std::make_shared<detail::host_slot>()},
      ptys_{std::make_shared<detail::pty_bridges>()},
      thread_live_{std::make_shared<std::atomic<bool>>(false)},
      delivered_{std::make_shared<detail::delivered_end>()},
      collected_{std::make_shared<std::atomic<bool>>(false)},
      gil_entries_{std::make_shared<std::atomic<std::uint64_t>>(0)}
{
  auto const             bounds = bounds_of(config);
  reflex::py::nb::object cell;
  if(config.models_context and not config.models_context->is_none())
  {
    cell = reflex::py::nb::steal(PyWeakref_NewRef(config.models_context->ptr(), nullptr));
    if(not cell.is_valid())
    {
      throw reflex::py::nb::python_error();
    }
  }
  std::unique_ptr<core::machine> built;
  {
    detail::building_scope const building{
        {.alive = alive_, .collected = collected_, .host = slot_, .context = cell.ptr()}
    };
    for(auto const& name : config.usb_hosts | std::views::keys)
    {
      hubs_.emplace(name, detail::usb_hubs{});
    }
    built = std::make_unique<core::machine>(
        description, arch::find_arch(description),
        options_of(description, config, hubs_, *python_owned_));
  }
  engine_ = built.get();
  for(auto const& unit : description.peripherals)
  {
    if(unit.alias_of)
    {
      continue;
    }
    if(auto const python = detail::python_object(engine_->model(unit.name)); not python.is_none())
    {
      python_owned_->push_back(python);
    }
  }
  id_                = built->id();
  slot_->logs->token = log_token_bit | id_;
  log_hook_.emplace(
      engine_->add_log_sink(
          [logs = slot_->logs->queue, engine = engine_](core::log_record const& record) {
            logs->push(record, engine->virtual_clock().time());
          }),
      slot_);
  std::vector<std::unique_ptr<core::machine>> members;
  members.push_back(std::move(built));
  auto hooks     = detail::python_thread_hooks(thread_live_, gil_entries_);
  hooks.boundary = boundary_of(slot_, ptys_, *engine_);
  if(cell.is_valid())
  {
    hooks.held = held_in(std::make_shared<detail::python_cell const>(cell));
  }
  host_         = std::make_shared<core::host>(std::move(members), bounds, std::move(hooks));
  engine_owner_ = host_->member_owner(core::member_id{});
  slot_->host   = host_;
  host_->name(std::format("{} #{}", description.part, id()));
  detail::register_host(host_, slot_, description.part);
}

machine_handle::~machine_handle()
{
  detail::clear_alive(*alive_);
  if(host_->forked())
  {
    detail::abandon(std::move(host_));
    return;
  }
  detail::unregister_host(host_);
  if(Py_IsFinalizing() != 0 and thread_live_->load())
  {
    detail::abandon(std::move(host_));
    return;
  }
  contract_assert(PyGILState_Check() != 0);
  if(host_->abandoned())
  {
    detail::give_up(*slot_, host_->orphan());
    detail::report_late_failures(*slot_->logs);
    host_.reset();
    return;
  }
  if(not core::detail::on_host_thread and core::detail::host_locks_held == 0)
  {
    std::optional<core::held_end> ended;
    {
      reflex::py::nb::gil_scoped_release const released;
      ended = host_->halt(core::member_id{});
      host_->close();
    }
    if(host_->abandoned())
    {
      detail::give_up(*slot_, host_->orphan());
    }
    else
    {
      detail::run_retired(*slot_);
    }
    detail::report_late_failures(*slot_->logs);
    if(ended)
    {
      report_unclaimed(*ended, *delivered_);
    }
  }
  host_.reset();
}

void machine_handle::load(image const& loaded)
{
  access([&](core::machine& target) { target.load(loaded.native()); });
}

void machine_handle::reset(reset_kind kind)
{
  if(not is_enumerator(kind))
  {
    throw config_error("reset", 0, "kind", "a reset kind");
  }
  access([kind](core::machine& target) { target.reset(kind); });
}

void machine_handle::power_cycle()
{
  access([](core::machine& target) { target.power_cycle(); });
}

auto machine_handle::image_digests() const -> std::vector<std::string>
{
  return access([](core::machine& target) {
    return target.loaded_images()
         | std::views::transform(&popcorn::image::image::digest)
         | std::ranges::to<std::vector>();
  });
}

auto machine_handle::locate(address pc) const -> location_info
{
  return access([pc](core::machine& target) { return owned(target.locate(pc)); });
}

auto machine_handle::snapshot() const -> snapshot_handle
{
  return access([](core::machine& target) { return snapshot_handle{target.snapshot()}; });
}

void machine_handle::restore(snapshot_handle const& saved)
{
  access([&](core::machine& target) {
    reflex::py::nb::gil_scoped_release unlocked;
    for(auto const& hubs : hubs_ | std::views::values)
    {
      hubs.cdc->discard();
      hubs.hid->discard();
      hubs.uac2->discard();
    }
    {
      auto const lock = detail::lock_ptys(*ptys_);
      for(auto const& bridge : ptys_->bridges)
      {
        bridge->discard();
      }
    }
    target.restore(saved.native());
  });
}

auto machine_handle::open_uart_pty(std::string name, uart_queue const& link) -> pty_handle
{
  return open_pty(std::move(name), link.bridged_link());
}

auto machine_handle::open_cdc_pty(std::string name, cdc_link const& link) -> pty_handle
{
  return open_pty(std::move(name), link.bridged_link());
}

auto machine_handle::open_pty(std::string name, std::unique_ptr<detail::pty_link> link)
    -> pty_handle
{
  host_->refuse_forked();
  auto const bridge = std::make_shared<detail::pty_bridge>(std::move(name), std::move(link));
  {
    auto const lock = detail::lock_ptys(*ptys_);
    detail::add_bridge(*ptys_, lock, bridge);
  }
  auto const watched =
      host_->watch(core::member_id{}, bridge->master(), [wants = bridge->wants_write_flag()] {
        return wants->load(std::memory_order_relaxed);
      });
  return pty_handle{ptys_, bridge, host_, watched};
}

auto detail::lock_ptys(pty_bridges& ptys) -> std::unique_lock<std::mutex>
{
  std::unique_lock held{ptys.mutex, std::try_to_lock};
  if(held.owns_lock() or PyGILState_Check() == 0)
  {
    if(not held.owns_lock())
    {
      held.lock();
    }
    return held;
  }
  reflex::py::nb::gil_scoped_release const released;
  held.lock();
  return held;
}

pty_handle::pty_handle(
    std::weak_ptr<detail::pty_bridges>         owner,
    std::shared_ptr<detail::pty_bridge> const& bridge,
    std::weak_ptr<core::host>                  host,
    core::watch_id                             watched)
    : owner_{std::move(owner)}, bridge_{bridge}, host_{std::move(host)}, watched_{watched},
      path_{bridge->path()}
{}

auto pty_handle::path() const -> std::string
{
  return path_;
}

void pty_handle::close()
{
  if(auto const on = host_.lock())
  {
    if(on->forked())
    {
      return;
    }
    on->unwatch(watched_);
  }
  auto const owner  = owner_.lock();
  auto const target = bridge_.lock();
  if(owner == nullptr or target == nullptr)
  {
    return;
  }
  std::shared_ptr<detail::pty_bridge> closing;
  {
    auto const lock = detail::lock_ptys(*owner);
    closing         = detail::remove_bridge(*owner, lock, target.get());
  }
}

void machine_handle::begin_call(arch::call_frame const& frame)
{
  if(not host_->owns_engine() and not host_->abandoned())
  {
    auto const held = lock_releasing();
    host_->give_back_core(held, core::member_id{});
  }
  access([&](core::machine& target) { target.begin_call(frame); });
}

auto machine_handle::end_call() -> std::uint64_t
{
  return access([](core::machine& target) {
    if(not target.in_call())
    {
      throw config_error("call", 0, "end_call", "a call begun by begin_call");
    }
    return target.end_call();
  });
}

auto machine_handle::in_call() const -> bool
{
  return access([](core::machine& target) { return target.in_call(); });
}

auto machine_handle::pending_events() const -> std::size_t
{
  return access([](core::machine& target) { return target.pending_events(); });
}

auto machine_handle::model(std::string const& peripheral) const -> reflex::py::nb::object
{
  return detail::python_object(engine().model(peripheral));
}

void detail::host_abandon::operator()() const
{
  owned->clear();
  clear_alive(*alive);
  if(auto const on = host.lock())
  {
    reflex::py::nb::gil_scoped_release const released;
    on->abandon_thread();
  }
}

auto machine_handle::engine() const -> core::machine&
{
  if(host_->abandoned())
  {
    throw core::machine_closed("the host was abandoned");
  }
  return *engine_;
}

auto machine_handle::abandoner() const -> detail::host_abandon
{
  return {.owned = python_owned_, .alive = alive_, .host = host_};
}

auto machine_handle::id() const noexcept -> std::uint64_t
{
  return id_;
}

void machine_handle::forward_logs(reflex::py::nb::object emit)
{
  struct flusher
  {
    std::mutex                   mutex;
    std::atomic<std::thread::id> owner{};
  };
  auto const forward = detail::held_bare(std::move(emit));
  auto const state   = std::make_shared<flusher>();
  slot_->logs->flush = [logs = slot_->logs->queue, forward, state] {
    if(state->owner.load() == std::this_thread::get_id())
    {
      return;
    }
    std::unique_lock lock{state->mutex, std::try_to_lock};
    if(not lock.owns_lock())
    {
      reflex::py::nb::gil_scoped_release const released;
      lock.lock();
    }
    state->owner.store(std::this_thread::get_id());
    auto  batch = logs->take_all();
    vtime last{};
    try
    {
      while(not batch.empty())
      {
        auto next = std::move(batch.front());
        batch.pop_front();
        last = next.time;
        try
        {
          (*forward)(next.record, next.time.ps);
        }
        catch(reflex::py::nb::python_error const& raised)
        {
          if(not raised.matches(PyExc_Exception))
          {
            batch.push_front(std::move(next));
          }
          throw;
        }
      }
      if(auto const dropped = logs->take_dropped(); dropped > 0)
      {
        try
        {
          (*forward)(
              core::log_record{
                  .kind    = core::log_kind::model_warning,
                  .message = std::format(
                      "{} log records were dropped, the queue holds {}", dropped, logs->bound()),
              },
              last.ps);
        }
        catch(reflex::py::nb::python_error const& raised)
        {
          if(not raised.matches(PyExc_Exception))
          {
            logs->restore_dropped(dropped);
          }
          throw;
        }
      }
    }
    catch(...)
    {
      logs->restore_front(std::move(batch));
      state->owner.store({});
      throw;
    }
    state->owner.store({});
  };
}

void machine_handle::flush_logs()
{
  detail::flush_logs(*slot_->logs, detail::log_errors::defer);
}

auto machine_handle::log_token() const noexcept -> std::uint64_t
{
  return slot_->logs->token;
}

auto machine_handle::gil_entries() const noexcept -> std::uint64_t
{
  return gil_entries_->load(std::memory_order_relaxed);
}

auto machine_handle::owns_engine() const noexcept -> bool
{
  return host_->owns_engine();
}

auto machine_handle::native() const noexcept -> core::machine&
{
  return *engine_;
}

auto machine_handle::host() const noexcept -> std::shared_ptr<core::host> const&
{
  return host_;
}

auto machine_handle::slot() const noexcept -> detail::host_ref
{
  return slot_;
}
}
