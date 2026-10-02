#include "machine_impl.hpp"

#include <popcorn/core/coverage.hpp>
#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <ranges>
#include <utility>

namespace popcorn::core
{
namespace
{
constexpr std::size_t first_edge_slots = 1024;

constexpr address max_insn_bytes = 4;

auto edge_hash(address from, address to) noexcept -> std::uint64_t
{
  return (std::uint64_t{from} << 32 | to) * 0x9e37'79b9'7f4a'7c15;
}

auto holds(address_range const& range, address addr) noexcept -> bool
{
  return range.start <= addr and std::uint64_t{addr} < std::uint64_t{range.start} + range.size;
}

void bump(block_counters& counters, address addr) noexcept
{
  auto& hits = counters.hits[(addr - counters.range.start) >> 1];
  if(hits != std::numeric_limits<std::uint32_t>::max())
  {
    ++hits;
  }
}
}

void flat_edge_table::count(address from, address to)
{
  if(2 * (used_ + 1) > slots_.size())
  {
    grow();
  }
  auto const mask = slots_.size() - 1;
  for(auto at = edge_hash(from, to) >> 32 & mask;; at = (at + 1) & mask)
  {
    auto& held = slots_[at];
    if(held.hits == 0)
    {
      held = {.from = from, .to = to, .hits = 1};
      ++used_;
      return;
    }
    if(held.from == from and held.to == to)
    {
      ++held.hits;
      return;
    }
  }
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
    auto at = edge_hash(held.from, held.to) >> 32 & mask;
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
                 return edge_counter{.from = held.from, .to = held.to, .hits = held.hits};
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

void coverage_state::count(address addr, std::uint32_t insns)
{
  auto inside = false;
  if(last_range < blocks.size() and holds(blocks[last_range].range, addr) and not overlapping)
  {
    bump(blocks[last_range], addr);
    inside = true;
  }
  else
  {
    for(auto const& [index, counters] : std::views::enumerate(blocks))
    {
      if(holds(counters.range, addr))
      {
        bump(counters, addr);
        last_range = static_cast<std::size_t>(index);
        inside     = true;
        if(not overlapping)
        {
          break;
        }
      }
    }
  }
  if(not inside)
  {
    has_previous = false;
    open_insns   = 0;
    return;
  }
  if(has_previous)
  {
    edges.count(previous, addr);
  }
  previous     = addr;
  has_previous = true;
  open_insns   = insns;
}

void coverage_state::leave(arch::arch const& core, address pc, bool at_exit)
{
  auto const insns = std::exchange(open_insns, 0u);
  if(pc - previous > max_insn_bytes * insns)
  {
    return;
  }
  auto const ran = core.instructions_between(previous, pc);
  if(not ran or *ran > insns)
  {
    return;
  }
  if(*ran == insns and not(at_exit and core.ends_block_before(previous, pc) == false))
  {
    return;
  }
  ++cuts[{previous, pc}];
}

void machine::enable_coverage(std::vector<address_range> ranges)
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
  state.has_previous = false;
  run_->coverage_on  = true;
}

void machine::disable_coverage()
{
  require_stopped(*this);
  run_->coverage_on           = false;
  run_->coverage.has_previous = false;
}

auto machine::coverage_enabled() const noexcept -> bool
{
  return run_->coverage_on;
}

auto machine::coverage() const -> coverage_data
{
  require_view_access();
  auto const& state = run_->coverage;
  return {
      .blocks = state.blocks,
      .edges  = state.edges.edges(),
      .cuts   = state.cuts
              | std::views::transform([](auto const& held) {
                return cut_counter{
                    .start = held.first.first, .at = held.first.second, .hits = held.second};
                })
              | std::ranges::to<std::vector>(),
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
  state.has_previous = false;
}
}
