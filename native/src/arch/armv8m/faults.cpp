#include "cortex_m.hpp"
#include "exceptions.hpp"

#include <popcorn/core/description.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <cstdint>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t excp_prefetch_abort = 3;
constexpr std::uint32_t excp_data_abort     = 4;
constexpr std::uint32_t excp_nocp           = 17;
constexpr std::uint32_t excp_stkof          = 19;
constexpr std::uint32_t excp_unaligned      = 22;
constexpr std::uint32_t excp_divbyzero      = 23;
constexpr std::uint32_t epsr_thumb          = 1u << 24;
constexpr std::uint32_t fsr_status_mask     = 0x40f;
constexpr std::uint32_t fsr_alignment       = 0x1;
constexpr std::uint32_t fsr_external        = 0x8;

auto enable_bit(std::uint32_t target) noexcept -> std::uint32_t
{
  return 1u << (16 + target - exc_memmanage);
}
}

auto cortex_m::raise_fault(fault_cause const& cause) -> trap_result
{
  auto&      scs    = state_.scs;
  auto const config = priority_config();
  auto const state  = exception_target(state_.exceptions, config, cause.target, current_security());
  auto const nonsecure = state == security::nonsecure and banked(cause.target);
  auto&      cfsr      = nonsecure ? scs.nonsecure.cfsr : scs.cfsr;
  cfsr |= cause.cfsr_bits;
  if(cause.data_address)
  {
    if(cause.target == exc_busfault)
    {
      scs.bfar = static_cast<std::uint32_t>(*cause.data_address);
      scs.cfsr |= cfsr::bfarvalid;
    }
    else if(cause.target == exc_memmanage)
    {
      (nonsecure ? scs.nonsecure.mmfar : scs.mmfar) =
          static_cast<std::uint32_t>(*cause.data_address);
      cfsr |= cfsr::mmarvalid;
    }
  }
  auto const current  = current_priority();
  auto const preempts = [&](std::uint32_t number, security target) {
    return exception_priority(state_.exceptions, config, number, target, true) < current;
  };
  auto const enables = nonsecure ? scs.nonsecure.shcsr_enables : scs.shcsr_enables;
  auto       target  = cause.target;
  auto       bank    = state;
  if((enables & enable_bit(target)) == 0 or (cause.synchronous and not preempts(target, bank)))
  {
    target = exc_hardfault;
    bank   = exception_target(state_.exceptions, config, exc_hardfault, state);
    scs.hfsr |= hfsr_forced;
  }
  if(target == exc_hardfault and cause.synchronous and not preempts(exc_hardfault, bank))
  {
    return trap_result::fault(
        {.kind = fault_kind::hard, .fault_address = pc(), .status = cfsr, .detail = scs.hfsr});
  }
  if(context_->faults == fault_policy::stop)
  {
    return trap_result::fault(
        {.kind = cause.kind, .fault_address = cause.reported, .status = cfsr, .detail = scs.hfsr});
  }
  pend(target, bank);
  return trap_result::resume();
}

auto cortex_m::on_invalid_insn() -> trap_result
{
  auto const at = pc();
  if((read(UC_ARM_REG_EPSR) & epsr_thumb) == 0)
  {
    state_.scs.cfsr |= cfsr::invstate;
    return trap_result::fault(
        {.kind          = fault_kind::usage,
         .fault_address = at,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  if(hint())
  {
    return trap_result::resume();
  }
  return raise_fault(
      {.kind        = fault_kind::usage,
       .target      = exc_usagefault,
       .cfsr_bits   = cfsr::undefinstr,
       .reported    = at,
       .synchronous = true});
}

auto cortex_m::on_unmapped(address addr, memory_op op) -> trap_result
{
  auto const& peripherals   = context_->description.peripherals;
  auto const  in_peripheral = std::ranges::any_of(peripherals, [&](peripheral_desc const& unit) {
    return addr >= unit.base and addr - unit.base < unit.size;
  });
  if(in_peripheral)
  {
    return raise_fault(
        {.kind        = fault_kind::bus,
         .target      = exc_busfault,
         .cfsr_bits   = cfsr::impreciserr,
         .reported    = addr,
         .synchronous = false});
  }
  if(op == memory_op::fetch)
  {
    return raise_fault(
        {.kind      = fault_kind::bus,
         .target    = exc_busfault,
         .cfsr_bits = cfsr::ibuserr,
         .reported  = addr});
  }
  return raise_fault(
      {.kind         = fault_kind::bus,
       .target       = exc_busfault,
       .cfsr_bits    = cfsr::preciserr,
       .reported     = addr,
       .data_address = addr});
}

auto cortex_m::engine_fault(std::uint32_t intno) -> trap_result
{
  auto const at = pc();
  switch(intno)
  {
    case excp_prefetch_abort:
      if((read(UC_ARM_REG_FAULT_STATUS) & fsr_status_mask) == fsr_external)
      {
        return raise_fault(
            {.kind      = fault_kind::bus,
             .target    = exc_busfault,
             .cfsr_bits = cfsr::ibuserr,
             .reported  = at});
      }
      return raise_fault(
          {.kind      = fault_kind::mem_manage,
           .target    = exc_memmanage,
           .cfsr_bits = cfsr::iaccviol,
           .reported  = at});
    case excp_data_abort:
    {
      if((read(UC_ARM_REG_FAULT_STATUS) & fsr_status_mask) == fsr_alignment)
      {
        return raise_fault(
            {.kind      = fault_kind::usage,
             .target    = exc_usagefault,
             .cfsr_bits = cfsr::unaligned,
             .reported  = at});
      }
      auto const accessed = read(UC_ARM_REG_FAULT_ADDRESS);
      return raise_fault(
          {.kind         = fault_kind::bus,
           .target       = exc_busfault,
           .cfsr_bits    = cfsr::preciserr,
           .reported     = accessed,
           .data_address = accessed});
    }
    case excp_nocp:
      return raise_fault(
          {.kind      = fault_kind::usage,
           .target    = exc_usagefault,
           .cfsr_bits = cfsr::nocp,
           .reported  = at});
    case excp_stkof:
      return raise_fault(
          {.kind      = fault_kind::usage,
           .target    = exc_usagefault,
           .cfsr_bits = cfsr::stkof,
           .reported  = at});
    case excp_unaligned:
      return raise_fault(
          {.kind      = fault_kind::usage,
           .target    = exc_usagefault,
           .cfsr_bits = cfsr::unaligned,
           .reported  = at});
    case excp_divbyzero:
      return raise_fault(
          {.kind      = fault_kind::usage,
           .target    = exc_usagefault,
           .cfsr_bits = cfsr::divbyzero,
           .reported  = at});
    default:
      return trap_result::fault(
          {.kind = fault_kind::invalid, .fault_address = at, .detail = intno});
  }
}
}
