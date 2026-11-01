#pragma once

#include "model_base.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace popcorn::periph::stm32::rtc_calendar
{
using wide = unsigned __int128;

inline constexpr std::uint64_t seconds_per_day  = 86'400;
inline constexpr std::uint64_t days_per_century = 36'525;
inline constexpr std::uint32_t first_key        = 0xCA;
inline constexpr std::uint32_t second_key       = 0x53;
inline constexpr std::uint64_t init_cycles      = 2;
inline constexpr std::uint64_t exit_cycles      = 4;
inline constexpr std::uint64_t search_days      = 4 * 366;

inline constexpr std::array<std::string_view, 7> time_names{"PM",  "HT", "HU", "MNT",
                                                            "MNU", "ST", "SU"};
inline constexpr std::array<std::string_view, 7> date_names{"YT", "YU", "WDU", "MT",
                                                            "MU", "DT", "DU"};

/** @brief Calendar fields of one second, the year counted from 2000. */
struct date_time
{
  /** @brief Year from 2000, 0 to 99. */
  std::uint32_t year{};
  /** @brief Month, 1 to 12. */
  std::uint32_t month{};
  /** @brief Day of the month, 1 to 31. */
  std::uint32_t day{};
  /** @brief Weekday, 1 Monday to 7 Sunday. */
  std::uint32_t weekday{};
  /** @brief Hour, 0 to 23. */
  std::uint32_t hour{};
  /** @brief Minute, 0 to 59. */
  std::uint32_t minute{};
  /** @brief Second, 0 to 59. */
  std::uint32_t second{};
};

/** @brief Fields of an `RTC_ALRMxR` alarm register. */
struct alarm_match
{
  /** @brief MSK4, the date or weekday masked. */
  field_ref date_mask;
  /** @brief WDSEL, DU holds a weekday. */
  field_ref weekday_select;
  /** @brief DT, date tens. */
  field_ref date_tens;
  /** @brief DU, date units or weekday. */
  field_ref date_units;
  /** @brief MSK3, the hour masked. */
  field_ref hour_mask;
  /** @brief PM. */
  field_ref pm;
  /** @brief HT, hour tens. */
  field_ref hour_tens;
  /** @brief HU, hour units. */
  field_ref hour_units;
  /** @brief MSK2, the minutes masked. */
  field_ref minute_mask;
  /** @brief MNT, minute tens. */
  field_ref minute_tens;
  /** @brief MNU, minute units. */
  field_ref minute_units;
  /** @brief MSK1, the seconds masked. */
  field_ref second_mask;
  /** @brief ST, second tens. */
  field_ref second_tens;
  /** @brief SU, second units. */
  field_ref second_units;
};

/**
 * @brief Resolves the fields of an alarm register.
 * @param field Resolver of a field by register and field name, the model's `field`.
 * @param reg Alarm register name, such as `ALRMAR`.
 */
template <typename Resolve>
auto alarm_match_of(Resolve const& field, std::string_view reg) -> alarm_match
{
  return {
      .date_mask      = field(reg, "MSK4"),
      .weekday_select = field(reg, "WDSEL"),
      .date_tens      = field(reg, "DT"),
      .date_units     = field(reg, "DU"),
      .hour_mask      = field(reg, "MSK3"),
      .pm             = field(reg, "PM"),
      .hour_tens      = field(reg, "HT"),
      .hour_units     = field(reg, "HU"),
      .minute_mask    = field(reg, "MSK2"),
      .minute_tens    = field(reg, "MNT"),
      .minute_units   = field(reg, "MNU"),
      .second_mask    = field(reg, "MSK1"),
      .second_tens    = field(reg, "ST"),
      .second_units   = field(reg, "SU")};
}

/** @brief RTCCLK divider of a wakeup clock selection WUCKSEL 0 to 3, 16 down to 2. */
constexpr auto wakeup_divider(std::uint32_t select) noexcept -> std::uint64_t
{
  return std::uint64_t{16} >> (select & 3u);
}

/** @brief Whether a wakeup clock selection counts ck_spre, WUCKSEL 1xx. */
constexpr auto wakeup_on_spre(std::uint32_t select) noexcept -> bool
{
  return (select & 4u) != 0;
}

/** @brief Value of two BCD digits. */
constexpr auto bcd(std::uint32_t tens, std::uint32_t units) noexcept -> std::uint32_t
{
  return 10 * tens + units;
}

/** @brief Day 0 of the calendar, 2000-01-01. */
inline auto epoch() -> std::chrono::sys_days
{
  return std::chrono::sys_days{std::chrono::year{2000} / std::chrono::January / 1};
}

/** @brief Days from 2000-01-01 to a date, each field clamped to its range. */
inline auto days_of(std::uint32_t year, std::uint32_t month, std::uint32_t day) -> std::uint64_t
{
  using namespace std::chrono;
  auto const y   = std::chrono::year{2000 + static_cast<int>(std::min(year, 99u))};
  auto const m   = std::chrono::month{std::clamp(month, 1u, 12u)};
  auto const top = static_cast<unsigned>((y / m / last).day());
  auto const d   = std::chrono::day{std::clamp(day, 1u, top)};
  return static_cast<std::uint64_t>((sys_days{y / m / d} - epoch()).count());
}

/** @brief Date of a day count from 2000-01-01, modulo one century. */
inline auto date_of(std::uint64_t days) -> std::chrono::year_month_day
{
  return std::chrono::year_month_day{epoch() + std::chrono::days{days % days_per_century}};
}

/**
 * @brief Calendar counter of the STM32 RTC models, derived from the `rtc` clock domain and the
 *        virtual time of its last start, never ticked: RTCCLK cycles from an anchor time, the
 *        asynchronous and synchronous prescalers, the TR, DR and SSR words in 24 or 12-hour
 *        format and the TR and DR writes. It keeps no state of its own: it reads and writes the
 *        members `anchor_ps`, `anchor_frac`, `hz`, `cycle0`, `seconds`, `phase`, `start`,
 *        `prediv_a`, `prediv_s`, `counting` and `weekday_base` of the model's state, and
 *        `wakeup_base` and `wakeup_fired` for the wakeup timer.
 * @tparam State State of the model.
 */
template <typename State> class counter
{
public:
  /**
   * @brief Resolves the TR and DR fields and CR.FMT.
   * @param state State of the model, which outlives the counter.
   * @param regs Storage of the peripheral, which outlives the counter.
   * @param field Resolver of a field by register and field name, the model's `field`.
   */
  template <typename Resolve>
  counter(State& state, register_file const& regs, Resolve const& field)
      : state_{state}, regs_{regs}, format_{field("CR", "FMT")}
  {
    for(std::size_t index = 0; index < time_names.size(); ++index)
    {
      time_[index] = field("TR", time_names[index]);
      date_[index] = field("DR", date_names[index]);
    }
  }

  /** @brief RTCCLK cycle at a time since power-on. */
  [[nodiscard]] auto cycle_at(std::uint64_t ps) const -> std::uint64_t
  {
    if(state_.hz == 0)
    {
      return state_.cycle0;
    }
    auto const at = wide{ps} * state_.hz;
    return state_.cycle0
         + (at > anchor() ? static_cast<std::uint64_t>((at - anchor()) / ps_per_second) : 0);
  }

  /** @brief Time since power-on of an RTCCLK cycle, rounded up. */
  [[nodiscard]] auto time_of(std::uint64_t cycle) const -> std::uint64_t
  {
    auto const ahead = cycle > state_.cycle0 ? cycle - state_.cycle0 : 0;
    auto const at    = anchor() + wide{ahead} * ps_per_second;
    return static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz);
  }

  /** @brief Synchronous prescaler ticks per second, PREDIV_S + 1. */
  [[nodiscard]] auto sync_ticks() const -> std::uint64_t
  {
    return std::uint64_t{state_.prediv_s} + 1;
  }

  /** @brief Calendar position in synchronous ticks at an RTCCLK cycle. */
  [[nodiscard]] auto position_at(std::uint64_t cycle) const -> std::uint64_t
  {
    auto const base = state_.seconds * sync_ticks() + state_.phase;
    if(state_.counting == 0 or cycle < state_.start)
    {
      return base;
    }
    return base + (cycle - state_.start) / (std::uint64_t{state_.prediv_a} + 1);
  }

  /** @brief First RTCCLK cycle of a calendar position. */
  [[nodiscard]] auto cycle_of(std::uint64_t position) const -> std::uint64_t
  {
    auto const base = state_.seconds * sync_ticks() + state_.phase;
    return state_.start
         + (position > base ? position - base : 0) * (std::uint64_t{state_.prediv_a} + 1);
  }

  /** @brief Calendar fields of a second count from 2000-01-01. */
  [[nodiscard]] auto calendar(std::uint64_t seconds) const -> date_time
  {
    auto const days = seconds / seconds_per_day;
    auto const tod  = seconds % seconds_per_day;
    auto const ymd  = date_of(days);
    return {
        .year    = static_cast<std::uint32_t>(static_cast<int>(ymd.year()) - 2000),
        .month   = static_cast<unsigned>(ymd.month()),
        .day     = static_cast<unsigned>(ymd.day()),
        .weekday = static_cast<std::uint32_t>((state_.weekday_base + days) % 7 + 1),
        .hour    = static_cast<std::uint32_t>(tod / 3600),
        .minute  = static_cast<std::uint32_t>(tod / 60 % 60),
        .second  = static_cast<std::uint32_t>(tod % 60)};
  }

  /** @brief TR word of a calendar position, in the format CR.FMT selects. */
  [[nodiscard]] auto time_register(std::uint64_t position) const -> std::uint32_t
  {
    auto const t    = calendar(position / sync_ticks());
    auto       hour = t.hour;
    auto       pm   = std::uint32_t{};
    if(get(regs_, format_) != 0)
    {
      pm   = hour >= 12 ? 1 : 0;
      hour = hour % 12 == 0 ? 12 : hour % 12;
    }
    return word(
        time_,
        {pm, hour / 10, hour % 10, t.minute / 10, t.minute % 10, t.second / 10, t.second % 10});
  }

  /** @brief DR word of a calendar position. */
  [[nodiscard]] auto date_register(std::uint64_t position) const -> std::uint32_t
  {
    auto const t = calendar(position / sync_ticks());
    return word(
        date_,
        {t.year / 10, t.year % 10, t.weekday, t.month / 10, t.month % 10, t.day / 10, t.day % 10});
  }

  /** @brief SSR word of a calendar position, the synchronous prescaler's down-counter. */
  [[nodiscard]] auto subsecond_register(std::uint64_t position) const -> std::uint32_t
  {
    return static_cast<std::uint32_t>(state_.prediv_s - position % sync_ticks());
  }

  /** @brief Hour 0 to 23 of BCD hour digits, read in the format CR.FMT selects. */
  [[nodiscard]] auto hour_of(std::uint32_t tens, std::uint32_t units, bool pm) const
      -> std::uint32_t
  {
    auto const hour = bcd(tens, units);
    if(get(regs_, format_) == 0)
    {
      return hour;
    }
    return hour % 12 + (pm ? 12 : 0);
  }

  /**
   * @brief First second after @p after the alarm matches, within 4 years. An alarm whose
   *        unmasked fields hold a value the calendar never takes, such as seconds 79, matches
   *        nothing.
   * @param a Fields of the alarm register.
   * @param word Alarm register word.
   * @param after Second count from 2000-01-01 the search starts after.
   */
  [[nodiscard]] auto next_alarm(alarm_match const& a, std::uint32_t word, std::uint64_t after) const
      -> std::optional<std::uint64_t>
  {
    auto const any_sec = a.second_mask.of(word) != 0;
    auto const any_min = a.minute_mask.of(word) != 0;
    auto const any_hr  = a.hour_mask.of(word) != 0;
    auto const any_day = a.date_mask.of(word) != 0;
    auto const second  = bcd(a.second_tens.of(word), a.second_units.of(word));
    auto const minute  = bcd(a.minute_tens.of(word), a.minute_units.of(word));
    auto const hour    = hour_of(a.hour_tens.of(word), a.hour_units.of(word), a.pm.of(word) != 0);
    auto const date    = bcd(a.date_tens.of(word), a.date_units.of(word));
    auto const weekday = a.weekday_select.of(word) != 0;
    auto const hour_ok =
        get(regs_, format_) == 0
            ? a.hour_units.of(word) <= 9 and bcd(a.hour_tens.of(word), a.hour_units.of(word)) <= 23
            : a.hour_units.of(word) <= 9
                  and bcd(a.hour_tens.of(word), a.hour_units.of(word)) >= 1
                  and bcd(a.hour_tens.of(word), a.hour_units.of(word)) <= 12;
    auto const day_ok = weekday ? a.date_units.of(word) >= 1 and a.date_units.of(word) <= 7
                                : a.date_units.of(word) <= 9 and date >= 1 and date <= 31;
    auto const valid  = (any_sec or (a.second_units.of(word) <= 9 and second < 60))
                   and (any_min or (a.minute_units.of(word) <= 9 and minute < 60))
                   and (any_hr or hour_ok)
                   and (any_day or day_ok);
    if(not valid)
    {
      return std::nullopt;
    }
    auto const first = (after + 1) / seconds_per_day;
    for(std::uint64_t day = first; day < first + search_days; ++day)
    {
      if(not any_day)
      {
        auto const t = calendar(day * seconds_per_day);
        if(weekday ? a.date_units.of(word) != t.weekday : date != t.day)
        {
          continue;
        }
      }
      auto const from = day == first ? (after + 1) % seconds_per_day : 0;
      for(auto h = from / 3600; h < 24; ++h)
      {
        if(not any_hr and h != hour)
        {
          continue;
        }
        auto const from_minute = h == from / 3600 ? from / 60 % 60 : 0;
        for(auto m = from_minute; m < 60; ++m)
        {
          if(not any_min and m != minute)
          {
            continue;
          }
          auto const same = h == from / 3600 and m == from / 60 % 60;
          for(auto sec = same ? from % 60 : 0; sec < 60; ++sec)
          {
            if(any_sec or sec == second)
            {
              return day * seconds_per_day + h * 3600 + m * 60 + sec;
            }
          }
        }
      }
    }
    return std::nullopt;
  }

  /**
   * @brief RTCCLK cycle of the next wakeup of an enabled timer, from the members
   *        `wakeup_base` and `wakeup_fired`: every WUT + 1 periods of RTCCLK / 16 to / 2, or of
   *        ck_spre with 2^16 added for WUCKSEL 11x. A ck_spre timer of a stopped calendar has none.
   * @param select WUCKSEL.
   * @param reload WUT.
   */
  [[nodiscard]] auto wakeup_due(std::uint32_t select, std::uint32_t reload) const
      -> std::optional<std::uint64_t>
  {
    auto const periods = std::uint64_t{reload} + 1;
    if(wakeup_on_spre(select))
    {
      if(state_.counting == 0)
      {
        return std::nullopt;
      }
      auto const period = periods + ((select & 2u) != 0 ? 0x1'0000 : 0);
      auto const second = state_.wakeup_base + (state_.wakeup_fired + 1) * period;
      return cycle_of(second * sync_ticks());
    }
    return state_.wakeup_base + (state_.wakeup_fired + 1) * periods * wakeup_divider(select);
  }

  /** @brief Moves the anchor to a time and a new RTCCLK rate, keeping the cycle count. */
  void rebase(std::uint64_t ps, std::uint64_t hz)
  {
    state_.cycle0      = cycle_at(ps);
    state_.hz          = hz;
    state_.anchor_ps   = ps;
    state_.anchor_frac = 0;
  }

  /**
   * @brief Starts the stopped calendar from its second at a whole tick, with the prescalers
   *        taken from PRER, as the end of init mode does.
   * @param start RTCCLK cycle of the first prescaler edge.
   * @param prediv_a PRER.PREDIV_A.
   * @param prediv_s PRER.PREDIV_S.
   */
  void restart(std::uint64_t start, std::uint32_t prediv_a, std::uint32_t prediv_s)
  {
    state_.phase    = 0;
    state_.prediv_a = prediv_a;
    state_.prediv_s = prediv_s;
    state_.counting = 1;
    state_.start    = start;
  }

  /** @brief Stops the calendar at its position at an RTCCLK cycle. */
  void freeze(std::uint64_t cycle)
  {
    auto const position = position_at(cycle);
    state_.seconds      = position / sync_ticks();
    state_.phase        = position % sync_ticks();
    state_.counting     = 0;
  }

  /** @brief Sets the second count and the weekday from the board's power-on calendar. */
  void load(std::optional<std::chrono::sys_seconds> initial)
  {
    if(not initial)
    {
      return;
    }
    auto const since =
        std::chrono::floor<std::chrono::seconds>(*initial) - std::chrono::sys_seconds{epoch()};
    auto const total    = static_cast<std::uint64_t>(std::max<std::int64_t>(since.count(), 0))
                        % (days_per_century * seconds_per_day);
    auto const days     = total / seconds_per_day;
    auto const iso      = std::chrono::weekday{epoch() + std::chrono::days{days}}.iso_encoding();
    state_.seconds      = total;
    state_.weekday_base = static_cast<std::uint8_t>((iso + 6 - days % 7) % 7);
  }

  /** @brief Takes the time of day of a TR word, the date kept, the subseconds restarted. */
  void set_time(std::uint32_t now)
  {
    auto const f    = [&](std::size_t index) { return time_[index].of(now); };
    auto const hour = hour_of(f(1), f(2), f(0) != 0);
    auto const day  = state_.seconds / seconds_per_day;
    auto const tod  = std::uint64_t{hour} * 3600 + bcd(f(3), f(4)) * 60 + bcd(f(5), f(6));
    state_.seconds  = day * seconds_per_day + std::min<std::uint64_t>(tod, seconds_per_day - 1);
    state_.phase    = 0;
  }

  /** @brief Takes the date and weekday of a DR word, the time of day kept. */
  void set_date(std::uint32_t now)
  {
    auto const f        = [&](std::size_t index) { return date_[index].of(now); };
    auto const days     = days_of(bcd(f(0), f(1)), bcd(f(3), f(4)), bcd(f(5), f(6)));
    auto const weekday  = std::clamp(f(2), 1u, 7u);
    state_.seconds      = days * seconds_per_day + state_.seconds % seconds_per_day;
    state_.phase        = 0;
    state_.weekday_base = static_cast<std::uint8_t>((weekday + 6 - days % 7) % 7);
  }

  /**
   * @brief Applies CR.ADD1H or CR.SUB1H to a counting calendar. SUB1H leaves hour 0.
   * @param cycle RTCCLK cycle of the write.
   * @param add Whether ADD1H was written 1.
   * @param sub Whether SUB1H was written 1.
   */
  void shift_hour(std::uint64_t cycle, bool add, bool sub)
  {
    if(state_.counting == 0)
    {
      return;
    }
    auto const hour = position_at(cycle) / sync_ticks() % seconds_per_day / 3600;
    if(add)
    {
      state_.seconds += 3600;
    }
    else if(sub and hour != 0 and state_.seconds >= 3600)
    {
      state_.seconds -= 3600;
    }
  }

private:
  [[nodiscard]] auto anchor() const -> wide
  {
    return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
  }

  [[nodiscard]] static auto
      word(std::array<field_ref, 7> const& fields, std::array<std::uint32_t, 7> const& values)
          -> std::uint32_t
  {
    auto out = std::uint32_t{};
    for(std::size_t index = 0; index < values.size(); ++index)
    {
      out = fields[index].with(out, values[index]);
    }
    return out;
  }

  State&                   state_;
  register_file const&     regs_;
  field_ref                format_;
  std::array<field_ref, 7> time_{};
  std::array<field_ref, 7> date_{};
};
}
