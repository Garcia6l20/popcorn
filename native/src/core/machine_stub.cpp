#include <popcorn/core/machine.hpp>

#include <popcorn/core/errors.hpp>

#include <atomic>
#include <string>
#include <utility>

namespace popcorn::core
{
class hook_registry
{
public:
  bool running = false;

  void remove(std::uint64_t) noexcept
  {}
};

struct machine::mapped_region
{};

struct machine::peripheral_slot
{};

struct machine::run_state
{
  bool running    = false;
  bool in_handler = false;
  bool in_call    = false;
};

namespace
{
auto next_machine_id() -> std::uint64_t
{
  static std::atomic<std::uint64_t> last{0};
  return ++last;
}

auto core_clock_hz(mcu_description const& description, machine_options const& options)
    -> std::uint64_t
{
  std::uint64_t const hz = options.core_hz != 0 ? options.core_hz : description.core.reset_clock_hz;
  if(hz == 0)
  {
    throw config_error(
        description.part, 0, "core.reset_clock_hz",
        "a core clock above 0 Hz, or machine_options::core_hz");
  }
  return hz;
}

void require_stopped(machine const& target)
{
  if(target.running())
  {
    throw machine_running("the machine is running a slice");
  }
}
}

hook_handle::hook_handle(std::weak_ptr<hook_registry> registry, std::uint64_t id) noexcept
    : registry_{std::move(registry)}, id_{id}
{}

hook_handle::hook_handle(hook_handle&& other) noexcept
    : registry_{std::exchange(other.registry_, {})}, id_{std::exchange(other.id_, 0)}
{}

auto hook_handle::operator=(hook_handle&& other) noexcept -> hook_handle&
{
  if(this != &other)
  {
    release();
    registry_ = std::exchange(other.registry_, {});
    id_       = std::exchange(other.id_, 0);
  }
  return *this;
}

hook_handle::~hook_handle()
{
  release();
}

void hook_handle::reset()
{
  if(auto const registry = registry_.lock(); registry and registry->running)
  {
    throw machine_running("a hook cannot be removed while a slice runs");
  }
  release();
}

void hook_handle::release() noexcept
{
  if(auto const registry = registry_.lock())
  {
    registry->remove(id_);
  }
  registry_.reset();
  id_ = 0;
}

auto hook_handle::active() const noexcept -> bool
{
  return not registry_.expired();
}

intercept_call::intercept_call(
    machine&      target,
    address       function,
    address       return_address,
    std::uint64_t occurrence) noexcept
    : machine_{&target}, function_{function}, return_address_{return_address},
      occurrence_{occurrence}
{}

auto intercept_call::function() const noexcept -> address
{
  return function_;
}

auto intercept_call::return_address() const noexcept -> address
{
  return return_address_;
}

auto intercept_call::occurrence() const noexcept -> std::uint64_t
{
  return occurrence_;
}

auto intercept_call::args(std::span<arch::value_kind const>) const -> std::vector<std::uint64_t>
{
  throw not_implemented("intercept_call::args");
}

auto intercept_call::target() const noexcept -> machine&
{
  return *machine_;
}

void machine::require_view_access() const
{
  if(run_->running and not run_->in_handler)
  {
    throw machine_running("the machine is running a slice");
  }
}

auto memory_view::read(address, std::uint8_t) const -> std::expected<std::uint32_t, bus_error>
{
  machine_->require_view_access();
  throw not_implemented("memory_view::read");
}

auto memory_view::write(address, std::uint8_t, std::uint32_t) const
    -> std::expected<void, bus_error>
{
  machine_->require_view_access();
  throw not_implemented("memory_view::write");
}

auto memory_view::read_block(address, std::span<std::byte>) const
    -> std::expected<void, block_error>
{
  machine_->require_view_access();
  throw not_implemented("memory_view::read_block");
}

auto memory_view::write_block(address, std::span<std::byte const>) const
    -> std::expected<void, block_error>
{
  machine_->require_view_access();
  throw not_implemented("memory_view::write_block");
}

auto register_view::list() const -> std::span<arch::cpu_register const>
{
  throw not_implemented("register_view::list");
}

auto register_view::find(std::string_view) const -> std::optional<arch::cpu_register>
{
  throw not_implemented("register_view::find");
}

auto register_view::read(arch::cpu_register const&) const -> std::uint64_t
{
  machine_->require_view_access();
  throw not_implemented("register_view::read");
}

void register_view::write(arch::cpu_register const&, std::uint64_t) const
{
  machine_->require_view_access();
  throw not_implemented("register_view::write");
}

auto register_view::pc() const -> address
{
  machine_->require_view_access();
  throw not_implemented("register_view::pc");
}

void machine::engine_closer::operator()(uc_struct*) const noexcept
{}

machine::machine(mcu_description description, arch::arch_factory, machine_options options)
    : id_{next_machine_id()}, description_{std::move(description)}, options_{std::move(options)},
      clock_{core_clock_hz(description_, options_), options_.cpi}, clock_driver_{clock_},
      arch_events_{scheduler_, arch::arch_event_owner}, trace_{options_.trace_capacity},
      hooks_{std::make_shared<hook_registry>()}, run_{std::make_unique<run_state>()}
{}

machine::~machine() = default;

auto machine::description() const noexcept -> mcu_description const&
{
  return description_;
}

auto machine::id() const noexcept -> std::uint64_t
{
  return id_;
}

auto machine::generation() const noexcept -> std::uint64_t
{
  return generation_;
}

auto machine::running() const noexcept -> bool
{
  return run_->running;
}

auto machine::virtual_clock() const noexcept -> core::clock const&
{
  return clock_;
}

void machine::load(popcorn::image::image const&)
{
  require_stopped(*this);
  throw not_implemented("machine::load");
}

auto machine::loaded_images() const noexcept -> std::span<popcorn::image::image const>
{
  return images_;
}

void machine::reset(reset_kind)
{
  require_stopped(*this);
  throw not_implemented("machine::reset");
}

auto machine::run_slice(slice_request const&) -> slice_result
{
  if(running())
  {
    throw machine_busy("a slice is already running");
  }
  throw not_implemented("machine::run_slice");
}

void machine::begin_call(arch::call_frame const&)
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a call is already in progress");
  }
  throw not_implemented("machine::begin_call");
}

auto machine::end_call() -> std::uint64_t
{
  require_stopped(*this);
  throw not_implemented("machine::end_call");
}

auto machine::in_call() const noexcept -> bool
{
  return run_->in_call;
}

auto machine::call(arch::call_frame const& frame, icount) -> call_result
{
  begin_call(frame);
  throw not_implemented("machine::call");
}

auto machine::mem() noexcept -> memory_view
{
  return memory_view{*this};
}

auto machine::regs() noexcept -> register_view
{
  return register_view{*this};
}

auto machine::snapshot() const -> core::snapshot
{
  require_stopped(*this);
  throw not_implemented("machine::snapshot");
}

void machine::restore(core::snapshot const&)
{
  require_stopped(*this);
  throw not_implemented("machine::restore");
}

auto machine::add_intercept(address, intercept_handler, intercept_mode) -> intercept_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_intercept");
}

auto machine::add_return_hook(
    address,
    std::vector<arch::value_kind>,
    arch::value_kind,
    return_handler) -> intercept_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_return_hook");
}

auto machine::add_watch(address, std::uint32_t, watch_kind, watch_handler) -> watch_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_watch");
}

auto machine::trace() noexcept -> trace_ring&
{
  return trace_;
}

auto machine::add_trace_sink(trace_kinds, trace_sink, std::vector<address_range>) -> hook_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_trace_sink");
}

auto machine::add_log_sink(log_sink) -> hook_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_log_sink");
}

void machine::enable_coverage(std::vector<address_range>)
{
  require_stopped(*this);
  throw not_implemented("machine::enable_coverage");
}

void machine::disable_coverage()
{
  require_stopped(*this);
  throw not_implemented("machine::disable_coverage");
}

auto machine::coverage_enabled() const noexcept -> bool
{
  return false;
}

auto machine::coverage() const -> coverage_data
{
  require_view_access();
  throw not_implemented("machine::coverage");
}

void machine::clear_coverage()
{
  require_stopped(*this);
  throw not_implemented("machine::clear_coverage");
}

auto machine::peripheral(std::string_view) -> periph::register_file&
{
  throw not_implemented("machine::peripheral");
}

auto machine::model(std::string_view) -> periph::model*
{
  throw not_implemented("machine::model");
}

void machine::do_request_stop(stop_reason, std::string)
{
  throw not_implemented("machine::request_stop");
}

void machine::do_request_reset(reset_kind)
{
  throw not_implemented("machine::request_reset");
}

void machine::do_end_slice()
{
  throw not_implemented("machine::end_slice");
}

auto machine::do_read(address, std::uint8_t, initiator) -> std::expected<std::uint32_t, bus_error>
{
  throw not_implemented("machine::read");
}

auto machine::do_write(address, std::uint8_t, std::uint32_t, initiator)
    -> std::expected<void, bus_error>
{
  throw not_implemented("machine::write");
}

auto machine::do_read_block(address, std::span<std::byte>, initiator)
    -> std::expected<void, block_error>
{
  throw not_implemented("machine::read_block");
}

auto machine::do_write_block(address, std::span<std::byte const>, initiator)
    -> std::expected<void, block_error>
{
  throw not_implemented("machine::write_block");
}

void machine::do_trace(trace_record const& record)
{
  trace_.append(record, clock_.now());
}
}
