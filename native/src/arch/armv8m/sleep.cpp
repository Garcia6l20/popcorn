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
  auto const best = best_pending(state_.exceptions);
  return best
     and group_priority(priority_of(state_.exceptions, *best), state_.scs.prigroup)
       < current_priority(true);
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
  if(sleep.size != 0 and raw_pc() == sleep.at)
  {
    write(UC_ARM_REG_PC, (sleep.at + sleep.size) | 1u);
  }
  state_.sleep.reset();
}
}
