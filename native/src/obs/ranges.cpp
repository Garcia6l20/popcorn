#include <popcorn/obs/coverage.hpp>

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>

namespace popcorn::obs
{
namespace
{
auto holds(image::segment const& held, address addr) noexcept -> bool
{
  auto const end = std::uint64_t{held.start} + held.memory_size;
  return held.start <= addr and addr < end;
}

auto extent(image::image const& source, image::symbol const& found) -> std::uint32_t
{
  if(found.size != 0)
  {
    return found.size;
  }
  auto const segment = std::ranges::find_if(source.segments, [&](image::segment const& held) {
    return held.executable and holds(held, found.value);
  });
  auto const limit   = segment == source.segments.end()
                         ? std::uint64_t{found.value}
                         : std::uint64_t{segment->start} + segment->memory_size;
  auto const later   = [&](image::symbol const& candidate) {
    return candidate.kind
        == image::symbol_kind::function
       and candidate.value
         > found.value
       and (segment == source.segments.end() or candidate.value < limit);
  };
  auto const next = std::ranges::find_if(source.symbols, later);
  if(next != source.symbols.end())
  {
    return next->value - found.value;
  }
  if(segment == source.segments.end())
  {
    throw symbol_error(found.name, {});
  }
  return static_cast<std::uint32_t>(limit - found.value);
}

auto range_of(std::span<image::image const> images, std::string const& name) -> address_range
{
  for(auto const& source : images)
  {
    if(std::ranges::contains(source.symbols, name, &image::symbol::name))
    {
      auto const& found = source.lookup(name);
      return {.name = name, .start = found.value, .size = extent(source, found)};
    }
  }
  if(not images.empty())
  {
    static_cast<void>(images.back().lookup(name));
  }
  throw symbol_error(name, {});
}
}

auto function_ranges(std::span<image::image const> images, std::span<std::string const> names)
    -> std::vector<address_range>
{
  return names
       | std::views::transform([&](std::string const& name) { return range_of(images, name); })
       | std::ranges::to<std::vector>();
}
}
