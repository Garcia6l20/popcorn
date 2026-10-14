#include "tb.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <utility>

namespace popcorn::obs
{
namespace
{
constexpr auto page_start(address at, std::uint32_t page) noexcept -> address
{
  return at & ~(page - 1);
}

auto segment_bytes(image::segment const& held, address start, std::uint64_t end)
    -> std::span<std::byte const>
{
  auto const bytes_end = std::uint64_t{held.start} + held.bytes.size();
  if(start < held.start or start >= bytes_end)
  {
    return {};
  }
  auto const offset = start - held.start;
  auto const stop   = std::min(bytes_end, end);
  return std::span{held.bytes}.subspan(offset, static_cast<std::size_t>(stop - start));
}

constexpr std::uint64_t address_space_end = std::uint64_t{1} << 32;

auto ends_at_size_unit(
    arch::code_analysis const& analysis,
    std::span<std::byte const> rest) noexcept -> bool
{
  return not rest.empty() and not analysis.size(rest.first(rest.size() - 1));
}

}

code_bytes::code_bytes(
    std::span<image::image const> images,
    memory_reader                 fallback,
    arch::code_analysis const*    analysis)
    : images_{images}, fallback_{std::move(fallback)}, analysis_{analysis}
{}

auto code_bytes::analysis() const noexcept -> arch::code_analysis const*
{
  return analysis_;
}

auto code_bytes::read(address start, std::uint32_t count) -> std::span<std::byte const>
{
  auto const end = std::min(std::uint64_t{start} + count, address_space_end);
  for(auto const& source : images_ | std::views::reverse)
  {
    for(auto const& held : source.segments)
    {
      if(auto const found = segment_bytes(held, start, end); not found.empty())
      {
        return found;
      }
    }
  }
  scratch_.resize(static_cast<std::size_t>(end - start));
  auto const got = fallback_ ? fallback_(start, scratch_) : 0;
  return std::span{scratch_}.first(std::min(got, scratch_.size()));
}

auto code_bytes::page(address start) -> std::span<std::byte const>
{
  auto const page = analysis_->limits().page;
  return read(start, page_start(start, page) + page - start);
}

auto decode_tb(address start, code_bytes& code) -> tb_extent
{
  tb_extent         block{.start = start};
  auto const* const analysis = code.analysis();
  if(analysis == nullptr)
  {
    return block;
  }
  auto const    limits = analysis->limits();
  auto          bytes  = code.page(start);
  auto const    page   = page_start(start, limits.page);
  std::uint32_t offset = 0;
  while(block.insns < limits.max_insns)
  {
    auto const at      = start + offset;
    auto const rest    = bytes.subspan(std::min<std::size_t>(offset, bytes.size()));
    auto       decoded = analysis->decode(at, rest);
    if(not decoded and block.insns == 0)
    {
      if(auto const needed = analysis->size(rest);
         needed and *needed > rest.size() and ends_at_size_unit(*analysis, rest))
      {
        decoded = analysis->decode(at, code.read(at, *needed));
        bytes   = {};
      }
    }
    if(not decoded or (block.insns > 0 and analysis->crosses_page(at - page, decoded->size)))
    {
      break;
    }
    block.last = *decoded;
    offset += decoded->size;
    ++block.insns;
    if(block.last.kind != arch::ending::none)
    {
      break;
    }
  }
  block.size = offset;
  return block;
}

auto size_blocks(core::coverage_data const& data, code_bytes& code) -> sized_blocks
{
  std::map<address, std::uint64_t> counts;
  for(auto const& counters : data.blocks)
  {
    for(auto const& [index, hits] : std::views::enumerate(counters.hits))
    {
      if(hits != 0)
      {
        auto& held = counts[counters.range.start + static_cast<address>(2 * index)];
        held       = std::max<std::uint64_t>(held, hits);
      }
    }
  }

  sized_blocks result;
  for(auto const& [start, hits] : counts)
  {
    auto const block = decode_tb(start, code);
    if(block.size == 0)
    {
      result.dropped += hits;
      continue;
    }
    auto const                       end   = std::uint64_t{start} + block.size;
    std::uint64_t                    unrun = 0;
    std::map<address, std::uint64_t> early;
    for(auto const& edge :
        std::ranges::equal_range(data.edges, start, {}, &core::edge_counter::from))
    {
      if(edge.to > start and edge.to < end and edge.to != block.last.target)
      {
        early[edge.to] += edge.hits;
      }
    }
    for(auto const& cut : std::ranges::equal_range(data.cuts, start, {}, &core::cut_counter::start))
    {
      if(cut.at == start)
      {
        unrun += cut.hits;
      }
      else if(cut.at < end)
      {
        early[cut.at] += cut.hits;
      }
    }
    std::uint64_t ended = unrun;
    if(not early.empty())
    {
      ++result.split;
    }
    for(auto const& [at, count] : early)
    {
      result.blocks.push_back({.start = start, .size = at - start, .hits = count});
      ended += count;
    }
    if(hits > ended)
    {
      result.blocks.push_back({.start = start, .size = block.size, .hits = hits - ended});
    }
  }
  std::ranges::sort(
      result.blocks, {}, [](block_hit const& held) { return std::pair{held.start, held.size}; });
  return result;
}
}
