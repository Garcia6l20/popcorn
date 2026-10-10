#include "debug_freeze.hpp"
#include "factories.hpp"
#include "lsi_source.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/core_hold.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::uint32_t expiry        = 0;
constexpr std::uint32_t early_wakeup  = 1;
constexpr std::uint32_t first_update  = 2;
constexpr std::size_t   updates       = 4;
constexpr std::uint32_t update_cycles = 6;
constexpr std::uint32_t start_key     = 0xCCCC;
constexpr std::uint32_t refresh_key   = 0xAAAA;
constexpr std::uint32_t unlock_key    = 0x5555;
constexpr std::uint32_t counter_top   = 0xFFF;

enum update_index : std::uint8_t
{
  prescaler_update,
  reload_update,
  window_update,
  early_update,
};

struct iwdg_state
{
  std::uint64_t                                     anchor_ps{};
  std::uint64_t                                     anchor_frac{};
  std::uint64_t                                     hz{};
  std::array<std::uint32_t, first_update + updates> generation{};
  std::array<std::uint64_t, first_update + updates> queued{};
  std::array<std::uint64_t, updates>                update_due{};
  std::uint32_t                                     start{};
  std::uint32_t                                     divider{4};
  std::uint32_t                                     early{};
  std::uint8_t                                      running{};
  std::uint8_t                                      unlocked{};
  std::uint8_t                                      bitten{};
  std::uint8_t                                      waiting{};
  std::uint8_t                                      held{};
  std::uint64_t                                     held_ps{};

  friend auto operator==(iwdg_state const&, iwdg_state const&) -> bool = default;
};

/**
 * @brief IWDG of the STM32U5, RM0456 section 61 "Independent watchdog (IWDG)", software
 *        watchdog mode. The 12-bit down-counter is derived from the LSI clock and the virtual
 *        time of its last reload, never ticked. `KR` 0xCCCC starts it from 0xFFF and forces the
 *        LSI on through the RCC peer, 0xAAAA reloads it from RL, 0x5555 unlocks PR, RLR, WINR
 *        and EWCR until another key. PVU, RVU, WVU and EWU stay set for six LSI cycles after a
 *        write, the new prescaler applies then, a new window reloads the counter then, a new EWIT
 *        and EWIE apply then, RM0456 section 61.7.6. The counter reaching 0, or a reload while it
 *        is above WIN, requests a watchdog reset. EWIF and the IWDG IRQ follow the counter
 *        reaching EWIT - 1 while EWIE is set.
 *        The kernel clock is RCC's `lsi` domain, after LSIPREDIV. Every deadline is kept in
 *        virtual time, re-derived when the LSI rate changes. An LSI
 *        rate change keeps the partial division sequence, a stopped LSI drops it. Deviations: the
 *        new prescaler restarts the division sequence when PVU falls, an error under one period
 *        of the old prescaled clock, a refresh reloads at the key write instead of one period
 *        later, and EWIC acknowledges without the 0x5555 key, as LL_IWDG_ClearFlag_EWIF expects.
 *        Once started it runs until reset. While the machine holds its core with DBGMCU
 *        `DBG_IWDG_STOP` set, the counter, the early wake-up and the register update
 *        synchronisers stop at the time of the hold and resume at the release, their phase kept
 *        to the picosecond. RM0456 says only "IWDG is frozen while CPU is in debug mode": that
 *        the update synchronisers stop with the counter is an assumption. The bit is read at the
 *        hold. Not modelled: the hardware watchdog option.
 */
class iwdg final : public dispatch_model<iwdg, iwdg_state>,
                   public clock_listener,
                   public core_hold_listener
{
public:
  iwdg(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_clock_change() override;
  void on_core_hold(bool held) override;

  void                                key_written(std::uint32_t old, std::uint32_t now);
  template <update_index Update> void guarded_written(std::uint32_t old, std::uint32_t now);
  void                                early_written(std::uint32_t old, std::uint32_t now);
  void                                plain_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto count_at(std::uint64_t ps) const -> std::uint32_t;
  [[nodiscard]] auto divider() const -> std::uint32_t;

  void restart(std::uint32_t value);
  void start_update(update_index which);
  void schedule_update(std::size_t which);
  void schedule_updates();
  void rebase(std::uint64_t ps, std::uint64_t hz);
  void finish_update(update_index which);
  void reschedule();
  void schedule_at_tick(std::uint32_t kind, std::uint64_t ticks);
  void post(std::uint32_t kind, std::uint64_t due);
  void withdraw(std::uint32_t kind);
  void bite();
  void interrupt();

  register_file&           regs_;
  field_ref                key_;
  field_ref                prescaler_;
  field_ref                reload_;
  field_ref                window_;
  field_ref                early_time_;
  field_ref                early_clear_;
  field_ref                early_enable_;
  field_ref                early_flag_;
  std::array<field_ref, 4> busy_{};
  reg_ref                  early_register_;
  core::clock_domain       lsi_;
  irq_line&                line_;
  bool                     freeze_logged_ = false;
};

constexpr std::array<write_entry<iwdg>, 6> iwdg_writes{
    {
     {"KR", &iwdg::key_written},
     {"PR", &iwdg::guarded_written<prescaler_update>},
     {"RLR", &iwdg::guarded_written<reload_update>},
     {"SR", &iwdg::plain_written},
     {"WINR", &iwdg::guarded_written<window_update>},
     {"EWCR", &iwdg::early_written},
     }
};

iwdg::iwdg(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "IWDG", iwdg_writes}, regs_{ctx.regs}, key_{field("KR", "KEY")},
      prescaler_{field("PR", "PR")}, reload_{field("RLR", "RL")}, window_{field("WINR", "WIN")},
      early_time_{field("EWCR", "EWIT")}, early_clear_{field("EWCR", "EWIC")},
      early_enable_{field("EWCR", "EWIE")}, early_flag_{field("SR", "EWIF")},
      busy_{field("SR", "PVU"), field("SR", "RVU"), field("SR", "WVU"), field("SR", "EWU")},
      early_register_{reg("EWCR")}, lsi_{ctx.clock.domain("lsi")}, line_{irq("IWDG_IRQn")}
{}

auto iwdg::now_ps() const -> std::uint64_t
{
  return state_.held != 0 ? state_.held_ps : ctx_.clock.time().ps;
}

auto iwdg::divider() const -> std::uint32_t
{
  return std::uint32_t{4} << std::min<std::uint32_t>(get(regs_, prescaler_), 8);
}

auto iwdg::anchor() const -> wide
{
  return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
}

auto iwdg::tick_unit() const -> wide
{
  return wide{state_.divider} * ps_per_second;
}

auto iwdg::count_at(std::uint64_t ps) const -> std::uint32_t
{
  if(state_.running == 0 or state_.hz == 0)
  {
    return state_.start;
  }
  auto const at    = wide{ps} * state_.hz;
  auto const since = at > anchor() ? (at - anchor()) / tick_unit() : 0;
  return since >= state_.start ? 0 : static_cast<std::uint32_t>(state_.start - since);
}

void iwdg::restart(std::uint32_t value)
{
  state_.start       = value;
  state_.anchor_ps   = now_ps();
  state_.anchor_frac = 0;
  state_.hz          = ctx_.clock.hz(lsi_);
  reschedule();
}

void iwdg::schedule_at_tick(std::uint32_t kind, std::uint64_t ticks)
{
  auto const at  = anchor() + wide{ticks} * tick_unit();
  auto const due = std::max(static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz), now_ps());
  post(kind, due);
}

void iwdg::post(std::uint32_t kind, std::uint64_t due)
{
  withdraw(kind);
  if(state_.held != 0)
  {
    return;
  }
  state_.queued[kind] =
      ctx_.events.schedule_at(vtime{due}, kind, ++state_.generation[kind], ctx_.clock) + 1;
}

void iwdg::withdraw(std::uint32_t kind)
{
  if(auto const seq = std::exchange(state_.queued[kind], 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
}

void iwdg::reschedule()
{
  ++state_.generation[expiry];
  ++state_.generation[early_wakeup];
  if(state_.running == 0 or state_.hz == 0 or state_.bitten != 0)
  {
    return;
  }
  schedule_at_tick(expiry, state_.start);
  auto const threshold = early_time_.of(state_.early);
  auto const armed     = early_enable_.of(state_.early) != 0 and get(regs_, early_flag_) == 0;
  if(armed and threshold != 0 and state_.start >= threshold)
  {
    schedule_at_tick(early_wakeup, state_.start - threshold + 1);
  }
}

void iwdg::start_update(update_index which)
{
  update(regs_, busy_[which], 1);
  state_.waiting |= static_cast<std::uint8_t>(1u << which);
  state_.update_due[which] = 0;
  schedule_update(which);
}

void iwdg::schedule_update(std::size_t which)
{
  auto& due = state_.update_due[which];
  if(due == 0)
  {
    auto const hz = ctx_.clock.hz(lsi_);
    if(hz == 0)
    {
      return;
    }
    due = now_ps() + (update_cycles * ps_per_second + hz - 1) / hz;
  }
  post(first_update + static_cast<std::uint32_t>(which), std::max(due, now_ps()));
}

void iwdg::schedule_updates()
{
  for(std::size_t which = 0; which < updates; ++which)
  {
    if((state_.waiting >> which & 1u) != 0 and state_.update_due[which] == 0)
    {
      schedule_update(which);
    }
  }
}

void iwdg::rebase(std::uint64_t ps, std::uint64_t hz)
{
  auto const count  = count_at(ps);
  auto       cycles = wide{};
  if(state_.hz != 0)
  {
    auto const at    = wide{ps} * state_.hz;
    auto const since = at > anchor() ? at - anchor() : 0;
    cycles           = since % tick_unit() / ps_per_second;
  }
  state_.start = count;
  state_.hz    = hz;
  if(hz == 0)
  {
    return;
  }
  auto const at      = wide{ps} * hz;
  auto const back    = cycles * ps_per_second;
  auto const start   = at > back ? at - back : 0;
  state_.anchor_ps   = static_cast<std::uint64_t>(start / hz);
  state_.anchor_frac = static_cast<std::uint64_t>(start % hz);
}

void iwdg::finish_update(update_index which)
{
  state_.waiting &= static_cast<std::uint8_t>(~(1u << which));
  state_.update_due[which] = 0;
  update(regs_, busy_[which], 0);
  if(which == early_update)
  {
    state_.early = early_clear_.with(regs_.peek(early_register_.offset), 0);
    interrupt();
  }
  if(state_.running == 0)
  {
    return;
  }
  if(which == prescaler_update)
  {
    auto const count = count_at(now_ps());
    state_.divider   = divider();
    restart(count);
  }
  else if(which == window_update)
  {
    restart(get(regs_, reload_));
  }
  else
  {
    reschedule();
  }
}

void iwdg::bite()
{
  if(state_.bitten != 0)
  {
    return;
  }
  state_.bitten = 1;
  reschedule();
  ctx_.services.request_reset(reset_kind::watchdog);
}

void iwdg::interrupt()
{
  if(get(regs_, early_flag_) != 0 and early_enable_.of(state_.early) != 0)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void iwdg::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto generation = state_.generation;
  for(auto& value : generation)
  {
    ++value;
  }
  state_            = {};
  state_.generation = generation;
  line_.lower();
}

void iwdg::key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(key_.offset, 0);
  auto const key  = key_.of(now);
  state_.unlocked = key == unlock_key ? 1 : 0;
  if(key == start_key and state_.running == 0)
  {
    state_.running = 1;
    state_.divider = divider();
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const source = dynamic_cast<lsi_source*>(peer))
      {
        source->force_lsi();
      }
    }
    restart(counter_top);
  }
  else if(key == refresh_key and state_.running != 0)
  {
    if(count_at(now_ps()) > get(regs_, window_))
    {
      bite();
      return;
    }
    restart(get(regs_, reload_));
  }
}

template <update_index Update> void iwdg::guarded_written(std::uint32_t old, std::uint32_t now)
{
  if(state_.unlocked == 0 or get(regs_, busy_[Update]) != 0)
  {
    auto const& target = Update == prescaler_update ? prescaler_
                       : Update == reload_update    ? reload_
                                                    : window_;
    regs_.poke(target.offset, old);
    return;
  }
  if(old != now)
  {
    start_update(Update);
  }
}

void iwdg::early_written(std::uint32_t old, std::uint32_t now)
{
  auto kept = early_clear_.with(now, 0);
  if(state_.unlocked == 0 or get(regs_, busy_[early_update]) != 0)
  {
    kept = early_clear_.with(old, 0);
  }
  else if(kept != early_clear_.with(old, 0))
  {
    start_update(early_update);
  }
  regs_.poke(early_register_.offset, kept);
  if(early_clear_.of(now) != 0)
  {
    update(regs_, early_flag_, 0);
  }
  interrupt();
  reschedule();
}

void iwdg::plain_written(std::uint32_t, std::uint32_t)
{}

void iwdg::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= state_.generation.size() or payload != state_.generation[kind])
  {
    return;
  }
  if(kind >= first_update)
  {
    finish_update(static_cast<update_index>(kind - first_update));
    return;
  }
  if(state_.running == 0 or state_.bitten != 0)
  {
    return;
  }
  if(kind == early_wakeup)
  {
    update(regs_, early_flag_, 1);
    interrupt();
    return;
  }
  bite();
}

void iwdg::on_clock_change()
{
  schedule_updates();
  if(state_.running == 0)
  {
    return;
  }
  if(auto const hz = ctx_.clock.hz(lsi_); hz != state_.hz)
  {
    rebase(now_ps(), hz);
    reschedule();
  }
}

void iwdg::on_core_hold(bool held)
{
  if(held)
  {
    if(state_.held != 0 or not debug_frozen(freeze_logged_))
    {
      return;
    }
    state_.held_ps = ctx_.clock.time().ps;
    state_.held    = 1;
    for(std::uint32_t kind = 0; kind < state_.generation.size(); ++kind)
    {
      withdraw(kind);
      ++state_.generation[kind];
    }
    return;
  }
  if(state_.held == 0)
  {
    return;
  }
  auto const span = ctx_.clock.time().ps - state_.held_ps;
  state_.held     = 0;
  state_.held_ps  = 0;
  state_.anchor_ps += span;
  for(std::size_t which = 0; which < updates; ++which)
  {
    if((state_.waiting >> which & 1u) == 0)
    {
      continue;
    }
    if(state_.update_due[which] != 0)
    {
      state_.update_due[which] += span;
    }
    schedule_update(which);
  }
  reschedule();
}
}

auto iwdg_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<iwdg>(ctx, desc);
  };
}
}
