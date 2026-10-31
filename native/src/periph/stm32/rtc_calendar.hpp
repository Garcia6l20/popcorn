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

inline constexpr std::uint64_t ps_per_second    = 1'000'000'000'000;
inline constexpr std::uint64_t seconds_per_day  = 86'400;
inline constexpr std::uint64_t days_per_century = 36'525;
inline constexpr std::uint32_t first_key        = 0xCA;
inline constexpr std::uint32_t second_key       = 0x53;
inline constexpr std::uint64_t init_cycles      = 2;
inline constexpr std::uint64_t exit_cycles      = 4;

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
 *        `prediv_a`, `prediv_s`, `counting` and `weekday_base` of the model's state.
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

  /** @brief Moves the anchor to a time and a new RTCCLK rate, keeping the cycle count. */
  void rebase(std::uint64_t ps, std::uint64_t hz)
  {
    state_.cycle0      = cycle_at(ps);
    state_.hz          = hz;
    state_.anchor_ps   = ps;
    state_.anchor_frac = 0;
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
