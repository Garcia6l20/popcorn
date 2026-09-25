#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <atomic>
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

auto reset_vector_of(mcu_description const& description, machine_options const& options) -> address
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

machine::machine(mcu_description description, arch::arch_factory factory, machine_options options)
    : id_{next_machine_id()}, description_{std::move(description)}, options_{std::move(options)},
      clock_{core_clock_hz(description_, options_), options_.cpi}, clock_driver_{clock_},
      arch_events_{scheduler_, arch::arch_event_owner}, trace_{options_.trace_capacity},
      hooks_{std::make_shared<hook_registry>()}, run_{std::make_unique<run_state>(*this)}
{
  run_->reset_vector = reset_vector_of(description_, options_);
  hooks_->remover    = [run = run_.get()](std::uint64_t id) { run->remove_hook(id); };
  arch_              = factory(description_.core);
  if(not arch_)
  {
    throw config_error(
        description_.part, 0, "core.cpu",
        std::format("a core the arch factory implements, not {}", description_.core.cpu));
  }
  engine_.reset(open_engine(arch_->engine()).release());
  run_->engine = engine_.get();
  memory_      = mapped_region::build(engine_.get(), description_);
  try
  {
    arch_->attach(run_->arch_context.emplace(
        engine_.get(), description_, clock_, arch_events_, static_cast<machine_services&>(*this),
        options_.faults));
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
  hooks_->remover = nullptr;
  peripherals_.clear();
  arch_.reset();
  engine_.reset();
}

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

void machine::load(popcorn::image::image const& loaded)
{
  require_stopped(*this);
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
  images_.push_back(loaded);
}

auto machine::loaded_images() const noexcept -> std::span<popcorn::image::image const>
{
  return images_;
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

auto machine::mem() noexcept -> memory_view
{
  return memory_view{*this};
}

auto machine::regs() noexcept -> register_view
{
  return register_view{*this};
}

auto machine::trace() noexcept -> trace_ring&
{
  return trace_;
}

void machine::do_trace(trace_record const& record)
{
  trace_.append(record, clock_.now());
}
}
