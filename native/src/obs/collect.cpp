#include "refs.hpp"
#include "tb.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/obs/collect.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <tuple>

namespace popcorn::obs
{
auto collect(core::machine& target, std::span<image_source const> sources) -> coverage_set
{
  auto const images = target.loaded_images();
  for(auto const& [index, source] : std::views::enumerate(sources))
  {
    auto const key = std::format("sources[{}].path", index);
    switch(source.kind)
    {
      case image_kind::elf:
        if(source.path.empty())
        {
          throw config_error("collect", 0, key, "the path of the ELF");
        }
        break;
      case image_kind::raw:
        if(not source.path.empty())
        {
          throw config_error("collect", 0, key, "empty for a raw image");
        }
        break;
    }
  }
  auto const data = target.coverage();

  code_bytes code{images, [&target](address at, std::span<std::byte> out) -> std::size_t {
                    auto const read = target.mem().read_block(at, out);
                    return read ? out.size() : read.error().at - at;
                  }};
  auto       sized = size_blocks(data, code);

  coverage_group group{
      .images  = std::views::zip_transform(
                     [](image::image const& loaded, image_source const& source) {
                      return reference_of(loaded, source.kind, source.path);
                     },
                     images, sources)
               | std::ranges::to<std::vector>(),
      .ranges  = data.blocks
               | std::views::transform(&core::block_counters::range)
               | std::ranges::to<std::vector>(),
      .blocks  = std::move(sized.blocks),
      .edges   = data.edges
               | std::views::filter([](core::edge_counter const& edge) { return edge.hits != 0; })
               | std::views::transform([](core::edge_counter const& edge) {
                 return edge_hit{.from = edge.from, .to = edge.to, .hits = edge.hits};
                 })
               | std::ranges::to<std::vector>(),
      .split   = sized.split,
      .dropped = sized.dropped,
  };
  std::ranges::sort(group.ranges, {}, [](address_range const& range) {
    return std::tie(range.start, range.size, range.name);
  });
  return {.groups = {std::move(group)}};
}
}
