#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <ranges>

namespace popcorn::core
{
namespace
{
auto writable_by(region_desc const& region, initiator by) noexcept -> bool
{
  return by == initiator::debugger or (region.acc.write and region.kind != region_kind::rom);
}
}

void machine::require_view_access() const
{
  if(run_->running and not run_->in_handler)
  {
    throw machine_running("the machine is running a slice");
  }
}

auto machine::do_read(address addr, std::uint8_t size, initiator)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const* const region = mapped_region::find(memory_, addr, size);
  if(region == nullptr)
  {
    return std::unexpected(bus_error::unmapped);
  }
  auto const*   bytes = region->host(addr);
  std::uint32_t value = 0;
  for(auto const byte : std::span{bytes, size} | std::views::reverse)
  {
    value = value << 8 | std::to_integer<std::uint32_t>(byte);
  }
  return value;
}

auto machine::do_write(address addr, std::uint8_t size, std::uint32_t value, initiator by)
    -> std::expected<void, bus_error>
{
  auto const* const region = mapped_region::find(memory_, addr, size);
  if(region == nullptr)
  {
    return std::unexpected(bus_error::unmapped);
  }
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

auto machine::do_read_block(address addr, std::span<std::byte> out, initiator)
    -> std::expected<void, block_error>
{
  std::size_t done = 0;
  while(done < out.size())
  {
    auto const  at     = static_cast<address>(addr + done);
    auto const* region = mapped_region::find(memory_, at, 1);
    if(region == nullptr)
    {
      return std::unexpected(block_error{.error = bus_error::unmapped, .at = at});
    }
    auto const run = std::min(out.size() - done, region->remaining(at));
    std::memcpy(out.data() + done, region->host(at), run);
    done += run;
  }
  return {};
}

auto machine::do_write_block(address addr, std::span<std::byte const> in, initiator by)
    -> std::expected<void, block_error>
{
  std::size_t done = 0;
  while(done < in.size())
  {
    auto const  at     = static_cast<address>(addr + done);
    auto const* region = mapped_region::find(memory_, at, 1);
    if(region == nullptr)
    {
      return std::unexpected(block_error{.error = bus_error::unmapped, .at = at});
    }
    if(not writable_by(*region->desc, by))
    {
      return std::unexpected(block_error{.error = bus_error::denied, .at = at});
    }
    auto const run = std::min(in.size() - done, region->remaining(at));
    std::memcpy(region->host(at), in.data() + done, run);
    region->host_wrote(*run_, at, run);
    done += run;
  }
  return {};
}

auto memory_view::read(address addr, std::uint8_t size) const
    -> std::expected<std::uint32_t, bus_error>
{
  machine_->require_view_access();
  return machine_->do_read(addr, size, initiator::debugger);
}

auto memory_view::write(address addr, std::uint8_t size, std::uint32_t value) const
    -> std::expected<void, bus_error>
{
  machine_->require_view_access();
  return machine_->do_write(addr, size, value, initiator::debugger);
}

auto memory_view::read_block(address addr, std::span<std::byte> out) const
    -> std::expected<void, block_error>
{
  machine_->require_view_access();
  return machine_->do_read_block(addr, out, initiator::debugger);
}

auto memory_view::write_block(address addr, std::span<std::byte const> in) const
    -> std::expected<void, block_error>
{
  machine_->require_view_access();
  return machine_->do_write_block(addr, in, initiator::debugger);
}

auto register_view::list() const -> std::span<arch::cpu_register const>
{
  return machine_->arch_->registers();
}

auto register_view::find(std::string_view name) const -> std::optional<arch::cpu_register>
{
  auto const registers = list();
  auto const found     = std::ranges::find(registers, name, &arch::cpu_register::name);
  return found == registers.end() ? std::nullopt : std::optional{*found};
}

auto register_view::read(arch::cpu_register const& reg) const -> std::uint64_t
{
  machine_->require_view_access();
  auto* const engine = machine_->engine_.get();
  auto const  what   = std::format("reading register {}", reg.name);
  if(reg.bits > 32)
  {
    std::uint64_t value = 0;
    check(uc_reg_read(engine, reg.engine_id, &value), what);
    return value;
  }
  std::uint32_t value = 0;
  check(uc_reg_read(engine, reg.engine_id, &value), what);
  return value;
}

void register_view::write(arch::cpu_register const& reg, std::uint64_t value) const
{
  machine_->require_view_access();
  auto* const engine = machine_->engine_.get();
  auto const  what   = std::format("writing register {}", reg.name);
  if(reg.bits > 32)
  {
    check(uc_reg_write(engine, reg.engine_id, &value), what);
    return;
  }
  auto const narrow = static_cast<std::uint32_t>(value);
  check(uc_reg_write(engine, reg.engine_id, &narrow), what);
}

auto register_view::pc() const -> address
{
  machine_->require_view_access();
  return machine_->arch_->pc();
}
}
