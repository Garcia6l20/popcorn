#include "backup_domain.hpp"
#include "exti_classic.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "rtc_calendar.hpp"
#include "rtc_core.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/core_hold.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
using rtc_calendar::exit_cycles;
using rtc_calendar::first_key;
using rtc_calendar::init_cycles;
using rtc_calendar::second_key;

constexpr std::uint64_t sync_cycles = 2;

enum classic_event : std::uint8_t
{
  init_ready,
  shadow_ready,
  wakeup_ready,
  wakeup,
  alarm_a,
  alarm_b,
  alarm_a_write,
  alarm_b_write,
  classic_events,
};

struct rtc_classic_state
{
  std::uint64_t                             anchor_ps{};
  std::uint64_t                             anchor_frac{};
  std::uint64_t                             hz{};
  std::uint64_t                             cycle0{};
  std::uint64_t                             seconds{};
  std::uint64_t                             phase{};
  std::uint64_t                             start{};
  std::uint32_t                             prediv_a{127};
  std::uint32_t                             prediv_s{255};
  std::uint8_t                              counting{};
  std::uint8_t                              weekday_base{};
  std::uint8_t                              unlock{};
  std::uint8_t                              latched_valid{};
  std::uint64_t                             latched{};
  std::uint8_t                              shadow_reset{};
  std::uint64_t                             wakeup_base{};
  std::uint64_t                             wakeup_fired{};
  std::uint8_t                              lines{};
  std::array<std::uint64_t, classic_events> due{};
  std::array<std::uint8_t, classic_events>  armed{};
  std::array<std::uint32_t, classic_events> generation{};
  std::uint8_t                              held{};
  std::uint64_t                             held_ps{};

  friend auto operator==(rtc_classic_state const&, rtc_classic_state const&) -> bool = default;
};

constexpr std::array<std::string_view, 5> locked_names{
    "WUTR", "ALRMAR", "ALRMBR", "ALRMASSR", "ALRMBSSR"};
constexpr std::array<std::string_view, 4> read_only_names{"SSR", "TSTR", "TSDR", "TSSSR"};

enum unmodelled_function : std::uint8_t
{
  timestamp_function,
  tamper_function,
  coarse_function,
  smooth_function,
  shift_function,
  output_function,
  reference_function,
  unmodelled_functions,
};

struct alarm_names
{
  std::string_view reg;
  std::string_view subseconds;
  std::string_view enable;
  std::string_view interrupt;
  std::string_view flag;
  std::string_view write;
};

constexpr std::array<alarm_names, 2> classic_alarms{
    {
     {"ALRMAR", "ALRMASSR", "ALRAE", "ALRAIE", "ALRAF", "ALRAWF"},
     {"ALRMBR", "ALRMBSSR", "ALRBE", "ALRBIE", "ALRBF", "ALRBWF"},
     }
};

struct classic_alarm
{
  reg_ref                   reg;
  std::optional<reg_ref>    subseconds;
  field_ref                 enable;
  field_ref                 interrupt;
  field_ref                 flag;
  field_ref                 write;
  field_ref                 mask_ss;
  field_ref                 ss;
  rtc_calendar::alarm_match match;
};

struct unmodelled_entry
{
  std::string_view reg;
  std::string_view what;
  std::uint32_t    mask{};
};

/**
 * @brief Classic RTC of the STM32L1, RM0038 section 20, and the STM32L4, RM0351 section 38, BCD
 *        calendar, described for the STM32L1 first. The calendar is derived from
 *        the `rtc` clock domain and the virtual time of its last start, never ticked: the
 *        asynchronous and synchronous prescalers, SSR on Cat.2 to Cat.6, TR and DR in 24 or 12-hour
 *        format, days in month and leap years for 2000 to 2099, the weekday counted from the one
 *        the firmware wrote. Init mode with INITF at the second RTCCLK edge after INIT is set and
 *        the count restarting at the fourth edge after INIT clears, RM0038 20.3.5, the upper bounds
 *        of its 1 to 2 cycles and its 4 cycles. The 0xCA 0x53 key sequence of RTC_WPR unlocks every
 *        register but RTC_ISR[13:8], RTC_TAFCR and the backup registers, a wrong key locks them
 *        again, and a system reset keeps the lock state. CR.FMT, CR.DCE, CR.REFCKON, RTC_CALIBR,
 *        TR, DR and PRER take writes in init mode only. RTC_ISR: INIT and RSF under the key, ALRAF,
 *        ALRBF, WUTF, TSF, TSOVF and TAMP1F cleared by a 0 whatever the key, TAMP2F and TAMP3F
 *        cleared by a 0 under the key, RM0038 20.3.5 and the note under 20.6.4, which exempt bits
 *        13:8 only. Shadow registers: an SSR or TR read latches TR and DR until DR is read, RSF
 *        sets at the second RTCCLK edge after a clear, the end of init mode, a BYPSHAD clear or a
 *        system reset, inferred from the copy every two RTCCLK periods of RM0038 20.3.2, and is
 *        cleared in init mode and while CR.BYPSHAD is set, RM0038 20.6.4. A system reset clears
 *        INIT, INITF and RSF, and with BYPSHAD 0 TR, DR and SSR read their reset values until the
 *        next copy sets RSF, RM0038 20.6.1, 20.6.2 and 20.6.11. ADD1H and SUB1H apply at the write.
 *        The backup registers, as many as the description lists, keep their value on a system reset
 *        and take every write, a debugger write included, with no key. A power-on reset, a brown-
 *        out on a part whose variant says so and RCC_CSR.RTCRST bring every register and the
 *        calendar to their reset values, the board's `rtc_initial` loaded at power-on only. While
 *        the machine holds its core with DBGMCU `DBG_RTC_STOP` set, the calendar and the init and
 *        shadow timings stop at the time of the hold and resume at the release, on a variant whose
 *        DBGMCU has the bit. Alarms A and B on seconds, minutes, hours and date or weekday with
 *        their masks, and on Cat.2 to Cat.6 on SS[MASKSS-1:0] inside a matching second, RM0038
 *        20.3.3 and 20.6.18. ALRMxR takes writes with ALRxWF set or in init mode, ALRMxSSR with
 *        ALRxE clear or in init mode. On Cat.1 ALRxWF clears when ALRxE sets and in init mode, sets
 *        at the second RTCCLK edge after ALRxE clears and at the end of init mode with ALRxE clear,
 *        inferred from RM0038 20.3.5, which keeps them set on the other categories. The wakeup
 *        timer on RTCCLK / 16 to / 2 or on ck_spre, with 2^16 added for WUCKSEL 11x, counting from
 *        the write that sets WUTE, inferred, WUTWF clearing and setting at the second RTCCLK edge
 *        after WUTE changes, WUTR and WUCKSEL taking writes only with WUTE clear and WUTWF set,
 *        RM0038 20.3.4 and 20.6.4. A system reset keeps the alarms, the wakeup timer and, inferred
 *        from the bit text of RM0038 20.6.4, a pending set of WUTWF or ALRxWF. A system reset in
 *        init mode restarts the calendar as the end of init mode does, inferred: on Cat.1 ALRxWF
 *        reads 1 for an alarm with ALRxE clear, and a ck_spre wakeup counts its period from the
 *        restart. The alarm interrupt, ALRAF and ALRAIE or ALRBF and ALRBIE, drives the alarm EXTI
 *        line of the variant, WUTF and WUTIE the wakeup line, each change of the level an edge for
 *        the EXTI, RM0038 20.5, the flag gated by its interrupt enable inferred from Table 90.
 *        Logged once, a model_error under strict_peripherals at the CPU or DMA write that selects
 *        them before the store, a debugger write stored with neither: timestamps, tamper detection,
 *        coarse and smooth calibration, the shift register, the RTC outputs and the reference clock
 *        detection.
 *        TSTR, TSDR, TSSSR, CALIBR, CALR, SHIFTR and TAFCR are storage the model claims for the key
 *        and the read-only rules, `storage_only` to coverage and host views. PWR_CR.DBP gates no
 *        write.
 *        The STM32L4 RTC runs the same rules on its own register set: RTC_TAMPCR in place of
 *        RTC_TAFCR, RTC_OR, no RTC_CALIBR and no CR.DCE, and CR.ITSE logged with the timestamps.
 *        Its key leaves RTC_ISR[31:8], RTC_TAMPCR, RTC_OR and the backup registers open, RM0351
 *        38.3.7, so TAMP2F and TAMP3F clear by a 0 without it. RSF sets at the first RTCCLK edge
 *        after a clear, the end of init mode, a BYPSHAD clear or a system reset, inferred from the
 *        copy every RTCCLK period of RM0351 38.3.4 and 38.3.8, where the BYPSHAD text of 38.6.3
 *        says every two cycles. A brown-out keeps the backup domain, RM0351 6.1.3. The alarm
 *        interrupt drives EXTI line 18, RM0351 Table 62. RTC_TAMPCR and RTC_OR are storage.
 */
class rtc_classic final : public dispatch_model<rtc_classic, rtc_classic_state>,
                          public clock_listener,
                          public backup_domain,
                          public core_hold_listener,
                          public rtc_core
{
  friend rtc_core;

  static constexpr std::uint32_t wakeup_event = wakeup;

public:
  rtc_classic(
      model_context&                          ctx,
      peripheral_desc const&                  desc,
      std::optional<std::chrono::sys_seconds> initial,
      rtc_classic_variant const&              variant);

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
  void time_written(std::uint32_t old, std::uint32_t now)
  {
    rtc_core::time_written(old, now);
  }
  void date_written(std::uint32_t old, std::uint32_t now)
  {
    rtc_core::date_written(old, now);
  }
  void prescaler_written(std::uint32_t old, std::uint32_t now)
  {
    rtc_core::prescaler_written(old, now);
  }
  void key_written(std::uint32_t old, std::uint32_t now)
  {
    rtc_core::key_written(old, now);
  }
  [[nodiscard]] auto storage_only(std::uint32_t offset) const -> bool override;

  void                              status_written(std::uint32_t old, std::uint32_t now);
  void                              control_written(std::uint32_t old, std::uint32_t now);
  void                              coarse_written(std::uint32_t old, std::uint32_t now);
  void                              smooth_written(std::uint32_t old, std::uint32_t now);
  void                              shift_written(std::uint32_t old, std::uint32_t now);
  void                              tamper_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Index> void locked_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Index> void read_only_written(std::uint32_t old, std::uint32_t now);
  void                              plain_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto bypassed() const -> bool;
  [[nodiscard]] auto keep_locked(reg_ref target, std::uint32_t old) -> bool;

  [[nodiscard]] auto next_alarm(std::size_t which, std::uint64_t after) const
      -> std::optional<std::uint64_t>;

  void clear_backup(register_file& regs, bool board_calendar);
  void restart_timers(std::uint64_t cycle);
  void plan_alarm(std::size_t which);
  void plan_all();
  void update_lines();
  void alarm_enables_changed(std::uint32_t old, std::uint32_t now, std::uint64_t cycle);
  void settle(std::uint64_t cycle);
  void refuse_unmodelled(std::string_view name, std::uint32_t old, std::uint32_t now);

  std::optional<std::chrono::sys_seconds>            initial_;
  rtc_classic_variant                                variant_;
  register_file&                                     regs_;
  reg_ref                                            time_;
  reg_ref                                            date_;
  std::optional<reg_ref>                             subsecond_;
  reg_ref                                            status_;
  reg_ref                                            control_;
  reg_ref                                            key_;
  field_ref                                          init_;
  field_ref                                          init_flag_;
  field_ref                                          init_status_;
  field_ref                                          shadow_flag_;
  std::optional<field_ref>                           bypass_;
  field_ref                                          prediv_a_;
  field_ref                                          prediv_s_;
  field_ref                                          format_;
  field_ref                                          add_hour_;
  field_ref                                          sub_hour_;
  std::uint32_t                                      init_only_{};
  std::uint32_t                                      open_flags_{};
  std::uint32_t                                      keyed_flags_{};
  std::vector<std::uint32_t>                         storage_;
  std::array<classic_alarm, 2>                       alarms_{};
  field_ref                                          wakeup_reload_;
  field_ref                                          wakeup_clock_;
  field_ref                                          wakeup_enable_;
  field_ref                                          wakeup_interrupt_;
  field_ref                                          wakeup_flag_;
  field_ref                                          wakeup_write_;
  std::array<unmodelled_entry, unmodelled_functions> unmodelled_{};
  std::array<bool, unmodelled_functions>             logged_{};
  rtc_calendar::counter<rtc_classic_state>           count_;
  core::clock_domain                                 domain_;
  bool                                               freeze_logged_  = false;
  bool                                               debugger_write_ = false;
};

constexpr std::array<write_entry<rtc_classic>, 11> classic_writes{
    {
     {"TR", &rtc_classic::time_written},
     {"DR", &rtc_classic::date_written},
     {"CR", &rtc_classic::control_written},
     {"ISR", &rtc_classic::status_written},
     {"PRER", &rtc_classic::prescaler_written},
     {"WUTR", &rtc_classic::locked_written<0>},
     {"ALRMAR", &rtc_classic::locked_written<1>},
     {"ALRMBR", &rtc_classic::locked_written<2>},
     {"WPR", &rtc_classic::key_written},
     {"TSTR", &rtc_classic::read_only_written<1>},
     {"TSDR", &rtc_classic::read_only_written<2>},
     }
};

constexpr std::array<write_entry<rtc_classic>, 5> subsecond_writes{
    {
     {"SSR", &rtc_classic::read_only_written<0>},
     {"SHIFTR", &rtc_classic::shift_written},
     {"TSSSR", &rtc_classic::read_only_written<3>},
     {"ALRMASSR", &rtc_classic::locked_written<3>},
     {"ALRMBSSR", &rtc_classic::locked_written<4>},
     }
};

constexpr std::array<write_entry<rtc_classic>, 1> smooth_writes{
    {{"CALR", &rtc_classic::smooth_written}}};

constexpr std::array<write_entry<rtc_classic>, 1> coarse_writes{
    {{"CALIBR", &rtc_classic::coarse_written}}};

constexpr std::array<write_entry<rtc_classic>, 1> option_writes{
    {{"OR", &rtc_classic::plain_written}}};

rtc_classic::rtc_classic(
    model_context&                          ctx,
    peripheral_desc const&                  desc,
    std::optional<std::chrono::sys_seconds> initial,
    rtc_classic_variant const&              variant)
    : dispatch_model{ctx, desc, "RTC", classic_writes}, initial_{initial}, variant_{variant},
      regs_{ctx.regs}, time_{reg("TR")}, date_{reg("DR")}, status_{reg("ISR")}, control_{reg("CR")},
      key_{reg("WPR")}, init_{field("ISR", "INIT")}, init_flag_{field("ISR", "INITF")},
      init_status_{field("ISR", "INITS")}, shadow_flag_{field("ISR", "RSF")},
      prediv_a_{field("PRER", "PREDIV_A")}, prediv_s_{field("PRER", "PREDIV_S")},
      format_{field("CR", "FMT")}, add_hour_{field("CR", "ADD1H")}, sub_hour_{field("CR", "SUB1H")},
      wakeup_reload_{field("WUTR", "WUT")}, wakeup_clock_{field("CR", "WCKSEL")},
      wakeup_enable_{field("CR", "WUTE")}, wakeup_interrupt_{field("CR", "WUTIE")},
      wakeup_flag_{field("ISR", "WUTF")}, wakeup_write_{field("ISR", "WUTWF")},
      count_{
          state_, ctx.regs, [this](std::string_view r, std::string_view f) { return field(r, f); }},
      domain_{ctx.clock.domain("rtc")}
{
  for(std::size_t which = 0; which < alarms_.size(); ++which)
  {
    auto const& names = classic_alarms[which];
    auto&       a     = alarms_[which];
    a.reg             = reg(names.reg);
    a.enable          = field("CR", names.enable);
    a.interrupt       = field("CR", names.interrupt);
    a.flag            = field("ISR", names.flag);
    a.write           = field("ISR", names.write);
    a.match           = rtc_calendar::alarm_match_of(
        [this](std::string_view r, std::string_view f) { return field(r, f); }, names.reg);
    if(variant.subseconds)
    {
      a.subseconds = reg(names.subseconds);
      a.mask_ss    = field(names.subseconds, "MASKSS");
      a.ss         = field(names.subseconds, "SS");
    }
  }
  for(auto const& name : {"SSR", "SHIFTR", "TSSSR", "ALRMASSR", "ALRMBSSR"})
  {
    expect_register(name, variant.subseconds);
  }
  expect_field("CR", "BYPSHAD", variant.subseconds);
  expect_field("ISR", "SHPF", variant.subseconds);
  expect_register("CALR", variant.smooth_calibration);
  expect_field("ISR", "RECALPF", variant.smooth_calibration);
  for(auto const& name : {"TAMP2F", "TAMP3F"})
  {
    expect_field("ISR", name, variant.three_tampers);
  }
  for(auto const& name : {"TAMP2E", "TAMP3E"})
  {
    expect_field(variant.tamper_register, name, variant.three_tampers);
  }
  expect_register("CALIBR", variant.coarse_calibration);
  expect_field("CR", "DCE", variant.coarse_calibration);
  expect_register("OR", variant.option_register);
  expect_field("CR", "ITSE", variant.internal_timestamp);
  if(variant.subseconds)
  {
    bind(subsecond_writes);
    subsecond_ = reg("SSR");
    bypass_    = field("CR", "BYPSHAD");
  }
  if(variant.smooth_calibration)
  {
    bind(smooth_writes);
  }
  if(variant.coarse_calibration)
  {
    bind(coarse_writes);
  }
  if(variant.option_register)
  {
    bind(option_writes);
  }
  std::vector<write_entry<rtc_classic>> named{
      {variant.tamper_register, &rtc_classic::tamper_written}
  };
  for(auto const& described : desc.registers)
  {
    if(described.name.starts_with("BKP") and described.name.ends_with("R"))
    {
      named.push_back({described.name, &rtc_classic::plain_written});
    }
  }
  bind(named);
  for(auto const& name :
      {std::string_view{"TSTR"}, std::string_view{"TSDR"}, std::string_view{"TSSSR"},
       std::string_view{"CALIBR"}, std::string_view{"CALR"}, std::string_view{"SHIFTR"},
       variant.tamper_register, std::string_view{"OR"}})
  {
    if(has_register(name))
    {
      storage_.push_back(reg(name).offset);
    }
  }
  init_only_ = field("CR", "FMT").mask() | field("CR", "REFCKON").mask();
  if(variant.coarse_calibration)
  {
    init_only_ |= field("CR", "DCE").mask();
  }
  for(auto const& name : {"ALRAF", "ALRBF", "WUTF", "TSF", "TSOVF", "TAMP1F"})
  {
    open_flags_ |= field("ISR", name).mask();
  }
  keyed_flags_ = shadow_flag_.mask();
  if(variant.three_tampers)
  {
    auto const later = field("ISR", "TAMP2F").mask() | field("ISR", "TAMP3F").mask();
    (variant.keyed_tamper_flags ? keyed_flags_ : open_flags_) |= later;
  }
  auto const bits = [&](std::string_view r, std::initializer_list<std::string_view> names) {
    auto mask = std::uint32_t{};
    for(auto const name : names)
    {
      mask |= field(r, name).mask();
    }
    return mask;
  };
  auto tampers = bits(variant.tamper_register, {"TAMP1E"});
  if(variant.three_tampers)
  {
    tampers |= bits(variant.tamper_register, {"TAMP2E", "TAMP3E"});
  }
  auto stamps = bits("CR", {"TSE"});
  if(variant.internal_timestamp)
  {
    stamps |= bits("CR", {"ITSE"});
  }
  unmodelled_[timestamp_function] = {"CR", "timestamps", stamps};
  unmodelled_[tamper_function]    = {variant.tamper_register, "tamper detection", tampers};
  unmodelled_[coarse_function]    = {
      "CR", "coarse digital calibration",
      variant.coarse_calibration ? bits("CR", {"DCE"}) : std::uint32_t{}};
  unmodelled_[smooth_function] = {"CALR", "smooth digital calibration", ~std::uint32_t{}};
  unmodelled_[shift_function]  = {"SHIFTR", "the shift control", ~std::uint32_t{}};
  unmodelled_[output_function] = {
      "CR", "the RTC_CALIB and RTC_ALARM outputs", bits("CR", {"COE", "OSEL"})};
  unmodelled_[reference_function] = {
      "CR", "the reference clock detection", bits("CR", {"REFCKON"})};
}

auto rtc_classic::bypassed() const -> bool
{
  return bypass_ and get(regs_, *bypass_) != 0;
}

auto rtc_classic::keep_locked(reg_ref target, std::uint32_t old) -> bool
{
  if(unlocked())
  {
    return false;
  }
  regs_.poke(target.offset, old);
  return true;
}

void rtc_classic::settle(std::uint64_t cycle)
{
  auto const position = count_.position_at(cycle);
  regs_.poke(time_.offset, count_.time_register(position));
  regs_.poke(date_.offset, count_.date_register(position));
  if(subsecond_)
  {
    regs_.poke(subsecond_->offset, count_.subsecond_register(position));
  }
  update(regs_, init_status_, count_.calendar(position / count_.sync_ticks()).year != 0 ? 1 : 0);
}

void rtc_classic::refuse_unmodelled(std::string_view name, std::uint32_t old, std::uint32_t now)
{
  if(debugger_write_)
  {
    return;
  }
  for(std::size_t index = 0; index < unmodelled_.size(); ++index)
  {
    auto const& entry = unmodelled_[index];
    if(entry.reg != name or (now & entry.mask) == 0)
    {
      continue;
    }
    if(not std::exchange(logged_[index], true))
    {
      warn(entry.reg, std::format("{}: not modelled, the bits are storage", entry.what));
    }
    if(ctx_.strict_peripherals)
    {
      auto const target = reg(name);
      regs_.poke(target.offset, old);
      throw model_error(std::string{desc_.name}, std::string{name}, target.offset);
    }
  }
}

void rtc_classic::clear_backup(register_file& regs, bool board_calendar)
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

auto rtc_classic::next_alarm(std::size_t which, std::uint64_t after) const
    -> std::optional<std::uint64_t>
{
  auto const& a      = alarms_[which];
  auto const  word   = regs_.peek(a.reg.offset);
  auto const  ticks  = count_.sync_ticks();
  auto const  second = after / ticks;
  auto const  masked = a.subseconds ? std::min(get(regs_, a.mask_ss), 15u) : 0u;
  if(masked == 0)
  {
    auto const next = count_.next_alarm(a.match, word, second);
    return next.transform([&](std::uint64_t at) { return at * ticks; });
  }
  auto const bits   = (std::uint64_t{1} << masked) - 1u;
  auto const target = std::uint64_t{get(regs_, a.ss)} & bits;
  auto const top    = std::uint64_t{state_.prediv_s};
  auto const first  = [&](std::uint64_t from) -> std::optional<std::uint64_t> {
    if(from > top)
    {
      return std::nullopt;
    }
    auto value = ((top - from) & ~bits) | target;
    if(value > top - from)
    {
      if(value < bits + 1)
      {
        return std::nullopt;
      }
      value -= bits + 1;
    }
    return top - value;
  };
  if(not first(0))
  {
    return std::nullopt;
  }
  auto const matches = [&](std::uint64_t at) {
    return at == 0 ? count_.next_alarm(a.match, word, ~std::uint64_t{}) == 0
                   : count_.next_alarm(a.match, word, at - 1) == at;
  };
  if(matches(second))
  {
    if(auto const tick = first(after % ticks + 1))
    {
      return second * ticks + *tick;
    }
  }
  auto const next = count_.next_alarm(a.match, word, second);
  if(not next)
  {
    return std::nullopt;
  }
  return *next * ticks + *first(0);
}

void rtc_classic::plan_alarm(std::size_t which)
{
  auto const kind    = static_cast<classic_event>(alarm_a + which);
  state_.armed[kind] = 0;
  ++state_.generation[kind];
  if(get(regs_, alarms_[which].enable) == 0 or state_.counting == 0)
  {
    return;
  }
  auto const position = count_.position_at(count_.cycle_at(now_ps()));
  if(auto const next = next_alarm(which, position))
  {
    arm(kind, count_.cycle_of(*next));
  }
}

void rtc_classic::plan_all()
{
  plan_wakeup();
  plan_alarm(0);
  plan_alarm(1);
}

void rtc_classic::update_lines()
{
  auto const raised = [&](field_ref flag, field_ref enable) {
    return get(regs_, flag) != 0 and get(regs_, enable) != 0;
  };
  auto levels = std::uint8_t{};
  if(raised(alarms_[0].flag, alarms_[0].interrupt) or raised(alarms_[1].flag, alarms_[1].interrupt))
  {
    levels |= 1u;
  }
  if(raised(wakeup_flag_, wakeup_interrupt_))
  {
    levels |= 2u;
  }
  auto const changed = static_cast<std::uint8_t>(levels ^ state_.lines);
  state_.lines       = levels;
  if(changed == 0)
  {
    return;
  }
  auto* const exti = dynamic_cast<exti_line_input*>(ctx_.peers.find("EXTI"));
  if(exti == nullptr)
  {
    return;
  }
  if((changed & 1u) != 0)
  {
    exti->line_changed(variant_.alarm_line, (levels & 1u) != 0);
  }
  if((changed & 2u) != 0)
  {
    exti->line_changed(variant_.wakeup_line, (levels & 2u) != 0);
  }
}

void rtc_classic::alarm_enables_changed(std::uint32_t old, std::uint32_t now, std::uint64_t cycle)
{
  if(not variant_.alarm_write_wait)
  {
    return;
  }
  for(std::size_t which = 0; which < alarms_.size(); ++which)
  {
    auto const& a    = alarms_[which];
    auto const  kind = static_cast<classic_event>(alarm_a_write + which);
    if(a.enable.of(old) == 0 and a.enable.of(now) != 0)
    {
      update(regs_, a.write, 0);
      state_.armed[kind] = 0;
    }
    else if(a.enable.of(old) != 0 and a.enable.of(now) == 0 and get(regs_, init_) == 0)
    {
      arm(kind, cycle + sync_cycles);
    }
  }
}

void rtc_classic::restart_timers(std::uint64_t cycle)
{
  if(not bypassed())
  {
    arm(shadow_ready, cycle + variant_.shadow_copy_cycles);
  }
  plan_all();
  schedule_all();
  settle(cycle);
  update_lines();
}

void rtc_classic::reset(register_file& regs, reset_kind kind)
{
  if(kind == reset_kind::power_on or (variant_.brown_out_resets and kind == reset_kind::brown_out))
  {
    clear_backup(regs, kind == reset_kind::power_on);
    state_.hz        = ctx_.clock.hz(domain_);
    state_.anchor_ps = now_ps();
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
    count_.restart(cycle, get(regs, prediv_a_), get(regs, prediv_s_));
    if(rtc_calendar::wakeup_on_spre(get(regs, wakeup_clock_)))
    {
      state_.wakeup_base  = state_.seconds;
      state_.wakeup_fired = 0;
    }
  }
  auto       isr      = regs.peek(status_.offset);
  auto const was_init = init_.of(isr) != 0;
  isr                 = init_.with(isr, 0);
  isr                 = init_flag_.with(isr, 0);
  isr                 = shadow_flag_.with(isr, 0);
  regs.poke(status_.offset, isr);
  if(was_init and variant_.alarm_write_wait)
  {
    for(auto const& a : alarms_)
    {
      update(regs, a.write, get(regs, a.enable) != 0 ? 0 : 1);
    }
  }
  state_.armed[init_ready]   = 0;
  state_.armed[shadow_ready] = 0;
  state_.latched_valid       = 0;
  state_.shadow_reset        = bypassed() ? 0 : 1;
  for(auto& value : state_.generation)
  {
    ++value;
  }
  restart_timers(cycle);
}

void rtc_classic::backup_reset()
{
  auto const held_ps = state_.held_ps;
  auto const lines   = state_.lines;
  auto const frozen  = state_.held != 0 and variant_.debug_freeze and debug_frozen(freeze_logged_);
  clear_backup(regs_, false);
  state_.lines = lines;
  state_.hz    = ctx_.clock.hz(domain_);
  if(frozen)
  {
    state_.held    = 1;
    state_.held_ps = held_ps;
  }
  state_.anchor_ps = now_ps();
  restart_timers(state_.cycle0);
}

auto rtc_classic::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     access) -> std::expected<std::uint32_t, bus_error>
{
  auto const cycle = count_.cycle_at(now_ps());
  settle(cycle);
  auto const word      = offset & ~3u;
  auto const subsecond = subsecond_ and word == subsecond_->offset;
  if(word != time_.offset and word != date_.offset and not subsecond)
  {
    return dispatch_model::on_read(regs, offset, size, access);
  }
  auto position = count_.position_at(cycle);
  if(access.by != initiator::debugger and not bypassed())
  {
    if(state_.shadow_reset != 0)
    {
      auto const name = word == time_.offset ? "TR" : word == date_.offset ? "DR" : "SSR";
      return read_lanes(describe(name).reset, offset, size);
    }
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

auto rtc_classic::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  settle(count_.cycle_at(now_ps()));
  debugger_write_   = access.by == initiator::debugger;
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  settle(count_.cycle_at(now_ps()));
  return result;
}

void rtc_classic::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= classic_events or payload != state_.generation[kind] or state_.armed[kind] == 0)
  {
    return;
  }
  auto const cycle   = count_.cycle_at(now_ps());
  state_.armed[kind] = 0;
  switch(static_cast<classic_event>(kind))
  {
    case init_ready:
      if(get(regs_, init_) != 0)
      {
        update(regs_, init_flag_, 1);
      }
      break;
    case shadow_ready:
      if(get(regs_, init_) == 0 and not bypassed())
      {
        update(regs_, shadow_flag_, 1);
        state_.shadow_reset = 0;
      }
      break;
    case wakeup_ready:
      update(regs_, wakeup_write_, get(regs_, wakeup_enable_) != 0 ? 0 : 1);
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
    case alarm_a_write:
    case alarm_b_write:
    {
      auto const& a = alarms_[static_cast<std::size_t>(kind - alarm_a_write)];
      if(get(regs_, a.enable) == 0 and get(regs_, init_) == 0)
      {
        update(regs_, a.write, 1);
      }
      break;
    }
    case classic_events:
      break;
  }
  settle(cycle);
  update_lines();
}

void rtc_classic::on_core_hold(bool held)
{
  if(not variant_.debug_freeze)
  {
    return;
  }
  hold_core(held);
}

auto rtc_classic::storage_only(std::uint32_t offset) const -> bool
{
  return std::ranges::contains(storage_, offset);
}

void rtc_classic::on_clock_change()
{
  clock_changed();
}

void rtc_classic::status_written(std::uint32_t old, std::uint32_t now)
{
  auto clears = open_flags_;
  auto word   = old;
  if(unlocked())
  {
    clears |= keyed_flags_;
    word = init_.with(word, init_.of(now));
  }
  word &= ~(clears & ~now);
  regs_.poke(status_.offset, word);
  auto const cycle = count_.cycle_at(now_ps());
  if(init_.of(old) == 0 and init_.of(word) != 0)
  {
    count_.freeze(cycle);
    update(regs_, shadow_flag_, 0);
    state_.armed[shadow_ready] = 0;
    arm(init_ready, cycle + init_cycles);
    if(variant_.alarm_write_wait)
    {
      for(std::size_t which = 0; which < alarms_.size(); ++which)
      {
        update(regs_, alarms_[which].write, 0);
        state_.armed[alarm_a_write + which] = 0;
      }
    }
    plan_all();
    update_lines();
    return;
  }
  if(init_.of(old) != 0 and init_.of(word) == 0)
  {
    update(regs_, init_flag_, 0);
    state_.armed[init_ready] = 0;
    count_.restart(cycle + exit_cycles, get(regs_, prediv_a_), get(regs_, prediv_s_));
    state_.latched_valid = 0;
    update(regs_, shadow_flag_, 0);
    if(not bypassed())
    {
      arm(shadow_ready, state_.start + variant_.shadow_copy_cycles);
    }
    if(variant_.alarm_write_wait)
    {
      for(auto const& a : alarms_)
      {
        update(regs_, a.write, get(regs_, a.enable) != 0 ? 0 : 1);
      }
    }
    if(rtc_calendar::wakeup_on_spre(get(regs_, wakeup_clock_)))
    {
      state_.wakeup_base  = state_.seconds;
      state_.wakeup_fired = 0;
    }
    plan_all();
    update_lines();
    return;
  }
  if(shadow_flag_.of(old) != 0
     and shadow_flag_.of(word) == 0
     and init_.of(word) == 0
     and not bypassed())
  {
    arm(shadow_ready, std::max(cycle, state_.start) + variant_.shadow_copy_cycles);
  }
  update_lines();
}

void rtc_classic::control_written(std::uint32_t old, std::uint32_t now)
{
  if(keep_locked(control_, old))
  {
    return;
  }
  auto word = now;
  if(not initializing())
  {
    word = (word & ~init_only_) | (old & init_only_);
  }
  if(wakeup_enable_.of(old) != 0 or get(regs_, wakeup_write_) == 0)
  {
    word = wakeup_clock_.with(word, wakeup_clock_.of(old));
  }
  word = add_hour_.with(word, 0);
  word = sub_hour_.with(word, 0);
  regs_.poke(control_.offset, word);
  refuse_unmodelled("CR", old, word);
  auto const cycle = count_.cycle_at(now_ps());
  count_.shift_hour(cycle, add_hour_.of(now) != 0, sub_hour_.of(now) != 0);
  if(wakeup_enable_.of(old) != wakeup_enable_.of(word))
  {
    arm(wakeup_ready, cycle + sync_cycles);
    if(wakeup_enable_.of(word) != 0)
    {
      auto const spre     = rtc_calendar::wakeup_on_spre(wakeup_clock_.of(word));
      state_.wakeup_base  = spre ? count_.position_at(cycle) / count_.sync_ticks() : cycle;
      state_.wakeup_fired = 0;
    }
  }
  alarm_enables_changed(old, word, cycle);
  plan_all();
  update_lines();
  if(not bypass_)
  {
    return;
  }
  auto const was = bypass_->of(old) != 0;
  auto const is  = bypass_->of(word) != 0;
  if(not was and is)
  {
    update(regs_, shadow_flag_, 0);
    state_.armed[shadow_ready] = 0;
    state_.latched_valid       = 0;
  }
  else if(was and not is and get(regs_, init_) == 0)
  {
    arm(shadow_ready, cycle + variant_.shadow_copy_cycles);
  }
}

void rtc_classic::coarse_written(std::uint32_t old, std::uint32_t)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(reg("CALIBR").offset, old);
  }
}

void rtc_classic::smooth_written(std::uint32_t old, std::uint32_t now)
{
  auto const target = reg("CALR");
  if(keep_locked(target, old))
  {
    return;
  }
  refuse_unmodelled("CALR", old, now);
}

void rtc_classic::shift_written(std::uint32_t old, std::uint32_t now)
{
  auto const target = reg("SHIFTR");
  if(not keep_locked(target, old))
  {
    refuse_unmodelled("SHIFTR", old, now);
  }
  regs_.poke(target.offset, 0);
}

void rtc_classic::tamper_written(std::uint32_t old, std::uint32_t now)
{
  refuse_unmodelled(variant_.tamper_register, old, now);
}

template <std::size_t Index> void rtc_classic::locked_written(std::uint32_t old, std::uint32_t)
{
  auto const target = reg(locked_names[Index]);
  if(keep_locked(target, old))
  {
    return;
  }
  auto allowed = initializing();
  if constexpr(Index == 0)
  {
    allowed = get(regs_, wakeup_write_) != 0;
  }
  else if constexpr(Index <= 2)
  {
    allowed = allowed or get(regs_, alarms_[Index - 1].write) != 0;
  }
  else
  {
    allowed = allowed or get(regs_, alarms_[Index - 3].enable) == 0;
  }
  if(not allowed)
  {
    regs_.poke(target.offset, old);
    return;
  }
  plan_all();
}

template <std::size_t Index> void rtc_classic::read_only_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(reg(read_only_names[Index]).offset, old);
}

void rtc_classic::plain_written(std::uint32_t, std::uint32_t)
{}
}

auto rtc_classic_factory(
    std::optional<std::chrono::sys_seconds> initial,
    rtc_classic_variant                     variant) -> model_factory
{
  return [initial,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rtc_classic>(ctx, desc, initial, variant);
  };
}
}
