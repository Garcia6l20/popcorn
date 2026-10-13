#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <reflex/enum.hpp>

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
#include <vector>

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
constexpr std::uint32_t sampling_end        = 1;
constexpr std::size_t   ranks               = 16;
constexpr std::size_t   fifo_depth          = 8;
constexpr std::uint32_t dma_one_shot        = 1;
constexpr std::uint32_t dma_mdf             = 2;
constexpr std::uint32_t dma_circular        = 3;
constexpr std::size_t   trigger_selections  = 32;
constexpr std::uint32_t exten_rising        = 1;
constexpr std::uint32_t exten_falling       = 2;
constexpr std::size_t   watchdogs           = 3;
constexpr std::uint8_t  max_filter_count    = 8;

enum class regular_trigger : std::uint8_t
{
  tim1_oc1   = 0,
  tim1_oc2   = 1,
  tim1_oc3   = 2,
  tim2_oc2   = 3,
  tim3_trgo  = 4,
  tim4_oc4   = 5,
  exti11     = 6,
  tim8_trgo  = 7,
  tim8_trgo2 = 8,
  tim1_trgo  = 9,
  tim1_trgo2 = 10,
  tim2_trgo  = 11,
  tim4_trgo  = 12,
  tim6_trgo  = 13,
  tim15_trgo = 14,
  tim3_oc4   = 15,
  exti15     = 16,
  lptim1_ch1 = 18,
  lptim2_ch1 = 19,
  lptim3_ch1 = 20,
  lptim4_out = 21,
};

[[nodiscard]] auto trigger_signal(std::uint32_t selection) -> std::string
{
  template for(constexpr auto e :
               std::define_static_array(std::meta::enumerators_of(^^regular_trigger)))
  {
    if(std::to_underlying([:e:]) == selection)
    {
      return std::string{std::meta::identifier_of(e)};
    }
  }
  return std::format("adc_ext_trg{}", selection);
}

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
  std::uint8_t                        rank{};
  std::uint8_t                        subgroup_left{};
  std::vector<std::uint32_t>          data;
  bool                                dma_stopped{};
  std::uint8_t                        filter_count{};

  friend auto operator==(adc_state const&, adc_state const&) -> bool = default;
};

struct adc_common_state
{
  friend auto operator==(adc_common_state const&, adc_common_state const&) -> bool = default;
};

[[nodiscard]] auto factory_halfword(part_description const& part, address at)
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
 *        regular sequences converted in virtual time. ADVREGEN out of deep power-down sets
 *        LDORDY, DEEPPWD clears ADVREGEN, ADCAL clears at once, ADEN sets ADRDY, ADDIS clears
 *        ADEN. With EXTEN 0, an ADC_CR write taking ADSTART from 0 to 1 with ADEN starts the
 *        regular sequence, the SQR1.L + 1 ranks of SQ1 to SQ16. Each rank is one conversion of
 *        N x (SMP + TSAR) ADC clock cycles, SMP from the SMPR field of its channel, the ADC clock
 *        being the `adc1` kernel clock divided by ADC12_CCR.PRESC, N the oversampling ratio with
 *        ROVSE and 1 without. A rank starts when the one before ends: sampling never overlaps the
 *        previous conversion. EOSMP sets at the end of each sampling phase, EOC with each result,
 *        EOS with the last rank's. CONT 0 stops after the last rank and clears ADSTART, CONT 1
 *        starts over at rank 0. DISCEN 1 converts DISCNUM + 1 ranks per start, fewer in the last
 *        subgroup, the next start going on from the next rank, clears ADSTART at the EOC ending
 *        the subgroup and ignores CONT.
 *
 *        Hardware triggers, RM0456 33.4.19: with EXTEN not 0, the ADSTART write starts nothing
 *        and ADC1 listens, as a trigger_sink, to the signal EXTSEL selects in RM0456 Table 308,
 *        such as `tim15_trgo` for 14, while ADEN and ADSTART are set. EXTEN 01 detects the
 *        signal's rising edges, 10 its falling edges, 11 both: a timer's TRGO pulse rises at its
 *        event and falls one timer kernel clock later. Each detected edge while no conversion
 *        runs starts the sequence, or the next DISCEN subgroup, tLATR later: 3, 7 or 13 ADC
 *        clock cycles for PRESC 0, 1 or 2, datasheet DS13737 table 103. The datasheet gives no
 *        value for a larger PRESC: a divider D of 4 or more takes 3 x D + 1 cycles, the rule its
 *        values for D 2 and 4 follow, unverified. An edge during a conversion or the latency is
 *        ignored. ADSTART stays set at the end of a sequence or subgroup, RM0456
 *        33.4.16, and clears with ADSTP or ADDIS only. ADC1 tells the selected signal's source
 *        when its listening starts or stops. A selection no model produces, the reserved EXTSEL
 *        17 and 22 to 31 included, logs a warning once per selection when ADC1 starts listening
 *        to it, a model_error under strict_peripherals, and no conversion starts. A
 *        config_error or model_error the per-sequence checks raise at a trigger surfaces from
 *        the source's event or register access that gave the edge.
 *
 *        ADSTP and ADDIS drop a conversion in progress and reset the sequence to rank 0.
 *        Without a kernel clock a conversion waits for one, logged once. Each ADEN rising edge
 *        with the ADC clock outside 5 to 55 MHz logs a warning and the conversions keep their
 *        computed time.
 *
 *        Results, RM0456 33.4.27: with OVRMOD 0, ADC_DR shows the head of an eight-result FIFO,
 *        a CPU or DMA read of ADC_DR pops it, and EOC stays set while results are left. A result
 *        finding the FIFO full sets OVR and is discarded, as is every result while OVR stays set.
 *        With OVRMOD 1, a result overwrites ADC_DR and sets OVR when EOC was still set. A
 *        debugger read pops nothing. DMNGT 01 and 11 hold the `adc1_dma` request high while a
 *        result is unread and OVR is clear. Under DMNGT 01, once the GPDMA channel serving that
 *        request completes, the conversion in progress is dropped, the sequence resets to rank
 *        0, ADSTART stays set, and no request rises until an ADC_CR write takes ADSTART from 0
 *        to 1: RM0456 ties the re-arm to nothing, this reading is unverified. A hardware trigger
 *        then starts sequences without a request, and their results land in ADC_DR: the data
 *        register RM0456 33.4.27 calls frozen after the DMA end is not modelled. DMNGT 10, the MDF
 *        path, requests nothing. ADC1_IRQn is high while an ADC_ISR flag is set with its ADC_IER
 *        enable.
 *
 *        Codes against VDDA, the board's `supply_options`: an external channel 1 to 17 reads
 *        its driven input, `analog_port`, as V x 16384 / VDDA, floored and saturated. VREFINT
 *        reads the factory VREFINT_CAL and the temperature sensor TS_CAL1, at 30 C, both scaled
 *        by 3000 / VDDA, when ADC12_CCR connects them, and 0 otherwise. Each word is read from
 *        the description's signature content when a sequence converting its channel starts, a
 *        config_error when absent. VBAT reads 0. A 14-bit code loses two bits per RES step. The
 *        oversampler sums N codes at the resolution and shifts the sum right by OVSS, rounding to
 *        nearest. Sample k of a conversion holds the input at the end of its sampling window,
 *        SMP cycles after k x (SMP + TSAR) cycles from the start, so a drive during a conversion
 *        reaches the samples held after it. The ADC clock a conversion started with times all
 *        its samples.
 *
 *        Injected conversions are not modelled and log a warning once.
 *        DMNGT 10, AUTDLY, LFTRIG, LSHIFT, offsets and gain compensation leave the conversions
 *        unchanged and log a warning once, a model_error under strict_peripherals. Two settings
 *        RM0456 forbids log a warning once, a model_error under strict_peripherals: an ADSTART
 *        write with ADEN 0 or ADDIS 1, ignored with ADSTART left at 0, and DISCEN with CONT,
 *        converted as DISCEN alone. A sequence converting a channel whose ADC_PCSEL bit is
 *        clear logs a warning once, a model_error under strict_peripherals, and converts it as if
 *        the bit were set, RM0456 33.4.12. Only the single DMA request is modelled: the burst
 * request RM0456 33.4.27 raises at four results is not. A GPDMA channel reading ADC_DR in bursts of
 * more than one beat starts each burst on the single request, and a beat finding the FIFO empty
 * reads the last result again. A reserved ADC12_CCR.PRESC divides by 256, logged once by ADC12, a
 * model_error under strict_peripherals.
 *
 *        Analog watchdogs, RM0456 33.4.30: each regular conversion's result, after the
 *        oversampler, is compared before alignment, as Q = result << (2 x RES), the 14-bit frame
 *        of RM0456 Table 318. A guarded result is out of window when Q < LTRx or Q > HTRx,
 *        strictly. AWD1 guards the AWD1CH channel with AWD1SGL, every channel without, while
 *        AWD1EN is set. AWD2 and AWD3 guard the channels set in AWD2CH and AWD3CH. An
 *        out-of-window result sets AWDx at its EOC, before the result lands, so a result an
 *        overrun discards still sets it. AWD1 counts consecutive out-of-window results of its
 *        guarded channels, saturating at 8, and sets AWD1 at each result once the count exceeds
 *        AWDFILT1: AWDFILT1 n flags from the n + 1-th on. An in-window result restarts the count.
 *        A changed input alone sets nothing. JAWD1EN guards the injected group, which never
 *        converts. AWDFILT1 with AWD1EN and DMNGT set gates the DMA requests by the window on
 *        hardware: not modelled, logged once at the sequence start, a model_error under
 *        strict_peripherals.
 */
class adc final : public dispatch_model<adc, adc_state>,
                  public analog_port,
                  public clock_listener,
                  public dma_requester,
                  public trigger_sink
{
public:
  adc(model_context& ctx, peripheral_desc const& desc, supply_options supply);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               on_clock_change() override;
  void               transfer_completed(std::uint32_t request) override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps) override;

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;

  void               drive(std::uint8_t channel, std::uint32_t microvolts) override;
  [[nodiscard]] auto input(std::uint8_t channel) const -> std::uint32_t override;
  [[nodiscard]] auto vdda_mv() const -> std::uint32_t override;

  void cr_written(std::uint32_t old, std::uint32_t now);
  void isr_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t, std::uint32_t)
  {
    signal();
  }
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
  [[nodiscard]] auto channel_at(std::size_t rank) const -> std::uint32_t;
  [[nodiscard]] auto length() const -> std::size_t;
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto external(std::uint8_t channel) const -> std::size_t;
  [[nodiscard]] auto calibration(address at, std::string_view name) const -> std::uint16_t;
  [[nodiscard]] auto sample(std::uint32_t channel) const -> std::uint32_t;
  [[nodiscard]] auto dma_level() const -> bool;
  [[nodiscard]] auto listened() const -> std::optional<std::uint32_t>;
  void               retune(std::optional<std::uint32_t> was);
  [[nodiscard]] auto trigger_latency_ps() -> std::uint64_t;
  void               require_factory_word(std::uint32_t channel) const;
  void               prepare();
  void               begin_sequence(std::uint64_t start_ps);
  void               begin_rank(std::uint64_t start_ps);
  void               schedule(std::uint64_t start_ps);
  void               schedule_sampling_end(std::uint64_t not_before_ps);
  void               finish();
  void               advance(std::uint64_t end_ps, bool last);
  [[nodiscard]] auto guarded(std::size_t watchdog, std::uint32_t channel) const -> bool;
  void               watch(std::uint32_t channel, std::uint32_t result);
  void               land(std::uint32_t result);
  void               pop();
  void               abort();
  void               signal();
  void               check_clock_range();
  void               check_unmodelled(std::uint32_t channel);
  void               check_preselection(std::uint32_t channel);
  void               warn_once(bool& logged, std::string_view reg, std::string_view what);
  void               unmodelled(bool& logged, std::string_view name, std::string_view what);
  void               forbidden(bool& logged, std::string_view name, std::string message);
  void               refuse_start(bool refused);

  register_file&                               regs_;
  supply_options                               supply_;
  core::clock_domain                           kernel_;
  irq_line&                                    line_;
  dma_signal                                   request_;
  reg_ref                                      isr_;
  field_ref                                    enable_;
  field_ref                                    disable_;
  field_ref                                    start_;
  field_ref                                    stop_;
  field_ref                                    injected_start_;
  field_ref                                    regulator_;
  field_ref                                    deep_power_down_;
  field_ref                                    calibrate_;
  field_ref                                    ready_;
  field_ref                                    end_of_sampling_;
  field_ref                                    end_of_conversion_;
  field_ref                                    end_of_sequence_;
  field_ref                                    overrun_;
  field_ref                                    regulator_ready_;
  field_ref                                    dma_mode_;
  field_ref                                    resolution_;
  field_ref                                    trigger_;
  field_ref                                    trigger_select_;
  field_ref                                    overrun_mode_;
  field_ref                                    continuous_;
  field_ref                                    auto_delay_;
  field_ref                                    discontinuous_;
  field_ref                                    discontinuous_count_;
  field_ref                                    length_;
  field_ref                                    data_;
  field_ref                                    oversampling_;
  field_ref                                    oversampling_ratio_;
  field_ref                                    oversampling_shift_;
  field_ref                                    low_frequency_;
  field_ref                                    left_shift_;
  field_ref                                    gain_;
  std::array<field_ref, ranks>                 sequence_;
  std::array<field_ref, channels>              sampling_;
  std::array<offset_fields, offsets>           offsets_;
  std::array<field_ref, channels>              preselection_;
  field_ref                                    watchdog1_;
  field_ref                                    watchdog1_single_;
  field_ref                                    watchdog1_channel_;
  field_ref                                    watchdog1_filter_;
  std::array<field_ref, watchdogs>             watchdog_flags_;
  std::array<field_ref, watchdogs>             watchdog_low_;
  std::array<field_ref, watchdogs>             watchdog_high_;
  std::array<field_ref, watchdogs>             watchdog_channels_;
  std::vector<std::pair<field_ref, field_ref>> interrupts_;
  std::uint32_t                                set_only_{};
  std::uint32_t                                clearable_{};
  std::array<bool, trigger_selections>         unproduced_logged_{};
  bool                                         injected_logged_{};
  bool                                         no_clock_logged_{};
  bool                                         auto_delay_logged_{};
  bool                                         low_frequency_logged_{};
  bool                                         mdf_logged_{};
  bool                                         start_refused_logged_{};
  bool                                         discontinuous_continuous_logged_{};
  bool                                         left_shift_logged_{};
  bool                                         offset_logged_{};
  bool                                         gain_logged_{};
  bool                                         watchdog_filter_dma_logged_{};
  bool                                         preselection_logged_{};
};

constexpr std::array<write_entry<adc>, 5> adc_writes{
    {
     {"ADC_ISR", &adc::isr_written},
     {"ADC_IER", &adc::plain_written},
     {"ADC_CR", &adc::cr_written},
     {"ADC_CFGR1", &adc::plain_written},
     {"ADC_DR", &adc::dr_written},
     }
};

constexpr std::array<std::string_view, offsets> offset_registers{
    "ADC_OFR1", "ADC_OFR2", "ADC_OFR3", "ADC_OFR4"};

adc::adc(model_context& ctx, peripheral_desc const& desc, supply_options supply)
    : dispatch_model{ctx, desc, "ADC", adc_writes}, regs_{ctx.regs}, supply_{supply},
      kernel_{ctx.clock.domain(lower(desc.name))}, line_{irq(desc.name + "_IRQn")},
      request_{ctx.peers, lower(desc.name) + "_dma"}, isr_{reg("ADC_ISR")},
      enable_{field("ADC_CR", "ADEN")}, disable_{field("ADC_CR", "ADDIS")},
      start_{field("ADC_CR", "ADSTART")}, stop_{field("ADC_CR", "ADSTP")},
      injected_start_{field("ADC_CR", "JADSTART")}, regulator_{field("ADC_CR", "ADVREGEN")},
      deep_power_down_{field("ADC_CR", "DEEPPWD")}, calibrate_{field("ADC_CR", "ADCAL")},
      ready_{field("ADC_ISR", "ADRDY")}, end_of_sampling_{field("ADC_ISR", "EOSMP")},
      end_of_conversion_{field("ADC_ISR", "EOC")}, end_of_sequence_{field("ADC_ISR", "EOS")},
      overrun_{field("ADC_ISR", "OVR")}, regulator_ready_{field("ADC_ISR", "LDORDY")},
      dma_mode_{field("ADC_CFGR1", "DMNGT")}, resolution_{field("ADC_CFGR1", "RES")},
      trigger_{field("ADC_CFGR1", "EXTEN")}, trigger_select_{field("ADC_CFGR1", "EXTSEL")},
      overrun_mode_{field("ADC_CFGR1", "OVRMOD")}, continuous_{field("ADC_CFGR1", "CONT")},
      auto_delay_{field("ADC_CFGR1", "AUTDLY")}, discontinuous_{field("ADC_CFGR1", "DISCEN")},
      discontinuous_count_{field("ADC_CFGR1", "DISCNUM")}, length_{field("ADC_SQR1", "L")},
      data_{field("ADC_DR", "RDATA")}, oversampling_{field("ADC_CFGR2", "ROVSE")},
      oversampling_ratio_{field("ADC_CFGR2", "OSR")},
      oversampling_shift_{field("ADC_CFGR2", "OVSS")}, low_frequency_{field("ADC_CFGR2", "LFTRIG")},
      left_shift_{field("ADC_CFGR2", "LSHIFT")}, gain_{field("ADC_GCOMP", "GCOMP")},
      watchdog1_{field("ADC_CFGR1", "AWD1EN")}, watchdog1_single_{field("ADC_CFGR1", "AWD1SGL")},
      watchdog1_channel_{field("ADC_CFGR1", "AWD1CH")},
      watchdog1_filter_{field("ADC_HTR1", "AWDFILT1")}
{
  for(std::size_t index = 0; index < watchdogs; ++index)
  {
    auto const number      = index + 1;
    watchdog_flags_[index] = field("ADC_ISR", std::format("AWD{}", number));
    watchdog_low_[index]   = field(std::format("ADC_LTR{}", number), std::format("LTR{}", number));
    watchdog_high_[index]  = field(std::format("ADC_HTR{}", number), std::format("HTR{}", number));
    if(index != 0)
    {
      watchdog_channels_[index] =
          field(std::format("ADC_AWD{}CR", number), std::format("AWD{}CH", number));
    }
  }
  for(std::size_t rank = 0; rank < ranks; ++rank)
  {
    sequence_[rank] =
        field(std::format("ADC_SQR{}", (rank + 1) / 5 + 1), std::format("SQ{}", rank + 1));
  }
  for(std::size_t index = 0; index < channels; ++index)
  {
    preselection_[index] = field("ADC_PCSEL", std::format("PCSEL{}", index));
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
  auto const& status = describe("ADC_ISR");
  for(auto const& flag : status.fields)
  {
    if(flag.on_write == write_effect::one_to_clear)
    {
      clearable_ |= field("ADC_ISR", flag.name).mask();
    }
  }
  for(auto const& enable : describe("ADC_IER").fields)
  {
    auto const flag = std::string_view{enable.name}.substr(0, enable.name.size() - 2);
    if(enable.name.ends_with("IE")
       and std::ranges::contains(status.fields, flag, &field_desc::name))
    {
      interrupts_.emplace_back(field("ADC_ISR", flag), field("ADC_IER", enable.name));
    }
  }
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
  while(state_.held < samples
        and after(std::uint64_t{state_.held} * conversion() + sampling()) <= ps)
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
  return channel_at(state_.rank);
}

auto adc::channel_at(std::size_t rank) const -> std::uint32_t
{
  return get(regs_, sequence_[std::min(rank, ranks - 1)]);
}

auto adc::length() const -> std::size_t
{
  return get(regs_, length_) + std::size_t{1};
}

auto adc::busy() const -> bool
{
  return state_.converting or state_.waiting_clock;
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
  state_.data.clear();
  state_.dma_stopped  = false;
  state_.filter_count = 0;
  request_.resolve();
  signal();
}

auto adc::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const value = dispatch_model::on_read(regs, offset, size, ctx);
  if((offset & ~3u) == data_.offset and ctx.by != initiator::debugger)
  {
    pop();
  }
  return value;
}

void adc::cr_written(std::uint32_t old, std::uint32_t now)
{
  now |= old & set_only_;
  auto const refused = start_.of(now) != 0
                   and start_.of(old) == 0
                   and (enable_.of(old) == 0 or disable_.of(now) != 0);
  if(refused)
  {
    now = start_.with(now, 0);
  }
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
    signal();
    refuse_start(refused);
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
    signal();
    return;
  }
  if(start_.of(now) != 0 and start_.of(old) == 0)
  {
    state_.dma_stopped = false;
    if(enable_.of(now) != 0 and not busy())
    {
      update(regs_, start_, 0);
      if(get(regs_, trigger_) == 0)
      {
        begin_sequence(ctx_.clock.time().ps);
      }
      else
      {
        prepare();
      }
      update(regs_, start_, 1);
    }
  }
  signal();
  refuse_start(refused);
}

void adc::refuse_start(bool refused)
{
  if(refused)
  {
    forbidden(
        start_refused_logged_, "ADC_CR",
        "ADSTART written with ADEN 0 or ADDIS 1, RM0456 33.6.3: the write is ignored");
  }
}

void adc::isr_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(isr_.offset, old & ~(now & clearable_));
  if(end_of_sampling_.of(old) != 0 and get(regs_, end_of_sampling_) == 0 and state_.converting)
  {
    schedule_sampling_end(ctx_.clock.time().ps);
  }
  signal();
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

void adc::prepare()
{
  if(get(regs_, discontinuous_) != 0 and get(regs_, continuous_) != 0)
  {
    forbidden(
        discontinuous_continuous_logged_, "ADC_CFGR1",
        "DISCEN and CONT both set, forbidden by RM0456 33.4.15: converted as DISCEN alone");
  }
  if(get(regs_, auto_delay_) != 0)
  {
    unmodelled(auto_delay_logged_, "ADC_CFGR1", "AUTDLY: auto-delayed conversions");
  }
  if(get(regs_, low_frequency_) != 0)
  {
    unmodelled(low_frequency_logged_, "ADC_CFGR2", "LFTRIG: low frequency trigger mode");
  }
  if(get(regs_, dma_mode_) == dma_mdf)
  {
    unmodelled(mdf_logged_, "ADC_CFGR1", "DMNGT 10: the MDF data path");
  }
  if(get(regs_, watchdog1_) != 0
     and get(regs_, watchdog1_filter_) != 0
     and get(regs_, dma_mode_) != 0)
  {
    unmodelled(
        watchdog_filter_dma_logged_, "ADC_HTR1",
        "AWDFILT1 with DMNGT: DMA requests gated by the AWD1 window");
  }
  for(std::size_t rank = 0; rank < length(); ++rank)
  {
    check_unmodelled(channel_at(rank));
    check_preselection(channel_at(rank));
    require_factory_word(channel_at(rank));
  }
}

void adc::begin_sequence(std::uint64_t start_ps)
{
  prepare();
  if(get(regs_, discontinuous_) == 0 or state_.rank >= length())
  {
    state_.rank = 0;
  }
  state_.subgroup_left = static_cast<std::uint8_t>(get(regs_, discontinuous_count_) + 1u);
  begin_rank(start_ps);
}

void adc::begin_rank(std::uint64_t start_ps)
{
  if(ctx_.clock.hz(kernel_) == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("ADC_CR", "no ADC kernel clock: the conversion waits");
    }
    state_.waiting_clock = true;
    return;
  }
  schedule(start_ps);
}

void adc::check_unmodelled(std::uint32_t channel)
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
    if(get(regs_, offset.value) != 0 and get(regs_, offset.channel) == channel)
    {
      unmodelled(offset_logged_, offset.reg, "OFFSET: offset of the result");
    }
  }
}

void adc::check_preselection(std::uint32_t channel)
{
  if(channel < channels and get(regs_, preselection_[channel]) == 0)
  {
    forbidden(
        preselection_logged_, "ADC_PCSEL",
        std::format(
            "channel {} converted with its PCSEL bit clear, RM0456 33.4.12: converted as if set",
            channel));
  }
}

void adc::schedule(std::uint64_t start_ps)
{
  state_.start_ps      = start_ps;
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
  if(get(regs_, end_of_sampling_) == 0)
  {
    schedule_sampling_end(start_ps);
  }
}

void adc::schedule_sampling_end(std::uint64_t not_before_ps)
{
  for(std::uint32_t k = 0, samples = ratio(); k < samples; ++k)
  {
    auto const at = after(std::uint64_t{k} * conversion() + sampling());
    if(at > not_before_ps)
    {
      ctx_.events.schedule_at(vtime{at}, sampling_end, state_.generation, ctx_.clock);
      return;
    }
  }
}

void adc::finish()
{
  auto const end    = after(std::uint64_t{ratio()} * conversion());
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
  watch(channel(), result);
  land(result);
  auto const last = std::size_t{state_.rank} + 1u >= length();
  if(last)
  {
    update(regs_, end_of_sequence_, 1);
  }
  advance(end, last);
  signal();
}

void adc::advance(std::uint64_t end_ps, bool last)
{
  state_.rank      = last ? std::uint8_t{0} : static_cast<std::uint8_t>(state_.rank + 1u);
  auto const stays = get(regs_, trigger_) != 0 ? 1u : 0u;
  if(get(regs_, discontinuous_) != 0)
  {
    if(last or state_.subgroup_left <= 1)
    {
      state_.subgroup_left = 0;
      update(regs_, start_, stays);
      return;
    }
    --state_.subgroup_left;
    begin_rank(end_ps);
    return;
  }
  if(last and get(regs_, continuous_) == 0)
  {
    update(regs_, start_, stays);
    return;
  }
  begin_rank(end_ps);
}

auto adc::guarded(std::size_t watchdog, std::uint32_t channel) const -> bool
{
  if(watchdog == 0)
  {
    return get(regs_, watchdog1_) != 0
       and (get(regs_, watchdog1_single_) == 0 or get(regs_, watchdog1_channel_) == channel);
  }
  return channel < channels and (get(regs_, watchdog_channels_[watchdog]) >> channel & 1u) != 0;
}

void adc::watch(std::uint32_t channel, std::uint32_t result)
{
  auto const compared = std::uint64_t{result} << (2 * get(regs_, resolution_));
  for(std::size_t watchdog = 0; watchdog < watchdogs; ++watchdog)
  {
    if(not guarded(watchdog, channel))
    {
      continue;
    }
    auto out = compared < get(regs_, watchdog_low_[watchdog])
            or compared > get(regs_, watchdog_high_[watchdog]);
    if(watchdog == 0)
    {
      state_.filter_count =
          out ? std::min<std::uint8_t>(state_.filter_count + 1u, max_filter_count) : 0;
      out = state_.filter_count > get(regs_, watchdog1_filter_);
    }
    if(out)
    {
      update(regs_, watchdog_flags_[watchdog], 1);
    }
  }
}

void adc::land(std::uint32_t result)
{
  if(get(regs_, overrun_mode_) == 0)
  {
    if(get(regs_, overrun_) != 0 or state_.data.size() >= fifo_depth)
    {
      update(regs_, overrun_, 1);
      return;
    }
  }
  else
  {
    if(get(regs_, end_of_conversion_) != 0)
    {
      update(regs_, overrun_, 1);
    }
    state_.data.clear();
  }
  state_.data.push_back(result);
  update(regs_, data_, state_.data.front());
  update(regs_, end_of_conversion_, 1);
}

void adc::pop()
{
  if(not state_.data.empty())
  {
    state_.data.erase(state_.data.begin());
  }
  if(not state_.data.empty())
  {
    update(regs_, data_, state_.data.front());
  }
  update(regs_, end_of_conversion_, state_.data.empty() ? 0u : 1u);
  signal();
}

void adc::abort()
{
  ++state_.generation;
  state_.converting    = false;
  state_.waiting_clock = false;
  state_.rank          = 0;
  state_.subgroup_left = 0;
}

auto adc::dma_level() const -> bool
{
  auto const mode    = get(regs_, dma_mode_);
  auto const enabled = mode == dma_circular or (mode == dma_one_shot and not state_.dma_stopped);
  return enabled and not state_.data.empty() and get(regs_, overrun_) == 0;
}

void adc::signal()
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
  request_.set(dma_level());
}

auto adc::listened() const -> std::optional<std::uint32_t>
{
  if(get(regs_, enable_) == 0 or get(regs_, start_) == 0 or get(regs_, trigger_) == 0)
  {
    return std::nullopt;
  }
  return get(regs_, trigger_select_);
}

auto adc::listens(std::string_view signal) const -> bool
{
  auto const selected = listened();
  auto const named    = reflex::to_enum_value<regular_trigger>(signal);
  return selected and named and std::to_underlying(*named) == *selected;
}

void adc::triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps)
{
  auto const polarity = get(regs_, trigger_);
  auto const detected = edge == trigger_edge::rising ? (polarity & exten_rising) != 0
                                                     : (polarity & exten_falling) != 0;
  if(not detected or busy() or not listens(signal))
  {
    return;
  }
  begin_sequence(at_ps + trigger_latency_ps());
}

auto adc::trigger_latency_ps() -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == 0)
  {
    return 0;
  }
  auto const div    = divider();
  auto const cycles = div == 1 ? 3u : 3u * div + 1u;
  return static_cast<std::uint64_t>((wide{cycles} * div * ps_per_second + hz - 1u) / hz);
}

auto adc::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  auto const word = offset & ~3u;
  if(word != start_.offset and word != trigger_.offset)
  {
    return dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  auto const config   = regs_.peek(trigger_.offset);
  auto const next     = word == trigger_.offset ? merge_write(config, offset, size, value) : config;
  auto const external = trigger_.of(config) != 0;
  auto const moved    = trigger_select_.of(next) != trigger_select_.of(config);
  if(external)
  {
    static_cast<void>(interest_changed(ctx_.peers, trigger_signal(trigger_select_.of(config))));
  }
  if(trigger_.of(next) != 0 and (not external or moved))
  {
    static_cast<void>(interest_changed(ctx_.peers, trigger_signal(trigger_select_.of(next))));
  }
  auto const was    = listened();
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  retune(was);
  return result;
}

void adc::retune(std::optional<std::uint32_t> was)
{
  auto const now = listened();
  if(now == was)
  {
    return;
  }
  if(was)
  {
    static_cast<void>(interest_changed(ctx_.peers, trigger_signal(*was)));
  }
  if(not now)
  {
    return;
  }
  auto const signal = trigger_signal(*now);
  if(not interest_changed(ctx_.peers, signal))
  {
    unmodelled(
        unproduced_logged_[*now], "ADC_CFGR1",
        signal.starts_with("adc_ext_trg")
            ? std::format("EXTSEL {}: reserved trigger selection", *now)
            : std::format("EXTSEL {}: trigger {}", *now, signal));
  }
}

void adc::transfer_completed(std::uint32_t request)
{
  if(get(regs_, dma_mode_) != dma_one_shot or not request_.carries(request))
  {
    return;
  }
  state_.dma_stopped = true;
  abort();
  signal();
}

void adc::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(payload != state_.generation or not state_.converting)
  {
    return;
  }
  if(kind == sampling_end)
  {
    update(regs_, end_of_sampling_, 1);
    signal();
    return;
  }
  if(kind == conversion_end)
  {
    finish();
  }
}

void adc::on_clock_change()
{
  if(state_.waiting_clock and ctx_.clock.hz(kernel_) != 0)
  {
    schedule(ctx_.clock.time().ps);
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

void adc::forbidden(bool& logged, std::string_view name, std::string message)
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
