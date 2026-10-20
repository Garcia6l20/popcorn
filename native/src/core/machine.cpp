#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::core
{
namespace
{
auto next_machine_id() -> std::uint64_t
{
  static std::atomic<std::uint64_t> last{0};
  return ++last;
}

void check_trustzone(part_description const& description, machine_options const& options)
{
  if(options.trustzone and not description.core.trustzone)
  {
    throw config_error(
        description.part, 0, "trustzone",
        std::format("a core with a security extension, {} has none", description.core.cpu));
  }
  if(not options.trustzone and not options.idau.empty())
  {
    throw config_error(description.part, 0, "idau", "no IDAU ranges without trustzone");
  }
  std::uint64_t after = 0;
  for(auto const& [index, range] : std::views::enumerate(options.idau))
  {
    if(range.start >= range.end or range.end > std::uint64_t{1} << 32 or range.start < after)
    {
      throw config_error(
          description.part, 0, std::format("idau[{}]", index),
          std::format(
              "a range with start below end, sorted and not overlapping, not {:#010x} to {:#010x}",
              range.start, range.end));
    }
    after = range.end;
  }
}

auto reset_vector_of(part_description const& description, machine_options const& options) -> address
{
  if(options.vector_table)
  {
    return *options.vector_table;
  }
  auto const& regions = description.regions;
  if(auto const at_zero = std::ranges::find(regions, address{0}, &region_desc::start);
     at_zero != regions.end())
  {
    return at_zero->start;
  }
  auto const executable =
      std::ranges::find_if(regions, [](region_desc const& region) { return region.acc.execute; });
  if(executable == regions.end())
  {
    throw config_error(
        description.part, 0, "regions",
        "a region with execute access, or machine_options::vector_table");
  }
  return executable->start;
}
}

void require_stopped(machine const& target)
{
  if(target.running())
  {
    throw machine_running("the machine is running a slice");
  }
}

auto core_clock_hz(part_description const& description, machine_options const& options)
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

machine::machine(part_description description, arch::arch_factory factory, machine_options options)
    : id_{next_machine_id()}, description_{std::move(description)}, options_{std::move(options)},
      clock_{core_clock_hz(description_, options_), options_.cpi}, clock_driver_{clock_},
      arch_events_{scheduler_, arch::arch_event_owner}, trace_{options_.trace_capacity},
      hooks_{std::make_shared<hook_registry>()}, run_{std::make_unique<run_state>(*this)}
{
  check_trustzone(description_, options_);
  run_->reset_vector       = reset_vector_of(description_, options_);
  run_->description_digest = description_digest(description_);
  run_->spin_on            = options_.spin_threshold != 0;
  hooks_->remover          = [run = run_.get()](std::uint64_t id) { run->remove_hook(id); };
  clock_driver_.on_rate_change([this] { scheduler_.rekey(clock_); });
  clock_driver_.on_charged_ahead([run = run_.get()] { return run->charged_ahead(); });
  arch_ = factory(description_.core);
  if(not arch_)
  {
    throw config_error(
        description_.part, 0, "core.cpu",
        std::format("a core the arch factory implements, not {}", description_.core.cpu));
  }
  engine_.reset(open_engine(arch_->engine()).release());
  run_->engine = engine_.get();
  memory_      = mapped_region::build(engine_.get(), description_, options_.scratch);
  try
  {
    arch_->attach(run_->arch_context.emplace(
        engine_.get(), description_, clock_, arch_events_, static_cast<machine_services&>(*this),
        options_.faults, options_.trustzone, options_.idau, options_.scratch));
    run_->attach_peripherals();
    run_->map_peripherals();
    run_->install_hooks();
  }
  catch(...)
  {
    peripherals_.clear();
    arch_.reset();
    engine_.reset();
    throw;
  }
}

machine::~machine()
{
  clock_driver_.on_rate_change({});
  clock_driver_.on_charged_ahead({});
  hooks_->remover = nullptr;
  peripherals_.clear();
  arch_.reset();
  engine_.reset();
}

auto machine::description() const noexcept -> part_description const&
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

auto machine::resets() const noexcept -> std::uint64_t
{
  return run_->resets;
}

auto machine::reset_counter() const -> std::shared_ptr<std::atomic<std::uint64_t> const>
{
  return run_->reset_count;
}

auto machine::running() const noexcept -> bool
{
  return run_->running;
}

auto machine::pending_events() const -> std::size_t
{
  return scheduler_.pending().size();
}

auto machine::next_event() const -> std::optional<icount>
{
  return scheduler_.next_deadline();
}

auto machine::held_through() const noexcept -> std::optional<icount>
{
  return run_->held_through;
}

auto machine::executed() const noexcept -> icount
{
  return run_->retired;
}

auto machine::virtual_clock() const noexcept -> core::clock const&
{
  return clock_;
}

namespace
{
void refuse_during_call_trace(bool traced)
{
  if(traced)
  {
    throw machine_busy(
        "a call trace is open and names symbols by the ids it was opened with: close the trace "
        "first");
  }
}
}

void machine::load(popcorn::image::image const& loaded)
{
  require_stopped(*this);
  refuse_during_call_trace(run_->traces_calls());
  std::vector<mapped_region*> targets;
  targets.reserve(loaded.segments.size());
  for(auto const& seg : loaded.segments)
  {
    if(seg.memory_size < seg.bytes.size())
    {
      throw image_error(
          seg.name, seg.start,
          std::format("memory size {} below its {} bytes", seg.memory_size, seg.bytes.size()));
    }
    if(seg.memory_size == 0)
    {
      targets.push_back(nullptr);
      continue;
    }
    auto* const region = mapped_region::find(memory_, seg.start, seg.memory_size);
    if(region == nullptr)
    {
      auto const* const first = mapped_region::find(memory_, seg.start, 1);
      throw image_error(
          seg.name, seg.start,
          first == nullptr
              ? std::string{"outside every region"}
              : std::format(
                    "{} bytes crossing the end of region {}", seg.memory_size, first->desc->name));
    }
    targets.push_back(region);
  }

  for(auto const& [seg, region] : std::views::zip(loaded.segments, targets))
  {
    if(region == nullptr)
    {
      continue;
    }
    auto* const out = std::ranges::copy(seg.bytes, region->host(seg.start)).out;
    std::ranges::fill_n(out, seg.memory_size - seg.bytes.size(), std::byte{0});
    region->host_wrote(*run_, seg.start, seg.memory_size);
  }
  std::size_t moved = 0;
  for(auto const& before : loaded_images())
  {
    moved += before.symbols.size();
  }
  images_.insert(images_.begin() + static_cast<std::ptrdiff_t>(loaded_count_), loaded);
  ++loaded_count_;
  run_->build_symbol_index();
  if(loaded_count_ < images_.size())
  {
    trace_.shift_call_symbols(
        static_cast<std::uint32_t>(moved), static_cast<std::uint32_t>(loaded.symbols.size()));
  }
}

void machine::add_symbols(popcorn::image::image const& symbols)
{
  require_stopped(*this);
  refuse_during_call_trace(run_->traces_calls());
  images_.push_back(symbols);
  run_->build_symbol_index();
}

auto machine::loaded_images() const noexcept -> std::span<popcorn::image::image const>
{
  return std::span{images_}.first(loaded_count_);
}

auto machine::observed_images() const noexcept -> std::span<popcorn::image::image const>
{
  return images_;
}

auto machine::locate(address pc) const -> location
{
  return run_->locate(pc);
}

void machine::reset(reset_kind kind)
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a reset would lose the frame of the call in progress");
  }
  run_->reset_machine(kind);
}

void machine::power_cycle()
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a power cycle would lose the frame of the call in progress");
  }
  auto       failure = run_->power_cut_models();
  auto const keep    = [&failure] {
    if(not failure)
    {
      failure = std::current_exception();
    }
  };
  try
  {
    run_->zero_ram();
  }
  catch(...)
  {
    keep();
  }
  try
  {
    run_->reset_machine(reset_kind::power_on);
  }
  catch(...)
  {
    keep();
  }
  if(failure)
  {
    std::rethrow_exception(failure);
  }
}

void machine::hold_core(bool held)
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a call is in progress");
  }
  if(run_->core_held == held)
  {
    return;
  }
  run_->core_held = held;
  try
  {
    run_->notify_core_hold(held);
  }
  catch(...)
  {
    run_->core_held = not held;
    run_->notify_core_hold(not held);
    throw;
  }
}

auto machine::core_held() const noexcept -> bool
{
  return run_->core_held;
}

auto machine::mem() noexcept -> memory_view
{
  return memory_view{*this};
}

auto machine::mem(bool secure) noexcept -> memory_view
{
  return memory_view{*this, secure};
}

auto machine::regs() noexcept -> register_view
{
  return register_view{*this};
}

auto machine::trace() noexcept -> trace_ring&
{
  return trace_;
}

void machine::set_ring_blocks(bool on)
{
  require_stopped(*this);
  run_->ring_blocks   = on;
  run_->block_records = run_->sinks_want_block or on;
}

auto machine::ring_blocks() const noexcept -> bool
{
  return run_->ring_blocks;
}

void machine::do_end_after_instruction()
{
  if(run_->in_engine)
  {
    run_->request_precise_exit();
  }
}

auto machine::do_emulated_instruction(address at) -> emulated_arrival
{
  return run_->in_engine ? run_->arrive_emulated(at) : emulated_arrival::proceed;
}

void machine::do_trace(trace_record const& record)
{
  if(record.kind == trace_kind::irq and record.flags == 1)
  {
    ++run_->counters.entries;
  }
  run_->emit(record, arch_->pc(), true);
}

void machine::do_log(log_record record)
{
  if(record.pc == 0)
  {
    record.pc = arch_->pc();
  }
  hooks_->log(record);
}
}
