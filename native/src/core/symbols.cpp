#include "machine_impl.hpp"

#include <algorithm>
#include <ranges>

namespace popcorn::core
{
void machine::run_state::build_symbol_index()
{
  symbols.clear();
  symbol_index.clear();
  for(auto const& loaded : owner.images_)
  {
    for(auto const& entry : loaded.symbols)
    {
      auto const index = static_cast<std::uint32_t>(symbols.size());
      symbols.push_back(&entry);
      if(entry.kind != image::symbol_kind::other)
      {
        symbol_index.push_back(
            {.value = entry.value, .end = std::uint64_t{entry.value} + entry.size, .index = index});
      }
    }
  }
  std::ranges::stable_sort(symbol_index, {}, &symbol_entry::value);
  for(auto [position, entry] : std::views::enumerate(symbol_index))
  {
    if(entry.end != entry.value)
    {
      continue;
    }
    auto const* const region = mapped_region::find(owner.memory_, entry.value, 1);
    auto const limit = region == nullptr ? std::uint64_t{entry.value} + 1
                                         : std::uint64_t{region->desc->start} + region->desc->size;
    auto const next  = std::ranges::upper_bound(
        symbol_index.begin() + position + 1, symbol_index.end(), entry.value, {},
        &symbol_entry::value);
    entry.end = next == symbol_index.end() ? limit : std::min<std::uint64_t>(next->value, limit);
  }
  symbol_max_end.clear();
  std::uint64_t reach = 0;
  for(auto const& entry : symbol_index)
  {
    reach = std::max(reach, entry.end);
    symbol_max_end.push_back(reach);
  }
}

auto machine::run_state::symbol_at(address pc) const noexcept
    -> std::optional<std::pair<std::uint32_t, std::uint32_t>>
{
  auto position = static_cast<std::size_t>(
      std::ranges::upper_bound(symbol_index, pc, {}, &symbol_entry::value) - symbol_index.begin());
  while(position > 0 and symbol_max_end[position - 1] > pc)
  {
    auto const& found = symbol_index[--position];
    if(pc < found.end)
    {
      return std::pair{found.index, pc - found.value};
    }
  }
  return std::nullopt;
}

auto machine::run_state::locate(address pc) const -> location
{
  location where{.pc = pc};
  if(auto const found = symbol_at(pc))
  {
    where.symbol = symbols[found->first]->name;
    where.offset = found->second;
  }
  return where;
}
}
