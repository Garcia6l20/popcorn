#include "test_arch.hpp"

#include "upper_case.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <reflex/enum.hpp>
#include <reflex/serde/bson.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace popcorn::testing
{
namespace
{
constexpr std::array<std::string_view, 21> register_names{
    "r0",  "r1",  "r2", "r3", "r4", "r5",   "r6",  "r7",  "r8",      "r9",     "r10",
    "r11", "r12", "sp", "lr", "pc", "xpsr", "msp", "psp", "control", "primask"};

consteval auto engine_register(std::string_view name) -> int
{
  return reflex::to_enum_value<uc_arm_reg>("UC_ARM_REG_" + upper_case(name)).value();
}

constexpr auto core_registers = [] consteval {
  std::array<popcorn::arch::cpu_register, register_names.size()> out{};
  for(std::size_t i = 0; i < register_names.size(); ++i)
  {
    out[i] = {
        .name = register_names[i], .engine_id = engine_register(register_names[i]), .bits = 32};
  }
  return out;
}();

constexpr std::uint32_t xpsr_thumb = 0x0100'0000;
constexpr std::uint32_t reset_lr   = 0xffff'ffff;

auto cpu_model_of(core_desc const& core) -> int
{
  auto const model =
      core.cpu.starts_with('C')
          ? reflex::to_enum_value<uc_cpu_arm>("UC_CPU_ARM_CORTEX_" + core.cpu.substr(1))
          : std::nullopt;
  if(not model)
  {
    throw config_error(
        "description", 0, "core.cpu",
        std::format("a Cortex core Unicorn models, such as CM33, not {}", core.cpu));
  }
  return *model;
}

auto on_wfi(uc_engine*, void* user) noexcept -> int
{
  static_cast<test_arch::state*>(user)->idle = true;
  return 0;
}

auto system_read(uc_engine*, std::uint64_t, unsigned size, void*) noexcept -> std::uint64_t
{
  return size >= 4 ? test_arch::system_pattern
                   : test_arch::system_pattern & ((1u << (size * 8)) - 1);
}

void system_write(uc_engine*, std::uint64_t, unsigned, std::uint64_t, void*) noexcept
{}

void require(uc_err err, std::string_view what)
{
  if(err != UC_ERR_OK)
  {
    throw emulation_error(std::format("test_arch {}: {}", what, uc_strerror(err)));
  }
}
}

test_arch::test_arch(core_desc const& core) : cpu_model_{cpu_model_of(core)}
{}

auto test_arch::engine() const -> popcorn::arch::engine_spec
{
  return {
      .arch      = UC_ARCH_ARM,
      .mode      = UC_MODE_THUMB | UC_MODE_MCLASS,
      .cpu_model = cpu_model_,
  };
}

void test_arch::attach(popcorn::arch::arch_context const& ctx)
{
  context_.emplace(ctx);
  uc_hook wfi = 0;
  require(
      uc_hook_add(
          ctx.engine, &wfi, UC_HOOK_INSN, reinterpret_cast<void*>(&on_wfi), &state_, 1, 0,
          UC_ARM_INS_WFI),
      "adding the WFI hook, popcorn needs the pinned dev Unicorn");
  require(
      uc_mmio_map(ctx.engine, system_base, system_size, &system_read, this, &system_write, this),
      "mapping the system page");
}

void test_arch::reset(address reset_vector)
{
  auto&      services = context_->services;
  auto const sp       = services.read(reset_vector, 4, initiator::cpu);
  auto const pc       = services.read(reset_vector + 4, 4, initiator::cpu);
  if(not sp or not pc)
  {
    throw emulation_error(
        std::format("test_arch reset: no vector table at {:#010x}", reset_vector));
  }
  state_ = {};
  write(UC_ARM_REG_CONTROL, 0);
  write(UC_ARM_REG_PRIMASK, 0);
  write(UC_ARM_REG_MSP, *sp);
  write(UC_ARM_REG_SP, *sp);
  write(UC_ARM_REG_LR, reset_lr);
  write(UC_ARM_REG_XPSR, xpsr_thumb);
  write(UC_ARM_REG_PC, *pc | 1);
  set_pending(false);
}

auto test_arch::deliver_pending() -> popcorn::arch::trap_result
{
  return popcorn::arch::trap_result::resume();
}

auto test_arch::deliverable_now() const -> bool
{
  return false;
}

void test_arch::set_irq_line(std::uint32_t, bool)
{}

auto test_arch::on_interrupt(std::uint32_t) -> popcorn::arch::trap_result
{
  throw not_implemented("test_arch::on_interrupt");
}

auto test_arch::on_invalid_insn() -> popcorn::arch::trap_result
{
  throw not_implemented("test_arch::on_invalid_insn");
}

auto test_arch::on_unmapped(address, popcorn::arch::memory_op) -> popcorn::arch::trap_result
{
  throw not_implemented("test_arch::on_unmapped");
}

void test_arch::on_event(std::uint32_t, std::uint64_t)
{}

auto test_arch::idle() const -> bool
{
  return state_.idle;
}

auto test_arch::pc() const -> address
{
  return read(UC_ARM_REG_PC);
}

auto test_arch::resume_address() const -> address
{
  return pc() | 1;
}

auto test_arch::registers() const -> std::span<popcorn::arch::cpu_register const>
{
  return core_registers;
}

void test_arch::prepare_call(popcorn::arch::call_frame const&)
{
  throw not_implemented("test_arch::prepare_call");
}

auto test_arch::call_result(popcorn::arch::call_frame const&) const -> std::uint64_t
{
  throw not_implemented("test_arch::call_result");
}

void test_arch::read_args(popcorn::arch::call_frame&) const
{
  throw not_implemented("test_arch::read_args");
}

auto test_arch::return_address() const -> address
{
  return read(UC_ARM_REG_LR) & ~address{1};
}

void test_arch::return_to_caller(
    std::optional<popcorn::arch::call_arg> const&,
    popcorn::arch::float_abi)
{
  throw not_implemented("test_arch::return_to_caller");
}

void test_arch::save(std::vector<std::byte>& out) const
{
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(state_);
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void test_arch::load(std::span<std::byte const> in)
{
  state_ = reflex::serde::bson::deserializer{in}.load<state>();
  set_pending(false);
}

auto test_arch::attached_engine() const noexcept -> uc_struct*
{
  return context_ ? context_->engine : nullptr;
}

auto test_arch::attached_services() const noexcept -> core::machine_services&
{
  return context_->services;
}

auto test_arch::read(int engine_id) const -> std::uint32_t
{
  std::uint32_t value = 0;
  require(uc_reg_read(context_->engine, engine_id, &value), "reading a register");
  return value;
}

void test_arch::write(int engine_id, std::uint32_t value) const
{
  require(uc_reg_write(context_->engine, engine_id, &value), "writing a register");
}

auto test_arch_factory() -> popcorn::arch::arch_factory
{
  return [](core_desc const& core) -> std::unique_ptr<popcorn::arch::arch> {
    return std::make_unique<test_arch>(core);
  };
}
}
