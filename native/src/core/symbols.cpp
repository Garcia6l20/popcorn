#include "machine_impl.hpp"

#include <algorithm>
#include <ranges>

namespace popcorn::core
{
void machine::run_state::build_symbol_index()
{
  symbols.clear();
  symbol_index       = index_symbols(owner.loaded_images());
  extra_symbol_index = index_symbols(owner.observed_images().subspan(owner.loaded_count_));
}

auto machine::run_state::index_symbols(std::span<popcorn::image::image const> images)
    -> symbol_lookup
{
  symbol_lookup out;
  auto&         index = out.entries;
  for(auto const& loaded : images)
  {
    for(auto const& entry : loaded.symbols)
    {
      auto const at = static_cast<std::uint32_t>(symbols.size());
      symbols.push_back(&entry);
      if(entry.kind != image::symbol_kind::other)
      {
        index.push_back(
            {.value = entry.value, .end = std::uint64_t{entry.value} + entry.size, .index = at});
      }
    }
  }
  std::ranges::stable_sort(index, {}, &symbol_entry::value);
  for(auto [position, entry] : std::views::enumerate(index))
  {
    if(entry.end != entry.value)
    {
      continue;
    }
    auto const* const region = mapped_region::find(owner.memory_, entry.value, 1);
    auto const limit = region == nullptr ? std::uint64_t{entry.value} + 1
                                         : std::uint64_t{region->desc->start} + region->desc->size;
    auto const next  = std::ranges::upper_bound(
        index.begin() + position + 1, index.end(), entry.value, {}, &symbol_entry::value);
    entry.end = next == index.end() ? limit : std::min<std::uint64_t>(next->value, limit);
  }
  std::uint64_t reach = 0;
  for(auto const& entry : index)
  {
    reach = std::max(reach, entry.end);
    out.max_end.push_back(reach);
  }
  return out;
}

namespace
{
template <typename Lookup>
auto lookup_symbol(Lookup const& lookup, address pc) noexcept
    -> std::optional<std::pair<std::uint32_t, std::uint32_t>>
{
  auto const& index    = lookup.entries;
  auto        position = static_cast<std::size_t>(
      std::ranges::upper_bound(index, pc, {}, [](auto const& entry) { return entry.value; })
      - index.begin());
  while(position > 0 and lookup.max_end[position - 1] > pc)
  {
    auto const& found = index[--position];
    if(pc < found.end)
    {
      return std::pair{found.index, pc - found.value};
    }
  }
  return std::nullopt;
}
}

auto machine::run_state::symbol_at(address pc) const noexcept
    -> std::optional<std::pair<std::uint32_t, std::uint32_t>>
{
  if(auto const found = lookup_symbol(symbol_index, pc))
  {
    return found;
  }
  return lookup_symbol(extra_symbol_index, pc);
}

auto machine::run_state::locate(address pc) const -> location
{
  location where{.pc = pc};
  if(auto const found = symbol_at(pc))
  {
    where.symbol = symbols[found->first]->name;
    where.offset = found->second;
  }
  auto const holds = [pc](image::image const& loaded) {
    return std::ranges::any_of(loaded.segments, [pc](image::segment const& seg) {
      return seg.start <= pc and pc < std::uint64_t{seg.start} + seg.memory_size;
    });
  };
  auto const images = owner.loaded_images() | std::views::reverse;
  if(auto const holder = std::ranges::find_if(images, holds); holder != images.end())
  {
    auto const source = holder->locate(pc);
    where.file        = source.file;
    where.line        = source.line;
  }
  for(auto const& extra :
      owner.observed_images().subspan(owner.loaded_count_) | std::views::reverse)
  {
    if(where.file.empty() and holds(extra))
    {
      auto const source = extra.locate(pc);
      where.file        = source.file;
      where.line        = source.line;
    }
  }
  return where;
}
}
