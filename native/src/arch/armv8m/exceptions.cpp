#include "exceptions.hpp"

#include "cortex_m.hpp"
#include "frame.hpp"
#include "registers.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/trace.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t control_npriv = 1u << 0;
constexpr std::uint32_t control_spsel = 1u << 1;
constexpr std::uint32_t control_fpca  = 1u << 2;
constexpr std::uint32_t control_sfpa  = 1u << 3;
constexpr std::uint32_t ipsr_mask     = 0x1ffu;

constexpr std::array<int, 8> frame_ids{UC_ARM_REG_R0, UC_ARM_REG_R1,  UC_ARM_REG_R2,
                                       UC_ARM_REG_R3, UC_ARM_REG_R12, UC_ARM_REG_LR,
                                       UC_ARM_REG_PC, UC_ARM_REG_XPSR};

void read_batch(uc_struct* engine, std::span<int const> ids, std::span<std::uint32_t> out)
{
  std::vector<void*> pointers;
  for(auto& value : out)
  {
    pointers.push_back(&value);
  }
  if(auto const err =
         uc_reg_read_batch(engine, ids.data(), pointers.data(), static_cast<int>(ids.size()));
     err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m reading a frame: {}", uc_strerror(err)));
  }
}

void write_batch(uc_struct* engine, std::span<int const> ids, std::span<std::uint32_t> values)
{
  std::vector<void*> pointers;
  for(auto& value : values)
  {
    pointers.push_back(&value);
  }
  if(auto const err =
         uc_reg_write_batch(engine, ids.data(), pointers.data(), static_cast<int>(ids.size()));
     err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m writing a frame: {}", uc_strerror(err)));
  }
}

constexpr auto fp_frame_ids = [] consteval {
  std::array<int, 17> out{};
  for(std::size_t index = 0; index < 17; ++index)
  {
    out[index] = engine_register(fp_register_names[index == 16 ? 32 : index]);
  }
  return out;
}();
constexpr std::uint32_t xpsr_ge = 0x000f'0000;

void write(uc_struct* engine, uc_arm_reg id, std::uint32_t value, std::string_view what)
{
  if(auto const err = uc_reg_write(engine, id, &value); err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m writing {}: {}", what, uc_strerror(err)));
  }
}

auto read(uc_struct* engine, uc_arm_reg id, std::string_view what) -> std::uint32_t
{
  std::uint32_t value = 0;
  if(auto const err = uc_reg_read(engine, id, &value); err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m reading {}: {}", what, uc_strerror(err)));
  }
  return value;
}
}

auto test_bit(std::vector<std::uint32_t> const& bits, std::uint32_t number) noexcept -> bool
{
  return number / 32 < bits.size() and ((bits[number / 32] >> (number % 32)) & 1u) != 0;
}

void put_bit(std::vector<std::uint32_t>& bits, std::uint32_t number, bool value) noexcept
{
  if(number / 32 >= bits.size())
  {
    return;
  }
  auto const mask   = 1u << (number % 32);
  bits[number / 32] = value ? bits[number / 32] | mask : bits[number / 32] & ~mask;
}

auto priority_of(exception_state const& exceptions, std::uint32_t number) noexcept -> int
{
  if(number == exc_nmi)
  {
    return -2;
  }
  if(number == exc_hardfault)
  {
    return -1;
  }
  return number < exceptions.priority.size() ? exceptions.priority[number] : 256;
}

auto execution_priority(
    exception_state const& exceptions,
    std::uint32_t          prigroup,
    masks                  held) noexcept -> int
{
  auto priority = 256;
  for(std::uint32_t number = 1; number < exceptions.priority.size(); ++number)
  {
    if(test_bit(exceptions.active, number))
    {
      priority = std::min(priority, group_priority(priority_of(exceptions, number), prigroup));
    }
  }
  if(held.basepri != 0)
  {
    priority = std::min(priority, group_priority(held.basepri, prigroup));
  }
  if(held.primask)
  {
    priority = std::min(priority, 0);
  }
  if(held.faultmask)
  {
    priority = std::min(priority, -1);
  }
  return priority;
}

namespace
{
constexpr std::uint32_t system_enabled = 0x0000'fffcu;

constexpr std::uint32_t synchronous_bits = [] consteval {
  std::uint32_t out = 0;
  for(std::uint32_t number = 0; number < 32; ++number)
  {
    out |= synchronous(number) ? 1u << number : 0u;
  }
  return out;
}();

auto candidates(
    exception_state const& exceptions,
    std::size_t            word,
    std::uint32_t          count,
    bool                   synchronous_only) noexcept -> std::uint32_t
{
  auto const enabled = word < exceptions.enabled.size() ? exceptions.enabled[word] : 0u;
  auto       bits =
      exceptions.pending[word] & (word == 0 ? (enabled & ~0xffffu) | system_enabled : enabled);
  if(synchronous_only)
  {
    bits &= word == 0 ? synchronous_bits : 0u;
  }
  if(auto const first = static_cast<std::uint64_t>(word) * 32; count - first < 32)
  {
    bits &= (1u << (count - first)) - 1u;
  }
  return bits;
}

auto scanned_words(exception_state const& exceptions, std::uint32_t count) noexcept -> std::size_t
{
  return std::min<std::size_t>(exceptions.pending.size(), (std::size_t{count} + 31) / 32);
}
}

auto best_pending_number(exception_state const& exceptions, bool synchronous_only) noexcept
    -> std::uint32_t
{
  auto const    count         = static_cast<std::uint32_t>(exceptions.priority.size());
  std::uint32_t best          = 0;
  auto          best_priority = 0;
  for(std::size_t word = 0; word < scanned_words(exceptions, count); ++word)
  {
    for(auto bits = candidates(exceptions, word, count, synchronous_only); bits != 0;
        bits &= bits - 1)
    {
      auto const number   = static_cast<std::uint32_t>(word * 32)
                          + static_cast<std::uint32_t>(std::countr_zero(bits));
      auto const priority = priority_of(exceptions, number);
      if(best == 0 or priority < best_priority)
      {
        best          = number;
        best_priority = priority;
      }
    }
  }
  return best;
}

auto best_pending(exception_state const& exceptions, bool synchronous_only) noexcept
    -> std::optional<std::uint32_t>
{
  auto const best = best_pending_number(exceptions, synchronous_only);
  return best == 0 ? std::nullopt : std::optional{best};
}

auto any_pending(
    exception_state const& exceptions,
    std::uint32_t          count,
    bool                   synchronous_only) noexcept -> bool
{
  for(std::size_t word = 0; word < scanned_words(exceptions, count); ++word)
  {
    if(candidates(exceptions, word, count, synchronous_only) != 0)
    {
      return true;
    }
  }
  return false;
}

void set_context(uc_struct* engine, special_registers const& target, bool stack_limits)
{
  write(engine, UC_ARM_REG_IPSR, 1, "IPSR");
  write(
      engine, UC_ARM_REG_CONTROL, read(engine, UC_ARM_REG_CONTROL, "CONTROL") & ~control_npriv,
      "CONTROL");
  write(engine, UC_ARM_REG_IPSR, 0, "IPSR");
  write(engine, UC_ARM_REG_CONTROL, target.control & ~control_npriv, "CONTROL");
  write(engine, UC_ARM_REG_MSP, target.msp, "MSP");
  write(engine, UC_ARM_REG_PSP, target.psp, "PSP");
  write(engine, UC_ARM_REG_PRIMASK, target.primask, "PRIMASK");
  write(engine, UC_ARM_REG_BASEPRI, target.basepri, "BASEPRI");
  write(engine, UC_ARM_REG_FAULTMASK, target.faultmask, "FAULTMASK");
  write(engine, UC_ARM_REG_IPSR, target.ipsr, "IPSR");
  write(engine, UC_ARM_REG_CONTROL, target.control, "CONTROL");
  write(engine, UC_ARM_REG_APSR_G, target.ge, "APSR.GE");
  if(stack_limits)
  {
    write(engine, UC_ARM_REG_MSPLIM, target.msplim, "MSPLIM");
    write(engine, UC_ARM_REG_PSPLIM, target.psplim, "PSPLIM");
  }
}

auto read_special(uc_struct* engine, bool stack_limits) -> special_registers
{
  auto const control      = read(engine, UC_ARM_REG_CONTROL, "CONTROL");
  auto const ipsr         = read(engine, UC_ARM_REG_IPSR, "IPSR");
  auto const unprivileged = ipsr == 0 and (control & control_npriv) != 0;
  if(unprivileged)
  {
    write(engine, UC_ARM_REG_IPSR, 1, "IPSR");
  }
  special_registers const out{
      .control   = control,
      .primask   = read(engine, UC_ARM_REG_PRIMASK, "PRIMASK"),
      .basepri   = read(engine, UC_ARM_REG_BASEPRI, "BASEPRI"),
      .faultmask = read(engine, UC_ARM_REG_FAULTMASK, "FAULTMASK"),
      .msp       = read(engine, UC_ARM_REG_MSP, "MSP"),
      .psp       = read(engine, UC_ARM_REG_PSP, "PSP"),
      .ipsr      = ipsr,
      .ge        = read(engine, UC_ARM_REG_XPSR, "xPSR") & xpsr_ge,
      .msplim    = stack_limits ? read(engine, UC_ARM_REG_MSPLIM, "MSPLIM") : 0u,
      .psplim    = stack_limits ? read(engine, UC_ARM_REG_PSPLIM, "PSPLIM") : 0u,
  };
  if(unprivileged)
  {
    write(engine, UC_ARM_REG_IPSR, 0, "IPSR");
  }
  return out;
}

namespace
{
struct bank_ids
{
  uc_arm_reg control, primask, basepri, faultmask, msp, psp, msplim, psplim;
};

constexpr std::array<bank_ids, 2> banked_ids{
    bank_ids{
             UC_ARM_REG_CONTROL_NS, UC_ARM_REG_PRIMASK_NS, UC_ARM_REG_BASEPRI_NS,
             UC_ARM_REG_FAULTMASK_NS,                                                                     UC_ARM_REG_MSP_NS, UC_ARM_REG_PSP_NS, UC_ARM_REG_MSPLIM_NS,
             UC_ARM_REG_PSPLIM_NS                                                                                                                                                        },
    bank_ids{
             UC_ARM_REG_CONTROL_S,  UC_ARM_REG_PRIMASK_S,  UC_ARM_REG_BASEPRI_S,  UC_ARM_REG_FAULTMASK_S,
             UC_ARM_REG_MSP_S,                                                                                               UC_ARM_REG_PSP_S,  UC_ARM_REG_MSPLIM_S,  UC_ARM_REG_PSPLIM_S},
};

auto ids_of(security bank) noexcept -> bank_ids const&
{
  return banked_ids[static_cast<std::size_t>(bank)];
}
}

auto read_security(uc_struct* engine) -> security
{
  return read(engine, UC_ARM_REG_SECURE, "SECURE") != 0 ? security::secure : security::nonsecure;
}

void write_security(uc_struct* engine, security state)
{
  write(engine, UC_ARM_REG_SECURE, state == security::secure ? 1u : 0u, "SECURE");
}

auto read_bank(uc_struct* engine, security bank) -> special_bank
{
  auto const& ids = ids_of(bank);
  return {
      .control   = read(engine, ids.control, "CONTROL of a bank"),
      .primask   = read(engine, ids.primask, "PRIMASK of a bank"),
      .basepri   = read(engine, ids.basepri, "BASEPRI of a bank"),
      .faultmask = read(engine, ids.faultmask, "FAULTMASK of a bank"),
      .msp       = read(engine, ids.msp, "MSP of a bank"),
      .psp       = read(engine, ids.psp, "PSP of a bank"),
      .msplim    = read(engine, ids.msplim, "MSPLIM of a bank"),
      .psplim    = read(engine, ids.psplim, "PSPLIM of a bank"),
  };
}

void write_bank(uc_struct* engine, security bank, special_bank const& values)
{
  auto const& ids = ids_of(bank);
  write(engine, ids.control, values.control, "CONTROL of a bank");
  write(engine, ids.msp, values.msp, "MSP of a bank");
  write(engine, ids.psp, values.psp, "PSP of a bank");
  write(engine, ids.primask, values.primask, "PRIMASK of a bank");
  write(engine, ids.basepri, values.basepri, "BASEPRI of a bank");
  write(engine, ids.faultmask, values.faultmask, "FAULTMASK of a bank");
  write(engine, ids.msplim, values.msplim, "MSPLIM of a bank");
  write(engine, ids.psplim, values.psplim, "PSPLIM of a bank");
}

auto cortex_m::enter(std::uint32_t number) -> trap_result
{
  auto* const engine   = engine_handle();
  auto&       services = context_->services;
  auto const  special  = read_special(engine, stack_limits());
  auto const  thread   = special.ipsr == 0;
  auto const  use_psp  = thread and (special.control & control_spsel) != 0;
  auto const  fp       = core_.fpu and (special.control & control_fpca) != 0;
  auto const  size     = fp ? extended_frame_size : basic_frame_size;
  auto const  sp       = use_psp ? special.psp : special.msp;
  auto const  align    = not v7() or (state_.scs.ccr & ccr_stkalign) != 0;
  auto const  frame_at = align ? (sp - size) & ~4u : sp - size;
  if(frame_at < (use_psp ? special.psplim : special.msplim))
  {
    state_.scs.cfsr |= cfsr::stkof;
    return trap_result::fault(
        {.kind          = fault_kind::usage,
         .fault_address = frame_at,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  std::vector<std::uint32_t> words(size / 4, 0);
  read_batch(engine, frame_ids, std::span{words}.first(frame_ids.size()));
  words[6] = pc() & ~1u;
  words[7] = (words[7] & ~(stacked_align_bit | stacked_sfpa_bit))
           | (align and frame_at != sp - size ? stacked_align_bit : 0u);
  if(profile_.secure_engine and (special.control & control_sfpa) != 0)
  {
    words[7] |= stacked_sfpa_bit;
  }
  if(fp)
  {
    read_batch(engine, fp_frame_ids, std::span{words}.subspan(8, fp_frame_ids.size()));
  }
  if(not services.write_block(frame_at, std::as_bytes(std::span{words}), initiator::cpu))
  {
    state_.scs.cfsr |= cfsr::stkerr;
    return trap_result::fault(
        {.kind          = fault_kind::bus,
         .fault_address = frame_at,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  auto const vector = services.read(state_.scs.vtor + 4 * number, 4, initiator::cpu);
  if(not vector)
  {
    state_.scs.hfsr |= hfsr_vecttbl;
    return trap_result::fault(
        {.kind          = fault_kind::hard,
         .fault_address = state_.scs.vtor + 4 * number,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  auto target    = special;
  target.ipsr    = number;
  target.control = special.control & ~(control_spsel | control_fpca | control_sfpa);
  (use_psp ? target.psp : target.msp) = frame_at;
  set_context(engine, target, stack_limits());
  write(UC_ARM_REG_EPSR, (*vector & 1u) << 24);
  write(
      UC_ARM_REG_LR,
      encode_exc_return({.thread = thread, .process_stack = use_psp, .basic_frame = not fp}));
  write(UC_ARM_REG_PC, *vector);
  put_bit(state_.exceptions.pending, number, false);
  put_bit(state_.exceptions.active, number, true);
  refresh_pending();
  state_.event_register = true;
  services.trace(
      {.kind = core::trace_kind::irq, .flags = 1, .aux = static_cast<std::uint16_t>(number)});
  return trap_result::resume();
}

auto cortex_m::valid_return(std::uint32_t exc_return) const noexcept -> bool
{
  return v7() ? valid_v7_exc_return(exc_return) : valid_exc_return(exc_return);
}

auto cortex_m::exception_return() -> trap_result
{
  auto* const engine   = engine_handle();
  auto&       services = context_->services;
  auto const  value    = read(UC_ARM_REG_PC) | 1u;
  auto        special  = read_special(engine, stack_limits());
  auto const  number   = special.ipsr;
  auto const  invalid  = [&] {
    state_.scs.cfsr |= cfsr::invpc;
    return trap_result::fault(
        {.kind          = fault_kind::usage,
         .fault_address = value,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  };
  if(number == 0)
  {
    auto const target = value & ~1u;
    write(UC_ARM_REG_PC, value);
    return raise_fault(
        {.kind      = fault_kind::mem_manage,
         .target    = exc_memmanage,
         .cfsr_bits = cfsr::iaccviol,
         .reported  = target});
  }
  if(not valid_return(value))
  {
    return invalid();
  }
  auto const                 returning = decode_exc_return(value);
  auto const                 size = returning.basic_frame ? basic_frame_size : extended_frame_size;
  auto const                 frame_at = returning.process_stack ? special.psp : special.msp;
  std::vector<std::uint32_t> words(size / 4, 0);
  if(not services.read_block(frame_at, std::as_writable_bytes(std::span{words}), initiator::cpu))
  {
    state_.scs.cfsr |= cfsr::unstkerr;
    return trap_result::fault(
        {.kind          = fault_kind::bus,
         .fault_address = frame_at,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  auto const stacked_ipsr = words[7] & ipsr_mask;
  if(returning.thread != (stacked_ipsr == 0))
  {
    return invalid();
  }
  put_bit(state_.exceptions.active, number, false);
  if(number != exc_nmi)
  {
    special.faultmask = 0;
  }
  write_batch(engine, std::span{frame_ids}.first(6), std::span{words}.first(6));
  if(not returning.basic_frame)
  {
    write_batch(engine, fp_frame_ids, std::span{words}.subspan(8, fp_frame_ids.size()));
  }
  write(UC_ARM_REG_PC, words[6] | 1u);
  write(UC_ARM_REG_XPSR_NZCVQG, words[7] & ~(stacked_align_bit | stacked_sfpa_bit));
  auto const popped = frame_at + size + ((words[7] & stacked_align_bit) != 0 ? 4u : 0u);
  auto       target = special;
  target.ipsr       = stacked_ipsr;
  auto const sfpa =
      profile_.secure_engine and (words[7] & stacked_sfpa_bit) != 0 ? control_sfpa : 0u;
  target.control = (special.control & ~(control_spsel | control_fpca | control_sfpa))
                 | (returning.process_stack ? control_spsel : 0u)
                 | (returning.basic_frame ? 0u : control_fpca)
                 | sfpa;
  (returning.process_stack ? target.psp : target.msp) = popped;
  target.ge                                           = words[7] & xpsr_ge;
  set_context(engine, target, stack_limits());
  if(level_sensitive(number) and test_bit(state_.exceptions.level, number))
  {
    note_pending(number);
    put_bit(state_.exceptions.pending, number, true);
  }
  state_.event_register = true;
  auto const idle_thread =
      stacked_ipsr == 0 and std::ranges::all_of(state_.exceptions.active, [](std::uint32_t word) {
        return word == 0;
      });
  if(idle_thread and (state_.scs.scr & scr_sleeponexit) != 0)
  {
    state_.sleep = sleep_state{.at = words[6] & ~1u, .size = 0, .kind = sleep_kind::sleep_on_exit};
    services.end_slice();
  }
  refresh_pending();
  services.trace(
      {.kind = core::trace_kind::irq, .flags = 0, .aux = static_cast<std::uint16_t>(number)});
  return trap_result::resume();
}
}
