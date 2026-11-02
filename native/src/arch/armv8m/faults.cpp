#include "cortex_m.hpp"
#include "exceptions.hpp"

#include <popcorn/arch/armv8m/thumb.hpp>
#include <popcorn/core/description.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <optional>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t excp_prefetch_abort = 3;
constexpr std::uint32_t excp_data_abort     = 4;
constexpr std::uint32_t excp_nocp           = 17;
constexpr std::uint32_t excp_stkof          = 19;
constexpr std::uint32_t excp_lserr          = 21;
constexpr std::uint32_t excp_unaligned      = 22;
constexpr std::uint32_t excp_divbyzero      = 23;
constexpr std::uint32_t epsr_thumb          = 1u << 24;
constexpr std::uint32_t epsr_it             = 0x0600'fc00;
constexpr std::uint32_t fsr_status_mask     = 0x40f;
constexpr std::uint32_t fsr_alignment       = 0x1;
constexpr std::uint32_t fsr_external        = 0x8;
constexpr std::uint32_t fsr_secure          = UC_ARM_FAULT_SECURE;
constexpr std::uint32_t fsr_nsc_exec        = UC_ARM_FAULT_NSC_EXEC;
constexpr std::uint32_t nsacr_cp10          = 1u << 10;

constexpr std::array<int, 15> core_register_ids{
    UC_ARM_REG_R0,  UC_ARM_REG_R1,  UC_ARM_REG_R2,  UC_ARM_REG_R3, UC_ARM_REG_R4,
    UC_ARM_REG_R5,  UC_ARM_REG_R6,  UC_ARM_REG_R7,  UC_ARM_REG_R8, UC_ARM_REG_R9,
    UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR};

auto enable_bit(std::uint32_t target) noexcept -> std::uint32_t
{
  return 1u << (16 + target - exc_memmanage);
}

auto names(deferred_fault const& deferred, std::uint32_t number, security state) noexcept -> bool
{
  return deferred.number == number and (not banked(number) or deferred.state == state);
}

struct base_update
{
  std::uint32_t reg{};
  std::int32_t  offset{};
};

auto list_bytes(std::uint32_t list) noexcept -> std::int32_t
{
  return 4 * std::popcount(list);
}

auto signed_offset(bool up, std::uint32_t bytes) noexcept -> std::int32_t
{
  return up ? static_cast<std::int32_t>(bytes) : -static_cast<std::int32_t>(bytes);
}

auto writeback_of(std::uint32_t first, std::uint32_t second, bool wide) noexcept
    -> std::optional<base_update>
{
  if(not wide)
  {
    auto const rn   = (first >> 8) & 0x7u;
    auto const list = first & 0xffu;
    if((first & 0xfe00u) == 0xb400u)
    {
      return base_update{.reg = 13, .offset = -list_bytes(first & 0x1ffu)};
    }
    if((first & 0xfe00u) == 0xbc00u)
    {
      return base_update{.reg = 13, .offset = list_bytes(first & 0x1ffu)};
    }
    if((first & 0xf800u) == 0xc000u or ((first & 0xf800u) == 0xc800u and ((list >> rn) & 1u) == 0))
    {
      return base_update{.reg = rn, .offset = list_bytes(list)};
    }
    return std::nullopt;
  }
  auto const rn        = first & 0xfu;
  auto const writeback = (first & 0x20u) != 0;
  auto const loads     = (first & 0x10u) != 0;
  if(rn == 15)
  {
    return std::nullopt;
  }
  if(((first & 0xffc0u) == 0xe880u or (first & 0xffc0u) == 0xe900u) and writeback)
  {
    if(loads and ((second >> rn) & 1u) != 0)
    {
      return std::nullopt;
    }
    return base_update{
        .reg    = rn,
        .offset = signed_offset((first & 0xffc0u) == 0xe880u, 4u * std::popcount(second))};
  }
  if((first & 0xfe40u) == 0xe840u and writeback)
  {
    return base_update{
        .reg = rn, .offset = signed_offset((first & 0x80u) != 0, 4u * (second & 0xffu))};
  }
  if((first & 0xfe80u) == 0xf800u and (second & 0x0900u) == 0x0900u)
  {
    return base_update{.reg = rn, .offset = signed_offset((second & 0x200u) != 0, second & 0xffu)};
  }
  if((first & 0xfe00u) == 0xec00u and (second & 0x0e00u) == 0x0a00u and writeback)
  {
    return base_update{
        .reg = rn, .offset = signed_offset((first & 0x80u) != 0, 4u * (second & 0xffu))};
  }
  return std::nullopt;
}
}

auto cortex_m::create_exception(fault_cause const& cause)
    -> std::expected<pending_exception, trap_result>
{
  auto&      scs    = state_.scs;
  auto const config = priority_config();
  auto const state  = exception_target(
      state_.exceptions, config, cause.target, cause.origin.value_or(current_security()));
  auto const nonsecure = state == security::nonsecure and banked(cause.target);
  auto&      cfsr      = cause.target == exc_securefault ? scs.sfsr
                       : nonsecure                       ? scs.nonsecure.cfsr
                                                         : scs.cfsr;
  cfsr |= cause.cfsr_bits;
  if(cause.data_address)
  {
    if(cause.target == exc_securefault)
    {
      scs.sfar = static_cast<std::uint32_t>(*cause.data_address);
      scs.sfsr |= sfsr::sfarvalid;
    }
    else if(cause.target == exc_busfault)
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
    return std::unexpected{trap_result::fault(
        {.kind          = fault_kind::hard,
         .fault_address = pc(),
         .status        = cause.target == exc_securefault ? scs.cfsr : cfsr,
         .detail        = scs.hfsr})};
  }
  if(context_->faults == fault_policy::stop)
  {
    if(not cause.synchronous
       and not preempts(target, bank)
       and (not state_.deferred or names(*state_.deferred, target, bank)))
    {
      if(not state_.deferred)
      {
        state_.deferred = deferred_fault{
            .kind = cause.kind, .reported = cause.reported, .number = target, .state = bank};
      }
      return pending_exception{.number = target, .state = bank};
    }
    return std::unexpected{trap_result::fault(
        {.kind = cause.kind, .fault_address = cause.reported, .status = cfsr, .detail = scs.hfsr})};
  }
  return pending_exception{.number = target, .state = bank};
}

auto cortex_m::raise_fault(fault_cause const& cause) -> trap_result
{
  auto const raised = create_exception(cause);
  if(not raised)
  {
    return raised.error();
  }
  pend(raised->number, raised->state);
  return trap_result::resume();
}

auto cortex_m::deferred_stop(pending_exception taken) -> std::optional<trap_result>
{
  auto const& deferred = state_.deferred;
  if(not deferred or not names(*deferred, taken.number, taken.state))
  {
    return std::nullopt;
  }
  auto const stopped = trap_result::fault(
      {.kind          = deferred->kind,
       .fault_address = deferred->reported,
       .status        = state_.scs.cfsr,
       .detail        = state_.scs.hfsr});
  if(taken.state == security::nonsecure and banked(taken.number))
  {
    set_nonsecure_pending(taken.number, false);
  }
  else
  {
    set_exception_pending(taken.number, false);
  }
  return stopped;
}

void cortex_m::drop_deferred(std::uint32_t number, security state) noexcept
{
  if(state_.deferred and names(*state_.deferred, number, state))
  {
    state_.deferred.reset();
  }
}

auto cortex_m::requested_priority_negative() const -> bool
{
  auto const& exceptions = state_.exceptions;
  if(not trustzone())
  {
    return test_bit(exceptions.active, exc_nmi)
        or test_bit(exceptions.active, exc_hardfault)
        or (read(UC_ARM_REG_FAULTMASK) & 1u) != 0;
  }
  auto const state = current_security();
  auto const nmi   = test_bit(exceptions.active, exc_nmi)
               and exception_target(exceptions, priority_config(), exc_nmi, state) == state;
  auto const hard = state == security::nonsecure
                      ? ((exceptions.nonsecure_active >> exc_hardfault) & 1u) != 0
                      : test_bit(exceptions.active, exc_hardfault);
  auto const faultmask =
      read(state == security::secure ? UC_ARM_REG_FAULTMASK_S : UC_ARM_REG_FAULTMASK_NS);
  return nmi or hard or (faultmask & 1u) != 0;
}

auto cortex_m::ignored_bus_error(address accessed) -> bool
{
  if((state_.scs.ccr & ccr_bfhfnmign) == 0 or not requested_priority_negative())
  {
    return false;
  }
  state_.scs.bfar = static_cast<std::uint32_t>(accessed);
  state_.scs.cfsr |= cfsr::preciserr | cfsr::bfarvalid;
  skip_instruction();
  return true;
}

void cortex_m::skip_instruction()
{
  auto const at    = pc();
  auto const first = halfword(at);
  auto const wide  = thumb::size(static_cast<std::uint16_t>(first)) == 4;
  auto const size  = wide ? 4u : 2u;
  if(auto const update = writeback_of(first, wide ? halfword(at + 2) : 0u, wide))
  {
    auto const id = core_register_ids.at(update->reg);
    write(id, read(id) + static_cast<std::uint32_t>(update->offset));
  }
  auto const epsr = read(UC_ARM_REG_EPSR);
  auto       it   = ((epsr >> 25) & 0x3u) | ((epsr >> 8) & 0xfcu);
  it              = (it & 0x7u) == 0 ? 0u : (it & 0xe0u) | ((it << 1) & 0x1fu);
  write(UC_ARM_REG_EPSR, (epsr & ~epsr_it) | ((it & 0x3u) << 25) | ((it & 0xfcu) << 8));
  write(UC_ARM_REG_PC, static_cast<std::uint32_t>(at + size) | 1u);
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
  if(ignored_bus_error(addr))
  {
    return trap_result::resume();
  }
  return raise_fault(
      {.kind         = fault_kind::bus,
       .target       = exc_busfault,
       .cfsr_bits    = cfsr::preciserr,
       .reported     = addr,
       .data_address = addr});
}

auto cortex_m::nonsecure_fp_denied() const -> bool
{
  return trustzone()
     and current_security() == security::nonsecure
     and (state_.scs.nsacr & nsacr_cp10) == 0;
}

[[gnu::noinline]] auto cortex_m::engine_fault(std::uint32_t intno) -> trap_result
{
  auto const at = pc();
  switch(intno)
  {
    case excp_prefetch_abort:
    {
      auto const status = read(UC_ARM_REG_FAULT_STATUS) & fsr_status_mask;
      if(status == fsr_nsc_exec)
      {
        return secure_entry(at);
      }
      if(status == fsr_secure)
      {
        return raise_fault(
            {.kind      = fault_kind::secure,
             .target    = exc_securefault,
             .cfsr_bits = current_security() == security::secure ? sfsr::invtran : sfsr::invep,
             .reported  = at});
      }
      if(status == fsr_external)
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
    }
    case excp_data_abort:
    {
      auto const status = read(UC_ARM_REG_FAULT_STATUS) & fsr_status_mask;
      if(status == fsr_alignment)
      {
        return raise_fault(
            {.kind      = fault_kind::usage,
             .target    = exc_usagefault,
             .cfsr_bits = cfsr::unaligned,
             .reported  = at});
      }
      auto const accessed = read(UC_ARM_REG_FAULT_ADDRESS);
      if(status == fsr_secure)
      {
        return raise_fault(
            {.kind         = fault_kind::secure,
             .target       = exc_securefault,
             .cfsr_bits    = sfsr::auviol,
             .reported     = accessed,
             .data_address = accessed});
      }
      if(ignored_bus_error(accessed))
      {
        return trap_result::resume();
      }
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
           .reported  = at,
           .origin    = nonsecure_fp_denied() ? std::optional{security::secure} : std::nullopt});
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
    case excp_lserr:
      return raise_fault(
          {.kind      = fault_kind::secure,
           .target    = exc_securefault,
           .cfsr_bits = sfsr::lserr,
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
