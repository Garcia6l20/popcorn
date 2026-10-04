#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr address       ts_cal1_address     = 0x0BFA'0710;
constexpr address       vrefint_cal_address = 0x0BFA'07A5;
constexpr std::uint32_t vrefint_channel     = 0;
constexpr std::uint32_t first_external      = 1;
constexpr std::uint32_t last_external       = 17;
constexpr std::uint32_t sensor_channel      = 19;
constexpr std::size_t   channels            = 20;
constexpr std::size_t   offsets             = 4;
constexpr std::uint32_t full_scale          = 1u << 14;
constexpr std::uint32_t factory_vdda_mv     = 3000;
constexpr std::uint64_t min_adc_hz          = 5'000'000;
constexpr std::uint64_t max_adc_hz          = 55'000'000;
constexpr std::uint64_t ps_per_second       = 1'000'000'000'000;
constexpr std::uint32_t conversion_end      = 0;

using wide = unsigned __int128;

constexpr std::array<std::uint32_t, 8>  sampling_cycles{5, 6, 12, 20, 36, 68, 391, 814};
constexpr std::array<std::uint32_t, 4>  sar_cycles{17, 15, 13, 11};
constexpr std::array<std::uint32_t, 12> prescalers{1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256};

struct adc_state
{
  std::array<std::uint32_t, channels> input_uv{};
  std::uint32_t                       generation{};
  std::uint64_t                       start_ps{};
  std::uint64_t                       clock_hz{};
  std::uint32_t                       divider{};
  std::uint32_t                       held{};
  std::uint32_t                       sum{};
  bool                                converting{};
  bool                                waiting_clock{};

  friend auto operator==(adc_state const&, adc_state const&) -> bool = default;
};

struct adc_common_state
{
  friend auto operator==(adc_common_state const&, adc_common_state const&) -> bool = default;
};

[[nodiscard]] auto factory_halfword(mcu_description const& part, address at)
    -> std::optional<std::uint16_t>
{
  for(auto const& region : part.regions)
  {
    if(not region.content)
    {
      continue;
    }
    for(auto const& piece : *region.content)
    {
      auto const first = region.start + piece.offset;
      if(at >= first and at + 2 <= first + piece.data.size())
      {
        auto const index = static_cast<std::size_t>(at - first);
        return static_cast<std::uint16_t>(piece.data[index] | (piece.data[index + 1] << 8));
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto code14(std::uint64_t microvolts, std::uint32_t vdda_mv)
    -> std::uint32_t
{
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      full_scale - 1u, microvolts * full_scale / (std::uint64_t{vdda_mv} * 1000u)));
}

[[nodiscard]] constexpr auto at_vdda(std::uint32_t factory_code, std::uint32_t vdda_mv)
    -> std::uint32_t
{
  return std::min<std::uint32_t>(full_scale - 1u, factory_code * factory_vdda_mv / vdda_mv);
}

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/**
 * @brief ADC12 common registers of the STM32U5, RM0456 section 33.7: ADC12_CCR connects
 *        VREFINT with VREFEN and the temperature sensor with VSENSESEL to the ADC1 internal
 *        channels, and divides the ADC kernel clock by PRESC. Storage otherwise.
 */
class adc_common final : public dispatch_model<adc_common, adc_common_state>
{
public:
  adc_common(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void ccr_written(std::uint32_t, std::uint32_t)
  {}

  [[nodiscard]] auto vrefint_connected() const -> bool
  {
    return get(regs_, vrefint_) != 0;
  }

  [[nodiscard]] auto sensor_connected() const -> bool
  {
    return get(regs_, sensor_) != 0;
  }

  [[nodiscard]] auto divider() -> std::uint32_t
  {
    auto const code = get(regs_, prescaler_);
    if(code >= prescalers.size())
    {
      if(not std::exchange(reserved_logged_, true))
      {
        warn(
            "ADC12_CCR",
            std::format("PRESC {:#06b}: a reserved prescaler not modelled, divided by 256", code));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, "ADC12_CCR", reg("ADC12_CCR").offset);
      }
    }
    return prescalers[std::min<std::size_t>(code, prescalers.size() - 1)];
  }

private:
  register_file& regs_;
  field_ref      vrefint_;
  field_ref      sensor_;
  field_ref      prescaler_;
  bool           reserved_logged_{};
};

constexpr std::array<write_entry<adc_common>, 1> adc_common_writes{
    {
     {"ADC12_CCR", &adc_common::ccr_written},
     }
};

adc_common::adc_common(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "ADC12", adc_common_writes}, regs_{ctx.regs},
      vrefint_{field("ADC12_CCR", "VREFEN")}, sensor_{field("ADC12_CCR", "VSENSESEL")},
      prescaler_{field("ADC12_CCR", "PRESC")}
{}

void adc_common::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

struct offset_fields
{
  std::string_view reg;
  field_ref        value;
  field_ref        channel;
};

/**
 * @brief ADC1 of the STM32U5, RM0456 section 33: its power-up handshake, done at the write, and
 *        software-triggered regular conversions of the SQ1 channel in virtual time. ADVREGEN out
 *        of deep power-down sets LDORDY, DEEPPWD clears ADVREGEN, ADCAL clears at once, ADEN
 *        sets ADRDY, ADDIS clears ADEN. ADSTART with ADEN starts a conversion of N x (SMP +
 *        TSAR) ADC clock cycles, the ADC clock being the `adc1` kernel clock divided by
 *        ADC12_CCR.PRESC, N the oversampling ratio with ROVSE and 1 without. At its end ADC_DR
 *        takes the code, EOC and EOS set, ADSTART clears, and a CPU or DMA read of ADC_DR clears
 *        EOC. ADSTP and ADDIS drop a conversion in progress. Without a kernel clock a conversion
 *        waits for one, logged once. Each ADEN rising edge with the ADC clock outside 5 to 55 MHz
 *        logs a warning and the conversions keep their computed time.
 *
 *        Codes against VDDA, the board's `supply_options`: an external channel 1 to 17 reads
 *        its driven input, `analog_port`, as V x 16384 / VDDA, floored and saturated. VREFINT
 *        reads the factory VREFINT_CAL and the temperature sensor TS_CAL1, at 30 C, both scaled
 *        by 3000 / VDDA, when ADC12_CCR connects them, and 0 otherwise. Each word is read from
 *        the description's signature content when a conversion of its channel starts, a
 *        config_error when absent. VBAT reads 0. A 14-bit code loses two bits per RES step. The
 *        oversampler sums N codes at the resolution and shifts the sum right by OVSS, rounding to
 *        nearest. Sample k of a conversion holds the input at the end of its sampling window,
 *        SMP cycles after k x (SMP + TSAR) cycles from the start, so a drive during a conversion
 *        reaches the samples held after it. The ADC clock a conversion started with times all
 *        its samples.
 *
 *        Continuous, discontinuous, triggered and multi-channel sequences, injected conversions,
 *        DMA, interrupts and watchdogs are not modelled and log a warning once. LSHIFT, offsets
 *        and gain compensation leave the result unchanged and log a warning once, a model_error
 *        under strict_peripherals. A reserved ADC12_CCR.PRESC divides by 256, logged once by
 *        ADC12, a model_error under strict_peripherals.
 */
class adc final : public dispatch_model<adc, adc_state>, public analog_port, public clock_listener
{
public:
  adc(model_context& ctx, peripheral_desc const& desc, supply_options supply);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_clock_change() override;

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

  void               drive(std::uint8_t channel, std::uint32_t microvolts) override;
  [[nodiscard]] auto input(std::uint8_t channel) const -> std::uint32_t override;
  [[nodiscard]] auto vdda_mv() const -> std::uint32_t override;

  void cr_written(std::uint32_t old, std::uint32_t now);
  void dr_written(std::uint32_t, std::uint32_t)
  {}

private:
  [[nodiscard]] auto common() const -> adc_common const*;
  [[nodiscard]] auto divider() -> std::uint32_t;
  [[nodiscard]] auto adc_hz() -> std::uint64_t;
  [[nodiscard]] auto sampling() const -> std::uint32_t;
  [[nodiscard]] auto conversion() const -> std::uint32_t;
  [[nodiscard]] auto code() const -> std::uint32_t;
  [[nodiscard]] auto after(std::uint64_t cycles) const -> std::uint64_t;
  void               hold_until(std::uint64_t ps);
  [[nodiscard]] auto ratio() const -> std::uint32_t;
  [[nodiscard]] auto channel() const -> std::uint32_t;
  [[nodiscard]] auto external(std::uint8_t channel) const -> std::size_t;
  [[nodiscard]] auto calibration(address at, std::string_view name) const -> std::uint16_t;
  [[nodiscard]] auto sample(std::uint32_t channel) const -> std::uint32_t;
  void               require_factory_word(std::uint32_t channel) const;
  void               begin();
  void               schedule();
  void               finish();
  void               abort();
  void               check_clock_range();
  void               check_unmodelled();
  void               warn_once(bool& logged, std::string_view reg, std::string_view what);
  void               unmodelled(bool& logged, std::string_view name, std::string_view what);

  register_file&                     regs_;
  supply_options                     supply_;
  core::clock_domain                 kernel_;
  field_ref                          enable_;
  field_ref                          disable_;
  field_ref                          start_;
  field_ref                          stop_;
  field_ref                          injected_start_;
  field_ref                          regulator_;
  field_ref                          deep_power_down_;
  field_ref                          calibrate_;
  field_ref                          ready_;
  field_ref                          end_of_conversion_;
  field_ref                          end_of_sequence_;
  field_ref                          regulator_ready_;
  field_ref                          resolution_;
  field_ref                          continuous_;
  field_ref                          discontinuous_;
  field_ref                          trigger_;
  field_ref                          length_;
  field_ref                          first_channel_;
  field_ref                          data_;
  field_ref                          oversampling_;
  field_ref                          oversampling_ratio_;
  field_ref                          oversampling_shift_;
  field_ref                          left_shift_;
  field_ref                          gain_;
  std::array<field_ref, channels>    sampling_;
  std::array<offset_fields, offsets> offsets_;
  std::uint32_t                      set_only_{};
  bool                               sequence_logged_{};
  bool                               injected_logged_{};
  bool                               no_clock_logged_{};
  bool                               left_shift_logged_{};
  bool                               offset_logged_{};
  bool                               gain_logged_{};
};

constexpr std::array<write_entry<adc>, 2> adc_writes{
    {
     {"ADC_CR", &adc::cr_written},
     {"ADC_DR", &adc::dr_written},
     }
};

constexpr std::array<std::string_view, offsets> offset_registers{
    "ADC_OFR1", "ADC_OFR2", "ADC_OFR3", "ADC_OFR4"};

adc::adc(model_context& ctx, peripheral_desc const& desc, supply_options supply)
    : dispatch_model{ctx, desc, "ADC", adc_writes}, regs_{ctx.regs}, supply_{supply},
      kernel_{ctx.clock.domain(lower(desc.name))}, enable_{field("ADC_CR", "ADEN")},
      disable_{field("ADC_CR", "ADDIS")}, start_{field("ADC_CR", "ADSTART")},
      stop_{field("ADC_CR", "ADSTP")}, injected_start_{field("ADC_CR", "JADSTART")},
      regulator_{field("ADC_CR", "ADVREGEN")}, deep_power_down_{field("ADC_CR", "DEEPPWD")},
      calibrate_{field("ADC_CR", "ADCAL")}, ready_{field("ADC_ISR", "ADRDY")},
      end_of_conversion_{field("ADC_ISR", "EOC")}, end_of_sequence_{field("ADC_ISR", "EOS")},
      regulator_ready_{field("ADC_ISR", "LDORDY")}, resolution_{field("ADC_CFGR1", "RES")},
      continuous_{field("ADC_CFGR1", "CONT")}, discontinuous_{field("ADC_CFGR1", "DISCEN")},
      trigger_{field("ADC_CFGR1", "EXTEN")}, length_{field("ADC_SQR1", "L")},
      first_channel_{field("ADC_SQR1", "SQ1")}, data_{field("ADC_DR", "RDATA")},
      oversampling_{field("ADC_CFGR2", "ROVSE")}, oversampling_ratio_{field("ADC_CFGR2", "OSR")},
      oversampling_shift_{field("ADC_CFGR2", "OVSS")}, left_shift_{field("ADC_CFGR2", "LSHIFT")},
      gain_{field("ADC_GCOMP", "GCOMP")}
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    sampling_[index] = field(index < 10 ? "ADC_SMPR1" : "ADC_SMPR2", std::format("SMP{}", index));
  }
  for(std::size_t index = 0; index < offsets; ++index)
  {
    auto const name = offset_registers[index];
    offsets_[index] = {
        .reg = name, .value = field(name, "OFFSET"), .channel = field(name, "OFFSET_CH")};
  }
  for(auto const bit : {enable_, disable_, start_, stop_, injected_start_, calibrate_})
  {
    set_only_ |= bit.mask();
  }
  set_only_ |= field("ADC_CR", "JADSTP").mask();
}

auto adc::common() const -> adc_common const*
{
  return dynamic_cast<adc_common const*>(ctx_.peers.find("ADC12"));
}

auto adc::divider() -> std::uint32_t
{
  auto* const shared = dynamic_cast<adc_common*>(ctx_.peers.find("ADC12"));
  return shared != nullptr ? shared->divider() : 1u;
}

auto adc::adc_hz() -> std::uint64_t
{
  return ctx_.clock.hz(kernel_) / divider();
}

auto adc::sampling() const -> std::uint32_t
{
  auto const which = channel();
  return sampling_cycles[which < channels ? get(regs_, sampling_[which]) : 0u];
}

auto adc::conversion() const -> std::uint32_t
{
  return sampling() + sar_cycles[get(regs_, resolution_)];
}

auto adc::code() const -> std::uint32_t
{
  return sample(channel()) >> (2 * get(regs_, resolution_));
}

auto adc::after(std::uint64_t cycles) const -> std::uint64_t
{
  auto const hz = state_.clock_hz;
  return state_.start_ps
       + static_cast<std::uint64_t>((wide{cycles} * state_.divider * ps_per_second + hz - 1u) / hz);
}

void adc::hold_until(std::uint64_t ps)
{
  auto const samples = ratio();
  while(
      state_.held < samples and after(std::uint64_t{state_.held} * conversion() + sampling()) <= ps)
  {
    state_.sum += code();
    ++state_.held;
  }
}

auto adc::ratio() const -> std::uint32_t
{
  return get(regs_, oversampling_) != 0 ? get(regs_, oversampling_ratio_) + 1u : 1u;
}

auto adc::channel() const -> std::uint32_t
{
  return get(regs_, first_channel_);
}

auto adc::external(std::uint8_t channel) const -> std::size_t
{
  if(channel < first_external or channel > last_external)
  {
    throw config_error(
        desc_.name, 0, "channel",
        std::format(
            "an external channel from {} to {}, not {}", first_external, last_external, channel));
  }
  return channel;
}

void adc::drive(std::uint8_t channel, std::uint32_t microvolts)
{
  auto const at = external(channel);
  if(state_.converting)
  {
    hold_until(ctx_.clock.time().ps);
  }
  state_.input_uv[at] = microvolts;
}

auto adc::input(std::uint8_t channel) const -> std::uint32_t
{
  return state_.input_uv[external(channel)];
}

auto adc::vdda_mv() const -> std::uint32_t
{
  return supply_.vdda_mv;
}

auto adc::calibration(address at, std::string_view name) const -> std::uint16_t
{
  auto const word = factory_halfword(ctx_.part, at);
  if(not word)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("regions.content.{:#010x}", at),
        std::format("the factory {} word the ADC model converts", name));
  }
  return *word;
}

void adc::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  abort();
}

auto adc::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const value = dispatch_model::on_read(regs, offset, size, ctx);
  if((offset & ~3u) == data_.offset and ctx.by != initiator::debugger)
  {
    update(regs_, end_of_conversion_, 0);
  }
  return value;
}

void adc::cr_written(std::uint32_t old, std::uint32_t now)
{
  now |= old & set_only_;
  if(deep_power_down_.of(now) != 0)
  {
    now = regulator_.with(now, 0);
  }
  regs_.poke(enable_.offset, now);
  update(
      regs_, regulator_ready_, regulator_.of(now) != 0 and deep_power_down_.of(now) == 0 ? 1u : 0u);
  update(regs_, calibrate_, 0);
  if(injected_start_.of(now) != 0)
  {
    update(regs_, injected_start_, 0);
    warn_once(injected_logged_, "ADC_CR", "JADSTART: injected conversions");
  }
  if(disable_.of(now) != 0)
  {
    update(regs_, disable_, 0);
    update(regs_, enable_, 0);
    update(regs_, start_, 0);
    update(regs_, stop_, 0);
    abort();
    return;
  }
  if(enable_.of(now) != 0 and enable_.of(old) == 0)
  {
    if(get(regs_, regulator_ready_) != 0)
    {
      update(regs_, ready_, 1);
    }
    check_clock_range();
  }
  if(stop_.of(now) != 0)
  {
    update(regs_, stop_, 0);
    update(regs_, start_, 0);
    abort();
    return;
  }
  auto const busy = state_.converting or state_.waiting_clock;
  if(start_.of(now) != 0 and enable_.of(now) != 0 and not busy)
  {
    begin();
  }
}

void adc::check_clock_range()
{
  if(ctx_.clock.hz(kernel_) == 0)
  {
    return;
  }
  auto const hz = adc_hz();
  if(hz < min_adc_hz or hz > max_adc_hz)
  {
    warn("ADC_CR", std::format("ADC clock {} Hz after PRESC is outside 5 to 55 MHz", hz));
  }
}

void adc::begin()
{
  if(get(regs_, continuous_)
     + get(regs_, discontinuous_)
     + get(regs_, trigger_)
     + get(regs_, length_)
     != 0)
  {
    warn_once(
        sequence_logged_, "ADC_CFGR1",
        "CONT, DISCEN, EXTEN or SQR1.L: continuous, discontinuous, triggered or multi-channel "
        "sequences, SQ1 converted once");
  }
  check_unmodelled();
  require_factory_word(channel());
  if(ctx_.clock.hz(kernel_) == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("ADC_CR", "no ADC kernel clock: the conversion waits");
    }
    state_.waiting_clock = true;
    return;
  }
  schedule();
}

void adc::check_unmodelled()
{
  if(get(regs_, left_shift_) != 0)
  {
    unmodelled(left_shift_logged_, "ADC_CFGR2", "LSHIFT: left shift of the result");
  }
  if(get(regs_, gain_) != 0)
  {
    unmodelled(gain_logged_, "ADC_GCOMP", "GCOMP: gain compensation");
  }
  for(auto const& offset : offsets_)
  {
    if(get(regs_, offset.value) != 0 and get(regs_, offset.channel) == channel())
    {
      unmodelled(offset_logged_, offset.reg, "OFFSET: offset of the result");
    }
  }
}

void adc::schedule()
{
  state_.start_ps      = ctx_.clock.time().ps;
  state_.clock_hz      = ctx_.clock.hz(kernel_);
  state_.divider       = divider();
  state_.held          = 0;
  state_.sum           = 0;
  state_.waiting_clock = false;
  state_.converting    = true;
  ++state_.generation;
  ctx_.events.schedule_at(
      vtime{after(std::uint64_t{ratio()} * conversion())}, conversion_end, state_.generation,
      ctx_.clock);
}

void adc::finish()
{
  state_.converting = false;
  for(auto const samples = ratio(); state_.held < samples; ++state_.held)
  {
    state_.sum += code();
  }
  auto result = state_.sum;
  if(get(regs_, oversampling_) != 0)
  {
    auto const shift = get(regs_, oversampling_shift_);
    result           = shift == 0 ? state_.sum : (state_.sum + (1u << (shift - 1u))) >> shift;
  }
  update(regs_, data_, result);
  update(regs_, end_of_conversion_, 1);
  update(regs_, end_of_sequence_, 1);
  update(regs_, start_, 0);
}

void adc::abort()
{
  ++state_.generation;
  state_.converting    = false;
  state_.waiting_clock = false;
}

void adc::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != conversion_end or payload != state_.generation or not state_.converting)
  {
    return;
  }
  finish();
}

void adc::on_clock_change()
{
  if(state_.waiting_clock and ctx_.clock.hz(kernel_) != 0)
  {
    schedule();
  }
}

void adc::require_factory_word(std::uint32_t channel) const
{
  auto const* const shared = common();
  if(shared == nullptr)
  {
    return;
  }
  if(channel == vrefint_channel and shared->vrefint_connected())
  {
    static_cast<void>(calibration(vrefint_cal_address, "VREFINT_CAL"));
  }
  if(channel == sensor_channel and shared->sensor_connected())
  {
    static_cast<void>(calibration(ts_cal1_address, "TS_CAL1"));
  }
}

auto adc::sample(std::uint32_t channel) const -> std::uint32_t
{
  auto const* const shared = common();
  if(channel == vrefint_channel)
  {
    return shared != nullptr and shared->vrefint_connected()
             ? at_vdda(calibration(vrefint_cal_address, "VREFINT_CAL"), supply_.vdda_mv)
             : 0u;
  }
  if(channel == sensor_channel)
  {
    return shared != nullptr and shared->sensor_connected()
             ? at_vdda(calibration(ts_cal1_address, "TS_CAL1"), supply_.vdda_mv)
             : 0u;
  }
  if(channel >= first_external and channel <= last_external)
  {
    return code14(state_.input_uv[channel], supply_.vdda_mv);
  }
  return 0;
}

void adc::warn_once(bool& logged, std::string_view reg, std::string_view what)
{
  if(std::exchange(logged, true))
  {
    return;
  }
  warn(reg, std::format("{} not modelled", what));
}

void adc::unmodelled(bool& logged, std::string_view name, std::string_view what)
{
  warn_once(logged, name, what);
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, reg(name).offset);
  }
}
}

auto adc_factory(supply_options supply) -> model_factory
{
  return [supply](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc>(ctx, desc, supply);
  };
}

auto adc_common_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_common>(ctx, desc);
  };
}
}
