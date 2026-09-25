#include "memory_map.hpp"

#include "engine.hpp"
#include "machine_impl.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <tuple>

namespace popcorn::core
{
namespace
{
constexpr std::align_val_t host_alignment{4096};

auto region_key(region_desc const& region) -> std::string
{
  return std::format("regions.{}", region.name);
}

auto region_end(region_desc const& region) noexcept -> std::uint64_t
{
  return std::uint64_t{region.start} + region.size;
}

auto protection(region_desc const& region) noexcept -> std::uint32_t
{
  std::uint32_t prot = UC_PROT_NONE;
  if(region.acc.read)
  {
    prot |= UC_PROT_READ;
  }
  if(region.acc.write and region.kind != region_kind::rom)
  {
    prot |= UC_PROT_WRITE;
  }
  if(region.acc.execute)
  {
    prot |= UC_PROT_EXEC;
  }
  return prot;
}

auto allocate(region_desc const& region) -> std::shared_ptr<host_buffer>
{
  auto buffer   = std::make_shared<host_buffer>();
  buffer->size  = region.size;
  buffer->bytes = std::unique_ptr<std::byte[], aligned_free>{
      static_cast<std::byte*>(::operator new[](region.size, host_alignment))};
  std::ranges::fill_n(
      buffer->bytes.get(), region.size,
      region.kind == region_kind::rom ? std::byte{0xff} : std::byte{0});
  return buffer;
}
}

void aligned_free::operator()(std::byte* bytes) const noexcept
{
  ::operator delete[](bytes, host_alignment);
}

auto machine::mapped_region::build(uc_struct* engine, mcu_description const& description)
    -> std::vector<std::unique_ptr<mapped_region>>
{
  std::uint32_t page = 0;
  check(uc_ctl_get_page_size(engine, &page), "reading the engine page size");

  std::vector<std::unique_ptr<mapped_region>> memory;
  memory.reserve(description.regions.size());
  for(auto const& region : description.regions)
  {
    if(region.start % page != 0 or region.size % page != 0)
    {
      throw config_error(
          description.part, 0, region_key(region),
          std::format("start and size multiples of the engine page size, {} bytes", page));
    }
    memory.push_back(
        std::make_unique<mapped_region>(mapped_region{.desc = &region, .buffer = {}, .offset = 0}));
  }

  for(auto& mapped : memory)
  {
    auto const& region = *mapped->desc;
    if(not region.physical)
    {
      if(not mapped->buffer)
      {
        mapped->buffer = allocate(region);
      }
      continue;
    }
    auto const target = std::ranges::find_if(memory, [&](auto const& candidate) {
      auto const& other = *candidate->desc;
      return not other.physical
         and other.start
          <= *region.physical
         and *region.physical
           + std::uint64_t{region.size}
          <= region_end(other);
    });
    if(target == memory.end())
    {
      throw config_error(
          description.part, 0, region_key(region) + ".physical",
          std::format(
              "{:#010x} to {:#010x} inside a region without physical", *region.physical,
              *region.physical + std::uint64_t{region.size}));
    }
    if(not(*target)->buffer)
    {
      (*target)->buffer = allocate(*(*target)->desc);
    }
    mapped->buffer = (*target)->buffer;
    mapped->offset = *region.physical - (*target)->desc->start;
  }

  std::size_t mapped_count = 0;
  try
  {
    for(auto const& mapped : memory)
    {
      auto const& region = *mapped->desc;
      check(
          uc_mem_map_ptr(
              engine, region.start, region.size, protection(region),
              mapped->buffer->bytes.get() + mapped->offset),
          std::format("mapping region {}", region.name));
      ++mapped_count;
      mapped->buffer->executable = mapped->buffer->executable or region.acc.execute;
      mapped->buffer->mappings.push_back(
          {.start    = region.start,
           .offset   = mapped->offset,
           .size     = region.size,
           .writable = (protection(region) & UC_PROT_WRITE) != 0});
    }
  }
  catch(...)
  {
    for(auto const& mapped : memory | std::views::take(mapped_count))
    {
      std::ignore = uc_mem_unmap(engine, mapped->desc->start, mapped->desc->size);
    }
    throw;
  }
  return memory;
}

auto machine::mapped_region::find(
    std::span<std::unique_ptr<mapped_region> const> memory,
    address                                         addr,
    std::size_t                                     size) -> mapped_region*
{
  auto const found = std::ranges::find_if(
      memory, [&](auto const& mapped) { return mapped->contains(addr, size); });
  return found == memory.end() ? nullptr : found->get();
}

auto machine::mapped_region::contains(address addr, std::size_t size) const noexcept -> bool
{
  return desc->start <= addr and addr + std::uint64_t{size} <= region_end(*desc);
}

auto machine::mapped_region::remaining(address addr) const noexcept -> std::size_t
{
  return static_cast<std::size_t>(region_end(*desc) - addr);
}

auto machine::mapped_region::host(address addr) const noexcept -> std::byte*
{
  return buffer->bytes.get() + offset + (addr - desc->start);
}

void remove_translated(
    uc_struct*         engine,
    host_buffer const& buffer,
    std::size_t        first,
    std::size_t        last)
{
  for(auto const& window : buffer.mappings)
  {
    auto const from = std::max(first, window.offset);
    auto const to   = std::min(last, window.offset + window.size);
    if(from < to)
    {
      auto const guest = std::uint64_t{window.start} - window.offset;
      check(
          uc_ctl_remove_cache(engine, guest + from, guest + to),
          std::format("removing translated code at {:#010x}", guest + from));
    }
  }
}

void machine::mapped_region::host_wrote(run_state& run, address addr, std::size_t size) const
{
  if(not buffer->executable or size == 0)
  {
    return;
  }
  auto const first = offset + (addr - desc->start);
  run.code_changed(*buffer, first, first + size);
}
}
