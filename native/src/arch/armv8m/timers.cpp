#include "cortex_m.hpp"
#include "exceptions.hpp"
#include "systick.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <tuple>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t systick_event           = 1;
constexpr std::uint32_t systick_nonsecure_event = 2;
constexpr std::uint32_t syst_csr_writable = systick::enable | systick::tickint | systick::clksource;
constexpr std::uint32_t syst_rvr_writable = 0x00ff'ffff;
constexpr std::uint32_t dwt_cyccntena     = 1u << 0;
constexpr std::uint32_t demcr_trcena      = 1u << 24;

auto merge(std::uint32_t old, std::uint32_t value, std::uint32_t mask) noexcept -> std::uint32_t
{
  return (old & ~mask) | (value & mask);
}

void take_passed_reload(systick_state& s, std::uint64_t t) noexcept
{
  if(systick::reload_passed(s, t))
  {
    s.reload = s.rvr;
  }
}
}

auto cortex_m::timer_now() const -> std::uint64_t
{
  return state_.held_at.value_or(context_->clock.now().value);
}

auto cortex_m::systick_of(security bank) noexcept -> systick_state&
{
  return bank == security::secure ? state_.systick : state_.systick_nonsecure;
}

auto cortex_m::systick_ticks(systick_state const& s) const -> std::uint64_t
{
  return systick::ticks(s, timer_now(), context_->clock.cpi());
}

void cortex_m::hold_core(bool held)
{
  if(held == state_.held_at.has_value())
  {
    return;
  }
  auto const now = context_->clock.now().value;
  if(held)
  {
    state_.held_at = now;
    ++generation_;
    return;
  }
  auto const span = now - *state_.held_at;
  state_.held_at.reset();
  state_.systick.anchor += span;
  state_.systick_nonsecure.anchor += span;
  state_.cyccnt.anchor += span;
  arm_systicks();
}

auto cortex_m::read_systick(std::uint32_t offset, security bank) -> std::uint32_t
{
  auto&      s = systick_of(bank);
  auto const t = systick_ticks(s);
  take_passed_reload(s, t);
  switch(offset)
  {
    case 0x010:
    {
      auto const flag = systick::countflag(s, t);
      s.flag          = false;
      s.flag_since    = t;
      return s.csr | (flag ? systick::countflag_bit : 0u);
    }
    case 0x014:
      return s.rvr;
    case 0x018:
      return systick::value_at(s, t);
    default:
      return 0;
  }
}

void cortex_m::write_systick(
    std::uint32_t offset,
    std::uint32_t value,
    std::uint32_t mask,
    security      bank)
{
  auto&      s   = systick_of(bank);
  auto const now = timer_now();
  auto const t   = systick_ticks(s);
  take_passed_reload(s, t);
  auto const csr     = s.csr;
  auto const current = systick::value_at(s, t);
  auto const flag    = systick::countflag(s, t);
  auto const pending = t < systick::start_tick(s) ? s.reload : std::nullopt;
  if(systick::interrupting(s) and systick::crossings_through(s, t) > s.fired)
  {
    if(bank == security::secure)
    {
      set_exception_pending(exc_systick, true);
    }
    else
    {
      set_nonsecure_pending(exc_systick, true);
    }
  }
  if(offset == 0x010)
  {
    s.csr = merge(s.csr, value, mask & syst_csr_writable);
  }
  else if(offset == 0x014)
  {
    s.rvr = merge(s.rvr, value, mask & syst_rvr_writable);
  }
  auto const cleared    = offset == 0x018;
  auto const running    = (s.csr & systick::enable) != 0;
  auto const same_clock = ((csr ^ s.csr) & systick::clksource) == 0;
  s.anchor              = now;
  s.anchor_value        = cleared ? 0 : current;
  s.reload              = std::nullopt;
  if(not cleared and running and same_clock and pending)
  {
    s.anchor_value = 0;
    s.reload       = pending;
  }
  s.flag       = not cleared and flag;
  s.flag_since = 0;
  s.fired      = 0;
  ++generation_;
  arm_systicks();
}

void cortex_m::arm_systicks()
{
  arm_systick(security::secure);
  if(trustzone())
  {
    arm_systick(security::nonsecure);
  }
}

void cortex_m::arm_systick(security bank)
{
  auto const& s = systick_of(bank);
  if(not systick::interrupting(s) or state_.held_at)
  {
    return;
  }
  auto const next = systick::crossing(s, s.fired + 1);
  if(not next)
  {
    return;
  }
  auto const& clock = context_->clock;
  auto const  at    = std::max(systick::expiry(s, *next, clock.cpi()), clock.now().value);
  std::ignore       = context_->events.schedule(
      icount{at}, bank == security::secure ? systick_event : systick_nonsecure_event, generation_);
}

void cortex_m::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if((kind != systick_event and kind != systick_nonsecure_event) or payload != generation_)
  {
    return;
  }
  auto const bank = kind == systick_event ? security::secure : security::nonsecure;
  auto&      s    = systick_of(bank);
  if(not systick::interrupting(s))
  {
    return;
  }
  s.fired = systick::crossings_through(s, systick_ticks(s));
  if(bank == security::secure)
  {
    set_exception_pending(exc_systick, true);
  }
  else
  {
    set_nonsecure_pending(exc_systick, true);
  }
  arm_systick(bank);
}

auto cortex_m::cyccnt_counting() const noexcept -> bool
{
  return (state_.scs.dwt_ctrl & dwt_cyccntena) != 0 and (state_.scs.demcr & demcr_trcena) != 0;
}

auto cortex_m::read_cyccnt() const -> std::uint32_t
{
  auto const& c = state_.cyccnt;
  if(not cyccnt_counting())
  {
    return c.base;
  }
  auto const& clock   = context_->clock;
  auto const  elapsed = timer_now() - c.anchor;
  auto const  cycles =
      clock.cpi() == 1.0
          ? elapsed
          : static_cast<std::uint64_t>(std::floor(static_cast<long double>(elapsed) * clock.cpi()));
  return static_cast<std::uint32_t>(c.base + cycles);
}

void cortex_m::rebase_cyccnt(std::uint32_t value)
{
  state_.cyccnt = {.base = value, .anchor = timer_now()};
}
}
