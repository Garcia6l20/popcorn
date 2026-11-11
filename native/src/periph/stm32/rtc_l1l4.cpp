#include "backup_domain.hpp"
#include "exti_l1l4.hpp"
#include "gpio_peers.hpp"
#include "model_base.hpp"
#include "read_protection.hpp"
#include "rtc_calendar.hpp"
#include "rtc_core.hpp"
#include "rtc_rows.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/core_hold.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

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

constexpr std::uint64_t sync_cycles        = 2;
constexpr std::uint64_t stamp_apre_cycles  = 2;
constexpr std::uint64_t tamper_apre_cycles = 3;

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
  timestamp_ready,
  tamper1_ready,
  tamper2_ready,
  tamper3_ready,
  tamper_sample,
  classic_events,
};

struct rtc_l1l4_state
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
  std::uint8_t                              stamp_pending{};
  std::uint32_t                             stamp_time{};
  std::uint32_t                             stamp_date{};
  std::uint32_t                             stamp_subsecond{};
  std::uint8_t                              tamper_pending{};
  std::array<std::uint8_t, 3>               tamper_samples{};

  friend auto operator==(rtc_l1l4_state const&, rtc_l1l4_state const&) -> bool = default;
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
 *        RTC_TAFCR, RTC_OR, no RTC_CALIBR and no CR.DCE, and CR.ITSE, the internal timestamp,
 *        logged as not modelled.
 *        Its key leaves RTC_ISR[31:8], RTC_TAMPCR, RTC_OR and the backup registers open, RM0351
 *        38.3.7, so TAMP2F and TAMP3F clear by a 0 without it. RSF sets at the first RTCCLK edge
 *        after a clear, the end of init mode, a BYPSHAD clear or a system reset, inferred from the
 *        copy every RTCCLK period of RM0351 38.3.4 and 38.3.8, where the BYPSHAD text of 38.6.3
 *        says every two cycles. A brown-out keeps the backup domain, RM0351 6.1.3. The alarm
 *        interrupt drives EXTI line 18, RM0351 Table 62, the flag gated by its interrupt enable
 *        inferred from Table 230. ALRAWF and ALRBWF read 1 as on Cat.2 to Cat.6, inferred:
 *        RM0351 38.3.7 programs the alarm with no ALRxWF poll. RTC_TAMPCR and RTC_OR are storage.
 *        A read protection regression from level 1 to level 0, which the STM32L4 FLASH reports,
 *        writes 0 to every backup register, RM0351 3.5.1.
 *        Timestamps, RM0351 38.3.13, with `rtc_l1l4_variant::timestamps`: with CR.TSE set, an
 *        edge of the RTC_TS input, rising with CR.TSEDGE 0 and falling with 1, captures TR, DR
 *        bits 15:0 without the year and SSR into RTC_TSTR, RTC_TSDR and RTC_TSSSR at the board
 *        time of the edge, `pin_edge_clock::edge_ps`, also when the GPIO applies it later,
 *        and sets TSF 2 ck_apre cycles later, 2 x (PREDIV_A + 1) RTCCLK cycles, inferred. An edge
 *        while TSF is set or that set is pending sets TSOVF at once and keeps the stamp, the
 *        pending case from the TSOVF note of 38.3.13. A TSF clear writes 0 to the three
 *        registers, 38.6.12 to 38.6.14. TSF and TSIE drive the tamper EXTI line of the variant,
 *        line 19, RM0351 Table 62. RTC_TS reads the variant's pin, PC13, whatever its GPIO mode,
 *        RM0351 38.3.2: the board drive, else the PUPDR pull-up, inferred. A PUPDR write and a
 *        TSEDGE write make no edge.
 *        Tampers, RM0351 38.3.14, with `rtc_l1l4_variant::tampers`: RTC_TAMP1 to RTC_TAMP3 on
 *        the variant's pins, PC13, PA0 and PE6, an unbonded pin never active. With TAMPFLT 0 an
 *        edge of the TAMPxTRG polarity with TAMPxE set is an event, with TAMPFLT 1 to 3 the
 *        input is sampled every 2^(15 - TAMPFREQ) RTCCLK cycles, on multiples of that period,
 *        and 2, 4 or 8 consecutive samples at the TAMPxTRG level make an event, the phase and
 *        the count restart after an event inferred. A sample of an undriven input reads high
 *        with TAMPPUDIS 0, the precharge, else its PUPDR pull-up. An event while TAMPxF is set
 *        with TAMPxMF 0, or while one is pending, is lost. TAMPxF sets at the event with
 *        TAMPFLT 0 and TAMPTS 0, else 3 ck_apre cycles after the board time of the event, and
 *        the backup registers are erased then unless TAMPxNOERASE, inferred. With TAMPxMF set
 *        no flag sets and nothing is erased.
 *        TAMPTS stamps the calendar of the event as a timestamp does, TSF with TAMPxF. A tamper
 *        flag with TAMPIE or its TAMPxIE drives the tamper EXTI line. ES0250 2.19.1: clearing
 *        TAMPxE of an enabled falling-edge tamper, TAMPFLT 0 and TAMPxTRG 1, while its input is
 *        high is an event, with the TAMPxNOERASE of the previous write.
 *
 * @limits rtc
 *
 * @group all rule "The STM32L1 RTC"
 * - INITF sets at the second RTCCLK edge after INIT, and the calendar restarts at the fourth edge
 *   after INIT clears, the upper bounds of RM0038 20.3.5.
 * - RSF sets at the second RTCCLK edge after a clear, the end of init mode, a BYPSHAD clear or a
 *   system reset, inferred from the copy every two RTCCLK periods of RM0038 20.3.2.
 * - TR, DR and SSR read the calendar at the time of the read. A read of SSR or TR holds TR and DR
 *   until DR is read, and after a system reset they read their reset values until RSF sets.
 * - TAMP2F and TAMP3F clear by a 0 under the `RTC_WPR` key only, RM0038 20.3.5 and the note under
 *   20.6.4, which exempt `RTC_ISR[13:8]`.
 * - `RTC_TSTR`, `RTC_TSDR`, `RTC_TSSSR`, `RTC_CALIBR`, `RTC_CALR`, `RTC_SHIFTR` and `RTC_TAFCR` are
 *   storage: coverage reports them `storage_only`, and `m.mem` and the register views refuse them
 *   as they refuse a peripheral without a model.
 * - ADD1H and SUB1H move the calendar at the write.
 * - RTC writes land whatever `PWR_CR.DBP` holds.
 * - On Cat.1 a PREDIV_A of 0 divides by 1.
 * - The alarm interrupt, ALRxF with ALRxIE, drives EXTI line 17 and RTC_Alarm, and WUTF with WUTIE
 *   drives EXTI line 20 and RTC_WKUP, RM0038 20.5. The flag gated by its interrupt enable is
 *   inferred from RM0038 Table 90. A flag set while its enable is 0 raises the line when the enable
 *   sets.
 * - On Cat.1 ALRAWF and ALRBWF clear when their alarm is enabled and in init mode, and set at the
 *   second RTCCLK edge after the alarm is disabled, inferred from RM0038 20.3.5. On Cat.2 to Cat.6
 *   they read 1, so `RTC_ALRMxR` takes writes while its alarm is enabled, inferred.
 * - The wakeup timer counts from the write that sets WUTE, inferred. WUTWF clears and sets at the
 *   second RTCCLK edge after WUTE changes, the upper bound of RM0038 20.6.4.
 * - A system reset keeps a pending set of WUTWF, ALRAWF or ALRBWF, inferred from the bit text of
 *   RM0038 20.6.4. A system reset in init mode restarts the calendar as the end of init mode does,
 *   inferred: on Cat.1 ALRxWF reads 1 at once for an alarm with ALRxE clear, and a ck_spre wakeup
 *   counts its period from the restart. The U5 and H5 RTC rebase a ck_spre wakeup the same way,
 *   inferred.
 * - Logged once, `ModelError` under `strict` at the CPU or DMA write that selects them, before the
 *   store: timestamps, tamper detection, coarse and smooth calibration, the shift control, the
 *   RTC_CALIB and RTC_ALARM outputs and the reference clock detection. A debugger write stores them
 *   with neither.
 *
 * @group l4 rule "The STM32L4 RTC, RM0351 section 38, runs the STM32L1 rules above on its own
 *       register set"
 * - `RTC_TAMPCR` at 0x40 in place of `RTC_TAFCR`, `RTC_OR` at 0x4C, no `RTC_CALIBR` and no
 *   `CR.DCE`.
 * - RSF sets at the first RTCCLK edge after a clear, the end of init mode, a BYPSHAD clear or a
 *   system reset, inferred from the copy every RTCCLK period of RM0351 38.3.4 and 38.3.8, where the
 *   BYPSHAD text of 38.6.3 says every two RTCCLK cycles.
 * - The `RTC_WPR` key leaves `RTC_ISR[31:8]`, `RTC_TAMPCR`, `RTC_OR` and the backup registers open,
 *   RM0351 38.3.7: ALRAF to TAMP3F clear by a 0 without it.
 * - `RTC_TAMPCR` and `RTC_OR` are storage: coverage reports them `storage_only`. `RTC_ISR.ITSF`,
 *   bit 17 in RM0351 38.6.4, is not in the description and reads 0.
 * - A brown-out keeps the calendar and the backup registers, RM0351 6.1.3.
 * - The alarm interrupt, ALRxF with ALRxIE, drives EXTI line 18 and RTC_Alarm, RM0351 Table 62, and
 *   WUTF with WUTIE drives EXTI line 20 and RTC_WKUP. The flag gated by its interrupt enable is
 *   inferred from RM0351 Table 230.
 * - ALRAWF and ALRBWF read 1, so `RTC_ALRMxR` takes writes while its alarm is enabled, inferred:
 *   RM0351 38.3.7 programs the alarm with no ALRxWF poll, where RM0038 20.3.5 skips the poll on the
 *   STM32L1 Cat.2 to Cat.6, whose flags are always set. `RTC_ALRMxSSR` takes writes with ALRxE
 *   clear or in init mode, RM0351 38.6.17 and 38.6.18.
 * - Logged once, `ModelError` under `strict` at the CPU or DMA write that selects them: the
 *   internal timestamp `CR.ITSE`, smooth calibration, the shift control, the outputs and the
 *   reference clock detection.
 * - Tampers, RM0351 38.3.14: RTC_TAMP1 on PC13, RTC_TAMP2 on PA0 and RTC_TAMP3 on PE6, DS10198
 *   Table 16. A pin the package does not bond, such as PE6 on the 64-pin parts, is never active.
 *   With TAMPFLT 0 an edge of the TAMPxTRG polarity is an event. With TAMPFLT 1 to 3 the input is
 *   sampled every 2^(15 - TAMPFREQ) RTCCLK cycles, on multiples of that period counted from the
 *   calendar's RTCCLK origin, and 2, 4 or 8 consecutive samples at the TAMPxTRG level make an
 *   event. The sample phase, and the count restarting after each event, are inferred.
 * - A sample of an undriven tamper input reads high with TAMPPUDIS 0, the precharge, and its PUPDR
 *   pull-up with TAMPPUDIS 1. TAMPPRCH sets no timing: the board drive is ideal.
 * - TAMPxF sets at the event with TAMPFLT 0 and TAMPTS 0, else 3 ck_apre cycles later, RM0351
 *   38.3.14, counted from the event, inferred. The backup registers are erased when TAMPxF sets,
 *   unless TAMPxNOERASE, inferred: RM0351 38.3.14 erases at the tamper event and gives the latency
 *   for TAMPxF only. An event while TAMPxF is set, or while one is pending, is lost.
 * - TAMPxMF masks TAMPxF and the erase. The tamper trigger to the LPTIM is not connected. A TAMPIE
 *   or TAMPxIE write with a TAMPxMF set is stored as written. With TAMPxMF an event is lost during
 *   the latency only, where RM0351 38.3.14 adds 2.5 ck_rtc cycles to that window.
 * - TAMPTS stamps the calendar of the tamper event as a timestamp does, at the board time of an
 *   edge. TSF sets with TAMPxF, and TSOVF at once when a stamp is set or pending, RM0351 38.3.13.
 *   TAMPIE, or TAMPxIE, with TAMPxF drives EXTI line 19.
 * - ES0250 2.19.1 is modelled: clearing TAMPxE of a falling-edge tamper, TAMPFLT 0 and TAMPxTRG 1,
 *   while its input is high is a tamper event. It erases unless TAMPxNOERASE was set by an earlier
 *   write.
 * - ES0250 2.19.5: with DBG_RTC_STOP a held core stops the alarms, the subsecond alarm included,
 *   and no flag repeats during the hold.
 * - ES0250 2.19.6: a tamper event right after a TSF clear stamps as any other event, with no APB
 *   cycle window.
 * - ES0250 2.19.7: Stop 2 is not modelled, and RTC_REFIN and RTC_OUT on PB2 are outputs and inputs
 *   the model does not drive.
 * - Timestamps, RM0351 38.3.13: with `CR.TSE` an edge of RTC_TS on PC13, rising with TSEDGE 0 and
 *   falling with 1, captures the calendar at the board time of the edge, also when the GPIO applies
 *   it at a later block boundary, into `RTC_TSTR`, `RTC_TSDR` and `RTC_TSSSR` and sets TSF 2 x
 *   (PREDIV_A + 1) RTCCLK cycles later, the "2 ck_apre cycles" of 38.3.13 counted from the edge,
 *   inferred. An edge while TSF is set or still pending sets TSOVF at once and keeps the stamp, the
 *   38.3.13 note "TSOVF can be seen as '1' while TSF is still '0'". A TSF clear zeroes the three
 *   registers. TSF with TSIE drives EXTI line 19 and TAMP_STAMP_IRQn.
 * - An edge right after a TSF clear records a new stamp with TSF set and TSOVF 0, where the 38.3.13
 *   Caution sets both TSF and TSOVF for an event immediately after the clear.
 * - RTC_TS reads PC13 whatever its GPIO mode, RM0351 38.3.2: the board drive, else the PUPDR
 *   pull-up, inferred for an undriven pin. A PUPDR write and a TSEDGE write make no timestamp edge.
 * - The 32 backup registers keep their value through a system reset. `RCC_BDCR.BDRST`, a power-on
 *   and a read protection regression from level 1 to level 0 clear them, RM0351 38.6.20, 6.1.3 and
 *   3.5.1.
 * - The model holds no VDD or VBAT supply level, so the missed backup domain power-on reset of
 *   ES0250 2.2.13 has no trigger.
 * - ES0250 2.19.2: an SSR or TR read always holds TR and DR until DR is read, whatever the APB
 *   clock phase against the shadow copy.
 * - ES0250 2.19.3: alarm A and alarm B drive one level on EXTI line 18, so a request that sets
 *   while the other is still set makes no new edge. An `EXTI_PR1` clear takes effect at the write,
 *   with no delay window.
 * - ES0250 2.19.4: INITF sets at the second RTCCLK edge after INIT for every INIT entry, a
 *   consecutive one included.
 */
class rtc_l1l4 final : public dispatch_model<rtc_l1l4, rtc_l1l4_state>,
                       public clock_listener,
                       public backup_domain,
                       public core_hold_listener,
                       public read_protection_listener,
                       public board_input_listener,
                       public rtc_core
{
  friend rtc_core;

  static constexpr std::uint32_t wakeup_event = wakeup;

public:
  rtc_l1l4(
      model_context&                          ctx,
      peripheral_desc const&                  desc,
      std::optional<std::chrono::sys_seconds> initial,
      rtc_l1l4_variant const&                 variant);

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
  void on_read_protection_regression() override;
  void board_input_changed(std::string_view port, std::uint8_t pin, bool level) override;
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
  void stamp(std::uint64_t delay, std::uint64_t at_ps);
  [[nodiscard]] auto tamper_count() const -> std::size_t;
  [[nodiscard]] auto tamper_input(std::size_t which, bool precharged) const -> std::optional<bool>;
  [[nodiscard]] auto tamper_raised() const -> bool;
  void               detect(std::size_t which, std::uint64_t at_ps);
  void               tamper_fire(std::size_t which);
  void               raise_tamper(std::size_t which, bool erase);
  void               sample_tampers();
  void               plan_samples();
  void               clear_stamp();

  std::optional<std::chrono::sys_seconds> initial_;
  rtc_l1l4_variant                        variant_;
  register_file&                          regs_;
  reg_ref                                 time_;
  reg_ref                                 date_;
  std::optional<reg_ref>                  subsecond_;
  reg_ref                                 status_;
  reg_ref                                 control_;
  reg_ref                                 key_;
  field_ref                               init_;
  field_ref                               init_flag_;
  field_ref                               init_status_;
  field_ref                               shadow_flag_;
  std::optional<field_ref>                bypass_;
  field_ref                               prediv_a_;
  field_ref                               prediv_s_;
  field_ref                               format_;
  field_ref                               add_hour_;
  field_ref                               sub_hour_;
  std::uint32_t                           init_only_{};
  std::uint32_t                           open_flags_{};
  std::uint32_t                           keyed_flags_{};
  std::vector<std::uint32_t>              storage_;
  std::vector<std::uint32_t>              backups_;
  std::array<classic_alarm, 2>            alarms_{};
  field_ref                               wakeup_reload_;
  field_ref                               wakeup_clock_;
  field_ref                               wakeup_enable_;
  field_ref                               wakeup_interrupt_;
  field_ref                               wakeup_flag_;
  field_ref                               wakeup_write_;
  field_ref                               stamp_enable_;
  field_ref                               stamp_edge_;
  field_ref                               stamp_interrupt_;
  field_ref                               stamp_flag_;
  field_ref                               stamp_overflow_;
  struct tamper_fields
  {
    field_ref enable;
    field_ref trigger;
    field_ref interrupt;
    field_ref no_erase;
    field_ref mask;
    field_ref flag;
  };
  std::array<tamper_fields, 3>                       tamper_{};
  field_ref                                          tamper_interrupt_;
  field_ref                                          tamper_stamp_;
  field_ref                                          tamper_frequency_;
  field_ref                                          tamper_filter_;
  field_ref                                          tamper_pull_up_off_;
  std::array<unmodelled_entry, unmodelled_functions> unmodelled_{};
  std::array<bool, unmodelled_functions>             logged_{};
  rtc_calendar::counter<rtc_l1l4_state>              count_;
  core::clock_domain                                 domain_;
  bool                                               freeze_logged_  = false;
  bool                                               debugger_write_ = false;
};

constexpr std::array<write_entry<rtc_l1l4>, 11> classic_writes{
    {
     {"TR", &rtc_l1l4::time_written},
     {"DR", &rtc_l1l4::date_written},
     {"CR", &rtc_l1l4::control_written},
     {"ISR", &rtc_l1l4::status_written},
     {"PRER", &rtc_l1l4::prescaler_written},
     {"WUTR", &rtc_l1l4::locked_written<0>},
     {"ALRMAR", &rtc_l1l4::locked_written<1>},
     {"ALRMBR", &rtc_l1l4::locked_written<2>},
     {"WPR", &rtc_l1l4::key_written},
     {"TSTR", &rtc_l1l4::read_only_written<1>},
     {"TSDR", &rtc_l1l4::read_only_written<2>},
     }
};

constexpr std::array<write_entry<rtc_l1l4>, 5> subsecond_writes{
    {
     {"SSR", &rtc_l1l4::read_only_written<0>},
     {"SHIFTR", &rtc_l1l4::shift_written},
     {"TSSSR", &rtc_l1l4::read_only_written<3>},
     {"ALRMASSR", &rtc_l1l4::locked_written<3>},
     {"ALRMBSSR", &rtc_l1l4::locked_written<4>},
     }
};

constexpr std::array<write_entry<rtc_l1l4>, 1> smooth_writes{{{"CALR", &rtc_l1l4::smooth_written}}};

constexpr std::array<write_entry<rtc_l1l4>, 1> coarse_writes{
    {{"CALIBR", &rtc_l1l4::coarse_written}}};

constexpr std::array<write_entry<rtc_l1l4>, 1> option_writes{{{"OR", &rtc_l1l4::plain_written}}};

rtc_l1l4::rtc_l1l4(
    model_context&                          ctx,
    peripheral_desc const&                  desc,
    std::optional<std::chrono::sys_seconds> initial,
    rtc_l1l4_variant const&                 variant)
    : dispatch_model{ctx, desc, "RTC", classic_writes}, initial_{initial}, variant_{variant},
      regs_{ctx.regs}, time_{reg("TR")}, date_{reg("DR")}, status_{reg("ISR")}, control_{reg("CR")},
      key_{reg("WPR")}, init_{field("ISR", "INIT")}, init_flag_{field("ISR", "INITF")},
      init_status_{field("ISR", "INITS")}, shadow_flag_{field("ISR", "RSF")},
      prediv_a_{field("PRER", "PREDIV_A")}, prediv_s_{field("PRER", "PREDIV_S")},
      format_{field("CR", "FMT")}, add_hour_{field("CR", "ADD1H")}, sub_hour_{field("CR", "SUB1H")},
      wakeup_reload_{field("WUTR", "WUT")}, wakeup_clock_{field("CR", "WCKSEL")},
      wakeup_enable_{field("CR", "WUTE")}, wakeup_interrupt_{field("CR", "WUTIE")},
      wakeup_flag_{field("ISR", "WUTF")}, wakeup_write_{field("ISR", "WUTWF")},
      stamp_enable_{field("CR", "TSE")}, stamp_edge_{field("CR", "TSEDGE")},
      stamp_interrupt_{field("CR", "TSIE")}, stamp_flag_{field("ISR", "TSF")},
      stamp_overflow_{field("ISR", "TSOVF")},
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
  std::vector<write_entry<rtc_l1l4>> named{
      {variant.tamper_register, &rtc_l1l4::tamper_written}
  };
  for(auto const& described : desc.registers)
  {
    if(described.name.starts_with("BKP") and described.name.ends_with("R"))
    {
      named.push_back({described.name, &rtc_l1l4::plain_written});
      backups_.push_back(described.offset);
    }
  }
  bind(named);
  for(auto const& name :
      {std::string_view{"TSTR"}, std::string_view{"TSDR"}, std::string_view{"TSSSR"},
       std::string_view{"CALIBR"}, std::string_view{"CALR"}, std::string_view{"SHIFTR"},
       variant.tamper_register, std::string_view{"OR"}})
  {
    if(variant.timestamps and name.starts_with("TS"))
    {
      continue;
    }
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
  auto stamps = variant.timestamps ? std::uint32_t{} : bits("CR", {"TSE"});
  if(variant.internal_timestamp)
  {
    stamps |= bits("CR", {"ITSE"});
  }
  unmodelled_[timestamp_function] = {
      "CR", variant.timestamps ? "the internal timestamp" : "timestamps", stamps};
  unmodelled_[tamper_function] = {
      variant.tamper_register, "tamper detection", variant.tampers ? std::uint32_t{} : tampers};
  if(variant.tampers)
  {
    auto const& tamper_reg = variant.tamper_register;
    for(std::size_t which = 0; which < tamper_count(); ++which)
    {
      auto const name = [&](std::string_view suffix) {
        return std::format("TAMP{}{}", which + 1, suffix);
      };
      tamper_[which] = {
          .enable    = field(tamper_reg, name("E")),
          .trigger   = field(tamper_reg, name("TRG")),
          .interrupt = field(tamper_reg, name("IE")),
          .no_erase  = field(tamper_reg, name("NOERASE")),
          .mask      = field(tamper_reg, name("MF")),
          .flag      = field("ISR", name("F"))};
    }
    tamper_interrupt_   = field(tamper_reg, "TAMPIE");
    tamper_stamp_       = field(tamper_reg, "TAMPTS");
    tamper_frequency_   = field(tamper_reg, "TAMPFREQ");
    tamper_filter_      = field(tamper_reg, "TAMPFLT");
    tamper_pull_up_off_ = field(tamper_reg, "TAMPPUDIS");
  }
  unmodelled_[coarse_function] = {
      "CR", "coarse digital calibration",
      variant.coarse_calibration ? bits("CR", {"DCE"}) : std::uint32_t{}};
  unmodelled_[smooth_function] = {"CALR", "smooth digital calibration", ~std::uint32_t{}};
  unmodelled_[shift_function]  = {"SHIFTR", "the shift control", ~std::uint32_t{}};
  unmodelled_[output_function] = {
      "CR", "the RTC_CALIB and RTC_ALARM outputs", bits("CR", {"COE", "OSEL"})};
  unmodelled_[reference_function] = {
      "CR", "the reference clock detection", bits("CR", {"REFCKON"})};
}

auto rtc_l1l4::bypassed() const -> bool
{
  return bypass_ and get(regs_, *bypass_) != 0;
}

auto rtc_l1l4::keep_locked(reg_ref target, std::uint32_t old) -> bool
{
  if(unlocked())
  {
    return false;
  }
  regs_.poke(target.offset, old);
  return true;
}

void rtc_l1l4::settle(std::uint64_t cycle)
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

void rtc_l1l4::refuse_unmodelled(std::string_view name, std::uint32_t old, std::uint32_t now)
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

void rtc_l1l4::clear_backup(register_file& regs, bool board_calendar)
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

auto rtc_l1l4::next_alarm(std::size_t which, std::uint64_t after) const
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

void rtc_l1l4::plan_alarm(std::size_t which)
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

void rtc_l1l4::plan_all()
{
  plan_wakeup();
  plan_alarm(0);
  plan_alarm(1);
}

void rtc_l1l4::update_lines()
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
  if((variant_.timestamps and raised(stamp_flag_, stamp_interrupt_)) or tamper_raised())
  {
    levels |= 4u;
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
  if((changed & 4u) != 0)
  {
    exti->line_changed(variant_.tamper_line, (levels & 4u) != 0);
  }
}

void rtc_l1l4::alarm_enables_changed(std::uint32_t old, std::uint32_t now, std::uint64_t cycle)
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

void rtc_l1l4::restart_timers(std::uint64_t cycle)
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

void rtc_l1l4::reset(register_file& regs, reset_kind kind)
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

void rtc_l1l4::backup_reset()
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

auto rtc_l1l4::on_read(
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

auto rtc_l1l4::on_write(
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

void rtc_l1l4::on_event(std::uint32_t kind, std::uint64_t payload)
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
    case timestamp_ready:
      if(std::exchange(state_.stamp_pending, 0) != 0)
      {
        regs_.poke(reg("TSTR").offset, state_.stamp_time);
        regs_.poke(reg("TSDR").offset, state_.stamp_date);
        if(subsecond_)
        {
          regs_.poke(reg("TSSSR").offset, state_.stamp_subsecond);
        }
        update(regs_, stamp_flag_, 1);
      }
      break;
    case tamper1_ready:
    case tamper2_ready:
    case tamper3_ready:
      tamper_fire(static_cast<std::size_t>(kind - tamper1_ready));
      break;
    case tamper_sample:
      sample_tampers();
      plan_samples();
      break;
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

void rtc_l1l4::on_core_hold(bool held)
{
  if(not variant_.debug_freeze)
  {
    return;
  }
  hold_core(held);
}

void rtc_l1l4::board_input_changed(std::string_view port, std::uint8_t pin, bool level)
{
  if(not variant_.timestamps and not variant_.tampers)
  {
    return;
  }
  auto const now = now_ps();
  settle(count_.cycle_at(now));
  auto const* const clock = dynamic_cast<pin_edge_clock const*>(ctx_.peers.find(port));
  auto const        time  = ctx_.clock.time().ps;
  auto const        late  = clock != nullptr ? time - std::min(clock->edge_ps(), time) : 0;
  auto const        at    = now - std::min(late, now);
  if(variant_.tampers and get(regs_, tamper_filter_) == 0)
  {
    for(std::size_t which = 0; which < tamper_count(); ++which)
    {
      auto const& t = tamper_[which];
      if(port == variant_.tamper_ports[which]
         and pin == variant_.tamper_pins[which]
         and get(regs_, t.enable) != 0
         and level != (get(regs_, t.trigger) != 0))
      {
        detect(which, at);
      }
    }
  }
  if(variant_.timestamps
     and port == variant_.timestamp_port
     and pin == variant_.timestamp_pin
     and get(regs_, stamp_enable_) != 0
     and level != (get(regs_, stamp_edge_) != 0))
  {
    stamp(stamp_apre_cycles, at);
  }
  update_lines();
}

auto rtc_l1l4::tamper_count() const -> std::size_t
{
  return variant_.three_tampers ? 3 : 1;
}

auto rtc_l1l4::tamper_input(std::size_t which, bool precharged) const -> std::optional<bool>
{
  auto const* const peer   = ctx_.peers.find(variant_.tamper_ports[which]);
  auto const* const source = dynamic_cast<board_input_source const*>(peer);
  auto const* const port   = dynamic_cast<gpio_port const*>(peer);
  auto const        pin    = variant_.tamper_pins[which];
  if(source == nullptr or port == nullptr or (port->bonded_pins() >> pin & 1u) == 0)
  {
    return std::nullopt;
  }
  if(auto const drive = source->board_drive(pin))
  {
    return drive;
  }
  return (precharged and get(regs_, tamper_pull_up_off_) == 0) or source->board_level(pin);
}

auto rtc_l1l4::tamper_raised() const -> bool
{
  if(not variant_.tampers)
  {
    return false;
  }
  auto const all = get(regs_, tamper_interrupt_) != 0;
  for(std::size_t which = 0; which < tamper_count(); ++which)
  {
    auto const& t = tamper_[which];
    if(get(regs_, t.flag) != 0 and (all or get(regs_, t.interrupt) != 0))
    {
      return true;
    }
  }
  return false;
}

void rtc_l1l4::detect(std::size_t which, std::uint64_t at_ps)
{
  auto const& t   = tamper_[which];
  auto const  bit = static_cast<std::uint8_t>(1u << which);
  if((state_.tamper_pending & bit) != 0 or (get(regs_, t.flag) != 0 and get(regs_, t.mask) == 0))
  {
    return;
  }
  auto const filtered = get(regs_, tamper_filter_) != 0;
  auto const stamped  = get(regs_, tamper_stamp_) != 0;
  if(stamped)
  {
    stamp(tamper_apre_cycles, at_ps);
  }
  if(not filtered and not stamped)
  {
    tamper_fire(which);
    return;
  }
  state_.tamper_pending |= bit;
  arm(static_cast<classic_event>(tamper1_ready + which),
      count_.cycle_at(at_ps) + tamper_apre_cycles * (std::uint64_t{state_.prediv_a} + 1));
}

void rtc_l1l4::tamper_fire(std::size_t which)
{
  auto const& t = tamper_[which];
  state_.tamper_pending &= static_cast<std::uint8_t>(~(1u << which));
  if(get(regs_, t.mask) != 0)
  {
    return;
  }
  raise_tamper(which, get(regs_, t.no_erase) == 0);
}

void rtc_l1l4::raise_tamper(std::size_t which, bool erase)
{
  update(regs_, tamper_[which].flag, 1);
  if(erase)
  {
    for(auto const offset : backups_)
    {
      regs_.poke(offset, 0);
    }
  }
}

void rtc_l1l4::sample_tampers()
{
  auto const needed = std::uint8_t{1} << get(regs_, tamper_filter_);
  for(std::size_t which = 0; which < tamper_count(); ++which)
  {
    auto const& t     = tamper_[which];
    auto const  input = tamper_input(which, true);
    auto&       count = state_.tamper_samples[which];
    if(get(regs_, t.enable) == 0 or not input or *input != (get(regs_, t.trigger) != 0))
    {
      count = 0;
      continue;
    }
    count = static_cast<std::uint8_t>(std::min<unsigned>(count + 1u, needed));
    if(count >= needed)
    {
      count = 0;
      detect(which, now_ps());
    }
  }
}

void rtc_l1l4::plan_samples()
{
  state_.armed[tamper_sample] = 0;
  ++state_.generation[tamper_sample];
  if(not variant_.tampers or get(regs_, tamper_filter_) == 0)
  {
    return;
  }
  auto enabled = false;
  for(std::size_t which = 0; which < tamper_count(); ++which)
  {
    enabled = enabled or get(regs_, tamper_[which].enable) != 0;
  }
  if(not enabled)
  {
    return;
  }
  auto const period = std::uint64_t{1} << (15u - get(regs_, tamper_frequency_));
  auto const cycle  = count_.cycle_at(now_ps());
  arm(tamper_sample, (cycle / period + 1) * period);
}

void rtc_l1l4::stamp(std::uint64_t delay, std::uint64_t at_ps)
{
  if(get(regs_, stamp_flag_) != 0 or state_.stamp_pending != 0)
  {
    update(regs_, stamp_overflow_, 1);
    return;
  }
  auto const cycle       = count_.cycle_at(at_ps);
  auto const position    = count_.position_at(cycle);
  state_.stamp_time      = count_.time_register(position);
  state_.stamp_date      = count_.date_register(position);
  state_.stamp_subsecond = subsecond_ ? count_.subsecond_register(position) : 0;
  state_.stamp_pending   = 1;
  arm(timestamp_ready, cycle + delay * (std::uint64_t{state_.prediv_a} + 1));
}

void rtc_l1l4::clear_stamp()
{
  for(auto const name : {"TSTR", "TSDR", "TSSSR"})
  {
    if(has_register(name))
    {
      regs_.poke(reg(name).offset, 0);
    }
  }
}

void rtc_l1l4::on_read_protection_regression()
{
  for(auto const offset : backups_)
  {
    regs_.poke(offset, 0);
  }
}

auto rtc_l1l4::storage_only(std::uint32_t offset) const -> bool
{
  return std::ranges::contains(storage_, offset);
}

void rtc_l1l4::on_clock_change()
{
  clock_changed();
}

void rtc_l1l4::status_written(std::uint32_t old, std::uint32_t now)
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
  if(variant_.timestamps and stamp_flag_.of(old) != 0 and stamp_flag_.of(word) == 0)
  {
    clear_stamp();
  }
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

void rtc_l1l4::control_written(std::uint32_t old, std::uint32_t now)
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

void rtc_l1l4::coarse_written(std::uint32_t old, std::uint32_t)
{
  if(not unlocked() or not initializing())
  {
    regs_.poke(reg("CALIBR").offset, old);
  }
}

void rtc_l1l4::smooth_written(std::uint32_t old, std::uint32_t now)
{
  auto const target = reg("CALR");
  if(keep_locked(target, old))
  {
    return;
  }
  refuse_unmodelled("CALR", old, now);
}

void rtc_l1l4::shift_written(std::uint32_t old, std::uint32_t now)
{
  auto const target = reg("SHIFTR");
  if(not keep_locked(target, old))
  {
    refuse_unmodelled("SHIFTR", old, now);
  }
  regs_.poke(target.offset, 0);
}

void rtc_l1l4::tamper_written(std::uint32_t old, std::uint32_t now)
{
  refuse_unmodelled(variant_.tamper_register, old, now);
  if(not variant_.tampers)
  {
    return;
  }
  for(std::size_t which = 0; which < tamper_count(); ++which)
  {
    auto const& t = tamper_[which];
    if(t.enable.of(old) != 0
       and t.enable.of(now) == 0
       and tamper_filter_.of(old) == 0
       and t.trigger.of(old) != 0
       and tamper_input(which, false).value_or(false))
    {
      if(get(regs_, t.flag) == 0 and t.mask.of(old) == 0)
      {
        raise_tamper(which, t.no_erase.of(old) == 0);
      }
    }
  }
  plan_samples();
  update_lines();
}

template <std::size_t Index> void rtc_l1l4::locked_written(std::uint32_t old, std::uint32_t)
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

template <std::size_t Index> void rtc_l1l4::read_only_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(reg(read_only_names[Index]).offset, old);
}

void rtc_l1l4::plain_written(std::uint32_t, std::uint32_t)
{}
}

auto rtc_l1l4_factory(std::optional<std::chrono::sys_seconds> initial, rtc_l1l4_variant variant)
    -> model_factory
{
  return [initial,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rtc_l1l4>(ctx, desc, initial, variant);
  };
}
}
