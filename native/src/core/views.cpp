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
  machine_->run_->coverage.has_previous = false;
  auto* const engine                    = machine_->engine_.get();
  auto const  what                      = std::format("writing register {}", reg.name);
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
