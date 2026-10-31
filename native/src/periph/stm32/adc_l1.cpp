#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t   channels        = 32;
constexpr std::size_t   max_ranks       = 28;
constexpr std::uint32_t sensor_channel  = 16;
constexpr std::uint32_t vrefint_channel = 17;
constexpr std::uint32_t vcomp_channel   = 26;
constexpr std::uint64_t ps_per_second   = 1'000'000'000'000;
constexpr std::uint64_t power_up_ps     = 3'500'000;
constexpr std::uint32_t factory_vdda_mv = 3000;
constexpr std::uint32_t conversion_end  = 0;
constexpr std::uint32_t ready_end       = 1;

constexpr std::array<std::uint32_t, 8> sampling_cycles{4, 9, 16, 24, 48, 96, 192, 384};
constexpr std::array<std::uint32_t, 4> sar_cycles{12, 11, 9, 7};
constexpr std::array<std::uint32_t, 4> prescalers{1, 2, 4, 4};

using wide = unsigned __int128;

[[nodiscard]] constexpr auto cycles_ps(std::uint64_t cycles, std::uint64_t hz) -> std::uint64_t
{
  return static_cast<std::uint64_t>((wide{cycles} * ps_per_second + hz - 1u) / hz);
}

[[nodiscard]] auto calibration_word(part_description const& part, std::uint32_t at)
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

struct adc_l1_state
{
  std::array<std::uint32_t, channels> input_uv{};
  std::uint32_t                       generation{};
  std::uint32_t                       ready_generation{};
  std::uint64_t                       start_ps{};
  std::uint64_t                       clock_hz{};
  std::uint32_t                       held_code{};
  bool                                held{};
  bool                                converting{};
  bool                                waiting_clock{};
  bool                                ready_waiting{};
  bool                                unread{};
  std::uint8_t                        rank{};

  friend auto operator==(adc_l1_state const&, adc_l1_state const&) -> bool = default;
};

/**
 * @brief The ADC of the STM32L1, `aditf3_v1_1_Cube`, RM0038 section 12, one instance `ADC` with
 *        the common ADC_CSR and ADC_CCR at offsets 0x300 and 0x304: power-up, the regular group
 *        in single, continuous and scan mode, resolution, alignment, EOC, OVR and the channel
 *        banks, converted in virtual time.
 *
 *        Power, RM0038 12.3.1: an ADC_CR2 write taking ADON from 0 to 1 sets ADONS tSTAB = 3.5 us
 *        later, the DS10002 and DS6876 maximum. The ADC clock is HSI16 divided by ADCPRE of
 *        ADC_CCR, 12.3.2: without HSI the power-up and each conversion wait for it, logged once,
 *        and a conversion keeps the clock it started with. ADCPRE 11 is reserved: it divides by
 *        4 and logs once, a model_error under strict_peripherals. ADON 0 clears ADONS at once
 *        and drops a conversion in progress. ADC1EN of RCC_APB2ENR is not checked.
 *
 *        Regular group, RM0038 12.3.4 to 12.3.8: an ADC_CR2 write setting SWSTART with ADONS
 *        set and no conversion running starts the group, SWSTART clearing when the first
 *        conversion starts. With SCAN 0 the group is SQ1 of ADC_SQR5, with SCAN 1 the L + 1 ranks
 *        of SQ1 to SQ28 across ADC_SQR5 to ADC_SQR1, 27 ranks at most on Cat.1 and Cat.2, where L
 *        26 and above converts 27, logged once, a model_error under strict_peripherals. Each
 *        conversion takes SMP + TCONV ADC clock cycles, SMP 4, 9, 16, 24, 48, 96, 192 or 384 from
 *        the channel's field in ADC_SMPR3 to ADC_SMPR0, TCONV 12, 11, 9 and 7 for RES 12, 10, 8
 *        and 6 bits, 12.8. A conversion starts when the one before ends. STRT sets at each
 *        conversion start, EOC at the end of each conversion with EOCS 1 and at the end of the
 *        group with EOCS 0. CONT 1 starts the group over after its last rank. A SWSTART while a
 *        conversion runs is ignored. A write of ADC_SQR1 to ADC_SQR5 during a conversion drops it,
 *        12.7.2, and the group waits for a new SWSTART. RCNR and JCNR read 0. SWSTART with ADONS
 *        clear logs a warning once, a model_error under strict_peripherals, and is ignored.
 *
 *        Results, RM0038 12.4 and 12.11: ADC_DR holds the latest result, right-aligned, or with
 *        ALIGN 1 left-aligned on bit 15, a 6-bit code on bit 7, inferred. A CPU or DMA read of
 *        ADC_DR clears EOC, a debugger read clears nothing. With EOCS 1, a result finding ADC_DR
 *        unread, neither read nor its EOC cleared, sets OVR and is lost, the group going on,
 *        inferred. With EOCS 0 a result overwrites ADC_DR. ADC_SR flags OVR, STRT, JSTRT, JEOC,
 *        EOC and AWD clear on a write of 0 and keep on a write of 1. ADC1_IRQn is high while EOC,
 *        JEOC, AWD or OVR is set with EOCIE, JEOCIE, AWDIE or OVRIE. ADC_CSR reads ADC_SR bits 6
 *        to 0, 12.15.19.
 *
 *        Codes against VDDA, the board's `supply_options`: an external channel reads its driven
 *        input, `analog_port`, as V x 4096 / VDDA, floored and saturated, two bits fewer per RES
 *        step. A conversion holds the input at the end of its sampling phase. With TSVREFE of
 *        ADC_CCR set, channel 16 reads the factory TS_CAL1 and channel 17 VREFINT_CAL, both
 *        scaled by 3000 / VDDA, and 0 with TSVREFE clear, 12.12. A group converting one of them
 *        reads the word from the description's factory content when it starts, a config_error
 *        when absent. On Cat.3 to Cat.6 ADC_CFG 1 selects bank B: its channels sharing a bank A
 *        pad, RM0038 Table 41, read that pad's input, channels 16 and 17 as in bank A. A group
 *        converting a bank B channel on a pad of its own, VCOMP on channel 26 or a channel the
 *        category lacks reads 0 and logs once, a model_error under strict_peripherals.
 *
 *        Not modelled, each logged once, a model_error under strict_peripherals: JSWSTART, which
 *        clears, and EXTEN or JEXTEN other than 0 at the ADC_CR2 write, and at the group start
 *        DMA, DELS, PDI, PDD, DISCEN, JAUTO and AWDEN.
 */
class adc_l1_model final : public dispatch_model<adc_l1_model, adc_l1_state>,
                           public analog_port,
                           public clock_listener,
                           public bus_reset_listener
{
  using base = dispatch_model<adc_l1_model, adc_l1_state>;

public:
  adc_l1_model(
      model_context&         ctx,
      peripheral_desc const& desc,
      supply_options         supply,
      adc_l1_variant         variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_clock_change() override;

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

  void               drive(std::uint8_t channel, std::uint32_t microvolts) override;
  [[nodiscard]] auto input(std::uint8_t channel) const -> std::uint32_t override;
  [[nodiscard]] auto external_channels() const -> std::uint32_t override
  {
    return variant_.external;
  }
  [[nodiscard]] auto bonded_channels() const -> std::uint32_t override
  {
    return bonded_channels_;
  }
  [[nodiscard]] auto vdda_mv() const -> std::uint32_t override
  {
    return supply_.vdda_mv;
  }

  void sr_written(std::uint32_t old, std::uint32_t now);
  void cr2_written(std::uint32_t old, std::uint32_t now);
  void sequence_written(std::uint32_t, std::uint32_t);
  void plain_written(std::uint32_t, std::uint32_t)
  {
    signal();
  }

private:
  void               restore_reset_values();
  [[nodiscard]] auto adc_hz() -> std::uint64_t;
  [[nodiscard]] auto length() const -> std::size_t;
  [[nodiscard]] auto channel_at(std::size_t rank) const -> std::uint32_t;
  [[nodiscard]] auto bank_b() const -> bool;
  [[nodiscard]] auto sampling(std::uint32_t channel) const -> std::uint32_t;
  [[nodiscard]] auto conversion_cycles() const -> std::uint64_t;
  [[nodiscard]] auto sample_end_ps() const -> std::uint64_t;
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto readable(std::uint32_t channel) const -> bool;
  [[nodiscard]] auto calibration(std::uint32_t at, std::string_view name) const -> std::uint16_t;
  [[nodiscard]] auto code(std::uint32_t channel) const -> std::uint32_t;
  void               external(std::uint8_t channel) const;
  void               hold_until(std::uint64_t ps);
  void               power_up();
  void               check_group();
  void               check_channel(std::uint32_t channel);
  void               begin(std::uint64_t start_ps);
  void               schedule(std::uint64_t start_ps);
  void               finish();
  void               land(std::uint32_t result);
  void               abort();
  void               signal();
  void               unmodelled(bool& logged, std::string_view name, std::string_view what);
  void               forbidden(bool& logged, std::string_view name, std::string message);

  register_file&                                  regs_;
  supply_options                                  supply_;
  adc_l1_variant                                  variant_;
  std::uint32_t                                   bonded_channels_{};
  core::clock_domain                              hsi_;
  irq_line&                                       line_;
  reg_ref                                         status_;
  reg_ref                                         common_status_;
  field_ref                                       ready_;
  field_ref                                       overrun_;
  field_ref                                       started_;
  field_ref                                       end_of_conversion_;
  field_ref                                       power_;
  field_ref                                       start_;
  field_ref                                       injected_start_;
  field_ref                                       trigger_;
  field_ref                                       injected_trigger_;
  field_ref                                       continuous_;
  field_ref                                       end_selection_;
  field_ref                                       align_;
  field_ref                                       dma_;
  field_ref                                       delay_;
  std::optional<field_ref>                        bank_;
  field_ref                                       scan_;
  field_ref                                       resolution_;
  field_ref                                       discontinuous_;
  field_ref                                       auto_injected_;
  field_ref                                       watchdog_;
  field_ref                                       power_down_idle_;
  field_ref                                       power_down_delay_;
  field_ref                                       length_;
  field_ref                                       data_;
  field_ref                                       sensors_;
  field_ref                                       prescaler_;
  std::array<std::optional<field_ref>, max_ranks> sequence_;
  std::array<reg_ref, 4>                          sampling_regs_;
  std::array<std::pair<field_ref, field_ref>, 4>  interrupts_;
  std::uint32_t                                   clearable_{};
  bool                                            injected_logged_{};
  bool                                            trigger_logged_{};
  bool                                            injected_trigger_logged_{};
  bool                                            dma_logged_{};
  bool                                            delay_logged_{};
  bool                                            power_down_logged_{};
  bool                                            discontinuous_logged_{};
  bool                                            auto_injected_logged_{};
  bool                                            watchdog_logged_{};
  bool                                            start_refused_logged_{};
  bool                                            length_logged_{};
  bool                                            prescaler_logged_{};
  bool                                            no_clock_logged_{};
  std::array<bool, channels>                      channel_logged_{};
};

constexpr std::array<write_entry<adc_l1_model>, 13> banked_writes{
    {
     {"SR", &adc_l1_model::sr_written},
     {"CR1", &adc_l1_model::plain_written},
     {"CR2", &adc_l1_model::cr2_written},
     {"SMPR1", &adc_l1_model::sequence_written},
     {"SMPR2", &adc_l1_model::sequence_written},
     {"SMPR3", &adc_l1_model::sequence_written},
     {"SQR1", &adc_l1_model::sequence_written},
     {"SQR2", &adc_l1_model::sequence_written},
     {"SQR3", &adc_l1_model::sequence_written},
     {"SQR4", &adc_l1_model::sequence_written},
     {"SQR5", &adc_l1_model::sequence_written},
     {"DR", nullptr},
     {"SMPR0", &adc_l1_model::sequence_written},
     }
};

constexpr std::array<write_entry<adc_l1_model>, 2> common_writes{
    {
     {"CSR", nullptr},
     {"CCR", nullptr},
     }
};

[[nodiscard]] auto writes_of(adc_l1_variant const& variant)
    -> std::span<write_entry<adc_l1_model> const>
{
  return std::span{banked_writes}.first(
      variant.banks ? banked_writes.size() : banked_writes.size() - 1);
}

adc_l1_model::adc_l1_model(
    model_context&         ctx,
    peripheral_desc const& desc,
    supply_options         supply,
    adc_l1_variant         variant)
    : base{ctx, desc, "ADC", writes_of(variant), {}}, regs_{ctx.regs}, supply_{supply},
      variant_{variant}, hsi_{ctx.clock.domain("hsi")}, line_{irq("ADC1_IRQn")}, status_{reg("SR")},
      common_status_{reg("CSR")}, ready_{field("SR", "ADONS")}, overrun_{field("SR", "OVR")},
      started_{field("SR", "STRT")}, end_of_conversion_{field("SR", "EOC")},
      power_{field("CR2", "ADON")}, start_{field("CR2", "SWSTART")},
      injected_start_{field("CR2", "JSWSTART")}, trigger_{field("CR2", "EXTEN")},
      injected_trigger_{field("CR2", "JEXTEN")}, continuous_{field("CR2", "CONT")},
      end_selection_{field("CR2", "EOCS")}, align_{field("CR2", "ALIGN")},
      dma_{field("CR2", "DMA")}, delay_{field("CR2", "DELS")}, scan_{field("CR1", "SCAN")},
      resolution_{field("CR1", "RES")}, discontinuous_{field("CR1", "DISCEN")},
      auto_injected_{field("CR1", "JAUTO")}, watchdog_{field("CR1", "AWDEN")},
      power_down_idle_{field("CR1", "PDI")}, power_down_delay_{field("CR1", "PDD")},
      length_{field("SQR1", "L")}, data_{field("DR", "RegularDATA")},
      sensors_{field("CCR", "TSVREFE")}, prescaler_{field("CCR", "ADCPRE")}
{
  bind(common_writes);
  expect_register("SMPR0", variant.banks);
  expect_field("CR2", "ADC_CFG", variant.banks);
  expect_field("SQR1", "SQ28", variant.banks);
  if(variant.banks)
  {
    bank_ = field("CR2", "ADC_CFG");
  }
  auto const ranks = variant.banks ? max_ranks : max_ranks - 1;
  for(std::size_t rank = 0; rank < ranks; ++rank)
  {
    sequence_[rank] = field(std::format("SQR{}", 5 - rank / 6), std::format("SQ{}", rank + 1));
  }
  for(std::size_t index = 0; index < sampling_regs_.size(); ++index)
  {
    if(index != 0 or variant.banks)
    {
      sampling_regs_[index] = reg(std::format("SMPR{}", index));
    }
  }
  interrupts_ = {
      {{end_of_conversion_, field("CR1", "EOCIE")},
       {field("SR", "JEOC"), field("CR1", "JEOCIE")},
       {field("SR", "AWD"), field("CR1", "AWDIE")},
       {overrun_, field("CR1", "OVRIE")}}
  };
  for(auto const& name : {"OVR", "STRT", "JSTRT", "JEOC", "EOC", "AWD"})
  {
    clearable_ |= field("SR", name).mask();
  }
  bonded_channels_ = desc.bonded_channels.value_or(variant.external);
  if((bonded_channels_ & ~variant.external) != 0)
  {
    throw config_error(desc.name, 0, "bonded_channels", "a mask of external channels");
  }
}

void adc_l1_model::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void adc_l1_model::on_bus_reset()
{
  restore_reset_values();
}

void adc_l1_model::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  abort();
  ++state_.ready_generation;
  state_.ready_waiting = false;
  state_.unread        = false;
  signal();
}

auto adc_l1_model::adc_hz() -> std::uint64_t
{
  return ctx_.clock.hz(hsi_) / prescalers[get(regs_, prescaler_)];
}

auto adc_l1_model::length() const -> std::size_t
{
  if(get(regs_, scan_) == 0)
  {
    return 1;
  }
  auto const ranks = variant_.banks ? max_ranks : max_ranks - 1;
  return std::min<std::size_t>(get(regs_, length_) + 1u, ranks);
}

auto adc_l1_model::channel_at(std::size_t rank) const -> std::uint32_t
{
  return get(regs_, *sequence_[rank]);
}

auto adc_l1_model::bank_b() const -> bool
{
  return bank_ and get(regs_, *bank_) != 0;
}

auto adc_l1_model::sampling(std::uint32_t channel) const -> std::uint32_t
{
  auto const index = 3 - channel / 10;
  if(index == 0 and not variant_.banks)
  {
    return sampling_cycles[0];
  }
  auto const& word = sampling_regs_[index];
  return sampling_cycles[(regs_.peek(word.offset) >> (3 * (channel % 10))) & 7u];
}

auto adc_l1_model::conversion_cycles() const -> std::uint64_t
{
  return std::uint64_t{sampling(channel_at(state_.rank))} + sar_cycles[get(regs_, resolution_)];
}

auto adc_l1_model::sample_end_ps() const -> std::uint64_t
{
  return state_.start_ps + cycles_ps(sampling(channel_at(state_.rank)), state_.clock_hz);
}

auto adc_l1_model::busy() const -> bool
{
  return state_.converting or state_.waiting_clock;
}

auto adc_l1_model::readable(std::uint32_t channel) const -> bool
{
  if((variant_.external >> channel & 1u) == 0)
  {
    return false;
  }
  return not bank_b() or (variant_.bank_b_only >> channel & 1u) == 0;
}

auto adc_l1_model::calibration(std::uint32_t at, std::string_view name) const -> std::uint16_t
{
  auto const word = calibration_word(ctx_.part, at);
  if(not word)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("regions.{:#010x}", at),
        std::format("the factory {} word the ADC model converts", name));
  }
  return *word;
}

auto adc_l1_model::code(std::uint32_t channel) const -> std::uint32_t
{
  constexpr std::uint32_t full_scale = 4096;
  auto const              shift      = 2 * get(regs_, resolution_);
  if(channel == sensor_channel or channel == vrefint_channel)
  {
    if(get(regs_, sensors_) == 0)
    {
      return 0;
    }
    auto const factory = calibration(
        channel == sensor_channel ? variant_.ts_cal1 : variant_.vrefint_cal,
        channel == sensor_channel ? "TS_CAL1" : "VREFINT_CAL");
    return std::min<std::uint32_t>(full_scale - 1u, factory * factory_vdda_mv / supply_.vdda_mv)
        >> shift;
  }
  if(not readable(channel))
  {
    return 0;
  }
  auto const scaled = std::uint64_t{state_.input_uv[channel]}
                    * full_scale
                    / (std::uint64_t{supply_.vdda_mv} * 1000u);
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(full_scale - 1u, scaled)) >> shift;
}

void adc_l1_model::external(std::uint8_t channel) const
{
  if(channel >= channels or (variant_.external >> channel & 1u) == 0)
  {
    throw config_error(
        desc_.name, 0, "channel",
        std::format("an external channel of mask {:#010x}, not {}", variant_.external, channel));
  }
}

void adc_l1_model::drive(std::uint8_t channel, std::uint32_t microvolts)
{
  external(channel);
  if(state_.converting)
  {
    hold_until(ctx_.clock.time().ps);
  }
  state_.input_uv[channel] = microvolts;
}

auto adc_l1_model::input(std::uint8_t channel) const -> std::uint32_t
{
  external(channel);
  return state_.input_uv[channel];
}

void adc_l1_model::hold_until(std::uint64_t ps)
{
  if(not state_.held and sample_end_ps() <= ps)
  {
    state_.held_code = code(channel_at(state_.rank));
    state_.held      = true;
  }
}

auto adc_l1_model::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     ctx) -> std::expected<std::uint32_t, bus_error>
{
  auto const word = offset & ~3u;
  if(word == common_status_.offset)
  {
    regs_.poke(common_status_.offset, regs_.peek(status_.offset) & 0x7Fu);
  }
  auto const value = base::on_read(regs, offset, size, ctx);
  if(word == data_.offset and ctx.by != initiator::debugger)
  {
    state_.unread = false;
    update(regs_, end_of_conversion_, 0);
    signal();
  }
  return value;
}

void adc_l1_model::sr_written(std::uint32_t old, std::uint32_t now)
{
  auto const kept = old & (now | ~clearable_);
  regs_.poke(status_.offset, kept);
  if(end_of_conversion_.of(kept) == 0)
  {
    state_.unread = false;
  }
  signal();
}

void adc_l1_model::cr2_written(std::uint32_t old, std::uint32_t now)
{
  if(injected_start_.of(now) != 0)
  {
    update(regs_, injected_start_, 0);
    unmodelled(injected_logged_, "CR2", "JSWSTART: injected conversions");
  }
  if(trigger_.of(now) != 0 and trigger_.of(old) == 0)
  {
    unmodelled(trigger_logged_, "CR2", "EXTEN: regular hardware triggers");
  }
  if(injected_trigger_.of(now) != 0 and injected_trigger_.of(old) == 0)
  {
    unmodelled(injected_trigger_logged_, "CR2", "JEXTEN: injected hardware triggers");
  }
  if(power_.of(now) == 0 and power_.of(old) != 0)
  {
    update(regs_, ready_, 0);
    ++state_.ready_generation;
    state_.ready_waiting = false;
    abort();
    signal();
  }
  else if(power_.of(now) != 0 and power_.of(old) == 0)
  {
    power_up();
  }
  if(start_.of(now) == 0)
  {
    return;
  }
  if(get(regs_, ready_) == 0)
  {
    update(regs_, start_, 0);
    forbidden(
        start_refused_logged_, "CR2",
        "SWSTART with ADONS clear, forbidden by RM0038 12.15.3: ignored");
    return;
  }
  if(busy())
  {
    if(not state_.waiting_clock)
    {
      update(regs_, start_, 0);
    }
    return;
  }
  check_group();
  state_.rank = 0;
  begin(ctx_.clock.time().ps);
}

void adc_l1_model::sequence_written(std::uint32_t, std::uint32_t)
{
  if(busy())
  {
    abort();
    update(regs_, start_, 0);
  }
}

void adc_l1_model::power_up()
{
  ++state_.ready_generation;
  if(ctx_.clock.hz(hsi_) == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("CR2", "no HSI16, the ADC clock: the power-up and the conversions wait for it");
    }
    state_.ready_waiting = true;
    return;
  }
  state_.ready_waiting = false;
  ctx_.events.schedule_at(
      vtime{ctx_.clock.time().ps + power_up_ps}, ready_end, state_.ready_generation, ctx_.clock);
}

void adc_l1_model::check_group()
{
  if(get(regs_, prescaler_) == 3)
  {
    forbidden(prescaler_logged_, "CCR", "ADCPRE 11: a reserved prescaler, divided by 4");
  }
  if(get(regs_, dma_) != 0)
  {
    unmodelled(dma_logged_, "CR2", "DMA: regular DMA requests");
  }
  if(get(regs_, delay_) != 0)
  {
    unmodelled(delay_logged_, "CR2", "DELS: the delay between conversions");
  }
  if(get(regs_, power_down_idle_) != 0 or get(regs_, power_down_delay_) != 0)
  {
    unmodelled(power_down_logged_, "CR1", "PDI and PDD: the automatic power-down");
  }
  if(get(regs_, discontinuous_) != 0)
  {
    unmodelled(discontinuous_logged_, "CR1", "DISCEN: discontinuous mode");
  }
  if(get(regs_, auto_injected_) != 0)
  {
    unmodelled(auto_injected_logged_, "CR1", "JAUTO: the auto-injected group");
  }
  if(get(regs_, watchdog_) != 0)
  {
    unmodelled(watchdog_logged_, "CR1", "AWDEN: the analog watchdog");
  }
  if(get(regs_, scan_) != 0 and not variant_.banks and get(regs_, length_) + 1u >= max_ranks)
  {
    forbidden(
        length_logged_, "SQR1",
        std::format(
            "L {:#07b}: 28 conversions, available from Cat.3 only: 27 converted",
            get(regs_, length_)));
  }
  for(std::size_t rank = 0; rank < length(); ++rank)
  {
    check_channel(channel_at(rank));
  }
}

void adc_l1_model::check_channel(std::uint32_t channel)
{
  if(channel == sensor_channel or channel == vrefint_channel)
  {
    if(get(regs_, sensors_) != 0)
    {
      static_cast<void>(code(channel));
    }
    return;
  }
  if(readable(channel))
  {
    return;
  }
  auto const what =
      channel == vcomp_channel ? std::string{"channel 26, VCOMP,"}
      : bank_b() and (variant_.external >> channel & 1u) != 0
          ? std::format("bank B channel {}b on its own pad, outside the analog port,", channel)
          : std::format("channel {}, absent from this category,", channel);
  unmodelled(channel_logged_[channel], "SQR1", std::format("{} read as 0", what));
}

void adc_l1_model::begin(std::uint64_t start_ps)
{
  auto const hz = adc_hz();
  if(hz == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("CR2", "no HSI16, the ADC clock: the power-up and the conversions wait for it");
    }
    state_.waiting_clock = true;
    return;
  }
  state_.clock_hz = hz;
  schedule(start_ps);
}

void adc_l1_model::schedule(std::uint64_t start_ps)
{
  state_.start_ps      = start_ps;
  state_.held          = false;
  state_.waiting_clock = false;
  state_.converting    = true;
  ++state_.generation;
  update(regs_, start_, 0);
  update(regs_, started_, 1);
  ctx_.events.schedule_at(
      vtime{start_ps + cycles_ps(conversion_cycles(), state_.clock_hz)}, conversion_end,
      state_.generation, ctx_.clock);
}

void adc_l1_model::finish()
{
  auto const end    = state_.start_ps + cycles_ps(conversion_cycles(), state_.clock_hz);
  auto const result = state_.held ? state_.held_code : code(channel_at(state_.rank));
  state_.converting = false;
  auto const last   = std::size_t{state_.rank} + 1u >= length();
  land(result);
  if(get(regs_, end_selection_) != 0 or last)
  {
    update(regs_, end_of_conversion_, 1);
  }
  state_.rank = last ? std::uint8_t{0} : static_cast<std::uint8_t>(state_.rank + 1u);
  if(not last or get(regs_, continuous_) != 0)
  {
    begin(end);
  }
  signal();
}

void adc_l1_model::land(std::uint32_t result)
{
  if(get(regs_, end_selection_) != 0 and state_.unread)
  {
    update(regs_, overrun_, 1);
    return;
  }
  if(get(regs_, align_) != 0)
  {
    auto const resolution = get(regs_, resolution_);
    result <<= resolution == 3 ? 2u : 4u + 2u * resolution;
  }
  update(regs_, data_, result);
  state_.unread = get(regs_, end_selection_) != 0;
}

void adc_l1_model::abort()
{
  ++state_.generation;
  state_.converting    = false;
  state_.waiting_clock = false;
  state_.rank          = 0;
}

void adc_l1_model::signal()
{
  auto const level = std::ranges::any_of(interrupts_, [this](auto const& pair) {
    return get(regs_, pair.first) != 0 and get(regs_, pair.second) != 0;
  });
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void adc_l1_model::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == ready_end)
  {
    if(payload == state_.ready_generation and get(regs_, power_) != 0)
    {
      update(regs_, ready_, 1);
    }
    return;
  }
  if(kind == conversion_end and payload == state_.generation and state_.converting)
  {
    finish();
  }
}

void adc_l1_model::on_clock_change()
{
  if(ctx_.clock.hz(hsi_) == 0)
  {
    return;
  }
  if(state_.ready_waiting and get(regs_, power_) != 0)
  {
    power_up();
  }
  if(state_.waiting_clock)
  {
    begin(ctx_.clock.time().ps);
  }
}

void adc_l1_model::unmodelled(bool& logged, std::string_view name, std::string_view what)
{
  if(not std::exchange(logged, true))
  {
    warn(name, std::format("{} not modelled", what));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, reg(name).offset);
  }
}

void adc_l1_model::forbidden(bool& logged, std::string_view name, std::string message)
{
  if(not std::exchange(logged, true))
  {
    warn(name, std::move(message));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, reg(name).offset);
  }
}
}

auto adc_l1_factory(supply_options supply, adc_l1_variant variant) -> model_factory
{
  return
      [supply, variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
        return std::make_shared<adc_l1_model>(ctx, desc, supply, variant);
      };
}
}
