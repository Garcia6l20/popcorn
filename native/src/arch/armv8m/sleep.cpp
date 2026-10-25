#include "cortex_m.hpp"
#include "exceptions.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <string_view>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t wfi_narrow      = 0xbf30;
constexpr std::uint32_t wfi_wide_first  = 0xf3af;
constexpr std::uint32_t wfi_wide_second = 0x8003;
constexpr std::uint32_t hint_wide_first = 0xf3af;
constexpr std::uint32_t wfe_narrow      = 0xbf20;
constexpr std::uint32_t yield_narrow    = 0xbf10;
constexpr std::uint32_t sev_narrow      = 0xbf40;
constexpr std::uint32_t wfe_wide_second = 0x8002;
constexpr std::uint32_t yield_wide      = 0x8001;
constexpr std::uint32_t sev_wide_second = 0x8004;
constexpr std::uint32_t b_self_narrow   = 0xe7fe;
constexpr std::uint32_t b_self_first    = 0xf7ff;
constexpr std::uint32_t b_self_second   = 0xbffe;
constexpr std::uint32_t xpsr_it         = 0x0600'fc00;

void require(uc_err err, std::string_view what)
{
  if(err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m {}: {}", what, uc_strerror(err)));
  }
}
}

auto cortex_m::wfi_hook(uc_struct*, void* user) noexcept -> int
{
  auto* const self = static_cast<cortex_m*>(user);
  try
  {
    self->wait_for_interrupt();
  }
  catch(...)
  {
    self->keep_failure(std::current_exception());
  }
  return 0;
}

auto cortex_m::raw_pc() const -> address
{
  return read(UC_ARM_REG_PC);
}

auto cortex_m::in_exit_list(address at) const -> bool
{
  auto* const engine = engine_handle();
  std::size_t count  = 0;
  require(uc_ctl_get_exits_cnt(engine, &count), "reading the exit count");
  exits_.resize(count);
  if(count != 0)
  {
    require(uc_ctl_get_exits(engine, exits_.data(), count), "reading the exits");
  }
  return std::ranges::contains(exits_, std::uint64_t{at});
}

auto cortex_m::wfi_size(address at) const -> std::uint32_t
{
  auto&      services = context_->services;
  auto const first    = services.read(at, 2, initiator::debugger);
  if(first == wfi_narrow)
  {
    return 2;
  }
  if(first == wfi_wide_first and services.read(at + 2, 2, initiator::debugger) == wfi_wide_second)
  {
    return 4;
  }
  return 0;
}

void cortex_m::wait_for_interrupt()
{
  auto const at = raw_pc();
  if(in_exit_list(at))
  {
    return;
  }
  for(address const back : std::array<address, 3>{0, 2, 4})
  {
    if(at < back)
    {
      continue;
    }
    auto const size = wfi_size(at - back);
    if(size != 0 and (back == 0 or size == back))
    {
      state_.sleep = sleep_state{.at = at - back, .size = size, .kind = sleep_kind::wfi};
      context_->services.end_slice();
      return;
    }
  }
}

auto cortex_m::sleeping() const -> bool
{
  if(not state_.sleep)
  {
    return false;
  }
  auto const at = raw_pc();
  return at == state_.sleep->at or at == state_.sleep->at + state_.sleep->size;
}

auto cortex_m::wake_event() const -> bool
{
  auto const wfe = state_.sleep and state_.sleep->kind == sleep_kind::wfe;
  if(wfe and state_.event_register)
  {
    return true;
  }
  if(trustzone())
  {
    auto const best = best_pending(state_.exceptions, priority_config());
    return best.number != 0
       and exception_priority(state_.exceptions, priority_config(), best.number, best.state, true)
               < current_priority(not wfe);
  }
  auto const best = best_pending(state_.exceptions);
  return best
     and group_priority(priority_of(state_.exceptions, *best), state_.scs.prigroup)
             < current_priority(not wfe);
}

auto cortex_m::may_wake(std::uint32_t source) const -> bool
{
  auto const number = 16 + source;
  if(number >= exception_count())
  {
    return true;
  }
  auto const asleep = sleeping();
  auto const wfe    = asleep and state_.sleep->kind == sleep_kind::wfe;
  if(wfe and (state_.scs.scr & scr_sevonpend) != 0)
  {
    return true;
  }
  if(not is_enabled(number))
  {
    return false;
  }
  auto const current = current_priority(asleep and not wfe);
  if(trustzone())
  {
    auto const config = priority_config();
    auto const target = exception_target(state_.exceptions, config, number, current_security());
    return exception_priority(state_.exceptions, config, number, target, true) < current;
  }
  return group_priority(priority_of(state_.exceptions, number), state_.scs.prigroup) < current;
}

auto cortex_m::wake_pending() const -> bool
{
  return sleeping() ? wake_event() : deliverable_now();
}

void cortex_m::note_pending(std::uint32_t number) noexcept
{
  if((state_.scs.scr & scr_sevonpend) != 0 and not test_bit(state_.exceptions.pending, number))
  {
    state_.event_register = true;
  }
}

auto cortex_m::halfword(address at) const -> std::uint32_t
{
  return context_->services.read(at, 2, initiator::debugger).value_or(0);
}

auto cortex_m::self_branch(address at) const -> bool
{
  auto const first = halfword(at);
  auto const loops =
      first == b_self_narrow or (first == b_self_first and halfword(at + 2) == b_self_second);
  return loops and (read(UC_ARM_REG_XPSR) & xpsr_it) == 0;
}

auto cortex_m::hint() -> bool
{
  auto const at   = raw_pc();
  auto const here = halfword(at);
  if((here & 0xff00u) == 0xde00u
     or ((here & 0xfff0u) == 0xf7f0u and (halfword(at + 2) & 0xf000u) == 0xa000u))
  {
    return false;
  }
  address       insn = 0;
  std::uint32_t size = 0;
  bool          wfe  = false;
  if(auto const before = at >= 2 ? halfword(at - 2) : 0;
     before == wfe_narrow or before == yield_narrow)
  {
    insn = at - 2;
    size = 2;
    wfe  = before == wfe_narrow;
  }
  else if(
      auto const second = at >= 4 ? halfword(at - 2) : 0;
      at >= 4
      and halfword(at - 4) == hint_wide_first
      and (second == wfe_wide_second or second == yield_wide))
  {
    insn = at - 4;
    size = 4;
    wfe  = second == wfe_wide_second;
  }
  else
  {
    return false;
  }
  if(not wfe)
  {
    return true;
  }
  auto const sev = (insn >= 2 and halfword(insn - 2) == sev_narrow)
                or (insn >= 4
                    and halfword(insn - 4) == hint_wide_first
                    and halfword(insn - 2) == sev_wide_second);
  if(sev or state_.event_register)
  {
    state_.event_register = false;
    return true;
  }
  state_.sleep = sleep_state{.at = insn, .size = size, .kind = sleep_kind::wfe};
  context_->services.end_slice();
  return true;
}

void cortex_m::wake_if_due()
{
  if(not state_.sleep)
  {
    return;
  }
  if(not sleeping())
  {
    state_.sleep.reset();
    return;
  }
  if(not wake_event())
  {
    return;
  }
  auto const& sleep = *state_.sleep;
  if(sleep.kind == sleep_kind::wfe and state_.event_register)
  {
    state_.event_register = false;
  }
  if(sleep.size != 0 and raw_pc() == sleep.at)
  {
    write(UC_ARM_REG_PC, (sleep.at + sleep.size) | 1u);
  }
  state_.sleep.reset();
}
}
