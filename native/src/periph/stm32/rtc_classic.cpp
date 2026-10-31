#include "backup_domain.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "rtc_calendar.hpp"

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

constexpr std::uint64_t shadow_cycles = 2;

enum classic_event : std::uint8_t
{
  init_ready,
  shadow_ready,
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
  std::array<std::uint64_t, classic_events> due{};
  std::array<std::uint8_t, classic_events>  armed{};
  std::array<std::uint32_t, classic_events> generation{};
  std::uint8_t                              held{};
  std::uint64_t                             held_ps{};

  friend auto operator==(rtc_classic_state const&, rtc_classic_state const&) -> bool = default;
};

enum unmodelled_function : std::uint8_t
{
  alarm_a_function,
  alarm_b_function,
  wakeup_function,
  timestamp_function,
  tamper_function,
  coarse_function,
  smooth_function,
  shift_function,
  output_function,
  reference_function,
  unmodelled_functions,
};

constexpr std::array<std::string_view, 5> locked_names{
    "WUTR", "ALRMAR", "ALRMBR", "ALRMASSR", "ALRMBSSR"};
constexpr std::array<std::string_view, 4> read_only_names{"SSR", "TSTR", "TSDR", "TSSSR"};

struct unmodelled_entry
{
  std::string_view reg;
  std::string_view what;
  std::uint32_t    mask{};
};

/**
 * @brief Classic RTC of the STM32L1, RM0038 section 20, BCD calendar. The calendar is derived from
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
 *        cleared by a 0 under the key, inferred from RM0038 20.6.4, which exempts bits 13:8 only.
 *        Shadow registers: an SSR or TR read latches TR and DR until DR is read, RSF sets at the
 *        second RTCCLK edge after a clear, the end of init mode, a BYPSHAD clear or a system reset,
 *        inferred from the copy every two RTCCLK periods of RM0038 20.3.2, and is cleared in init
 *        mode and while CR.BYPSHAD is set, RM0038 20.6.4. A system reset clears INIT, INITF and
 *        RSF, and with BYPSHAD 0 TR, DR and SSR read their reset values until the next copy sets
 *        RSF, RM0038 20.6.1, 20.6.2 and 20.6.11. ADD1H and SUB1H apply at the write. The backup
 *        registers, as many as the description lists, keep their value on a system reset and take
 *        every write, a debugger write included, with no key. A power-on reset, a brown-out on a
 *        part whose variant says so and RCC_CSR.RTCRST bring every register and the calendar to
 *        their reset values, the board's `rtc_initial` loaded at power-on only. While the machine
 *        holds its core with DBGMCU `DBG_RTC_STOP` set, the calendar and the init and shadow
 *        timings stop at the time of the hold and resume at the release, on a variant whose DBGMCU
 *        has the bit. Logged once, a model_error under strict_peripherals at the CPU or DMA write
 *        that selects them before the store, a debugger write stored with neither: alarms A and B,
 *        the wakeup timer, timestamps, tamper detection, coarse and smooth calibration, the shift
 *        register, the RTC outputs and the reference clock detection. PWR_CR.DBP gates no write.
 */
class rtc_classic final : public dispatch_model<rtc_classic, rtc_classic_state>,
                          public clock_listener,
                          public backup_domain,
                          public core_hold_listener
{
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

  void                              time_written(std::uint32_t old, std::uint32_t now);
  void                              date_written(std::uint32_t old, std::uint32_t now);
  void                              status_written(std::uint32_t old, std::uint32_t now);
  void                              prescaler_written(std::uint32_t old, std::uint32_t now);
  void                              control_written(std::uint32_t old, std::uint32_t now);
  void                              key_written(std::uint32_t old, std::uint32_t now);
  void                              coarse_written(std::uint32_t old, std::uint32_t now);
  void                              smooth_written(std::uint32_t old, std::uint32_t now);
  void                              shift_written(std::uint32_t old, std::uint32_t now);
  void                              tamper_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Index> void locked_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Index> void read_only_written(std::uint32_t old, std::uint32_t now);
  void                              plain_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto unlocked() const -> bool;
  [[nodiscard]] auto initializing() const -> bool;
  [[nodiscard]] auto bypassed() const -> bool;
  [[nodiscard]] auto keep_locked(reg_ref target, std::uint32_t old) -> bool;

  void clear_backup(register_file& regs, bool board_calendar);
  void restart_shadow(std::uint64_t cycle);
  void arm(classic_event kind, std::uint64_t cycle);
  void schedule(classic_event kind);
  void schedule_all();
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
  std::array<unmodelled_entry, unmodelled_functions> unmodelled_{};
  std::array<bool, unmodelled_functions>             logged_{};
  rtc_calendar::counter<rtc_classic_state>           count_;
  core::clock_domain                                 domain_;
  bool                                               freeze_logged_  = false;
  bool                                               debugger_write_ = false;
};

constexpr std::array<write_entry<rtc_classic>, 13> classic_writes{
    {
     {"TR", &rtc_classic::time_written},
     {"DR", &rtc_classic::date_written},
     {"CR", &rtc_classic::control_written},
     {"ISR", &rtc_classic::status_written},
     {"PRER", &rtc_classic::prescaler_written},
     {"WUTR", &rtc_classic::locked_written<0>},
     {"CALIBR", &rtc_classic::coarse_written},
     {"ALRMAR", &rtc_classic::locked_written<1>},
     {"ALRMBR", &rtc_classic::locked_written<2>},
     {"WPR", &rtc_classic::key_written},
     {"TSTR", &rtc_classic::read_only_written<1>},
     {"TSDR", &rtc_classic::read_only_written<2>},
     {"TAFCR", &rtc_classic::tamper_written},
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
      count_{
          state_, ctx.regs, [this](std::string_view r, std::string_view f) { return field(r, f); }},
      domain_{ctx.clock.domain("rtc")}
{
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
    expect_field("TAFCR", name, variant.three_tampers);
  }
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
  std::vector<write_entry<rtc_classic>> backups;
  for(auto const& described : desc.registers)
  {
    if(described.name.starts_with("BKP") and described.name.ends_with("R"))
    {
      backups.push_back({described.name, &rtc_classic::plain_written});
    }
  }
  bind(backups);
  for(auto const& name : {"FMT", "DCE", "REFCKON"})
  {
    init_only_ |= field("CR", name).mask();
  }
  for(auto const& name : {"ALRAF", "ALRBF", "WUTF", "TSF", "TSOVF", "TAMP1F"})
  {
    open_flags_ |= field("ISR", name).mask();
  }
  keyed_flags_ = shadow_flag_.mask();
  if(variant.three_tampers)
  {
    keyed_flags_ |= field("ISR", "TAMP2F").mask() | field("ISR", "TAMP3F").mask();
  }
  auto const bits = [&](std::string_view r, std::initializer_list<std::string_view> names) {
    auto mask = std::uint32_t{};
    for(auto const name : names)
    {
      mask |= field(r, name).mask();
    }
    return mask;
  };
  auto tampers = bits("TAFCR", {"TAMP1E"});
  if(variant.three_tampers)
  {
    tampers |= bits("TAFCR", {"TAMP2E", "TAMP3E"});
  }
  unmodelled_[alarm_a_function]   = {"CR", "alarm A", bits("CR", {"ALRAE"})};
  unmodelled_[alarm_b_function]   = {"CR", "alarm B", bits("CR", {"ALRBE"})};
  unmodelled_[wakeup_function]    = {"CR", "the wakeup timer", bits("CR", {"WUTE"})};
  unmodelled_[timestamp_function] = {"CR", "timestamps", bits("CR", {"TSE"})};
  unmodelled_[tamper_function]    = {"TAFCR", "tamper detection", tampers};
  unmodelled_[coarse_function]    = {"CR", "coarse digital calibration", bits("CR", {"DCE"})};
  unmodelled_[smooth_function]    = {"CALR", "smooth digital calibration", ~std::uint32_t{}};
  unmodelled_[shift_function]     = {"SHIFTR", "the shift control", ~std::uint32_t{}};
  unmodelled_[output_function]    = {
      "CR", "the RTC_CALIB and RTC_ALARM outputs", bits("CR", {"COE", "OSEL"})};
  unmodelled_[reference_function] = {
      "CR", "the reference clock detection", bits("CR", {"REFCKON"})};
}

auto rtc_classic::now_ps() const -> std::uint64_t
{
  return state_.held != 0 ? state_.held_ps : ctx_.clock.since_power_on().ps;
}

auto rtc_classic::unlocked() const -> bool
{
  return state_.unlock == 2;
}

auto rtc_classic::initializing() const -> bool
{
  return get(regs_, init_flag_) != 0;
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

void rtc_classic::arm(classic_event kind, std::uint64_t cycle)
{
  state_.armed[kind] = 1;
  state_.due[kind]   = cycle;
  schedule(kind);
}

void rtc_classic::schedule(classic_event kind)
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

void rtc_classic::schedule_all()
{
  for(std::size_t kind = 0; kind < classic_events; ++kind)
  {
    schedule(static_cast<classic_event>(kind));
  }
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

void rtc_classic::restart_shadow(std::uint64_t cycle)
{
  if(not bypassed())
  {
    arm(shadow_ready, cycle + shadow_cycles);
  }
  schedule_all();
  settle(cycle);
}

void rtc_classic::reset(register_file& regs, reset_kind kind)
{
  if(kind == reset_kind::power_on or (variant_.brown_out_resets and kind == reset_kind::brown_out))
  {
    clear_backup(regs, kind == reset_kind::power_on);
    state_.hz        = ctx_.clock.hz(domain_);
    state_.anchor_ps = now_ps();
    restart_shadow(state_.cycle0);
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
  auto isr = regs.peek(status_.offset);
  isr      = init_.with(isr, 0);
  isr      = init_flag_.with(isr, 0);
  isr      = shadow_flag_.with(isr, 0);
  regs.poke(status_.offset, isr);
  state_.armed         = {};
  state_.latched_valid = 0;
  state_.shadow_reset  = bypassed() ? 0 : 1;
  for(auto& value : state_.generation)
  {
    ++value;
  }
  restart_shadow(cycle);
}

void rtc_classic::backup_reset()
{
  auto const held_ps = state_.held_ps;
  auto const frozen  = state_.held != 0 and variant_.debug_freeze and debug_frozen(freeze_logged_);
  clear_backup(regs_, false);
  state_.hz = ctx_.clock.hz(domain_);
  if(frozen)
  {
    state_.held    = 1;
    state_.held_ps = held_ps;
  }
  state_.anchor_ps = now_ps();
  restart_shadow(state_.cycle0);
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
    case classic_events:
      break;
  }
  settle(cycle);
}

void rtc_classic::on_core_hold(bool held)
{
  if(not variant_.debug_freeze)
  {
    return;
  }
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
      for(std::uint32_t kind = 0; kind < classic_events; ++kind)
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

void rtc_classic::on_clock_change()
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

void rtc_classic::time_written(std::uint32_t old, std::uint32_t now)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(time_.offset, old);
    return;
  }
  count_.set_time(now);
}

void rtc_classic::date_written(std::uint32_t old, std::uint32_t now)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(date_.offset, old);
    return;
  }
  count_.set_date(now);
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
    return;
  }
  if(init_.of(old) != 0 and init_.of(word) == 0)
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
    if(not bypassed())
    {
      arm(shadow_ready, state_.start + shadow_cycles);
    }
    return;
  }
  if(shadow_flag_.of(old) != 0
     and shadow_flag_.of(word) == 0
     and init_.of(word) == 0
     and not bypassed())
  {
    arm(shadow_ready, std::max(cycle, state_.start) + shadow_cycles);
  }
}

void rtc_classic::prescaler_written(std::uint32_t old, std::uint32_t)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(prediv_a_.offset, old);
  }
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
  word = add_hour_.with(word, 0);
  word = sub_hour_.with(word, 0);
  regs_.poke(control_.offset, word);
  refuse_unmodelled("CR", old, word);
  auto const cycle = count_.cycle_at(now_ps());
  count_.shift_hour(cycle, add_hour_.of(now) != 0, sub_hour_.of(now) != 0);
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
    arm(shadow_ready, cycle + shadow_cycles);
  }
}

void rtc_classic::key_written(std::uint32_t, std::uint32_t now)
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
  refuse_unmodelled("TAFCR", old, now);
}

template <std::size_t Index> void rtc_classic::locked_written(std::uint32_t old, std::uint32_t)
{
  static_cast<void>(keep_locked(reg(locked_names[Index]), old));
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
