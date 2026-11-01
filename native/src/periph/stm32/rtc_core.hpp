#pragma once

#include "model_base.hpp"
#include "rtc_calendar.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief Calendar, event and write-protection logic shared by the RTC and the classic RTC. It
 *        holds no state and no virtual function: every member takes the model as its deduced
 *        object parameter and works on the model's members, so the model derives from this
 *        class and befriends it.
 *
 *        The model provides:
 *        - `wakeup_event`, the `std::uint32_t` event kind of the wake-up timer, an
 *          `on_event(std::uint32_t, std::uint64_t)` and a `settle(std::uint64_t)`.
 *        - `debug_frozen()` of the model base and the flag `freeze_logged_` it takes.
 *        - `state_` with `anchor_ps`, `hz`, `unlock`, `held` and `held_ps`, and the arrays
 *          `due`, `armed` and `generation` indexed by event kind.
 *        - `ctx_`, `regs_`, the calendar counter `count_` and the clock domain `domain_`.
 *        - the register refs `time_`, `date_` and `key_`, and the field refs `prediv_a_`,
 *          `init_flag_`, `wakeup_enable_`, `wakeup_clock_` and `wakeup_reload_`.
 *
 *        The register write handlers are protected here. A model forwards each one in a member of
 *        its own, because a function with a deduced object parameter has no address that fits a
 *        `write_entry`.
 */
class rtc_core
{
protected:
  /** @brief TR write: kept at its old value outside init mode under the key, else sets the time. */
  void time_written(this auto& self, std::uint32_t old, std::uint32_t now)
  {
    if(not self.unlocked() or not self.initializing())
    {
      self.regs_.poke(self.time_.offset, old);
      return;
    }
    self.count_.set_time(now);
  }

  /** @brief DR write: kept at its old value outside init mode under the key, else sets the date. */
  void date_written(this auto& self, std::uint32_t old, std::uint32_t now)
  {
    if(not self.unlocked() or not self.initializing())
    {
      self.regs_.poke(self.date_.offset, old);
      return;
    }
    self.count_.set_date(now);
  }

  /** @brief PRER write: kept at its old value outside init mode under the key. */
  void prescaler_written(this auto& self, std::uint32_t old, std::uint32_t)
  {
    if(not self.unlocked() or not self.initializing())
    {
      self.regs_.poke(self.prediv_a_.offset, old);
    }
  }

  /** @brief WPR write: the 0xCA 0x53 key sequence advances or resets the lock state. */
  void key_written(this auto& self, std::uint32_t, std::uint32_t now)
  {
    self.regs_.poke(self.key_.offset, 0);
    auto const key = now & 0xFFu;
    if(key == rtc_calendar::first_key)
    {
      self.state_.unlock = 1;
    }
    else if(key == rtc_calendar::second_key and self.state_.unlock == 1)
    {
      self.state_.unlock = 2;
    }
    else
    {
      self.state_.unlock = 0;
    }
  }

  [[nodiscard]] auto now_ps(this auto const& self) -> std::uint64_t
  {
    return self.state_.held != 0 ? self.state_.held_ps : self.ctx_.clock.since_power_on().ps;
  }

  [[nodiscard]] auto unlocked(this auto const& self) -> bool
  {
    return self.state_.unlock == 2;
  }

  [[nodiscard]] auto initializing(this auto const& self) -> bool
  {
    return get(self.regs_, self.init_flag_) != 0;
  }

  void arm(this auto& self, std::uint32_t kind, std::uint64_t cycle)
  {
    self.state_.armed[kind] = 1;
    self.state_.due[kind]   = cycle;
    self.schedule(kind);
  }

  void schedule(this auto& self, std::uint32_t kind)
  {
    auto const generation = ++self.state_.generation[kind];
    if(self.state_.armed[kind] == 0 or self.state_.hz == 0 or self.state_.held != 0)
    {
      return;
    }
    auto const now    = self.now_ps();
    auto const offset = now - self.ctx_.clock.time().ps;
    auto const due    = std::max(self.count_.time_of(self.state_.due[kind]), now) - offset;
    self.ctx_.events.schedule_at(vtime{due}, kind, generation, self.ctx_.clock);
  }

  void schedule_all(this auto& self)
  {
    for(std::size_t kind = 0; kind < self.state_.due.size(); ++kind)
    {
      self.schedule(static_cast<std::uint32_t>(kind));
    }
  }

  void plan_wakeup(this auto& self)
  {
    self.state_.armed[self.wakeup_event] = 0;
    ++self.state_.generation[self.wakeup_event];
    if(get(self.regs_, self.wakeup_enable_) == 0)
    {
      return;
    }
    if(auto const due = self.count_.wakeup_due(
           get(self.regs_, self.wakeup_clock_), get(self.regs_, self.wakeup_reload_)))
    {
      self.arm(self.wakeup_event, *due);
    }
  }

  /** @brief Re-derives the calendar and every deadline when the RTC clock domain changes rate. */
  void clock_changed(this auto& self)
  {
    auto const hz = self.ctx_.clock.hz(self.domain_);
    if(hz == self.state_.hz)
    {
      return;
    }
    auto const ps = self.now_ps();
    self.count_.rebase(ps, hz);
    self.schedule_all();
    self.settle(self.count_.cycle_at(ps));
  }

  /**
   * @brief Freezes the calendar at a core hold when DBGMCU says so, and resumes it at the release.
   *        The caller has checked that the variant freezes at all.
   */
  void hold_core(this auto& self, bool held)
  {
    if(held)
    {
      auto const frozen = self.debug_frozen(self.freeze_logged_);
      if(self.state_.held != 0)
      {
        if(not frozen)
        {
          self.hold_core(false);
        }
        return;
      }
      if(not frozen)
      {
        return;
      }
      auto const cycle = self.count_.cycle_at(self.now_ps());
      for(auto fired = true; fired;)
      {
        fired = false;
        for(std::uint32_t kind = 0; kind < self.state_.due.size(); ++kind)
        {
          if(self.state_.armed[kind] != 0 and self.state_.due[kind] <= cycle)
          {
            self.on_event(kind, self.state_.generation[kind]);
            fired = true;
          }
        }
      }
      self.settle(cycle);
      self.state_.held_ps = self.ctx_.clock.since_power_on().ps;
      self.state_.held    = 1;
      for(auto& value : self.state_.generation)
      {
        ++value;
      }
      return;
    }
    if(self.state_.held == 0)
    {
      return;
    }
    self.state_.anchor_ps += self.ctx_.clock.since_power_on().ps - self.state_.held_ps;
    self.state_.held    = 0;
    self.state_.held_ps = 0;
    self.schedule_all();
    self.settle(self.count_.cycle_at(self.now_ps()));
  }
};
}
