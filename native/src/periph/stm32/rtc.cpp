#include "backup_domain.hpp"
#include "factories.hpp"
#include "gtzc.hpp"
#include "model_base.hpp"
#include "rtc_calendar.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/core_hold.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
using rtc_calendar::bcd;
using rtc_calendar::exit_cycles;
using rtc_calendar::first_key;
using rtc_calendar::init_cycles;
using rtc_calendar::second_key;
using rtc_calendar::seconds_per_day;

constexpr std::uint64_t search_days = 4 * 366;

enum event_kind : std::uint8_t
{
  init_ready,
  shadow_ready,
  wakeup_ready,
  wakeup,
  alarm_a,
  alarm_b,
  event_kinds,
};

struct rtc_state
{
  std::uint64_t                          anchor_ps{};
  std::uint64_t                          anchor_frac{};
  std::uint64_t                          hz{};
  std::uint64_t                          cycle0{};
  std::uint64_t                          seconds{};
  std::uint64_t                          phase{};
  std::uint64_t                          start{};
  std::uint32_t                          prediv_a{127};
  std::uint32_t                          prediv_s{255};
  std::uint8_t                           counting{};
  std::uint8_t                           weekday_base{};
  std::uint8_t                           unlock{};
  std::uint8_t                           latched_valid{};
  std::uint64_t                          latched{};
  std::uint64_t                          wakeup_base{};
  std::uint64_t                          wakeup_fired{};
  std::array<std::uint64_t, event_kinds> due{};
  std::array<std::uint8_t, event_kinds>  armed{};
  std::array<std::uint32_t, event_kinds> generation{};
  std::uint8_t                           held{};
  std::uint64_t                          held_ps{};

  friend auto operator==(rtc_state const&, rtc_state const&) -> bool = default;
};

struct alarm_fields
{
  std::string_view reg;
  std::string_view subseconds;
  std::string_view enable;
  std::string_view flag;
};

constexpr std::array<alarm_fields, 2> alarm_names{
    {
     {"ALRMAR", "ALRMASSR", "ALRAE", "ALRAF"},
     {"ALRMBR", "ALRMBSSR", "ALRBE", "ALRBF"},
     }
};

struct alarm_refs
{
  field_ref enable;
  field_ref flag;
  field_ref interrupt;
  reg_ref   reg;
  field_ref subsecond_mask;
  field_ref date_mask;
  field_ref weekday_select;
  field_ref date_tens;
  field_ref date_units;
  field_ref hour_mask;
  field_ref pm;
  field_ref hour_tens;
  field_ref hour_units;
  field_ref minute_mask;
  field_ref minute_tens;
  field_ref minute_units;
  field_ref second_mask;
  field_ref second_tens;
  field_ref second_units;
};

/**
 * @brief RTC of the STM32U5, RM0456 section 63 "Real-time clock (RTC)", BCD mode. The calendar is
 *        derived from the `rtc` clock domain and the virtual time of its last start, never ticked:
 *        the asynchronous and synchronous prescalers, SSR, TR and DR in 24 or 12-hour format,
 *        days in month and leap years for 2000 to 2099, the weekday counted from the one the
 *        firmware wrote. Init mode with INITF after 2 RTCCLK cycles and the count restarting 4
 *        cycles after INIT clears, the 0xCA 0x53 write protection of TR, DR, PRER, CALR, SHIFTR,
 *        INIT, BIN, BCDU, FMT, ADD1H, SUB1H and REFCKON, shadow registers latched by an SSR or TR
 *        read until DR is read, RSF one RTCCLK cycle after a clear, ADD1H and SUB1H. The wake-up
 *        timer on RTCCLK / 16 to / 2 or on ck_spre, 16 or 17 bits, with WUTWF. Alarms A and B on
 *        seconds, minutes, hours and date or weekday with their masks. Every deadline is kept in
 *        RTCCLK cycles, scheduled in virtual time, re-derived when RTCCLK changes rate. A reset
 *        other than power-on keeps the calendar, the configuration, the wake-up timer and the
 *        alarms, and resets the shadow registers. A backup domain reset, RCC_BDCR.BDRST, brings
 *        everything to its reset values. A ck_spre wake-up period restarts when INIT clears. A
 *        change of the RTC clock rate floors the partial RTCCLK cycle, an error under one cycle,
 *        31 us at 32768 Hz. The RTC counts in the machine's virtual time since power-on, which a
 *        system reset does not restart. An alarm field holding a digit the hardware never matches,
 * such as seconds 79, never fires. Not modelled: PWR_DBPR.DBP does not gate writes. Not modelled,
 *        logged once: binary and mixed modes, subsecond alarm comparison, smooth calibration, the
 *        shift register, WUTOCLR, ALRAFCLR, ALRBFCLR, SSRUIE, timestamps and the outputs.
 *        While the machine holds its core with DBGMCU `DBG_RTC_STOP` set, the calendar, the
 *        prescalers, the wake-up timer, the alarms and the init and shadow timings stop at the
 *        time of the hold and resume at the release. A reset that clears DBGMCU while the core
 *        stays held, a brown-out, releases the RTC, and a backup domain reset keeps it frozen
 *        while the bit is set. The bit is read at the hold: without a
 *        DBGMCU model or field the RTC runs on, a warning logged once, a model_error under
 *        strict_peripherals. `RTC_SECCFGR` and `RTC_PRIVCR` are storage: under TrustZone the
 *        first write to either logs one warning, the security and privilege they select are not
 *        modelled. Under TrustZone a security bit set in `RTC_SECCFGR` is a secure function of
 *        the RTC, RM0456 11.5.1: it makes `RCC_BDCR.BDRST` and the RTC's RCC bits Secure.
 */
class rtc final : public dispatch_model<rtc, rtc_state>,
                  public clock_listener,
                  public backup_domain,
                  public core_hold_listener,
                  public trustzone_aware
{
public:
  rtc(model_context&                          ctx,
      peripheral_desc const&                  desc,
      std::optional<std::chrono::sys_seconds> initial,
      rtc_variant const&                      variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  void on_clock_change() override;
  void backup_reset() override;
  void on_core_hold(bool held) override;

  void               time_written(std::uint32_t old, std::uint32_t now);
  void               date_written(std::uint32_t old, std::uint32_t now);
  void               status_written(std::uint32_t old, std::uint32_t now);
  void               prescaler_written(std::uint32_t old, std::uint32_t now);
  void               wakeup_written(std::uint32_t old, std::uint32_t now);
  void               control_written(std::uint32_t old, std::uint32_t now);
  void               key_written(std::uint32_t old, std::uint32_t now);
  void               calibration_written(std::uint32_t old, std::uint32_t now);
  void               shift_written(std::uint32_t old, std::uint32_t now);
  void               alarm_written(std::uint32_t old, std::uint32_t now);
  void               clear_written(std::uint32_t old, std::uint32_t now);
  void               plain_written(std::uint32_t old, std::uint32_t now);
  void               security_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;

private:
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto unlocked() const -> bool;
  [[nodiscard]] auto initializing() const -> bool;
  [[nodiscard]] auto next_alarm(std::size_t which, std::uint64_t after) const
      -> std::optional<std::uint64_t>;
  [[nodiscard]] auto wakeup_divider() const -> std::uint64_t;
  [[nodiscard]] auto wakeup_on_spre() const -> bool;

  void clear_backup(register_file& regs, bool board_calendar);
  void restart_timers(std::uint64_t cycle);
  void arm(event_kind kind, std::uint64_t cycle);
  void plan_wakeup();
  void plan_alarm(std::size_t which);
  void schedule(event_kind kind);
  void schedule_all();
  void settle(std::uint64_t cycle);
  void warn_once(std::size_t index, std::string_view reg, std::string_view message);
  void unsupported();

  std::optional<std::chrono::sys_seconds>      initial_;
  register_file&                               regs_;
  reg_ref                                      time_;
  reg_ref                                      date_;
  reg_ref                                      subsecond_;
  reg_ref                                      status_;
  reg_ref                                      masked_;
  reg_ref                                      clear_;
  reg_ref                                      key_;
  reg_ref                                      shift_;
  field_ref                                    init_;
  field_ref                                    init_flag_;
  field_ref                                    init_status_;
  field_ref                                    shadow_flag_;
  field_ref                                    wakeup_flag_write_;
  field_ref                                    shift_pending_;
  field_ref                                    binary_;
  field_ref                                    binary_units_;
  field_ref                                    prediv_a_;
  field_ref                                    prediv_s_;
  field_ref                                    wakeup_reload_;
  field_ref                                    wakeup_clear_;
  field_ref                                    wakeup_clock_;
  field_ref                                    wakeup_enable_;
  field_ref                                    wakeup_interrupt_;
  field_ref                                    wakeup_flag_;
  field_ref                                    bypass_;
  field_ref                                    format_;
  field_ref                                    add_hour_;
  field_ref                                    sub_hour_;
  field_ref                                    reference_;
  std::array<alarm_refs, 2>                    alarms_{};
  std::vector<field_ref>                       calibration_;
  std::vector<field_ref>                       outputs_;
  std::vector<std::pair<field_ref, field_ref>> clears_;
  std::vector<std::pair<field_ref, field_ref>> masks_;
  rtc_calendar::counter<rtc_state>             count_;
  core::clock_domain                           domain_;
  irq_line&                                    line_;
  std::array<bool, 6>                          logged_{};
  bool                                         freeze_logged_ = false;
  bool                                         security_logged_{};
  reg_ref                                      security_;
  std::uint32_t                                security_bits_{};
};

constexpr std::array<write_entry<rtc>, 23> rtc_writes{
    {
     {"TR", &rtc::time_written},          {"DR", &rtc::date_written},
     {"SSR", &rtc::plain_written},        {"ICSR", &rtc::status_written},
     {"PRER", &rtc::prescaler_written},   {"WUTR", &rtc::wakeup_written},
     {"CR", &rtc::control_written},       {"WPR", &rtc::key_written},
     {"CALR", &rtc::calibration_written}, {"SHIFTR", &rtc::shift_written},
     {"ALRMAR", &rtc::alarm_written},     {"ALRMASSR", &rtc::alarm_written},
     {"ALRMBR", &rtc::alarm_written},     {"ALRMBSSR", &rtc::alarm_written},
     {"SR", &rtc::plain_written},         {"MISR", &rtc::plain_written},
     {"SCR", &rtc::clear_written},        {"SECCFGR", &rtc::security_written},
     {"ALRABINR", &rtc::plain_written},   {"ALRBBINR", &rtc::plain_written},
     {"TSTR", &rtc::plain_written},       {"TSDR", &rtc::plain_written},
     {"TSSSR", &rtc::plain_written},
     }
};

rtc::rtc(
    model_context&                          ctx,
    peripheral_desc const&                  desc,
    std::optional<std::chrono::sys_seconds> initial,
    rtc_variant const&                      variant)
    : dispatch_model{ctx, desc, "RTC", rtc_writes}, initial_{initial}, regs_{ctx.regs},
      time_{reg("TR")}, date_{reg("DR")}, subsecond_{reg("SSR")}, status_{reg("SR")},
      masked_{reg("MISR")}, clear_{reg("SCR")}, key_{reg("WPR")}, shift_{reg("SHIFTR")},
      init_{field("ICSR", "INIT")}, init_flag_{field("ICSR", "INITF")},
      init_status_{field("ICSR", "INITS")}, shadow_flag_{field("ICSR", "RSF")},
      wakeup_flag_write_{field("ICSR", "WUTWF")}, shift_pending_{field("ICSR", "SHPF")},
      binary_{field("ICSR", "BIN")}, binary_units_{field("ICSR", "BCDU")},
      prediv_a_{field("PRER", "PREDIV_A")}, prediv_s_{field("PRER", "PREDIV_S")},
      wakeup_reload_{field("WUTR", "WUT")}, wakeup_clear_{field("WUTR", "WUTOCLR")},
      wakeup_clock_{field("CR", "WUCKSEL")}, wakeup_enable_{field("CR", "WUTE")},
      wakeup_interrupt_{field("CR", "WUTIE")}, wakeup_flag_{field("SR", "WUTF")},
      bypass_{field("CR", "BYPSHAD")}, format_{field("CR", "FMT")}, add_hour_{field("CR", "ADD1H")},
      sub_hour_{field("CR", "SUB1H")}, reference_{field("CR", "REFCKON")},
      count_{
          state_, ctx.regs, [this](std::string_view r, std::string_view f) { return field(r, f); }},
      domain_{ctx.clock.domain("rtc")}, line_{irq("RTC_IRQn")}, security_{reg("SECCFGR")}
{
  bind(std::array<write_entry<rtc>, 1>{{{variant.privilege_register, &rtc::security_written}}});
  for(auto const& bits : describe("SECCFGR").fields)
  {
    security_bits_ |= field("SECCFGR", bits.name).mask();
  }
  constexpr std::array<std::string_view, 2> interrupts{"ALRAIE", "ALRBIE"};
  for(std::size_t which = 0; which < alarms_.size(); ++which)
  {
    auto const& names = alarm_names[which];
    auto&       a     = alarms_[which];
    a.enable          = field("CR", names.enable);
    a.flag            = field("SR", names.flag);
    a.interrupt       = field("CR", interrupts[which]);
    a.reg             = reg(names.reg);
    a.subsecond_mask  = field(names.subseconds, "MASKSS");
    a.date_mask       = field(names.reg, "MSK4");
    a.weekday_select  = field(names.reg, "WDSEL");
    a.date_tens       = field(names.reg, "DT");
    a.date_units      = field(names.reg, "DU");
    a.hour_mask       = field(names.reg, "MSK3");
    a.pm              = field(names.reg, "PM");
    a.hour_tens       = field(names.reg, "HT");
    a.hour_units      = field(names.reg, "HU");
    a.minute_mask     = field(names.reg, "MSK2");
    a.minute_tens     = field(names.reg, "MNT");
    a.minute_units    = field(names.reg, "MNU");
    a.second_mask     = field(names.reg, "MSK1");
    a.second_tens     = field(names.reg, "ST");
    a.second_units    = field(names.reg, "SU");
  }
  for(auto const& name : {"CALP", "CALW8", "CALW16", "LPCAL", "CALM"})
  {
    calibration_.push_back(field("CALR", name));
  }
  for(auto const& name :
      {"TSE", "ITSE", "COE", "OSEL", "TAMPTS", "TAMPOE", "OUT2EN", "ALRAFCLR", "ALRBFCLR",
       "SSRUIE"})
  {
    outputs_.push_back(field("CR", name));
  }
  auto const& flags = describe("SR").fields;
  for(auto const& clear : describe("SCR").fields)
  {
    auto const target = std::string_view{clear.name}.substr(1);
    if(clear.name.starts_with("C") and std::ranges::contains(flags, target, &field_desc::name))
    {
      clears_.emplace_back(field("SCR", clear.name), field("SR", target));
    }
  }
  for(auto const& flag : flags)
  {
    std::string_view const name{flag.name};
    auto const             base   = name.substr(0, name.size() - 1);
    auto const             enable = std::format("{}IE", base);
    auto const             masked = std::format("{}MF", base);
    if(std::ranges::contains(describe("CR").fields, enable, &field_desc::name)
       and std::ranges::contains(describe("MISR").fields, masked, &field_desc::name))
    {
      masks_.emplace_back(field("CR", enable), field("SR", name));
    }
  }
}

auto rtc::now_ps() const -> std::uint64_t
{
  return state_.held != 0 ? state_.held_ps : ctx_.clock.since_power_on().ps;
}

auto rtc::unlocked() const -> bool
{
  return state_.unlock == 2;
}

auto rtc::initializing() const -> bool
{
  return get(regs_, init_flag_) != 0;
}

auto rtc::next_alarm(std::size_t which, std::uint64_t after) const -> std::optional<std::uint64_t>
{
  auto const& a       = alarms_[which];
  auto const  word    = regs_.peek(a.reg.offset);
  auto const  any_sec = a.second_mask.of(word) != 0;
  auto const  any_min = a.minute_mask.of(word) != 0;
  auto const  any_hr  = a.hour_mask.of(word) != 0;
  auto const  any_day = a.date_mask.of(word) != 0;
  auto const  second  = bcd(a.second_tens.of(word), a.second_units.of(word));
  auto const  minute  = bcd(a.minute_tens.of(word), a.minute_units.of(word));
  auto const hour = count_.hour_of(a.hour_tens.of(word), a.hour_units.of(word), a.pm.of(word) != 0);
  auto const date = bcd(a.date_tens.of(word), a.date_units.of(word));
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
      auto const t = count_.calendar(day * seconds_per_day);
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

auto rtc::wakeup_divider() const -> std::uint64_t
{
  return std::uint64_t{16} >> (get(regs_, wakeup_clock_) & 3u);
}

auto rtc::wakeup_on_spre() const -> bool
{
  return (get(regs_, wakeup_clock_) & 4u) != 0;
}

void rtc::arm(event_kind kind, std::uint64_t cycle)
{
  state_.armed[kind] = 1;
  state_.due[kind]   = cycle;
  schedule(kind);
}

void rtc::plan_wakeup()
{
  state_.armed[wakeup] = 0;
  ++state_.generation[wakeup];
  if(get(regs_, wakeup_enable_) == 0)
  {
    return;
  }
  auto const reload = std::uint64_t{get(regs_, wakeup_reload_)} + 1;
  if(wakeup_on_spre())
  {
    if(state_.counting == 0)
    {
      return;
    }
    auto const period = reload + ((get(regs_, wakeup_clock_) & 2u) != 0 ? 0x1'0000 : 0);
    auto const second = state_.wakeup_base + (state_.wakeup_fired + 1) * period;
    arm(wakeup, count_.cycle_of(second * count_.sync_ticks()));
    return;
  }
  arm(wakeup, state_.wakeup_base + (state_.wakeup_fired + 1) * reload * wakeup_divider());
}

void rtc::plan_alarm(std::size_t which)
{
  auto const kind    = static_cast<event_kind>(alarm_a + which);
  state_.armed[kind] = 0;
  ++state_.generation[kind];
  if(get(regs_, alarms_[which].enable) == 0 or state_.counting == 0)
  {
    return;
  }
  auto const now_second = count_.position_at(count_.cycle_at(now_ps())) / count_.sync_ticks();
  if(auto const next = next_alarm(which, now_second))
  {
    arm(kind, count_.cycle_of(*next * count_.sync_ticks()));
  }
}

void rtc::schedule(event_kind kind)
{
  auto const generation = ++state_.generation[kind];
  if(state_.armed[kind] == 0 or state_.hz == 0 or state_.held != 0)
  {
    return;
  }
  auto const now    = now_ps();
  auto const offset = now - ctx_.clock.time().ps;
  auto const due    = std::max(count_.time_of(state_.due[kind]), now) - offset;
  ctx_.events.schedule_at(vtime{due}, kind, generation, ctx_.clock);
}

void rtc::schedule_all()
{
  for(std::size_t kind = 0; kind < event_kinds; ++kind)
  {
    schedule(static_cast<event_kind>(kind));
  }
}

void rtc::settle(std::uint64_t cycle)
{
  auto const position = count_.position_at(cycle);
  regs_.poke(time_.offset, count_.time_register(position));
  regs_.poke(date_.offset, count_.date_register(position));
  regs_.poke(subsecond_.offset, count_.subsecond_register(position));
  update(regs_, init_status_, count_.calendar(position / count_.sync_ticks()).year != 0 ? 1 : 0);
  auto masked = std::uint32_t{};
  auto level  = false;
  for(auto const& [enable, flag] : masks_)
  {
    if(get(regs_, enable) != 0 and get(regs_, flag) != 0)
    {
      masked |= flag.mask();
      level = true;
    }
  }
  regs_.poke(masked_.offset, masked);
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void rtc::warn_once(std::size_t index, std::string_view reg, std::string_view message)
{
  if(not std::exchange(logged_[index], true))
  {
    warn(reg, std::string{message});
  }
}

void rtc::unsupported()
{
  if(get(regs_, binary_) != 0)
  {
    warn_once(
        0, "ICSR", "binary and mixed modes are not modelled, the calendar counts in BCD mode");
  }
  if(std::ranges::any_of(calibration_, [&](field_ref bits) { return get(regs_, bits) != 0; }))
  {
    warn_once(1, "CALR", "smooth calibration is not modelled");
  }
  if(std::ranges::any_of(outputs_, [&](field_ref bits) { return get(regs_, bits) != 0; }))
  {
    warn_once(2, "CR", "timestamps, the RTC outputs, ALRxFCLR and SSRUIE are not modelled");
  }
  if(get(regs_, wakeup_clear_) != 0)
  {
    warn_once(3, "WUTR", "WUTOCLR is not modelled, WUTF stays set until SCR clears it");
  }
  for(auto const& a : alarms_)
  {
    if(get(regs_, a.subsecond_mask) != 0)
    {
      warn_once(4, "ALRMASSR", "subsecond alarm comparison is not modelled, MASKSS reads as 0");
    }
  }
}

void rtc::clear_backup(register_file& regs, bool board_calendar)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto generation = state_.generation;
  state_          = {};
  state_.prediv_a = get(regs, prediv_a_);
  state_.prediv_s = get(regs, prediv_s_);
  if(board_calendar)
  {
    count_.load(initial_);
  }
  state_.counting = 1;
  for(auto& value : generation)
  {
    ++value;
  }
  state_.generation = generation;
}

void rtc::restart_timers(std::uint64_t cycle)
{
  state_.armed[shadow_ready] = 1;
  state_.due[shadow_ready]   = cycle + 1;
  plan_wakeup();
  plan_alarm(0);
  plan_alarm(1);
  schedule_all();
  settle(cycle);
}

void rtc::reset(register_file& regs, reset_kind kind)
{
  if(kind == reset_kind::power_on)
  {
    clear_backup(regs, true);
    restart_timers(state_.cycle0);
    return;
  }
  auto const ps = now_ps();
  if(auto const hz = ctx_.clock.hz(domain_); hz != state_.hz)
  {
    count_.rebase(ps, hz);
  }
  auto const cycle = count_.cycle_at(ps);
  if(state_.counting == 0)
  {
    state_.counting = 1;
    state_.start    = cycle;
  }
  auto icsr = regs.peek(init_.offset);
  icsr      = init_.with(icsr, 0);
  icsr      = init_flag_.with(icsr, 0);
  icsr      = shadow_flag_.with(icsr, 0);
  regs.poke(init_.offset, icsr);
  state_.armed[init_ready] = 0;
  state_.latched_valid     = 0;
  for(auto& value : state_.generation)
  {
    ++value;
  }
  restart_timers(cycle);
}

void rtc::backup_reset()
{
  auto const held_ps = state_.held_ps;
  auto const frozen  = state_.held != 0 and debug_frozen(freeze_logged_);
  clear_backup(regs_, false);
  state_.hz = ctx_.clock.hz(domain_);
  if(frozen)
  {
    state_.held    = 1;
    state_.held_ps = held_ps;
  }
  state_.anchor_ps = now_ps();
  restart_timers(state_.cycle0);
}

auto rtc::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const cycle = count_.cycle_at(now_ps());
  settle(cycle);
  auto const word     = offset & ~3u;
  auto const calendar = word == time_.offset or word == date_.offset or word == subsecond_.offset;
  if(not calendar)
  {
    return dispatch_model::on_read(regs, offset, size, access);
  }
  auto position = count_.position_at(cycle);
  if(access.by != initiator::debugger and get(regs_, bypass_) == 0)
  {
    if(word != date_.offset and state_.latched_valid == 0)
    {
      state_.latched       = position;
      state_.latched_valid = 1;
    }
    if(state_.latched_valid != 0)
    {
      position = state_.latched;
    }
    if(word == date_.offset)
    {
      state_.latched_valid = 0;
    }
  }
  auto const value = word == time_.offset ? count_.time_register(position)
                   : word == date_.offset ? count_.date_register(position)
                                          : count_.subsecond_register(position);
  return read_lanes(value, offset, size);
}

auto rtc::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  auto const cycle = count_.cycle_at(now_ps());
  settle(cycle);
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  unsupported();
  settle(count_.cycle_at(now_ps()));
  return result;
}

void rtc::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= event_kinds or payload != state_.generation[kind] or state_.armed[kind] == 0)
  {
    return;
  }
  auto const cycle   = count_.cycle_at(now_ps());
  state_.armed[kind] = 0;
  switch(static_cast<event_kind>(kind))
  {
    case init_ready:
      if(get(regs_, init_) != 0)
      {
        update(regs_, init_flag_, 1);
      }
      break;
    case shadow_ready:
      update(regs_, shadow_flag_, 1);
      break;
    case wakeup_ready:
      update(regs_, wakeup_flag_write_, get(regs_, wakeup_enable_) != 0 ? 0 : 1);
      break;
    case wakeup:
      update(regs_, wakeup_flag_, 1);
      ++state_.wakeup_fired;
      plan_wakeup();
      break;
    case alarm_a:
    case alarm_b:
    {
      auto const which = static_cast<std::size_t>(kind - alarm_a);
      update(regs_, alarms_[which].flag, 1);
      plan_alarm(which);
      break;
    }
    case event_kinds:
      break;
  }
  settle(cycle);
}

void rtc::on_core_hold(bool held)
{
  if(held)
  {
    auto const frozen = debug_frozen(freeze_logged_);
    if(state_.held != 0)
    {
      if(not frozen)
      {
        on_core_hold(false);
      }
      return;
    }
    if(not frozen)
    {
      return;
    }
    auto const cycle = count_.cycle_at(now_ps());
    for(auto fired = true; fired;)
    {
      fired = false;
      for(std::uint32_t kind = 0; kind < event_kinds; ++kind)
      {
        if(state_.armed[kind] != 0 and state_.due[kind] <= cycle)
        {
          on_event(kind, state_.generation[kind]);
          fired = true;
        }
      }
    }
    settle(cycle);
    state_.held_ps = ctx_.clock.since_power_on().ps;
    state_.held    = 1;
    for(auto& value : state_.generation)
    {
      ++value;
    }
    return;
  }
  if(state_.held == 0)
  {
    return;
  }
  state_.anchor_ps += ctx_.clock.since_power_on().ps - state_.held_ps;
  state_.held    = 0;
  state_.held_ps = 0;
  schedule_all();
  settle(count_.cycle_at(now_ps()));
}

void rtc::on_clock_change()
{
  auto const hz = ctx_.clock.hz(domain_);
  if(hz == state_.hz)
  {
    return;
  }
  auto const ps = now_ps();
  count_.rebase(ps, hz);
  schedule_all();
  settle(count_.cycle_at(ps));
}

void rtc::time_written(std::uint32_t old, std::uint32_t now)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(time_.offset, old);
    return;
  }
  count_.set_time(now);
}

void rtc::date_written(std::uint32_t old, std::uint32_t now)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(date_.offset, old);
    return;
  }
  count_.set_date(now);
}

void rtc::status_written(std::uint32_t old, std::uint32_t now)
{
  auto word = old;
  if(unlocked())
  {
    for(auto const bits : {init_, binary_, binary_units_})
    {
      word = bits.with(word, bits.of(now));
    }
  }
  if(shadow_flag_.of(now) == 0)
  {
    word = shadow_flag_.with(word, 0);
  }
  regs_.poke(init_.offset, word);
  auto const cycle = count_.cycle_at(now_ps());
  if(shadow_flag_.of(old) != 0 and shadow_flag_.of(word) == 0)
  {
    arm(shadow_ready, std::max(cycle, state_.counting != 0 ? state_.start : cycle) + 1);
  }
  if(init_.of(old) == 0 and init_.of(word) != 0)
  {
    count_.freeze(cycle);
    arm(init_ready, cycle + init_cycles);
    plan_wakeup();
    plan_alarm(0);
    plan_alarm(1);
  }
  else if(init_.of(old) != 0 and init_.of(word) == 0)
  {
    update(regs_, init_flag_, 0);
    state_.armed[init_ready] = 0;
    state_.phase             = 0;
    state_.prediv_a          = get(regs_, prediv_a_);
    state_.prediv_s          = get(regs_, prediv_s_);
    state_.counting          = 1;
    state_.start             = cycle + exit_cycles;
    state_.latched_valid     = 0;
    update(regs_, shadow_flag_, 0);
    arm(shadow_ready, state_.start + 1);
    if(wakeup_on_spre())
    {
      state_.wakeup_base  = state_.seconds;
      state_.wakeup_fired = 0;
    }
    plan_wakeup();
    plan_alarm(0);
    plan_alarm(1);
  }
}

void rtc::prescaler_written(std::uint32_t old, std::uint32_t)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(prediv_a_.offset, old);
  }
}

void rtc::wakeup_written(std::uint32_t old, std::uint32_t)
{
  if(get(regs_, wakeup_flag_write_) == 0)
  {
    regs_.poke(wakeup_reload_.offset, old);
  }
}

void rtc::control_written(std::uint32_t old, std::uint32_t now)
{
  auto word = now;
  if(not unlocked())
  {
    for(auto const bits : {format_, reference_})
    {
      word = bits.with(word, bits.of(old));
    }
  }
  if(get(regs_, wakeup_flag_write_) == 0)
  {
    word = wakeup_clock_.with(word, wakeup_clock_.of(old));
  }
  word = add_hour_.with(word, 0);
  word = sub_hour_.with(word, 0);
  regs_.poke(format_.offset, word);
  auto const cycle = count_.cycle_at(now_ps());
  if(unlocked())
  {
    count_.shift_hour(cycle, add_hour_.of(now) != 0, sub_hour_.of(now) != 0);
  }
  auto const was_on = wakeup_enable_.of(old) != 0;
  auto const is_on  = wakeup_enable_.of(word) != 0;
  if(was_on != is_on)
  {
    auto const settle_cycles =
        wakeup_on_spre() ? std::uint64_t{state_.prediv_a} + 1 : wakeup_divider();
    arm(wakeup_ready, cycle + settle_cycles + 1);
    if(is_on)
    {
      state_.wakeup_base =
          wakeup_on_spre() ? count_.position_at(cycle) / count_.sync_ticks() : cycle;
      state_.wakeup_fired = 0;
    }
  }
  plan_wakeup();
  plan_alarm(0);
  plan_alarm(1);
}

void rtc::key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(key_.offset, 0);
  auto const key = now & 0xFFu;
  if(key == first_key)
  {
    state_.unlock = 1;
  }
  else if(key == second_key and state_.unlock == 1)
  {
    state_.unlock = 2;
  }
  else
  {
    state_.unlock = 0;
  }
}

void rtc::calibration_written(std::uint32_t old, std::uint32_t)
{
  if(not unlocked())
  {
    regs_.poke(calibration_.front().offset, old);
  }
}

void rtc::shift_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(shift_.offset, 0);
  if(unlocked() and now != 0)
  {
    warn_once(5, "SHIFTR", "the shift register is not modelled, the calendar is not shifted");
  }
}

void rtc::alarm_written(std::uint32_t, std::uint32_t)
{
  plan_alarm(0);
  plan_alarm(1);
}

void rtc::clear_written(std::uint32_t, std::uint32_t now)
{
  for(auto const& [clear, flag] : clears_)
  {
    if(clear.of(now) != 0)
    {
      update(regs_, flag, 0);
    }
  }
  regs_.poke(clear_.offset, 0);
}

void rtc::plain_written(std::uint32_t, std::uint32_t)
{}

auto rtc::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone ? regs_.peek(security_.offset) & security_bits_ : 0;
}

auto rtc::secure_function() const -> bool
{
  return secure_word() != 0;
}

void rtc::security_written(std::uint32_t, std::uint32_t)
{
  if(ctx_.trustzone and not std::exchange(security_logged_, true))
  {
    warn(
        "RTC_SECCFGR", "RTC_SECCFGR and RTC_PRIVCR are storage: the RTC security and privilege "
                       "they select are not modelled");
  }
}

/**
 * @brief TAMP of the STM32U5, RM0456 section 64 "Tamper and backup registers (TAMP)", as backup
 *        domain storage: every register keeps its value on every reset but power-on and a backup
 *        domain reset, the backup registers TAMP_BKP0R to TAMP_BKP31R included. Tamper
 *        detection and the monotonic counter are not modelled, their registers are plain storage.
 *        `TAMP_SECCFGR` and `TAMP_PRIVCR` are storage: under TrustZone the first write to either
 *        logs one warning, the security and privilege they select are not modelled. Under
 *        TrustZone TAMPSEC, CNT1SEC, a BKPRWSEC above 0 or a BKPWSEC above BKPRWSEC, a Secure
 *        backup register zone, is a secure function of the TAMP, RM0456 11.5.1 and 64.7.9: it
 *        makes `RCC_BDCR.BDRST` Secure.
 */
struct tamp_state
{
  std::uint8_t unused{};

  friend auto operator==(tamp_state const&, tamp_state const&) -> bool = default;
};

class tamp final : public dispatch_model<tamp, tamp_state>,
                   public backup_domain,
                   public trustzone_aware
{
public:
  tamp(model_context& ctx, peripheral_desc const& desc, rtc_variant const& variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void backup_reset() override;

  void               plain_written(std::uint32_t old, std::uint32_t now);
  void               security_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;

private:
  void clear(register_file& regs);

  bool      security_logged_{};
  reg_ref   security_;
  field_ref tamper_;
  field_ref counter_;
  field_ref read_write_;
  field_ref write_;
};

auto tamp_writes(peripheral_desc const& desc, rtc_variant const& variant)
    -> std::vector<write_entry<tamp>>
{
  std::vector<write_entry<tamp>> out;
  for(auto const& described : desc.registers)
  {
    auto const security =
        described.name == "SECCFGR" or described.name == variant.privilege_register;
    out.push_back({described.name, security ? &tamp::security_written : &tamp::plain_written});
  }
  return out;
}

tamp::tamp(model_context& ctx, peripheral_desc const& desc, rtc_variant const& variant)
    : dispatch_model{ctx, desc, "TAMP", tamp_writes(desc, variant)}, security_{reg("SECCFGR")},
      tamper_{field("SECCFGR", "TAMPSEC")}, counter_{field("SECCFGR", "CNT1SEC")},
      read_write_{field("SECCFGR", "BKPRWSEC")}, write_{field("SECCFGR", "BKPWSEC")}
{
  expect_register(variant.privilege_register, true);
}

auto tamp::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone ? ctx_.regs.peek(security_.offset) : 0;
}

auto tamp::secure_function() const -> bool
{
  auto const word = secure_word();
  if(word == 0)
  {
    return false;
  }
  return tamper_.of(word) != 0
      or counter_.of(word) != 0
      or read_write_.of(word) != 0
      or write_.of(word) > read_write_.of(word);
}

void tamp::clear(register_file& regs)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

void tamp::reset(register_file& regs, reset_kind kind)
{
  if(kind == reset_kind::power_on)
  {
    clear(regs);
  }
}

void tamp::backup_reset()
{
  clear(ctx_.regs);
}

void tamp::on_event(std::uint32_t, std::uint64_t)
{}

void tamp::plain_written(std::uint32_t, std::uint32_t)
{}

void tamp::security_written(std::uint32_t, std::uint32_t)
{
  if(ctx_.trustzone and not std::exchange(security_logged_, true))
  {
    warn(
        "TAMP_SECCFGR", "TAMP_SECCFGR and TAMP_PRIVCR are storage: the TAMP security and privilege "
                        "they select are not modelled");
  }
}
}

auto rtc_factory(std::optional<std::chrono::sys_seconds> initial, rtc_variant variant)
    -> model_factory
{
  return [initial,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rtc>(ctx, desc, initial, variant);
  };
}

auto tamp_factory(rtc_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<tamp>(ctx, desc, variant);
  };
}
}
