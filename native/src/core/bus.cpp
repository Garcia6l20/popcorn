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
constexpr auto engine_attrs(bus_attrs attrs) noexcept -> uc_mem_attrs
{
  return {.secure = attrs.secure, .privileged = attrs.privileged};
}

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

auto machine::do_read(address addr, std::uint8_t size, bus_attrs attrs)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const by = attrs.by;
  if(auto const* const region = mapped_region::find(memory_, addr, size))
  {
    switch(run_->check_memory(addr, size, attrs, arch::memory_op::read))
    {
      case periph::bus_verdict::allow:
        break;
      case periph::bus_verdict::read_zero_write_ignore:
        run_->note_watched(addr, size, 0, false, by);
        return 0u;
      case periph::bus_verdict::bus_error:
        return std::unexpected(bus_error::denied);
    }
    if(by == initiator::dma and not run_->controller_runs.empty())
    {
      run_->note_controller_read(*region, addr, size, attrs);
    }
    auto const value = read_host(region->host(addr), size);
    run_->note_watched(addr, size, value, false, by);
    return value;
  }
  if(auto const* const window = run_->window_at(addr, size))
  {
    auto read              = run_->dispatch_read(*window, addr - window->base, size, attrs);
    run_->firewall_refused = false;
    return read;
  }
  std::uint32_t value = 0;
  if(uc_mem_read_attrs(engine_.get(), addr, &value, size, engine_attrs(attrs)) != UC_ERR_OK)
  {
    return std::unexpected(bus_error::unmapped);
  }
  return value;
}

auto machine::do_write(address addr, std::uint8_t size, std::uint32_t value, bus_attrs attrs)
    -> std::expected<void, bus_error>
{
  auto const by = attrs.by;
  if(auto const* const region = mapped_region::find(memory_, addr, size))
  {
    switch(run_->check_memory(addr, size, attrs, arch::memory_op::write))
    {
      case periph::bus_verdict::allow:
        break;
      case periph::bus_verdict::read_zero_write_ignore:
        run_->note_watched(addr, size, value, true, by);
        return {};
      case periph::bus_verdict::bus_error:
        return std::unexpected(bus_error::denied);
    }
    if(not writable_by(*region->desc, by))
    {
      if(region->controller != nullptr)
      {
        return run_->dispatch_controller_write(*region, addr, size, value, attrs);
      }
      return std::unexpected(bus_error::denied);
    }
    auto bits = value;
    for(auto& byte : std::span{region->host(addr), size})
    {
      byte = static_cast<std::byte>(bits & 0xff);
      bits >>= 8;
    }
    region->host_wrote(*run_, addr, size);
    run_->note_watched(addr, size, value, true, by);
    return {};
  }
  if(auto const* const window = run_->window_at(addr, size))
  {
    auto done              = run_->dispatch_write(*window, addr - window->base, size, value, attrs);
    run_->firewall_refused = false;
    return done;
  }
  if(uc_mem_write_attrs(engine_.get(), addr, &value, size, engine_attrs(attrs)) != UC_ERR_OK)
  {
    return std::unexpected(bus_error::unmapped);
  }
  return {};
}

auto machine::do_read_block(address addr, std::span<std::byte> out, bus_attrs attrs)
    -> std::expected<void, block_error>
{
  auto const  by   = attrs.by;
  std::size_t done = 0;
  while(done < out.size())
  {
    auto const        at     = static_cast<address>(addr + done);
    auto const* const region = mapped_region::find(memory_, at, 1);
    auto const        misses = [&] {
      auto const first = region->offset + (at - region->desc->start);
      auto const bytes = std::min(out.size() - done, region->remaining(at));
      return by != initiator::dma
          or run_->controller_runs.empty()
          or not run_->controller_block_hit(region->buffer.get(), first, first + bytes - 1);
    };
    if(region != nullptr
       and (by == initiator::debugger or (run_->watches.empty() and not run_->filters_host(by) and misses())))
    {
      auto const run = std::min(out.size() - done, region->remaining(at));
      std::memcpy(out.data() + done, region->host(at), run);
      done += run;
      continue;
    }
    auto const size  = block_access_size(at, out.size() - done);
    auto const value = do_read(at, size, attrs);
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

auto machine::do_write_block(address addr, std::span<std::byte const> in, bus_attrs attrs)
    -> std::expected<void, block_error>
{
  auto const  by   = attrs.by;
  std::size_t done = 0;
  while(done < in.size())
  {
    auto const        at     = static_cast<address>(addr + done);
    auto const* const region = mapped_region::find(memory_, at, 1);
    if(region != nullptr and region->controller != nullptr and by != initiator::debugger)
    {
      return std::unexpected(block_error{.error = bus_error::denied, .at = at});
    }
    if(region != nullptr
       and (by == initiator::debugger or (run_->watches.empty() and not run_->filters_host(by))))
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
    auto const done_write = do_write(at, size, read_host(in.data() + done, size), attrs);
    if(not done_write)
    {
      return std::unexpected(block_error{.error = done_write.error(), .at = at});
    }
    done += size;
  }
  return {};
}
}
