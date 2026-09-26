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

auto best_pending(exception_state const& exceptions, bool synchronous_only) noexcept
    -> std::optional<std::uint32_t>
{
  std::optional<std::uint32_t> best;
  for(std::uint32_t number = exc_nmi; number < exceptions.priority.size(); ++number)
  {
    auto const enabled = number < exc_irq0 or test_bit(exceptions.enabled, number);
    if(not enabled
       or not test_bit(exceptions.pending, number)
       or (synchronous_only and not synchronous(number)))
    {
      continue;
    }
    if(not best or priority_of(exceptions, number) < priority_of(exceptions, *best))
    {
      best = number;
    }
  }
  return best;
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
    return stop_engine_on_pend(raise_fault(
        {.kind      = fault_kind::mem_manage,
         .target    = exc_memmanage,
         .cfsr_bits = cfsr::iaccviol,
         .reported  = target}));
  }
  if(not(v7() ? valid_v7_exc_return(value) : valid_exc_return(value)))
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
  if(number >= exc_irq0 and test_bit(state_.exceptions.level, number))
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
