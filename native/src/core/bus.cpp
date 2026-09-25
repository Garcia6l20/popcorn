#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstring>
#include <ranges>

namespace popcorn::core
{
namespace
{
auto writable_by(region_desc const& region, initiator by) noexcept -> bool
{
  return by == initiator::debugger or (region.acc.write and region.kind != region_kind::rom);
}

auto read_host(std::byte const* bytes, std::uint8_t size) noexcept -> std::uint32_t
{
  std::uint32_t value = 0;
  for(auto const byte : std::span{bytes, size} | std::views::reverse)
  {
    value = value << 8 | std::to_integer<std::uint32_t>(byte);
  }
  return value;
}
}

auto machine::do_read(address addr, std::uint8_t size, initiator by)
    -> std::expected<std::uint32_t, bus_error>
{
  if(auto const* const region = mapped_region::find(memory_, addr, size))
  {
    return read_host(region->host(addr), size);
  }
  if(auto const* const window = run_->window_at(addr, size))
  {
    return run_->dispatch_read(*window, addr - window->base, size, by);
  }
  std::uint32_t value = 0;
  if(uc_mem_read(engine_.get(), addr, &value, size) != UC_ERR_OK)
  {
    return std::unexpected(bus_error::unmapped);
  }
  return value;
}

auto machine::do_write(address addr, std::uint8_t size, std::uint32_t value, initiator by)
    -> std::expected<void, bus_error>
{
  if(auto const* const region = mapped_region::find(memory_, addr, size))
  {
    if(not writable_by(*region->desc, by))
    {
      return std::unexpected(bus_error::denied);
    }
    for(auto& byte : std::span{region->host(addr), size})
    {
      byte = static_cast<std::byte>(value & 0xff);
      value >>= 8;
    }
    region->host_wrote(*run_, addr, size);
    return {};
  }
  if(auto const* const window = run_->window_at(addr, size))
  {
    return run_->dispatch_write(*window, addr - window->base, size, value, by);
  }
  if(uc_mem_write(engine_.get(), addr, &value, size) != UC_ERR_OK)
  {
    return std::unexpected(bus_error::unmapped);
  }
  return {};
}

auto machine::do_read_block(address addr, std::span<std::byte> out, initiator by)
    -> std::expected<void, block_error>
{
  std::size_t done = 0;
  while(done < out.size())
  {
    auto const at = static_cast<address>(addr + done);
    if(auto const* const region = mapped_region::find(memory_, at, 1))
    {
      auto const run = std::min(out.size() - done, region->remaining(at));
      std::memcpy(out.data() + done, region->host(at), run);
      done += run;
      continue;
    }
    auto const size  = block_access_size(at, out.size() - done);
    auto const value = do_read(at, size, by);
    if(not value)
    {
      return std::unexpected(block_error{.error = value.error(), .at = at});
    }
    for(std::size_t i = 0; i < size; ++i)
    {
      out[done + i] = static_cast<std::byte>(*value >> (8 * i));
    }
    done += size;
  }
  return {};
}

auto machine::do_write_block(address addr, std::span<std::byte const> in, initiator by)
    -> std::expected<void, block_error>
{
  std::size_t done = 0;
  while(done < in.size())
  {
    auto const at = static_cast<address>(addr + done);
    if(auto const* const region = mapped_region::find(memory_, at, 1))
    {
      if(not writable_by(*region->desc, by))
      {
        return std::unexpected(block_error{.error = bus_error::denied, .at = at});
      }
      auto const run = std::min(in.size() - done, region->remaining(at));
      std::memcpy(region->host(at), in.data() + done, run);
      region->host_wrote(*run_, at, run);
      done += run;
      continue;
    }
    auto const size       = block_access_size(at, in.size() - done);
    auto const done_write = do_write(at, size, read_host(in.data() + done, size), by);
    if(not done_write)
    {
      return std::unexpected(block_error{.error = done_write.error(), .at = at});
    }
    done += size;
  }
  return {};
}
}
