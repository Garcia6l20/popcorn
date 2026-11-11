#include "adc4_rows.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t regulator_end      = 0;
constexpr std::uint32_t ready_end          = 1;
constexpr std::uint32_t calibration_end    = 2;
constexpr std::uint32_t disable_end        = 3;
constexpr std::uint32_t calibration_factor = 0;
constexpr std::uint16_t reserved_divider   = 256;

using wide = unsigned __int128;

[[nodiscard]] constexpr auto cycles_ps(std::uint64_t cycles, std::uint64_t hz) -> std::uint64_t
{
  return static_cast<std::uint64_t>((wide{cycles} * ps_per_second + hz - 1u) / hz);
}

struct adc4_state
{
  std::uint32_t regulator_generation{};
  std::uint32_t ready_generation{};
  std::uint32_t calibration_generation{};
  std::uint32_t disable_generation{};
  bool          regulator_ready{};
  bool          ready_waiting{};
  bool          calibration_waiting{};
  bool          disable_waiting{};
  std::uint64_t enable_lock_ps{};
  std::uint64_t regulator_ps{};
  std::uint64_t calibration_ps{};

  friend auto operator==(adc4_state const&, adc4_state const&) -> bool = default;
};

struct refusal
{
  bool*            logged{};
  std::string_view message;
};

/**
 * @brief The ADC4 of the STM32U5, `aditf4_v4_U5_Cube`, RM0456 section 34: the voltage
 *        regulator, the calibration, the enable and disable sequence, the ADC clock and the
 *        interrupt, in virtual time. ADC_CCR sits at offset 0x308 of the ADC4 block.
 *
 *        Voltage regulator, RM0456 34.4.3: an ADC_CR write taking ADVREGEN from 0 to 1 sets LDORDY
 *        tADCVREG_STUP later, 25 us, the DS13737 Table 106 and DS13086 Table 108 maximum. LDORDY is
 *        rc_w1 and stays set when ADVREGEN clears, as the disable sequence of 34.4.3 has software
 *        clear it. ADVREGEN 0 drops a pending LDORDY and resets the calibration factor, ADC_CALFACT
 *        reading 0. ADVREGEN written 0 with ADEN or ADCAL set keeps ADVREGEN at 1, RM0456 34.7.3.
 *
 *        Calibration, RM0456 34.4.4: an ADC_CR write setting ADCAL with ADEN 0, AUTOFF 0, ADVREGEN
 *        1 and the regulator ready starts it. The regulator is ready from its LDORDY event on,
 *        whether software cleared the flag, inferred. ADCAL stays set for tOFF_CAL, 123 ADC clock
 *        cycles, the DS13737 Table 106 and DS13086 Table 108 typical value, then clears, EOCAL
 *        sets, and ADC_CALFACT and ADC_DR read the factor, 0 for a converter with no offset,
 *        inferred. ADC_DR reads 0 from the ADCAL write, Figure 279. Without an ADC clock the
 *        calibration waits for one, logged once. A write changing ADC_CALFACT needs ADEN 1, ADCAL 0
 *        and ADSTART 0, RM0456 34.7.16. Conversions ignore the factor.
 *
 *        On-off control, RM0456 34.4.5: an ADC_CR write setting ADEN sets ADRDY tSTAB later, 3 ADC
 *        clock cycles and one conversion, DS13737 Table 106 and DS13086 Table 108, here 17 ADC
 *        clock cycles, a 12-bit conversion at SMP 000, inferred. With the regulator not yet ready
 *        ADRDY waits for LDORDY, then tSTAB, inferred. ADEN written with ADVREGEN 0 sets ADVREGEN
 *        and starts the regulator, ES0499 2.9.2, its timing inferred as above. With AUTOFF 1 ADRDY
 *        does not set, 34.4.5 Note. An ADEN write while ADCAL is set or within 4 ADC clock cycles
 *        after it clears is lost, ADEN reading 0, 34.4.5 Caution. ADDIS with ADEN 1 and ADSTART 0
 *        clears ADEN and ADDIS 6 ADC clock cycles later, ES0499 2.9.2 for its startup sequence,
 *        inferred for every other use, and drops a pending ADRDY. ADSTP with ADSTART 0 clears
 *        itself, the write 34.4.13 Note and the 34.4.5 disable sequence ask for after a software-
 *        triggered sequence. ADRDY stays set, 34.4.5. ADEN, ADDIS, ADSTART, ADSTP and ADCAL are rs:
 *        a write of 0 keeps them. Each ADEN edge with the ADC clock outside 0.14 to 55 MHz, fADC of
 *        DS13737 Table 106, logs a warning.
 *
 *        ADC clock, RM0456 34.4.6: the `adc4` kernel clock of RCC_CCIPR3.ADCDACSEL, gated by
 *        ADC4EN, divided by ADC_CCR.PRESC. A reserved PRESC, 1100 to 1111, divides by 256 and logs
 *        once, a model_error under strict_peripherals that keeps the old ADC_CCR. A PRESC write
 *        with ADEN or ADCAL set keeps the old PRESC, RM0456 34.7.18.
 *
 *        ADC_ISR flags clear on a write of 1. ADC4_IRQn is high while a flag of ADC_ISR is set with
 *        its enable bit of ADC_IER, RM0456 34.6. A write changing ADC_PWRR needs ADEN 0, RM0456
 *        34.7.13. An RCC reset of ADC4 restores every register and drops every pending event.
 *
 *        Forbidden writes of RM0456 34.4.8 and 34.7.3 are ignored and logged once each, a
 *        model_error under strict_peripherals that keeps the old register: ADCAL as above, ADSTART
 *        with ADEN 0 or ADDIS 1, ADDIS with ADEN 0 or ADSTART 1, ADSTP with ADEN 0 or ADDIS 1,
 *        ADC_CALFACT, ADC_PWRR and PRESC as above.
 *
 *        Not modelled, logged once, a model_error under strict_peripherals that keeps the old
 *        ADC_CR: the conversions. ADSTART with ADEN 1 and ADDIS 0 stays set and converts nothing,
 *        and ADSTP clears it with itself.
 */
class adc4_model final : public dispatch_model<adc4_model, adc4_state>,
                         public clock_listener,
                         public bus_reset_listener
{
  using base = dispatch_model<adc4_model, adc4_state>;

public:
  adc4_model(model_context& ctx, peripheral_desc const& desc, adc4_variant variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_clock_change() override;

  void isr_written(std::uint32_t old, std::uint32_t now);
  void cr_written(std::uint32_t old, std::uint32_t now);
  void calfact_written(std::uint32_t old, std::uint32_t now);
  void pwrr_written(std::uint32_t old, std::uint32_t now);
  void ccr_written(std::uint32_t old, std::uint32_t now);
  void ier_written(std::uint32_t, std::uint32_t)
  {
    signal();
  }

private:
  void               restore_reset_values();
  [[nodiscard]] auto divider() const -> std::uint32_t;
  [[nodiscard]] auto adc_hz() const -> std::uint64_t;
  [[nodiscard]] auto no_clock() -> bool;
  void               check_clock_range();
  void               regulator_written(std::uint32_t old, std::uint32_t now);
  void               try_ready(std::uint64_t from_ps);
  void               start_calibration();
  void               end_calibration();
  void               start_disable();
  void               signal();
  void               forbidden(bool& logged, std::string_view name, std::string_view message);

  register_file&     regs_;
  adc4_variant       variant_;
  core::clock_domain kernel_;
  irq_line&          line_;
  reg_ref            isr_;
  reg_ref            ier_;
  reg_ref            control_;
  reg_ref            calfact_reg_;
  reg_ref            pwrr_;
  reg_ref            ccr_;
  field_ref          regulator_ready_;
  field_ref          calibration_done_;
  field_ref          ready_;
  field_ref          calibrate_;
  field_ref          regulator_;
  field_ref          stop_;
  field_ref          start_;
  field_ref          disable_;
  field_ref          enable_;
  field_ref          data_;
  field_ref          auto_off_;
  field_ref          factor_;
  field_ref          prescaler_;
  std::uint32_t      clearable_{};
  std::uint32_t      set_only_{};
  bool               regulator_refused_logged_{};
  bool               calibration_refused_logged_{};
  bool               enable_lost_logged_{};
  bool               start_refused_logged_{};
  bool               disable_refused_logged_{};
  bool               stop_refused_logged_{};
  bool               conversion_logged_{};
  bool               calfact_refused_logged_{};
  bool               pwrr_refused_logged_{};
  bool               prescaler_refused_logged_{};
  bool               reserved_logged_{};
  bool               no_clock_logged_{};
};

constexpr std::array<write_entry<adc4_model>, 7> writes{
    {
     {"ISR", &adc4_model::isr_written},
     {"IER", &adc4_model::ier_written},
     {"CR", &adc4_model::cr_written},
     {"DR", nullptr},
     {"PWR", &adc4_model::pwrr_written},
     {"CALFACT", &adc4_model::calfact_written},
     {"CCR", &adc4_model::ccr_written},
     }
};

adc4_model::adc4_model(model_context& ctx, peripheral_desc const& desc, adc4_variant variant)
    : base{ctx, desc, "ADC4", writes}, regs_{ctx.regs}, variant_{variant},
      kernel_{ctx.clock.domain(lower(desc.name))}, line_{irq(desc.name + "_IRQn")},
      isr_{reg("ISR")}, ier_{reg("IER")}, control_{reg("CR")}, calfact_reg_{reg("CALFACT")},
      pwrr_{reg("PWR")}, ccr_{reg("CCR")}, regulator_ready_{field("ISR", "LDORDY")},
      calibration_done_{field("ISR", "EOCAL")}, ready_{field("ISR", "ADRDY")},
      calibrate_{field("CR", "ADCAL")}, regulator_{field("CR", "ADVREGEN")},
      stop_{field("CR", "ADSTP")}, start_{field("CR", "ADSTART")}, disable_{field("CR", "ADDIS")},
      enable_{field("CR", "ADEN")}, data_{field("DR", "DATA")}, auto_off_{field("PWR", "AUTOFF")},
      factor_{field("CALFACT", "CALFACT")}, prescaler_{field("CCR", "PRESC")}
{
  for(auto const& name :
      {"LDORDY", "EOCAL", "AWD3", "AWD2", "AWD1", "OVR", "EOS", "EOC", "EOSMP", "ADRDY"})
  {
    clearable_ |= field("ISR", name).mask();
  }
  set_only_ = calibrate_.mask() | stop_.mask() | start_.mask() | disable_.mask() | enable_.mask();
}

void adc4_model::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void adc4_model::on_bus_reset()
{
  restore_reset_values();
}

void adc4_model::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  ++state_.ready_generation;
  state_.regulator_ready     = false;
  state_.ready_waiting       = false;
  state_.calibration_waiting = false;
  state_.disable_waiting     = false;
  state_.enable_lock_ps      = 0;
  signal();
}

auto adc4_model::divider() const -> std::uint32_t
{
  auto const code = get(regs_, prescaler_);
  return code < variant_.prescalers.size() ? variant_.prescalers[code] : reserved_divider;
}

auto adc4_model::adc_hz() const -> std::uint64_t
{
  return ctx_.clock.hz(kernel_) / divider();
}

auto adc4_model::no_clock() -> bool
{
  if(adc_hz() != 0)
  {
    return false;
  }
  if(not std::exchange(no_clock_logged_, true))
  {
    warn("CR", "no ADC4 kernel clock: ADRDY, the calibration and ADDIS wait for it");
  }
  return true;
}

void adc4_model::check_clock_range()
{
  auto const hz = adc_hz();
  if(hz != 0 and (hz < variant_.min_hz or hz > variant_.max_hz))
  {
    warn(
        "CR", std::format(
                  "ADC clock {} Hz is outside {} to {} Hz, DS13737 Table 106", hz, variant_.min_hz,
                  variant_.max_hz));
  }
}

void adc4_model::isr_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(isr_.offset, old & ~(now & clearable_));
  signal();
}

void adc4_model::cr_written(std::uint32_t old, std::uint32_t now)
{
  now |= old & set_only_;
  std::array<refusal, 6> refused{};
  std::size_t            count = 0;
  auto const rises = [&](field_ref const& bit) { return bit.of(now) != 0 and bit.of(old) == 0; };
  if(regulator_.of(now) == 0
     and regulator_.of(old) != 0
     and (enable_.of(old) != 0 or calibrate_.of(old) != 0))
  {
    now              = regulator_.with(now, 1);
    refused[count++] = {
        &regulator_refused_logged_,
        "ADVREGEN cleared with ADEN or ADCAL set, RM0456 34.7.3: ADVREGEN stays 1"};
  }
  auto const calibration_blocked = enable_.of(old) != 0
                                or regulator_.of(now) == 0
                                or not state_.regulator_ready
                                or get(regs_, auto_off_) != 0;
  if(rises(calibrate_) and calibration_blocked)
  {
    now              = calibrate_.with(now, 0);
    refused[count++] = {
        &calibration_refused_logged_,
        "ADCAL written with ADEN 1, AUTOFF 1 or the regulator off or not ready, RM0456 34.4.4: "
        "the write is ignored"};
  }
  if(rises(enable_) and (calibrate_.of(now) != 0 or ctx_.clock.time().ps < state_.enable_lock_ps))
  {
    now              = enable_.with(now, 0);
    refused[count++] = {
        &enable_lost_logged_,
        "ADEN set while ADCAL is set or within 4 ADC clock cycles after it clears, RM0456 34.4.5: "
        "ADEN is cleared and the ADC stays disabled"};
  }
  if(rises(start_) and (enable_.of(old) == 0 or disable_.of(now) != 0))
  {
    now              = start_.with(now, 0);
    refused[count++] = {
        &start_refused_logged_,
        "ADSTART written with ADEN 0 or ADDIS 1, RM0456 34.7.3: the write is ignored"};
  }
  if(rises(disable_) and (enable_.of(old) == 0 or start_.of(now) != 0))
  {
    now              = disable_.with(now, 0);
    refused[count++] = {
        &disable_refused_logged_,
        "ADDIS written with ADEN 0 or ADSTART 1, RM0456 34.7.3: the write is ignored"};
  }
  if(rises(stop_) and (enable_.of(old) == 0 or disable_.of(now) != 0))
  {
    now              = stop_.with(now, 0);
    refused[count++] = {
        &stop_refused_logged_,
        "ADSTP written with ADEN 0 or ADDIS 1, RM0456 34.4.8: the write is ignored"};
  }
  if(count != 0 and ctx_.strict_peripherals)
  {
    regs_.poke(control_.offset, old);
  }
  for(auto const& entry : std::span{refused}.first(count))
  {
    forbidden(*entry.logged, "CR", entry.message);
  }
  if(rises(start_))
  {
    if(ctx_.strict_peripherals)
    {
      regs_.poke(control_.offset, old);
    }
    warn_once_or_refuse(
        conversion_logged_, "CR", ctx_.strict_peripherals ? control_.offset : 0u,
        "ADSTART: the ADC4 conversions not modelled, ADSTART stays set until ADSTP");
  }
  auto const powered = rises(enable_) and regulator_.of(now) == 0;
  if(powered)
  {
    now = regulator_.with(now, 1);
  }
  if(stop_.of(now) != 0)
  {
    now = start_.with(stop_.with(now, 0), 0);
  }
  regs_.poke(control_.offset, now);
  regulator_written(old, now);
  if(rises(calibrate_))
  {
    update(regs_, data_, 0);
    check_clock_range();
    start_calibration();
  }
  if(rises(enable_))
  {
    state_.ready_waiting = true;
    check_clock_range();
    try_ready(ctx_.clock.time().ps);
  }
  if(rises(disable_))
  {
    ++state_.ready_generation;
    state_.ready_waiting = false;
    start_disable();
  }
  signal();
}

void adc4_model::regulator_written(std::uint32_t old, std::uint32_t now)
{
  if(regulator_.of(now) == regulator_.of(old))
  {
    return;
  }
  ++state_.regulator_generation;
  state_.regulator_ready = false;
  if(regulator_.of(now) == 0)
  {
    update(regs_, factor_, 0);
    return;
  }
  state_.regulator_ps = ctx_.clock.time().ps + variant_.regulator_setup_ns * 1000u;
  ctx_.events.schedule_at(
      vtime{state_.regulator_ps}, regulator_end, state_.regulator_generation, ctx_.clock);
}

void adc4_model::try_ready(std::uint64_t from_ps)
{
  if(not state_.ready_waiting
     or not state_.regulator_ready
     or get(regs_, enable_) == 0
     or get(regs_, disable_) != 0
     or no_clock())
  {
    return;
  }
  state_.ready_waiting = false;
  ctx_.events.schedule_at(
      vtime{from_ps + cycles_ps(variant_.stabilisation_cycles, adc_hz())}, ready_end,
      ++state_.ready_generation, ctx_.clock);
}

void adc4_model::start_calibration()
{
  ++state_.calibration_generation;
  state_.calibration_waiting = no_clock();
  if(state_.calibration_waiting)
  {
    return;
  }
  state_.calibration_ps = ctx_.clock.time().ps + cycles_ps(variant_.calibration_cycles, adc_hz());
  ctx_.events.schedule_at(
      vtime{state_.calibration_ps}, calibration_end, state_.calibration_generation, ctx_.clock);
}

void adc4_model::end_calibration()
{
  update(regs_, calibrate_, 0);
  update(regs_, calibration_done_, 1);
  update(regs_, factor_, calibration_factor);
  update(regs_, data_, calibration_factor);
  auto const hz = adc_hz();
  state_.enable_lock_ps =
      state_.calibration_ps + (hz == 0 ? 0 : cycles_ps(variant_.enable_lockout_cycles, hz));
  signal();
}

void adc4_model::start_disable()
{
  ++state_.disable_generation;
  state_.disable_waiting = no_clock();
  if(state_.disable_waiting)
  {
    return;
  }
  ctx_.events.schedule_at(
      vtime{ctx_.clock.time().ps + cycles_ps(variant_.disable_cycles, adc_hz())}, disable_end,
      state_.disable_generation, ctx_.clock);
}

void adc4_model::calfact_written(std::uint32_t old, std::uint32_t now)
{
  if(now == old
     or (get(regs_, enable_) != 0 and get(regs_, calibrate_) == 0 and get(regs_, start_) == 0))
  {
    return;
  }
  regs_.poke(calfact_reg_.offset, old);
  forbidden(
      calfact_refused_logged_, "CALFACT",
      "CALFACT written with ADEN 0, ADCAL 1 or ADSTART 1, RM0456 34.7.16: the write is ignored");
}

void adc4_model::pwrr_written(std::uint32_t old, std::uint32_t now)
{
  if(now == old or get(regs_, enable_) == 0)
  {
    return;
  }
  regs_.poke(pwrr_.offset, old);
  forbidden(
      pwrr_refused_logged_, "PWR",
      "ADC_PWRR written with ADEN 1, RM0456 34.7.13: the write is ignored");
}

void adc4_model::ccr_written(std::uint32_t old, std::uint32_t now)
{
  if(prescaler_.of(now) == prescaler_.of(old))
  {
    return;
  }
  if(get(regs_, enable_) != 0 or get(regs_, calibrate_) != 0)
  {
    if(ctx_.strict_peripherals)
    {
      regs_.poke(ccr_.offset, old);
    }
    else
    {
      update(regs_, prescaler_, prescaler_.of(old));
    }
    forbidden(
        prescaler_refused_logged_, "CCR",
        "PRESC written with ADEN 1 or ADCAL 1, RM0456 34.7.18: the old PRESC stays");
    return;
  }
  if(prescaler_.of(now) >= variant_.prescalers.size())
  {
    if(ctx_.strict_peripherals)
    {
      regs_.poke(ccr_.offset, old);
    }
    forbidden(
        reserved_logged_, "CCR",
        std::format(
            "PRESC {:#06b}: a reserved prescaler not modelled, divided by {}", prescaler_.of(now),
            reserved_divider));
  }
}

void adc4_model::signal()
{
  if((regs_.peek(isr_.offset) & regs_.peek(ier_.offset) & clearable_) != 0)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void adc4_model::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == regulator_end)
  {
    if(payload == state_.regulator_generation and get(regs_, regulator_) != 0)
    {
      state_.regulator_ready = true;
      update(regs_, regulator_ready_, 1);
      try_ready(state_.regulator_ps);
      signal();
    }
    return;
  }
  if(kind == ready_end)
  {
    if(payload == state_.ready_generation and get(regs_, auto_off_) == 0)
    {
      update(regs_, ready_, 1);
      signal();
    }
    return;
  }
  if(kind == calibration_end)
  {
    if(payload == state_.calibration_generation and get(regs_, calibrate_) != 0)
    {
      end_calibration();
    }
    return;
  }
  if(kind == disable_end and payload == state_.disable_generation and get(regs_, disable_) != 0)
  {
    update(regs_, disable_, 0);
    update(regs_, enable_, 0);
    signal();
  }
}

void adc4_model::on_clock_change()
{
  if(adc_hz() == 0)
  {
    return;
  }
  if(state_.calibration_waiting and get(regs_, calibrate_) != 0)
  {
    start_calibration();
  }
  if(state_.disable_waiting and get(regs_, disable_) != 0)
  {
    start_disable();
  }
  try_ready(ctx_.clock.time().ps);
}

void adc4_model::forbidden(bool& logged, std::string_view name, std::string_view message)
{
  warn_once_or_refuse(
      logged, name, ctx_.strict_peripherals ? reg(name).offset : 0u, std::string{message});
}

constexpr auto adc4_row_table = std::to_array<builtin>({
    row_of<^^adc4_factory, ^^u5_adc4>(
        {.ip = "ADC", .ip_version_glob = "aditf4_v4*", .families = family::u5}),
});
}

auto adc4_factory(adc4_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc4_model>(ctx, desc, variant);
  };
}

auto adc4_rows() -> std::span<builtin const>
{
  return adc4_row_table;
}
}
