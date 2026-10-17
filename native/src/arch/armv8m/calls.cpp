#include "aapcs.hpp"
#include "cortex_m.hpp"
#include "exceptions.hpp"
#include "frame.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <vector>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr address       system_region = 0xE000'0000;
constexpr std::uint32_t control_npriv = 1u << 0;
constexpr std::uint32_t control_spsel = 1u << 1;
constexpr std::uint32_t control_fpca  = 1u << 2;
constexpr std::uint32_t control_sfpa  = 1u << 3;
constexpr std::uint32_t xpsr_thumb    = 0x0100'0000;

constexpr std::array core_ids{UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3};

void check_abi(float_abi abi, bool fpu)
{
  if(abi == float_abi::hard_single)
  {
    throw config_error(
        "call", 0, "abi", "soft or hard_double on Arm, hard_single is a RISC-V convention");
  }
  if(abi == float_abi::hard_double and not fpu)
  {
    throw config_error("call", 0, "abi", "soft on a core without an FPU");
  }
}

auto single_id(std::uint32_t index) -> int
{
  return UC_ARM_REG_S0 + static_cast<int>(index);
}

auto floating(value_kind kind) noexcept -> bool
{
  return kind == value_kind::f32 or kind == value_kind::f64;
}
}

void cortex_m::check_call(call_frame const& frame) const
{
  check_abi(frame.abi, core_.fpu);
  if(frame.return_sentinel >= system_region)
  {
    throw config_error(
        "call", 0, "return_sentinel",
        "an address below 0xE0000000, the core's execute-never system region");
  }
}

void cortex_m::prepare_call(call_frame const& frame)
{
  rethrow_failure();
  check_call(frame);
  auto* const engine  = engine_handle();
  auto&       bus     = context_->services;
  auto const  layout  = aapcs::allocate(frame.args, frame.abi == float_abi::hard_double);
  auto        special = read_special(engine, stack_limits());
  auto const  thread  = special.ipsr == 0;
  auto const  process = [&] {
    switch(frame.stack_select)
    {
      case call_stack::main:
        return false;
      case call_stack::process:
        return true;
      case call_stack::current:
        return thread and (special.control & control_spsel) != 0;
    }
    return false;
  }();
  auto const                 base = frame.stack.value_or(process ? special.psp : special.msp);
  auto const                 sp   = static_cast<address>((base - layout.stack_bytes) & ~address{7});
  std::vector<std::uint32_t> stack(layout.stack_bytes / 4, 0);
  for(auto const [arg, place] : std::views::zip(frame.args, layout.args))
  {
    if(place.where == aapcs::place::stack)
    {
      stack[place.index / 4] = static_cast<std::uint32_t>(arg.bits);
      if(place.words == 2)
      {
        stack[place.index / 4 + 1] = static_cast<std::uint32_t>(arg.bits >> 32);
      }
    }
  }
  auto const below   = std::max<address>(layout.stack_bytes, 8);
  auto const from    = layout.stack_bytes == 0 ? sp - 8 : sp;
  auto       covered = std::uint64_t{from};
  auto const end     = std::uint64_t{from} + below;
  for(auto grew = true; covered < end and grew;)
  {
    grew = false;
    for(auto const& r : context_->description.regions)
    {
      auto const last = std::uint64_t{r.start} + r.size;
      if(r.acc.write and covered >= r.start and covered < last)
      {
        covered = last;
        grew    = true;
      }
    }
  }
  auto const fits = covered >= end;
  if(not fits)
  {
    throw config_error(
        "call", 0, "stack", std::format("writable memory below {:#010x}", from + below));
  }
  auto const stacking = bus_attrs{
      .by = initiator::cpu, .secure = not trustzone() or read_security(engine) == security::secure};
  if(not stack.empty() and not bus.write_block(sp, std::as_bytes(std::span{stack}), stacking))
  {
    throw config_error("call", 0, "stack", std::format("writable memory at {:#010x}", sp));
  }

  state_.sleep.reset();
  state_.call_masked = not frame.interrupts;
  refresh_pending();
  for(auto const [arg, place] : std::views::zip(frame.args, layout.args))
  {
    auto const low  = static_cast<std::uint32_t>(arg.bits);
    auto const high = static_cast<std::uint32_t>(arg.bits >> 32);
    if(place.where == aapcs::place::core)
    {
      write(core_ids[place.index], low);
      if(place.words == 2)
      {
        write(core_ids[place.index + 1], high);
      }
    }
    else if(place.where == aapcs::place::vfp)
    {
      write(single_id(place.index), low);
      if(place.words == 2)
      {
        write(single_id(place.index + 1), high);
      }
    }
  }
  write(UC_ARM_REG_LR, frame.return_sentinel | 1u);
  write(UC_ARM_REG_XPSR_NZCVQG, xpsr_thumb);
  write(UC_ARM_REG_PC, frame.entry | 1u);
  auto target = special;
  target.ipsr = 0;
  target.control =
      (special.control & ~(control_npriv | control_spsel | control_fpca | control_sfpa))
      | (frame.privileged ? 0u : control_npriv)
      | (process ? control_spsel : 0u);
  (process ? target.psp : target.msp) = sp;
  set_context(engine, target, stack_limits());
  write(UC_ARM_REG_EPSR, xpsr_thumb);
  in_call_ = true;
}

auto cortex_m::call_result(call_frame const& frame) const -> std::uint64_t
{
  auto const vfp = frame.abi == float_abi::hard_double and floating(frame.returns);
  auto const low = std::uint64_t{read(vfp ? UC_ARM_REG_S0 : UC_ARM_REG_R0)};
  if(not aapcs::wide(frame.returns))
  {
    return low;
  }
  return low | std::uint64_t{read(vfp ? UC_ARM_REG_S1 : UC_ARM_REG_R1)} << 32;
}

void cortex_m::read_args(call_frame& frame) const
{
  check_abi(frame.abi, core_.fpu);
  auto const layout = aapcs::allocate(frame.args, frame.abi == float_abi::hard_double);
  std::vector<std::uint32_t> stack(layout.stack_bytes / 4, 0);
  if(not stack.empty())
  {
    auto const sp = read(UC_ARM_REG_SP);
    if(not context_->services.read_block(
           sp, std::as_writable_bytes(std::span{stack}), initiator::debugger))
    {
      throw emulation_error(std::format("cortex_m reading stack arguments at {:#010x}", sp));
    }
  }
  for(auto&& [arg, place] : std::views::zip(frame.args, layout.args))
  {
    auto const word = [&](std::uint32_t offset) -> std::uint64_t {
      switch(place.where)
      {
        case aapcs::place::core:
          return read(core_ids[place.index + offset]);
        case aapcs::place::vfp:
          return read(single_id(place.index + offset));
        case aapcs::place::stack:
          return stack[place.index / 4 + offset];
      }
      return 0;
    };
    arg.bits = word(0) | (place.words == 2 ? word(1) << 32 : 0);
  }
}

auto cortex_m::return_address() const -> address
{
  return read(UC_ARM_REG_LR) & ~address{1};
}

void cortex_m::return_to_caller(std::optional<call_arg> const& value, float_abi abi)
{
  check_abi(abi, core_.fpu);
  if(value)
  {
    auto const vfp  = abi == float_abi::hard_double and floating(value->kind);
    auto const low  = static_cast<std::uint32_t>(value->bits);
    auto const high = static_cast<std::uint32_t>(value->bits >> 32);
    write(vfp ? UC_ARM_REG_S0 : UC_ARM_REG_R0, low);
    if(aapcs::wide(value->kind))
    {
      write(vfp ? UC_ARM_REG_S1 : UC_ARM_REG_R1, high);
    }
  }
  write(UC_ARM_REG_PC, read(UC_ARM_REG_LR) | 1u);
}
}
