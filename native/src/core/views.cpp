#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <ranges>

namespace popcorn::core
{

void machine::require_view_access() const
{
  if(run_->running and not run_->in_handler)
  {
    throw machine_running("the machine is running a slice");
  }
}

auto memory_view::filtered() const noexcept -> bool
{
  return secure_.has_value() and machine_->options_.trustzone;
}

auto memory_view::filtered_attrs(address addr) const -> std::optional<bus_attrs>
{
  return machine_->arch_->debug_attrs(addr, *secure_);
}

auto memory_view::read_filtered(address addr, std::span<std::byte> out) const
    -> std::expected<void, block_error>
{
  std::size_t done = 0;
  while(done < out.size())
  {
    auto const at    = static_cast<address>(addr + done);
    auto const size  = block_access_size(at, out.size() - done);
    auto const attrs = filtered_attrs(at);
    if(not attrs)
    {
      return std::unexpected(block_error{.error = bus_error::denied, .at = at});
    }
    auto const value = machine_->do_read(at, size, *attrs);
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

auto memory_view::write_filtered(address addr, std::span<std::byte const> in) const
    -> std::expected<void, block_error>
{
  std::size_t done = 0;
  while(done < in.size())
  {
    auto const at    = static_cast<address>(addr + done);
    auto const size  = block_access_size(at, in.size() - done);
    auto const attrs = filtered_attrs(at);
    if(not attrs)
    {
      return std::unexpected(block_error{.error = bus_error::denied, .at = at});
    }
    std::uint32_t value = 0;
    for(std::size_t i = size; i-- > 0;)
    {
      value = value << 8 | std::to_integer<std::uint32_t>(in[done + i]);
    }
    if(auto const written = machine_->do_write(at, size, value, *attrs); not written)
    {
      return std::unexpected(block_error{.error = written.error(), .at = at});
    }
    done += size;
  }
  return {};
}

auto memory_view::read(address addr, std::uint8_t size) const
    -> std::expected<std::uint32_t, bus_error>
{
  machine_->require_view_access();
  machine_->run_->refuse_unmodelled(addr, size);
  if(filtered())
  {
    auto const attrs = filtered_attrs(addr);
    if(not attrs)
    {
      return std::unexpected(bus_error::denied);
    }
    return machine_->do_read(addr, size, *attrs);
  }
  return machine_->do_read(addr, size, bus_attrs{.by = initiator::debugger});
}

auto memory_view::is_plain_memory(address addr, std::uint8_t size) const noexcept -> bool
{
  return machine::mapped_region::find(machine_->memory_, addr, size) != nullptr;
}

auto memory_view::write(address addr, std::uint8_t size, std::uint32_t value) const
    -> std::expected<void, bus_error>
{
  machine_->require_view_access();
  machine_->run_->refuse_unmodelled(addr, size);
  if(filtered())
  {
    auto const attrs = filtered_attrs(addr);
    if(not attrs)
    {
      return std::unexpected(bus_error::denied);
    }
    return machine_->do_write(addr, size, value, *attrs);
  }
  return machine_->do_write(addr, size, value, bus_attrs{.by = initiator::debugger});
}

auto memory_view::read_block(address addr, std::span<std::byte> out) const
    -> std::expected<void, block_error>
{
  machine_->require_view_access();
  machine_->run_->refuse_unmodelled(addr, out.size());
  if(filtered())
  {
    return read_filtered(addr, out);
  }
  return machine_->do_read_block(addr, out, bus_attrs{.by = initiator::debugger});
}

auto memory_view::write_block(address addr, std::span<std::byte const> in) const
    -> std::expected<void, block_error>
{
  machine_->require_view_access();
  machine_->run_->refuse_unmodelled(addr, in.size());
  if(filtered())
  {
    return write_filtered(addr, in);
  }
  return machine_->do_write_block(addr, in, bus_attrs{.by = initiator::debugger});
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
  if(machine_->run_->running and machine_->arch_->translation_register(reg))
  {
    throw machine_running(
        std::format(
            "{} cannot be written from a handler: code already translated keeps the old value "
            "for the rest of the run, write it from a stop",
            reg.name));
  }
  machine_->run_->coverage.has_previous = false;
  auto* const engine                    = machine_->engine_.get();
  auto const  what                      = std::format("writing register {}", reg.name);
  value                                 = machine_->arch_->host_register_value(reg, value);
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
