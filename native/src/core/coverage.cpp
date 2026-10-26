#include "machine_impl.hpp"

#include <popcorn/core/coverage.hpp>
#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <bit>
#include <ranges>
#include <utility>

namespace popcorn::core
{
namespace
{
constexpr std::size_t first_edge_slots       = 1024;
constexpr std::size_t first_unmodelled_slots = 64;
}

void flat_edge_table::grow()
{
  auto const old =
      std::exchange(slots_, std::vector<slot>(std::max(first_edge_slots, 2 * slots_.size())));
  used_           = 0;
  auto const mask = slots_.size() - 1;
  for(auto const& held : old)
  {
    if(held.hits == 0)
    {
      continue;
    }
    auto at = hash(held.key) >> 32 & mask;
    while(slots_[at].hits != 0)
    {
      at = (at + 1) & mask;
    }
    slots_[at] = held;
    ++used_;
  }
}

auto flat_edge_table::edges() const -> std::vector<edge_counter>
{
  auto found = slots_
             | std::views::filter([](slot const& held) { return held.hits != 0; })
             | std::views::transform([](slot const& held) {
                 return edge_counter{
                     .from = static_cast<address>(held.key >> 32),
                     .to   = static_cast<address>(held.key),
                     .hits = held.hits};
               })
             | std::ranges::to<std::vector>();
  std::ranges::sort(
      found, {}, [](edge_counter const& edge) { return std::pair{edge.from, edge.to}; });
  return found;
}

void flat_edge_table::clear() noexcept
{
  slots_.clear();
  used_ = 0;
}

void unmodelled_table::grow()
{
  auto const old = std::exchange(
      slots_, std::vector<entry>(std::max(first_unmodelled_slots, 2 * slots_.size())));
  used_           = 0;
  last_           = slots_.size();
  auto const mask = slots_.size() - 1;
  for(auto const& held : old)
  {
    if(held.hits == 0)
    {
      continue;
    }
    auto index = hash(held.at, held.where) >> 32 & mask;
    while(slots_[index].hits != 0)
    {
      index = (index + 1) & mask;
    }
    slots_[index] = held;
    ++used_;
  }
}

auto unmodelled_table::entries() const -> std::vector<entry>
{
  return slots_
       | std::views::filter([](entry const& held) { return held.hits != 0; })
       | std::ranges::to<std::vector>();
}

void unmodelled_table::clear() noexcept
{
  slots_.clear();
  used_ = 0;
  last_ = 0;
}

void coverage_state::leave(arch::arch const& core, address pc, bool at_exit)
{
  auto const insns = std::exchange(open_insns, 0u);
  auto const from  = static_cast<address>(previous);
  if(pc - from > max_insn_bytes * insns)
  {
    return;
  }
  auto const ran = core.instructions_between(from, pc);
  if(not ran or *ran > insns)
  {
    return;
  }
  if(*ran == insns and not(at_exit and core.ends_block_before(from, pc) == false))
  {
    return;
  }
  ++cuts[{from, pc}];
}

void machine::enable_coverage(std::vector<address_range> ranges, bool record_unmodelled)
{
  require_stopped(*this);
  if(ranges.empty())
  {
    for(auto const& loaded : images_)
    {
      for(auto const& segment : loaded.segments)
      {
        if(segment.executable and segment.memory_size != 0)
        {
          ranges.push_back(
              {.name = segment.name, .start = segment.start, .size = segment.memory_size});
        }
      }
    }
  }
  auto& state = run_->coverage;
  for(auto& range : ranges)
  {
    auto const held = std::ranges::any_of(state.blocks, [&](block_counters const& counters) {
      return counters.range.start == range.start and counters.range.size == range.size;
    });
    if(not held)
    {
      auto const halfwords = (range.size + 1) / 2;
      state.blocks.push_back(
          {.range = std::move(range), .hits = std::vector<std::uint32_t>(halfwords)});
    }
  }
  state.overlapping = false;
  for(auto const& [index, first] : std::views::enumerate(state.blocks))
  {
    for(auto const& second : state.blocks | std::views::drop(index + 1))
    {
      auto const first_end  = std::uint64_t{first.range.start} + first.range.size;
      auto const second_end = std::uint64_t{second.range.start} + second.range.size;
      if(first.range.start < second_end and second.range.start < first_end)
      {
        state.overlapping = true;
      }
    }
  }
  if(record_unmodelled and not run_->record_unmodelled)
  {
    run_->acquire_exact_pc();
  }
  state.has_previous = false;
  run_->coverage_on  = true;
  if(run_->record_unmodelled and not record_unmodelled)
  {
    run_->release_exact_pc();
  }
  run_->record_unmodelled = record_unmodelled;
}

void machine::disable_coverage()
{
  require_stopped(*this);
  run_->coverage_on           = false;
  run_->coverage.has_previous = false;
  if(std::exchange(run_->record_unmodelled, false))
  {
    run_->release_exact_pc();
  }
}

auto machine::coverage_enabled() const noexcept -> bool
{
  return run_->coverage_on;
}

auto machine::unmodelled_recorded() const noexcept -> bool
{
  return run_->record_unmodelled;
}

auto machine::coverage() const -> coverage_data
{
  require_view_access();
  auto const& state = run_->coverage;
  return {
      .blocks     = state.blocks,
      .edges      = state.edges.edges(),
      .cuts       = state.cuts
                  | std::views::transform([](auto const& held) {
                return cut_counter{
                    .start = held.first.first, .at = held.first.second, .hits = held.second};
                    })
                  | std::ranges::to<std::vector>(),
      .unmodelled = run_->unmodelled_accesses(),
  };
}

void machine::clear_coverage()
{
  require_stopped(*this);
  auto& state = run_->coverage;
  for(auto& counters : state.blocks)
  {
    std::ranges::fill(counters.hits, 0u);
  }
  state.edges.clear();
  state.cuts.clear();
  run_->unmodelled_hits.clear();
  state.has_previous = false;
}
}
