#include <popcorn/image/image.hpp>

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <vector>

namespace popcorn::image
{
namespace
{
auto levenshtein(std::string_view a, std::string_view b) -> std::size_t
{
  std::vector<std::size_t> prev(b.size() + 1);
  std::vector<std::size_t> curr(b.size() + 1);
  for(std::size_t j = 0; j <= b.size(); ++j)
  {
    prev[j] = j;
  }
  for(std::size_t i = 1; i <= a.size(); ++i)
  {
    curr[0] = i;
    for(std::size_t j = 1; j <= b.size(); ++j)
    {
      auto const cost = a[i - 1] == b[j - 1] ? 0 : 1;
      curr[j]         = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, curr);
  }
  return prev[b.size()];
}

constexpr std::size_t max_close_matches = 5;

auto close_matches(std::string_view name, std::vector<symbol> const& symbols)
    -> std::vector<std::string>
{
  std::vector<std::string> names;
  names.reserve(symbols.size());
  for(auto const& sym : symbols)
  {
    names.push_back(sym.name);
  }
  std::ranges::sort(names);
  names.erase(std::ranges::unique(names).begin(), names.end());

  std::vector<std::pair<std::size_t, std::string const*>> ranked;
  ranked.reserve(names.size());
  for(auto const& candidate : names)
  {
    ranked.emplace_back(levenshtein(name, candidate), &candidate);
  }
  std::ranges::partial_sort(
      ranked,
      ranked.begin() + static_cast<std::ptrdiff_t>(std::min(max_close_matches, ranked.size())),
      [](auto const& lhs, auto const& rhs) {
        return lhs.first != rhs.first ? lhs.first < rhs.first : *lhs.second < *rhs.second;
      });
  std::vector<std::string> out;
  for(auto const& [dist, name_ptr] : ranked | std::views::take(max_close_matches))
  {
    out.push_back(*name_ptr);
  }
  return out;
}
}

auto image::lookup(std::string_view name) const -> symbol const&
{
  std::vector<symbol const*> matches;
  for(auto const& sym : symbols)
  {
    if(sym.name == name)
    {
      matches.push_back(&sym);
    }
  }

  if(matches.empty())
  {
    throw symbol_error{std::string{name}, close_matches(name, symbols)};
  }
  if(matches.size() == 1)
  {
    return *matches.front();
  }

  symbol const* global_match = nullptr;
  for(auto const* sym : matches)
  {
    if(sym->global)
    {
      if(global_match != nullptr)
      {
        global_match = nullptr;
        break;
      }
      global_match = sym;
    }
  }
  if(global_match != nullptr)
  {
    return *global_match;
  }

  std::vector<std::string> all;
  all.reserve(matches.size());
  for(auto const* sym : matches)
  {
    all.push_back(std::format("{}@{:#010x}", sym->name, sym->value));
  }
  throw symbol_error{std::string{name}, std::move(all)};
}

namespace
{
auto rank_kind(symbol_kind kind) -> int
{
  switch(kind)
  {
    case symbol_kind::function:
      return 0;
    case symbol_kind::object:
      return 1;
    case symbol_kind::other:
      return 2;
  }
  return 3;
}

auto max_symbol_size(std::vector<symbol> const& symbols) -> std::uint32_t
{
  std::uint32_t max_size = 0;
  for(auto const& sym : symbols)
  {
    max_size = std::max(max_size, sym.size);
  }
  return max_size;
}
}

auto image::locate(address addr) const -> location
{
  location result{.pc = addr};

  if(!symbols.empty())
  {
    auto const it    = std::ranges::upper_bound(symbols, addr, {}, &symbol::value);
    auto const bound = static_cast<std::size_t>(it - symbols.begin());
    auto const limit = max_symbol_size(symbols);

    symbol const* best = nullptr;
    for(std::size_t i = bound; i-- > 0;)
    {
      auto const& sym    = symbols[i];
      auto const  offset = addr - sym.value;
      if(offset > limit)
      {
        break;
      }
      auto const span = std::max<std::uint32_t>(sym.size, 1);
      if(offset >= span)
      {
        continue;
      }
      if(best
         == nullptr
         || rank_kind(sym.kind)
         < rank_kind(best->kind)
         || (rank_kind(sym.kind) == rank_kind(best->kind) && sym.size < best->size))
      {
        best = &sym;
      }
    }
    if(best != nullptr)
    {
      result.symbol = best->name;
      result.offset = addr - best->value;
    }
  }

  if(!lines.empty())
  {
    auto const it = std::ranges::upper_bound(lines, addr, {}, &line_row::addr);
    if(it != lines.begin())
    {
      auto const& row = *(it - 1);
      if(row.line != 0)
      {
        result.file =
            row.file < files.size() ? std::string_view{files[row.file]} : std::string_view{};
        result.line = row.line;
      }
    }
  }

  return result;
}
}
