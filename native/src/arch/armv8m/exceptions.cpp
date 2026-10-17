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
constexpr std::uint32_t control_npriv        = 1u << 0;
constexpr std::uint32_t control_spsel        = 1u << 1;
constexpr std::uint32_t control_fpca         = 1u << 2;
constexpr std::uint32_t control_sfpa         = 1u << 3;
constexpr std::uint32_t ipsr_mask            = 0x1ffu;
constexpr int           restricted_nonsecure = 0x80;
constexpr std::uint32_t xpsr_thumb           = 1u << 24;
constexpr std::uint32_t fnc_return           = 0xFEFF'FFFFu;

constexpr std::array<int, 8> frame_ids{UC_ARM_REG_R0, UC_ARM_REG_R1,  UC_ARM_REG_R2,
                                       UC_ARM_REG_R3, UC_ARM_REG_R12, UC_ARM_REG_LR,
                                       UC_ARM_REG_PC, UC_ARM_REG_XPSR};
constexpr std::array<int, 8> callee_ids{UC_ARM_REG_R4,  UC_ARM_REG_R5, UC_ARM_REG_R6,
                                        UC_ARM_REG_R7,  UC_ARM_REG_R8, UC_ARM_REG_R9,
                                        UC_ARM_REG_R10, UC_ARM_REG_R11};

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
constexpr auto fp_callee_ids = [] consteval {
  std::array<int, 16> out{};
  for(std::size_t index = 0; index < 16; ++index)
  {
    out[index] = engine_register(fp_register_names[16 + index]);
  }
  return out;
}();
constexpr std::uint32_t xpsr_ge    = 0x000f'0000;
constexpr std::uint32_t fpccr_ts   = 1u << 26;
constexpr std::uint32_t nsacr_cp10 = 1u << 10;

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

auto exception_target(
    exception_state const& exceptions,
    security_config const& config,
    std::uint32_t          number,
    security               origin) noexcept -> security
{
  if(not config.trustzone)
  {
    return security::secure;
  }
  auto const secure_if = [](bool secure) {
    return secure ? security::secure : security::nonsecure;
  };
  switch(number)
  {
    case exc_nmi:
    case exc_busfault:
      return secure_if(not config.bfhfnmins);
    case exc_hardfault:
      return secure_if(not config.bfhfnmins or origin == security::secure);
    case exc_securefault:
      return security::secure;
    case exc_debugmon:
      return secure_if(config.sdme);
    default:
      break;
  }
  if(number >= exc_irq0)
  {
    return secure_if(not test_bit(exceptions.targets_nonsecure, number));
  }
  return origin;
}

auto exception_priority(
    exception_state const& exceptions,
    security_config const& config,
    std::uint32_t          number,
    security               state,
    bool                   group) noexcept -> int
{
  auto const nonsecure = config.trustzone and state == security::nonsecure;
  if(number == exc_hardfault)
  {
    return config.trustzone and config.bfhfnmins and not nonsecure ? -3 : -1;
  }
  auto result = nonsecure and banked(number) ? int{exceptions.nonsecure_priority[number]}
                                             : priority_of(exceptions, number);
  if(result < 0)
  {
    return result;
  }
  if(group)
  {
    result = group_priority(result, nonsecure ? config.prigroup_nonsecure : config.prigroup);
  }
  return nonsecure and config.pris ? (result >> 1) + restricted_nonsecure : result;
}

auto execution_priority(
    exception_state const& exceptions,
    security_config const& config,
    masks                  secure,
    masks                  nonsecure) noexcept -> int
{
  auto raw = 256;
  for(std::uint32_t number = 1; number < exceptions.priority.size(); ++number)
  {
    if(test_bit(exceptions.active, number))
    {
      auto const state = banked(number)
                           ? security::secure
                           : exception_target(exceptions, config, number, security::secure);
      raw              = std::min(raw, exception_priority(exceptions, config, number, state, true));
    }
  }
  auto boosted = 256;
  if(config.trustzone)
  {
    for(auto bits = exceptions.nonsecure_active & banked_exceptions; bits != 0; bits &= bits - 1)
    {
      auto const number = static_cast<std::uint32_t>(std::countr_zero(bits));
      raw =
          std::min(raw, exception_priority(exceptions, config, number, security::nonsecure, true));
    }
    if(nonsecure.basepri != 0)
    {
      auto const group = group_priority(nonsecure.basepri, config.prigroup_nonsecure);
      boosted          = config.pris ? (group >> 1) + restricted_nonsecure : group;
    }
  }
  if(secure.basepri != 0)
  {
    boosted = std::min(boosted, group_priority(secure.basepri, config.prigroup));
  }
  if(config.trustzone and nonsecure.primask)
  {
    boosted = std::min(boosted, config.pris ? restricted_nonsecure : 0);
  }
  if(secure.primask)
  {
    boosted = std::min(boosted, 0);
  }
  if(config.trustzone and nonsecure.faultmask)
  {
    boosted = config.bfhfnmins ? -1 : std::min(boosted, config.pris ? restricted_nonsecure : 0);
  }
  if(secure.faultmask)
  {
    boosted = config.trustzone and config.bfhfnmins ? -3 : -1;
  }
  return std::min(boosted, raw);
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

[[gnu::always_inline]] inline auto candidates(
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

auto nonsecure_candidates(exception_state const& exceptions, bool synchronous_only) noexcept
    -> std::uint32_t
{
  return exceptions.nonsecure_pending
       & banked_exceptions
       & (synchronous_only ? synchronous_bits : ~0u);
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

auto best_pending(
    exception_state const& exceptions,
    security_config const& config,
    bool                   synchronous_only) noexcept -> pending_exception
{
  auto const        count = static_cast<std::uint32_t>(exceptions.priority.size());
  pending_exception best{};
  auto              best_priority = 0;
  auto const        consider      = [&](std::uint32_t number, security state) {
    auto const priority = exception_priority(exceptions, config, number, state, false);
    if(best.number == 0
       or priority < best_priority
       or (priority == best_priority and number < best.number))
    {
      best          = {.number = number, .state = state};
      best_priority = priority;
    }
  };
  for(std::size_t word = 0; word < scanned_words(exceptions, count); ++word)
  {
    for(auto bits = candidates(exceptions, word, count, synchronous_only); bits != 0;
        bits &= bits - 1)
    {
      auto const number = static_cast<std::uint32_t>(word * 32)
                        + static_cast<std::uint32_t>(std::countr_zero(bits));
      consider(
          number, banked(number) ? security::secure
                                 : exception_target(exceptions, config, number, security::secure));
    }
  }
  if(config.trustzone)
  {
    for(auto bits = nonsecure_candidates(exceptions, synchronous_only); bits != 0; bits &= bits - 1)
    {
      consider(static_cast<std::uint32_t>(std::countr_zero(bits)), security::nonsecure);
    }
  }
  return best;
}

auto any_nonsecure_pending(exception_state const& exceptions, bool synchronous_only) noexcept
    -> bool
{
  return nonsecure_candidates(exceptions, synchronous_only) != 0;
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

namespace
{
auto outranks(
    exception_state const& exceptions,
    security_config const& config,
    pending_exception      first,
    pending_exception      second) noexcept -> bool
{
  auto const lhs = exception_priority(exceptions, config, first.number, first.state, false);
  auto const rhs = exception_priority(exceptions, config, second.number, second.state, false);
  if(lhs != rhs)
  {
    return lhs < rhs;
  }
  if(first.number != second.number)
  {
    return first.number < second.number;
  }
  return first.state == security::secure and second.state == security::nonsecure;
}
}

template <bool extension>
auto cortex_m::enter_as(std::uint32_t number, security state) -> trap_result
{
  auto* const engine     = engine_handle();
  auto&       services   = context_->services;
  auto const  background = extension ? read_security(engine) : security::secure;
  auto const  special    = read_special(engine, stack_limits());
  auto const  thread     = special.ipsr == 0;
  auto const  use_psp    = thread and (special.control & control_spsel) != 0;
  auto const  fp         = core_.fpu and (special.control & control_fpca) != 0;
  auto const  fp_callee =
      extension and fp and background == security::secure and secure_fp_context();
  auto const nocp = extension
                and fp
                and background == security::nonsecure
                and (state_.scs.nsacr & nsacr_cp10) == 0;
  auto const size  = fp and not nocp ? extended_frame_size + (fp_callee ? fp_callee_frame_size : 0u)
                                     : basic_frame_size;
  auto const sp    = use_psp ? special.psp : special.msp;
  auto const align = not v7() or (state_.scs.ccr & ccr_stkalign) != 0;
  auto const frame_at = align ? (sp - size) & ~4u : sp - size;
  auto const callee = extension and background == security::secure and state == security::nonsecure
                        ? callee_frame_size
                        : 0u;
  auto const lowest = frame_at - callee;
  auto&      cfsr =
      extension and background == security::nonsecure ? state_.scs.nonsecure.cfsr : state_.scs.cfsr;
  auto const original = [&] { return pending_exception{.number = number, .state = state}; };
  auto const partial  = [&] {
    return encode_exc_return(
        {.thread = thread, .process_stack = use_psp, .basic_frame = not fp},
        {.secure_background = background == security::secure});
  };
  auto const sp_id = [&] {
    return ids_of(background).*(use_psp ? &bank_ids::psp : &bank_ids::msp);
  };
  auto const note_nocp = [&] {
    if(nocp)
    {
      state_.scs.cfsr |= cfsr::nocp;
    }
  };
  auto const invalidate_fp = [&] {
    std::array<std::uint32_t, 17> zeros{};
    write_batch(engine, fp_frame_ids, std::span{zeros});
    write_batch(engine, fp_callee_ids, std::span{zeros}.first(fp_callee_ids.size()));
  };
  if(lowest < (use_psp ? special.psplim : special.msplim))
  {
    if constexpr(extension)
    {
      if(auto const limit = use_psp ? special.psplim : special.msplim; frame_at < limit)
      {
        note_nocp();
        return derived_entry(
            original(),
            {.kind      = fault_kind::usage,
             .target    = exc_usagefault,
             .cfsr_bits = cfsr::stkof,
             .reported  = lowest,
             .origin    = background},
            partial(), sp_id(), limit);
      }
    }
    cfsr |= cfsr::stkof;
    return trap_result::fault(
        {.kind          = fault_kind::usage,
         .fault_address = lowest,
         .status        = cfsr,
         .detail        = state_.scs.hfsr});
  }
  if constexpr(extension)
  {
    if(background == security::nonsecure)
    {
      if(auto const denied = first_secure_word(lowest, frame_at + size))
      {
        note_nocp();
        return derived_entry(
            original(),
            {.kind         = fault_kind::secure,
             .target       = exc_securefault,
             .cfsr_bits    = sfsr::auviol,
             .reported     = *denied,
             .data_address = *denied},
            partial(), sp_id(), lowest);
      }
      if(nocp and context_->faults == fault_policy::stop)
      {
        state_.scs.cfsr |= cfsr::nocp;
        return trap_result::fault(
            {.kind          = fault_kind::usage,
             .fault_address = pc(),
             .status        = state_.scs.cfsr,
             .detail        = state_.scs.hfsr});
      }
    }
  }
  std::vector<std::uint32_t> words((callee + size) / 4, 0);
  auto const                 frame = std::span{words}.subspan(callee / 4);
  read_batch(engine, frame_ids, frame.first(frame_ids.size()));
  frame[6] = pc() & ~1u;
  frame[7] = (frame[7] & ~(stacked_align_bit | stacked_sfpa_bit))
           | (align and frame_at != sp - size ? stacked_align_bit : 0u);
  if(profile_.secure_engine and (special.control & control_sfpa) != 0)
  {
    frame[7] |= stacked_sfpa_bit;
  }
  if(fp and not nocp)
  {
    read_batch(engine, fp_frame_ids, frame.subspan(8, fp_frame_ids.size()));
  }
  if constexpr(extension)
  {
    if(fp_callee)
    {
      read_batch(
          engine, fp_callee_ids, frame.subspan(extended_frame_size / 4, fp_callee_ids.size()));
    }
    if(callee != 0)
    {
      words[0] = integrity_signature(core_.fpu, not fp);
      read_batch(engine, callee_ids, std::span{words}.subspan(2, callee_ids.size()));
    }
  }
  auto const stacking = bus_attrs{
      .by         = initiator::cpu,
      .secure     = background == security::secure,
      .privileged = not thread or (special.control & control_npriv) == 0,
  };
  if(not services.write_block(lowest, std::as_bytes(std::span{words}), stacking))
  {
    if constexpr(extension)
    {
      if(fp_callee)
      {
        invalidate_fp();
      }
      note_nocp();
      return derived_entry(
          original(),
          {.kind      = fault_kind::bus,
           .target    = exc_busfault,
           .cfsr_bits = cfsr::stkerr,
           .reported  = lowest,
           .origin    = background},
          partial(), sp_id(), frame_at);
    }
    state_.scs.cfsr |= cfsr::stkerr;
    return trap_result::fault(
        {.kind          = fault_kind::bus,
         .fault_address = lowest,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  if constexpr(extension)
  {
    if(fp_callee)
    {
      invalidate_fp();
    }
    if(nocp)
    {
      return derived_entry(
          original(),
          {.kind      = fault_kind::usage,
           .target    = exc_usagefault,
           .cfsr_bits = cfsr::nocp,
           .reported  = pc(),
           .origin    = security::secure},
          partial(), sp_id(), lowest);
    }
  }
  auto const table =
      extension and state == security::nonsecure ? state_.scs.nonsecure.vtor : state_.scs.vtor;
  auto const slot = table + 4 * number;
  auto const link = [&] {
    return encode_exc_return(
        {.thread        = thread,
         .process_stack = state == background
                            ? use_psp
                            : (read_bank(engine, state).control & control_spsel) != 0,
         .basic_frame   = not fp},
        {.secure_background = background == security::secure,
         .secure_exception  = state == security::secure});
  };
  std::optional<std::uint32_t> vector;
  if(not extension or state == security::secure or not first_secure_word(slot, slot + 4))
  {
    if(auto const read_back = services.read(
           slot, 4, bus_attrs{.by = initiator::cpu, .secure = state == security::secure}))
    {
      vector = static_cast<std::uint32_t>(*read_back);
    }
  }
  if(not vector)
  {
    state_.scs.hfsr |= hfsr_vecttbl;
    if constexpr(extension)
    {
      auto const secure_slot =
          state == security::nonsecure and first_secure_word(slot, slot + 4).has_value();
      auto const hard = pending_exception{
          .number = exc_hardfault,
          .state  = secure_slot ? security::secure
                                : exception_target(
                                      state_.exceptions, priority_config(), exc_hardfault, state)};
      if(context_->faults != fault_policy::stop
         and outranks(state_.exceptions, priority_config(), hard, original()))
      {
        write(sp_id(), lowest);
        if(state == security::nonsecure)
        {
          clear_caller_registers(engine, callee != 0);
        }
        pend(hard.number, hard.state);
        return enter_with_frame(hard, link(), true);
      }
    }
    return trap_result::fault(
        {.kind          = fault_kind::hard,
         .fault_address = slot,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  auto target = special;
  if constexpr(extension)
  {
    auto const exc_return = link();
    if(state != background)
    {
      write(sp_id(), lowest);
      write_security(engine, state);
      target = read_special(engine, stack_limits());
    }
    else
    {
      (use_psp ? target.psp : target.msp) = lowest;
    }
    target.ipsr    = number;
    target.control = target.control & ~(control_spsel | control_fpca | control_sfpa);
    if(state == security::nonsecure)
    {
      target.ge = 0;
    }
    set_context(engine, target, stack_limits());
    if(state == security::nonsecure)
    {
      write(UC_ARM_REG_CONTROL_S, read(UC_ARM_REG_CONTROL_S) & ~(control_fpca | control_sfpa));
      clear_caller_registers(engine, callee != 0);
    }
    write(UC_ARM_REG_EPSR, (*vector & 1u) << 24);
    write(UC_ARM_REG_LR, exc_return);
  }
  else
  {
    target.ipsr    = number;
    target.control = special.control & ~(control_spsel | control_fpca | control_sfpa);
    (use_psp ? target.psp : target.msp) = frame_at;
    set_context(engine, target, stack_limits());
    write(UC_ARM_REG_EPSR, (*vector & 1u) << 24);
    write(
        UC_ARM_REG_LR,
        encode_exc_return({.thread = thread, .process_stack = use_psp, .basic_frame = not fp}));
  }
  write(UC_ARM_REG_PC, *vector);
  if(extension and state == security::nonsecure and banked(number))
  {
    state_.exceptions.nonsecure_pending &= ~(1u << number);
    state_.exceptions.nonsecure_active |= 1u << number;
  }
  else
  {
    put_bit(state_.exceptions.pending, number, false);
    put_bit(state_.exceptions.active, number, true);
  }
  refresh_pending();
  state_.event_register = true;
  services.trace(
      {.kind = core::trace_kind::irq, .flags = 1, .aux = static_cast<std::uint16_t>(number)});
  return trap_result::resume();
}

template auto cortex_m::enter_as<false>(std::uint32_t number, security state) -> trap_result;
template auto cortex_m::enter_as<true>(std::uint32_t number, security state) -> trap_result;

auto cortex_m::valid_return(std::uint32_t exc_return) const noexcept -> bool
{
  if(v7())
  {
    return valid_v7_exc_return(exc_return);
  }
  return trustzone() ? (exc_return & 0xff00'0000u) == 0xff00'0000u : valid_exc_return(exc_return);
}

[[gnu::always_inline]] inline void cortex_m::repend_level(std::uint32_t number) noexcept
{
  if(level_sensitive(number) and test_bit(state_.exceptions.level, number))
  {
    note_pending(number);
    put_bit(state_.exceptions.pending, number, true);
  }
}

[[gnu::always_inline]] inline void cortex_m::trace_exit(std::uint32_t number)
{
  context_->services.trace(
      {.kind = core::trace_kind::irq, .flags = 0, .aux = static_cast<std::uint16_t>(number)});
}

[[gnu::always_inline]] inline auto cortex_m::returned(
    std::uint32_t number,
    std::uint32_t stacked_ipsr,
    address       resume_at,
    std::uint32_t scr) -> trap_result
{
  auto& services = context_->services;
  repend_level(number);
  state_.event_register  = true;
  auto const idle_thread = stacked_ipsr == 0
                       and state_.exceptions.nonsecure_active == 0
                       and std::ranges::all_of(state_.exceptions.active, [](std::uint32_t word) {
                             return word == 0;
                           });
  if(idle_thread and (scr & scr_sleeponexit) != 0)
  {
    state_.sleep = sleep_state{.at = resume_at, .size = 0, .kind = sleep_kind::sleep_on_exit};
    services.end_slice();
  }
  refresh_pending();
  trace_exit(number);
  return trap_result::resume();
}

auto cortex_m::exception_return() -> trap_result
{
  if(trustzone())
  {
    return banked_return();
  }
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
  auto const                 unstacking = bus_attrs{
      .by         = initiator::cpu,
      .secure     = not trustzone() or read_security(engine) == security::secure,
      .privileged = not returning.thread or (special.control & control_npriv) == 0,
  };
  if(not services.read_block(frame_at, std::as_writable_bytes(std::span{words}), unstacking))
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
  if(trustzone() and not decode_exc_security(value).secure_exception and banked(number))
  {
    state_.exceptions.nonsecure_active &= ~(1u << number);
  }
  else
  {
    put_bit(state_.exceptions.active, number, false);
  }
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
  return returned(number, stacked_ipsr, words[6] & ~1u, state_.scs.scr);
}

[[gnu::noinline]] auto cortex_m::banked_return() -> trap_result
{
  auto* const engine   = engine_handle();
  auto&       services = context_->services;
  auto const  value    = read(UC_ARM_REG_PC) | (read(UC_ARM_REG_XPSR) & xpsr_thumb) >> 24;
  if((value | 1u) == fnc_return)
  {
    return function_return();
  }
  auto const number  = read(UC_ARM_REG_IPSR) & ipsr_mask;
  auto const current = read_security(engine);
  auto const invalid = [&] {
    auto& cfsr = current == security::nonsecure ? state_.scs.nonsecure.cfsr : state_.scs.cfsr;
    cfsr |= cfsr::invpc;
    return trap_result::fault(
        {.kind          = fault_kind::usage,
         .fault_address = value,
         .status        = cfsr,
         .detail        = state_.scs.hfsr});
  };
  if(number == 0)
  {
    write(UC_ARM_REG_PC, value);
    return raise_fault(
        {.kind      = fault_kind::mem_manage,
         .target    = exc_memmanage,
         .cfsr_bits = cfsr::iaccviol,
         .reported  = value & ~1u});
  }
  if(not valid_return(value))
  {
    return invalid();
  }
  auto const fields    = decode_exc_security(value);
  auto const returning = decode_exc_return(value);
  auto const exception = fields.secure_exception ? security::secure : security::nonsecure;
  if((current == security::nonsecure and fields.secure_exception)
     or (not fields.secure_exception and not fields.default_callee))
  {
    auto const forced = current == security::nonsecure ? value & ~1u : value;
    return return_fault(
        forced, number, current == security::nonsecure ? security::nonsecure : exception, current,
        {.kind      = fault_kind::secure,
         .target    = exc_securefault,
         .cfsr_bits = sfsr::inver,
         .reported  = value});
  }
  auto const back          = fields.secure_background ? security::secure : security::nonsecure;
  auto const banked_number = exception == security::nonsecure and banked(number);
  auto const active =
      banked_number
          ? (state_.exceptions.nonsecure_active >> number & 1u) != 0
          : test_bit(state_.exceptions.active, number)
                and (banked(number) or exception_target(state_.exceptions, priority_config(), number, exception) == exception);
  if(not active)
  {
    return return_fault(
        value, number, exception, current,
        {.kind      = fault_kind::usage,
         .target    = exc_usagefault,
         .cfsr_bits = cfsr::invpc,
         .reported  = value});
  }
  auto const spsel = (value & (1u << 2)) != 0;
  auto const bank  = read_bank(engine, back);
  auto const to_psp =
      returning.thread and (back == exception ? spsel : (bank.control & control_spsel) != 0);
  auto const frame_at = to_psp ? bank.psp : bank.msp;
  auto const callee =
      back == security::secure and (exception == security::nonsecure or not fields.default_callee)
          ? callee_frame_size
          : 0u;
  auto const fp_callee =
      not returning.basic_frame and back == security::secure and secure_fp_context();
  auto const size = returning.basic_frame
                      ? basic_frame_size
                      : extended_frame_size + (fp_callee ? fp_callee_frame_size : 0u);
  if(back == security::nonsecure)
  {
    if(auto const denied = first_secure_word(frame_at, frame_at + callee + size))
    {
      return return_fault(
          value, number, exception, back,
          {.kind         = fault_kind::secure,
           .target       = exc_securefault,
           .cfsr_bits    = sfsr::auviol,
           .reported     = *denied,
           .data_address = *denied});
    }
  }
  std::vector<std::uint32_t> words((callee + size) / 4, 0);
  auto const                 unstacking = bus_attrs{
      .by         = initiator::cpu,
      .secure     = back == security::secure,
      .privileged = not returning.thread or (bank.control & control_npriv) == 0,
  };
  if(not services.read_block(frame_at, std::as_writable_bytes(std::span{words}), unstacking))
  {
    return return_fault(
        value, number, exception, back,
        {.kind      = fault_kind::bus,
         .target    = exc_busfault,
         .cfsr_bits = cfsr::unstkerr,
         .reported  = frame_at});
  }
  if(callee != 0 and words[0] != integrity_signature(core_.fpu, returning.basic_frame))
  {
    return return_fault(
        value, number, exception, back,
        {.kind      = fault_kind::secure,
         .target    = exc_securefault,
         .cfsr_bits = sfsr::invis,
         .reported  = frame_at});
  }
  auto const frame        = std::span{words}.subspan(callee / 4);
  auto const stacked_ipsr = frame[7] & ipsr_mask;
  if(returning.thread != (stacked_ipsr == 0))
  {
    return return_fault(
        value, number, exception, back,
        {.kind      = fault_kind::usage,
         .target    = exc_usagefault,
         .cfsr_bits = cfsr::invpc,
         .reported  = value});
  }
  if(back == security::nonsecure
     and not returning.basic_frame
     and (state_.scs.nsacr & nsacr_cp10) == 0)
  {
    return return_fault(
        value, number, exception, back,
        {.kind      = fault_kind::usage,
         .target    = exc_usagefault,
         .cfsr_bits = cfsr::nocp,
         .reported  = value,
         .origin    = security::secure});
  }
  if(banked_number)
  {
    state_.exceptions.nonsecure_active &= ~(1u << number);
  }
  else
  {
    put_bit(state_.exceptions.active, number, false);
  }
  auto const& exception_ids = ids_of(exception);
  if(number != exc_nmi)
  {
    write(exception_ids.faultmask, 0);
  }
  write(
      exception_ids.control,
      (read(exception_ids.control) & ~control_spsel) | (spsel ? control_spsel : 0u));
  write_batch(engine, std::span{frame_ids}.first(6), frame.first(6));
  if(callee != 0)
  {
    write_batch(engine, callee_ids, std::span{words}.subspan(2, callee_ids.size()));
  }
  if(not returning.basic_frame)
  {
    write_batch(engine, fp_frame_ids, frame.subspan(8, fp_frame_ids.size()));
  }
  if(fp_callee)
  {
    write_batch(
        engine, fp_callee_ids, frame.subspan(extended_frame_size / 4, fp_callee_ids.size()));
  }
  write(UC_ARM_REG_PC, frame[6] | 1u);
  write(UC_ARM_REG_XPSR_NZCVQG, frame[7] & ~(stacked_align_bit | stacked_sfpa_bit));
  if(back != current)
  {
    write_security(engine, back);
  }
  auto       target = read_special(engine, stack_limits());
  auto const popped = frame_at + callee + size + ((frame[7] & stacked_align_bit) != 0 ? 4u : 0u);
  auto const fpca   = returning.basic_frame ? 0u : control_fpca;
  auto const sfpa =
      back == security::secure and (frame[7] & stacked_sfpa_bit) != 0 ? control_sfpa : 0u;
  target.ipsr    = stacked_ipsr;
  target.control = (target.control & ~(control_spsel | control_fpca | control_sfpa))
                 | (to_psp ? control_spsel : 0u)
                 | fpca
                 | sfpa;
  (to_psp ? target.psp : target.msp) = popped;
  target.ge                          = frame[7] & xpsr_ge;
  set_context(engine, target, stack_limits());
  if(back == security::nonsecure)
  {
    write(UC_ARM_REG_CONTROL_S, (read(UC_ARM_REG_CONTROL_S) & ~control_fpca) | fpca);
  }
  return returned(
      number, stacked_ipsr, frame[6] & ~1u,
      back == security::nonsecure ? state_.scs.nonsecure.scr : state_.scs.scr);
}

auto cortex_m::function_return() -> trap_result
{
  auto* const engine  = engine_handle();
  auto const  number  = read(UC_ARM_REG_IPSR) & ipsr_mask;
  auto const  bank    = read_bank(engine, security::secure);
  auto const  process = number == 0 and (bank.control & control_spsel) != 0;
  auto const  sp      = process ? bank.psp : bank.msp;
  auto const  invalid = [&] {
    return raise_fault(
        {.kind      = fault_kind::usage,
         .target    = exc_usagefault,
         .cfsr_bits = cfsr::invpc,
         .reported  = pc(),
         .origin    = security::secure});
  };
  if(sp % 8 != 0)
  {
    return invalid();
  }
  std::array<std::uint32_t, 2> words{};
  auto const                   unstacking = bus_attrs{
      .by         = initiator::cpu,
      .secure     = true,
      .privileged = number != 0 or (bank.control & control_npriv) == 0,
  };
  if(not context_->services.read_block(sp, std::as_writable_bytes(std::span{words}), unstacking))
  {
    return raise_fault(
        {.kind      = fault_kind::bus,
         .target    = exc_busfault,
         .cfsr_bits = cfsr::unstkerr,
         .reported  = sp});
  }
  auto const [target, retpsr] = words;
  auto const exception        = retpsr & ipsr_mask;
  if(not((number == 0 and exception == 0) or (number == 1 and exception != 0)))
  {
    return invalid();
  }
  if(read_security(engine) != security::secure)
  {
    write_security(engine, security::secure);
  }
  write(process ? UC_ARM_REG_PSP_S : UC_ARM_REG_MSP_S, sp + 8);
  write(UC_ARM_REG_IPSR, exception);
  write(
      UC_ARM_REG_CONTROL_S, (read(UC_ARM_REG_CONTROL_S) & ~control_sfpa)
                                | ((retpsr & stacked_sfpa_bit) != 0 ? control_sfpa : 0u));
  write(UC_ARM_REG_EPSR, (target & 1u) << 24);
  write(UC_ARM_REG_PC, target);
  return trap_result::resume();
}

auto cortex_m::secure_fp_context() const noexcept -> bool
{
  return trustzone() and (state_.scs.fpccr & fpccr_ts) != 0;
}

auto cortex_m::derived_entry(
    pending_exception  original,
    fault_cause const& cause,
    std::uint32_t      exc_return,
    int                sp_id,
    std::uint32_t      sp) -> trap_result
{
  auto const raised = create_exception(cause);
  if(not raised)
  {
    return raised.error();
  }
  write(sp_id, sp);
  auto const derived_first = outranks(state_.exceptions, priority_config(), *raised, original);
  pend(raised->number, raised->state);
  return enter_with_frame(derived_first ? *raised : original, exc_return, false);
}

auto cortex_m::return_fault(
    std::uint32_t      exc_return,
    std::uint32_t      number,
    security           exception,
    security           to,
    fault_cause const& cause) -> trap_result
{
  auto* const engine           = engine_handle();
  auto const& ids              = ids_of(exception);
  auto const  active           = state_.exceptions.active;
  auto const  nonsecure_active = state_.exceptions.nonsecure_active;
  auto const  control          = read(ids.control);
  auto const  faultmask        = read(ids.faultmask);
  auto const  current          = read_security(engine);
  if(exception == security::nonsecure and banked(number))
  {
    state_.exceptions.nonsecure_active &= ~(1u << number);
  }
  else
  {
    put_bit(state_.exceptions.active, number, false);
  }
  if(number != exc_nmi)
  {
    write(ids.faultmask, 0);
  }
  write(
      ids.control,
      (control & ~control_spsel) | ((exc_return & (1u << 2)) != 0 ? control_spsel : 0u));
  if(to != current)
  {
    write_security(engine, to);
  }
  auto from         = cause;
  from.origin       = cause.origin.value_or(to);
  auto const raised = create_exception(from);
  if(not raised)
  {
    if(to != current)
    {
      write_security(engine, current);
    }
    write(ids.control, control);
    write(ids.faultmask, faultmask);
    state_.exceptions.active           = active;
    state_.exceptions.nonsecure_active = nonsecure_active;
    return raised.error();
  }
  repend_level(number);
  trace_exit(number);
  pend(raised->number, raised->state);
  auto const best = best_pending(state_.exceptions, priority_config(), state_.call_masked);
  return enter_with_frame(
      best.number != 0 and preempts(best.number, best.state) ? best : *raised, exc_return, true);
}

auto cortex_m::enter_with_frame(pending_exception taken, std::uint32_t exc_return, bool tail_chain)
    -> trap_result
{
  auto* const engine         = engine_handle();
  auto&       services       = context_->services;
  auto const [number, state] = taken;
  auto const fields          = decode_exc_security(exc_return);
  auto const thread          = (exc_return & (1u << 3)) != 0;
  auto const basic           = (exc_return & (1u << 4)) != 0;
  auto const spsel_bit       = (exc_return & (1u << 2)) != 0;
  auto       default_callee  = fields.default_callee;
  auto       push            = false;
  if(fields.secure_background)
  {
    if(state == security::secure)
    {
      default_callee = default_callee and not(tail_chain and not fields.secure_exception);
    }
    else
    {
      push           = default_callee and not(tail_chain and not fields.secure_exception);
      default_callee = true;
    }
  }
  if(push)
  {
    auto const bank    = read_bank(engine, security::secure);
    auto const process = thread and (tail_chain ? spsel_bit : (bank.control & control_spsel) != 0);
    auto const sp      = process ? bank.psp : bank.msp;
    auto const at      = sp - callee_frame_size;
    auto const limit   = process ? bank.psplim : bank.msplim;
    if(at < limit)
    {
      state_.scs.cfsr |= cfsr::stkof;
      return trap_result::fault(
          {.kind          = fault_kind::usage,
           .fault_address = at,
           .status        = state_.scs.cfsr,
           .detail        = state_.scs.hfsr});
    }
    std::array<std::uint32_t, callee_frame_size / 4> words{};
    words[0] = integrity_signature(core_.fpu, basic);
    read_batch(engine, callee_ids, std::span{words}.subspan(2, callee_ids.size()));
    auto const stacking = bus_attrs{
        .by         = initiator::cpu,
        .secure     = true,
        .privileged = not thread or (bank.control & control_npriv) == 0,
    };
    if(not services.write_block(at, std::as_bytes(std::span{words}), stacking))
    {
      state_.scs.cfsr |= cfsr::stkerr;
      return trap_result::fault(
          {.kind          = fault_kind::bus,
           .fault_address = at,
           .status        = state_.scs.cfsr,
           .detail        = state_.scs.hfsr});
    }
    write(process ? UC_ARM_REG_PSP_S : UC_ARM_REG_MSP_S, at);
  }
  auto const table = state == security::nonsecure ? state_.scs.nonsecure.vtor : state_.scs.vtor;
  auto const slot  = table + 4 * number;
  std::optional<std::uint32_t> vector;
  if(state == security::secure or not first_secure_word(slot, slot + 4))
  {
    if(auto const read_back = services.read(
           slot, 4, bus_attrs{.by = initiator::cpu, .secure = state == security::secure}))
    {
      vector = static_cast<std::uint32_t>(*read_back);
    }
  }
  if(not vector)
  {
    state_.scs.hfsr |= hfsr_vecttbl;
    return trap_result::fault(
        {.kind          = fault_kind::hard,
         .fault_address = slot,
         .status        = state_.scs.cfsr,
         .detail        = state_.scs.hfsr});
  }
  auto const link = encode_exc_return(
      {.thread        = thread,
       .process_stack = (read_bank(engine, state).control & control_spsel) != 0,
       .basic_frame   = basic},
      {.secure_background = fields.secure_background,
       .default_callee    = default_callee,
       .secure_exception  = state == security::secure});
  if(read_security(engine) != state)
  {
    write_security(engine, state);
  }
  auto target    = read_special(engine, stack_limits());
  target.ipsr    = number;
  target.control = target.control & ~(control_spsel | control_fpca | control_sfpa);
  if(state == security::nonsecure)
  {
    target.ge = 0;
  }
  set_context(engine, target, stack_limits());
  if(state == security::nonsecure)
  {
    write(UC_ARM_REG_CONTROL_S, read(UC_ARM_REG_CONTROL_S) & ~(control_fpca | control_sfpa));
    clear_caller_registers(engine, fields.secure_background);
  }
  write(UC_ARM_REG_EPSR, (*vector & 1u) << 24);
  write(UC_ARM_REG_LR, link);
  write(UC_ARM_REG_PC, *vector);
  if(state == security::nonsecure and banked(number))
  {
    state_.exceptions.nonsecure_pending &= ~(1u << number);
    state_.exceptions.nonsecure_active |= 1u << number;
  }
  else
  {
    put_bit(state_.exceptions.pending, number, false);
    put_bit(state_.exceptions.active, number, true);
  }
  refresh_pending();
  state_.event_register = true;
  services.trace(
      {.kind = core::trace_kind::irq, .flags = 1, .aux = static_cast<std::uint16_t>(number)});
  return trap_result::resume();
}

void cortex_m::clear_caller_registers(uc_struct* engine, bool callee) const
{
  std::array<std::uint32_t, 8> zeros{};
  write_batch(engine, std::span{frame_ids}.first(5), std::span{zeros}.first(5));
  if(callee)
  {
    write_batch(engine, callee_ids, std::span{zeros});
  }
  write(UC_ARM_REG_APSR, 0);
}
}
