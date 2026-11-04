#include "adc_pads.hpp"
#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "factory_data.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

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
constexpr std::uint64_t power_up_ps     = 3'500'000;
constexpr std::uint32_t factory_vdda_mv = 3000;
constexpr std::uint32_t conversion_end  = 0;
constexpr std::uint32_t ready_end       = 1;
constexpr std::uint32_t start_due       = 2;
constexpr std::uint32_t edge_due        = 3;
constexpr std::uint32_t exten_rising    = 1;
constexpr std::uint32_t exten_falling   = 2;
constexpr std::uint64_t latency_halves  = 7;
constexpr std::uint64_t sync_adc_cycles = 3;
constexpr std::uint64_t sync_pclk       = 1;
constexpr std::size_t   selections      = 16;
constexpr std::size_t   injected_ranks  = 4;

constexpr std::array<std::string_view, selections> regular_triggers{
    "tim9_cc2",  "tim9_trgo", "tim2_cc3", "tim2_cc2",  "tim3_trgo", "tim4_cc4",
    "tim2_trgo", "tim3_cc1",  "tim3_cc3", "tim4_trgo", "tim6_trgo", {},
    {},          {},          {},         "exti11"};
constexpr std::array<std::string_view, selections> injected_triggers{
    "tim9_cc1", "tim9_trgo", "tim2_trgo", "tim2_cc1",  "tim3_cc4",  "tim4_trgo",
    "tim4_cc1", "tim4_cc2",  "tim4_cc3",  "tim10_cc1", "tim7_trgo", {},
    {},         {},          {},          "exti15"};
constexpr std::array<std::uint32_t, 8> delay_pclk{0, 0, 7, 15, 31, 63, 127, 255};

constexpr std::array<std::uint32_t, 8> sampling_cycles{4, 9, 16, 24, 48, 96, 192, 384};
constexpr std::array<std::uint32_t, 4> sar_cycles{12, 11, 9, 7};
constexpr std::array<std::uint32_t, 4> prescalers{1, 2, 4, 4};

using wide = unsigned __int128;

[[nodiscard]] constexpr auto cycles_ps(std::uint64_t cycles, std::uint64_t hz) -> std::uint64_t
{
  return static_cast<std::uint64_t>((wide{cycles} * ps_per_second + hz - 1u) / hz);
}

struct adc_l1_edge
{
  bool          pending{};
  bool          falling{};
  std::uint8_t  selection{};
  std::uint64_t ps{};
  std::uint64_t queued{};

  friend auto operator==(adc_l1_edge const&, adc_l1_edge const&) -> bool = default;
};

struct adc_l1_state
{
  std::array<std::uint32_t, channels> input_uv{};
  std::array<std::uint32_t, channels> input_b_uv{};
  std::uint32_t                       generation{};
  std::uint32_t                       ready_generation{};
  std::uint64_t                       start_ps{};
  std::uint64_t                       sample_end_ps{};
  std::uint64_t                       end_ps{};
  std::uint64_t                       clock_hz{};
  std::uint32_t                       held_code{};
  bool                                held{};
  bool                                converting{};
  bool                                waiting_clock{};
  bool                                ready_waiting{};
  bool                                unread{};
  bool                                start_pending{};
  bool                                frozen{};
  bool                                resume_frozen{};
  std::uint64_t                       delay_until_ps{};
  std::uint8_t                        rank{};
  std::uint8_t                        resolution{};
  std::uint8_t                        subgroup_left{};
  bool                                injecting{};
  bool                                regular_resume{};
  bool                                injected_frozen{};
  std::uint64_t                       injected_delay_until_ps{};
  std::uint8_t                        injected_rank{};
  std::uint8_t                        injected_left{};
  std::uint64_t                       queued{};
  bool                                dma_done{};
  adc_l1_edge                         regular_edge{};
  adc_l1_edge                         injected_edge{};

  friend auto operator==(adc_l1_state const&, adc_l1_state const&) -> bool = default;
};

struct trigger_selection
{
  std::optional<std::uint32_t> regular;
  std::optional<std::uint32_t> injected;

  friend auto operator==(trigger_selection const&, trigger_selection const&) -> bool = default;
};

/**
 * @brief The ADC of the STM32L1, `aditf3_v1_1_Cube`, RM0038 section 12, one instance `ADC` with
 *        the common ADC_CSR and ADC_CCR at offsets 0x300 and 0x304: power-up, the regular group
 *        in single, continuous and scan mode, the injected group, resolution, alignment, EOC,
 *        JEOC, OVR and the channel banks, converted in virtual time.
 *
 *        Power, RM0038 12.3.1: an ADC_CR2 write taking ADON from 0 to 1 sets ADONS tSTAB = 3.5 us
 *        later, the DS10002 and DS6876 maximum. The ADC clock is HSI16 divided by ADCPRE of
 *        ADC_CCR, 12.3.2: without HSI the power-up and each conversion wait for it, logged once,
 *        and a conversion keeps the clock it started with. ADCPRE 11 is reserved: it divides by
 *        4 and logs once, a model_error under strict_peripherals. ADON 0 clears ADONS at once
 *        and drops a conversion in progress, clearing SWSTART. The ADC runs from HSI16 whatever
 *        ADC1EN of RCC_APB2ENR holds.
 *
 *        Regular group, RM0038 12.3.4 to 12.3.8: an ADC_CR2 write setting SWSTART with ADONS set
 *        and no conversion running starts the group, SWSTART clearing when the first conversion
 *        starts. With SCAN 0 the group is SQ1 of ADC_SQR5, with SCAN 1 the L + 1 ranks of SQ1 to
 *        SQ28 across ADC_SQR5 to ADC_SQR1, 27 ranks at most on Cat.1 and Cat.2, 28 from Cat.3. An L
 *        above the category's ranks converts all of them, logged once, a model_error under
 *        strict_peripherals: L 27 and above on Cat.1 and Cat.2, L 28 and above from Cat.3. Each
 *        conversion takes SMP + TCONV ADC clock cycles, SMP 4, 9, 16, 24, 48, 96, 192 or 384 from
 *        the channel's field in ADC_SMPR3 to ADC_SMPR0, TCONV 12, 11, 9 and 7 for RES 12, 10, 8 and
 *        6 bits, 12.8. A conversion keeps the end time, the sampling end and the resolution it
 *        started with: RES and SMPx, written with ADON 1 against 12.15.2 and 12.15.4, apply from
 *        the next conversion. A conversion starts when the one before ends. STRT sets at each
 *        conversion start, EOC at the end of each conversion with EOCS 1 and at the end of the
 *        group with EOCS 0. CONT 1 starts the group over after its last rank. A SWSTART while a
 *        conversion runs is cleared and ignored, inferred. A write of ADC_SQR1 to ADC_SQR5 during a
 *        conversion drops it, 12.7.2, and the group waits for a new SWSTART. RCNR and JCNR read 0.
 *        SWSTART with ADONS clear logs a warning once, a model_error under strict_peripherals, and
 *        is ignored.
 *
 *        Results, RM0038 12.4 and 12.11: ADC_DR holds the latest result, right-aligned, or with
 *        ALIGN 1 left-aligned on bit 15, a 6-bit code on bit 7, 12.4 Figure 46. A CPU or DMA
 *        read of ADC_DR clears EOC, a debugger read clears nothing. With EOCS 1, a result finding
 *        ADC_DR unread, neither read nor its EOC cleared, sets OVR and is lost, the group going on,
 *        inferred. With EOCS 0 a result overwrites ADC_DR. ADC_SR flags OVR, STRT, JSTRT, JEOC,
 *        EOC and AWD clear on a write of 0 and keep on a write of 1. ADC1_IRQn is high while EOC,
 *        JEOC, AWD or OVR is set with EOCIE, JEOCIE, AWDIE or OVRIE. ADC_CSR reads ADC_SR bits 6
 *        to 0, 12.15.19.
 *
 *        A description with `channel_pins` holds a positive pad for every external channel and a
 *        bank B pad for every bank B channel on a pad of its own, or construction throws
 *        config_error naming the channel. Every shipped description carries the table, and a
 *        hand-written one without it passes unchecked.
 *
 *        Codes against VDDA, the board's `supply_options`: an external channel reads its driven
 *        input, `analog_port`, as V x 4096 / VDDA, floored and saturated, two bits fewer per RES
 *        step. A conversion holds the input at the end of its sampling phase. With TSVREFE of
 *        ADC_CCR set, channel 16 reads the factory TS_CAL1 and channel 17 VREFINT_CAL, both
 *        scaled by 3000 / VDDA, from the TSVREFE write on, with no sensor tSTART, and 0 with
 *        TSVREFE clear, inferred from 12.12. A group converting one of them
 *        reads the word from the description's factory content when it starts, a config_error
 *        when absent. On Cat.3 to Cat.6 ADC_CFG 1 selects bank B: its channels on a pad of their
 *        own, RM0038 Table 41, read the bank B input of the analog port, `analog_bank::b`, and
 *        the channels sharing a bank A pad read that pad's input, channels 16 and 17 as in bank
 *        A. A group converting VCOMP on channel 26 or a channel the category lacks reads 0 and
 *        logs once, a model_error under strict_peripherals.
 *
 *        Injected group, RM0038 12.3.9 and 12.15.15: JSWSTART with ADONS set, or a JEXTEN
 *        edge, starts the group. With SCAN 1 its JL + 1 conversions run in the order of the
 *        ADC_JSQR note, JSQ4 alone for JL 0, JSQ3 then JSQ4 for JL 1, up to JSQ1 to JSQ4 for JL
 *        3. With SCAN 0 the group is the first of them, JSQ(4 - JL), also over a regular
 *        conversion, inferred against the 12.3.9 "switches to Scan-once mode". The k-th
 *        conversion of the group lands in ADC_JDRk less the JOFFSET of ADC_JOFRk, inferred from
 *        the HAL rank order. JSTRT sets at each injected conversion start, JSWSTART clearing, and
 *        JEOC at the end of the group. An injection drops a regular conversion in progress or
 *        due, and that rank converts again when the injected group ends, once the delay of the
 *        regular conversion before it has passed, 12.9. A regular trigger or SWSTART during the
 *        injected group starts the regular group at its end. A JSWSTART or an injected trigger
 *        while the injected group runs is ignored, JSWSTART cleared, inferred. A write of
 *        ADC_JSQR during the injected group drops it and resumes an interrupted regular
 *        conversion, 12.7.1. A write of ADC_JSQR, or of ADC_SQR1 to ADC_SQR5, between JDISCEN or
 *        DISCEN subgroups starts the next trigger's group at its first conversion, inferred.
 *        JSWSTART with ADONS clear logs a warning once, a model_error under strict_peripherals,
 *        and is ignored.
 *
 *        Injected data, RM0038 12.4 Figures 44 to 46: the code less the offset as a 16-bit two's
 *        complement value, right-aligned, or with ALIGN 1 one bit below a regular result, the
 *        sign on bit 15 at 12 bits and on bit 7 and above at 6 bits, inferred at 10 and 8 bits.
 *        Below 12 bits JOFFSET counts from bit 11 and its bits under the resolution are ignored,
 *        inferred from the LL_ADC_INJ_SetOffset note. A JDRx read clears nothing.
 *
 *        Hardware triggers, RM0038 12.6: with EXTEN not 0, ADON and ADONS set, the model listens,
 *        as a trigger_sink, to the signal EXTSEL selects in RM0038 Table 61, such as `tim2_trgo`
 *        for 0110. EXTEN 01 detects rising edges, 10 falling edges, 11 both. A detected edge
 *        while no conversion runs, starts or waits and outside a DELS delay starts the group, or
 *        the next DISCEN subgroup, tlatr = 3.5 ADC clock cycles later, the DS10002 maximum.
 *        SWSTART starts the group too. JEXTEN and JEXTSEL do the same for the injected group with
 *        RM0038 Table 62 and the same tlatr, inferred. The TRGO selections take the timer's
 *        `timx_trgo`, the TIMx_CCy selections its `timx_ccy` capture/compare event pulse, and
 *        EXTI11 and EXTI15 the EXTI's `exti11` and `exti15` line inputs, inferred, a pin edge
 *        counting at its own time. One edge of a signal both groups select starts both, the
 *        injected group first. A selection whose source has no model on the part, and the
 *        reserved 1011 to 1110, log once when the model starts listening, a model_error under
 *        strict_peripherals, and start nothing. A model_error of a selection surfaces from the
 *        ADC_CR2 write or the ADONS event that starts the listening. A config_error or
 *        model_error of the group checks raised at a trigger surfaces from the source's event or
 *        register access that gave the edge. Trigger detection stays on across an EXTSEL change,
 *        RM0038 12.6 blanks it for 2 PCLK cycles. An edge first completes the ADC's own
 *        conversion end or start due at or before the edge, so an edge and a conversion end in
 *        one core cycle are taken in time order. An edge its source gives ahead of the machine
 *        time, the falling edge of a pulse, is taken at its own time if the group that detected
 *        it still selects and detects it then, and a reset drops it. Each group holds one such
 *        edge: a second one ahead for a group already holding one, two pulses of its signal
 *        inside one timer clock, is decided against the ADC state at its arrival, so its
 *        conversion starts at its own time and the held edge then finds the group busy.
 *
 *        Discontinuous mode, RM0038 12.3.10: with DISCEN each trigger or SWSTART converts the
 *        next DISCNUM + 1 ranks, fewer in the last subgroup, the next one starting over at rank
 *        1. DISCEN with CONT converts as DISCEN alone, logged once, a model_error under
 *        strict_peripherals, inferred. With JDISCEN each JSWSTART or injected trigger converts
 *        the next DISCNUM + 1 conversions of the injected group, JEOC alone setting after the
 *        last, inferred. DISCEN with JDISCEN, against 12.3.10, logs once, a model_error under
 *        strict_peripherals, each group following its own bit.
 *
 *        Auto-injection, RM0038 12.3.9: with JAUTO the injected group converts after the last
 *        regular rank, whole, and with CONT the regular group starts over after it. JSWSTART is
 *        then ignored. At the regular group start JAUTO logs once, a model_error under
 *        strict_peripherals, with JEXTEN not 0 or with DISCEN or JDISCEN, against 12.3.9, and
 *        converts the whole injected group, its edges ignored, inferred.
 *
 *        Delay insertion, RM0038 12.9: DELS 001 holds the next conversion at each EOC, after
 *        each conversion with EOCS 1 and after the group with EOCS 0, inferred for EOCS 0,
 *        until ADC_DR is read or EOC cleared. DELS 010 to 111 apply after each regular
 *        conversion, after each group with EOCS 1, 12.9.1, and hold the next conversion 7, 15,
 *        31, 63, 127 or 255 PCLK2 cycles after the end of the conversion, plus 1 PCLK2 cycle
 *        and 3 ADC clock cycles of synchronisation, the maxima of the RM0038 Figure 48 note,
 *        inferred, at the PCLK2 rate of the conversion end. A trigger or SWSTART during the
 *        hold is ignored, 12.9 and the hardware or software trigger of Figure 47. The same holds
 *        apply after each injected group, DELS 001 until JEOC clears, and ignore injected
 *        triggers and JSWSTART only. An injected trigger during a regular hold starts at once.
 *
 *        Analog watchdog, RM0038 12.3.7 and Table 59: AWDEN guards the regular conversions,
 *        JAWDEN the injected ones, all channels or with AWDSGL the AWDCH channel only. A guarded
 *        conversion sets AWD when its code, before alignment and offset, is above HT of ADC_HTR
 *        or below LT of ADC_LTR, at the end of the conversion, the thresholds read then, 12.15.8.
 *        The code before the offset, below 12 bits the code shifted to 12 bits, and a result lost
 *        to an overrun are compared, each inferred. With SCAN and AWDIE set, a guarded conversion
 *        that sets AWD aborts the scan, RM0038 12.15.2: its result lands as the group's last, EOC
 *        and JEOC set as at the group end, and the next trigger or SWSTART starts at the first
 *        rank. CONT, the next DISCEN subgroup and JAUTO do not go on. The rank, the flags and the
 *        injected group are inferred.
 *
 *        DMA, RM0038 12.11.1: with DMA 1 each regular result requests `adc_dma`, DMA1 channel 1,
 *        until ADC_DR is read or a write of 0 to EOC, and a result finding ADC_DR unread sets
 *        OVR and is lost as with EOCS 1. With DMA 1 an overrun aborts the regular conversion and
 *        further regular triggers are ignored, 12.11.1. They stay ignored until OVR is cleared,
 *        SWSTART counting as a trigger, inferred. With DDS 0 the end of the count of the channel
 *        while DMA is 1, at the end of a normal channel or at a circular reload, stops the
 *        requests and the ADC converts as with DMA 0 until DMA is written 0, then 1, inferred.
 *        DDS 1 keeps the requests going.
 *
 *        Not modelled, each logged once, a model_error under strict_peripherals: at the regular
 *        group start PDI, PDD and DELS with JAUTO, and at the injected group start PDI and PDD.
 */
class adc_l1_model final : public dispatch_model<adc_l1_model, adc_l1_state>,
                           public analog_port,
                           public clock_listener,
                           public trigger_sink,
                           public dma_requester,
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
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;

  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;
  void transfer_completed(dma_controller const& by, std::uint32_t request) override;
  void count_ended(dma_controller const& by, std::uint32_t request) override;

  void               drive(analog_channel channel, std::uint32_t microvolts) override;
  [[nodiscard]] auto input(analog_channel channel) const -> std::uint32_t override;
  [[nodiscard]] auto external_channels(analog_bank bank) const -> std::uint32_t override
  {
    return bank == analog_bank::a ? variant_.external : variant_.bank_b_only;
  }
  [[nodiscard]] auto bonded_channels(analog_bank bank) const -> std::uint32_t override
  {
    return bank == analog_bank::a ? bonded_channels_ : bonded_channels_b_;
  }
  [[nodiscard]] auto vdda_mv() const -> std::uint32_t override
  {
    return supply_.vdda_mv;
  }

  void sr_written(std::uint32_t old, std::uint32_t now);
  void cr1_written(std::uint32_t old, std::uint32_t now);
  void cr2_written(std::uint32_t old, std::uint32_t now);
  void sequence_written(std::uint32_t, std::uint32_t);
  void injected_sequence_written(std::uint32_t, std::uint32_t);
  void plain_written(std::uint32_t, std::uint32_t)
  {
    signal();
  }

private:
  void               restore_reset_values();
  [[nodiscard]] auto selected(std::uint32_t control, bool ready) const -> trigger_selection;
  [[nodiscard]] auto listened() const -> trigger_selection;
  [[nodiscard]] auto blocked(std::uint64_t ps) const -> bool;
  [[nodiscard]] auto injected_blocked(std::uint64_t ps) const -> bool;
  void               check_selection(trigger_selection const& was, trigger_selection const& will);
  void               retune(trigger_selection const& was);
  void               notify(trigger_selection const& selection);
  void               software_start();
  void               injected_software_start();
  void               regular_start(std::uint64_t start_ps, std::uint64_t edge_ps, bool software);
  void               injected_start(std::uint64_t start_ps, std::uint64_t edge_ps, bool software);
  void               arm_group();
  void               start_injected(std::uint64_t start_ps);
  void               suspend_regular();
  void               resume_regular(std::uint64_t start_ps);
  void               start_now(std::uint64_t start_ps);
  void               release_freeze();
  void               hold(std::uint64_t end_ps, bool& frozen, std::uint64_t& until_ps);
  [[nodiscard]] auto adc_hz() -> std::uint64_t;
  [[nodiscard]] auto length() const -> std::size_t;
  [[nodiscard]] auto injected_length() const -> std::size_t;
  [[nodiscard]] auto channel_at(std::size_t rank) const -> std::uint32_t;
  [[nodiscard]] auto injected_channel(std::size_t rank) const -> std::uint32_t;
  [[nodiscard]] auto current_channel() const -> std::uint32_t;
  [[nodiscard]] auto bank_b() const -> bool;
  [[nodiscard]] auto sampling(std::uint32_t channel) const -> std::uint32_t;
  [[nodiscard]] auto conversion_cycles() const -> std::uint64_t;
  [[nodiscard]] auto sample_end_ps() const -> std::uint64_t;
  [[nodiscard]] auto engine_busy() const -> bool;
  [[nodiscard]] auto regular_busy() const -> bool;
  [[nodiscard]] auto readable(std::uint32_t channel) const -> bool;
  [[nodiscard]] auto calibration(std::uint32_t at, std::string_view name) const -> std::uint16_t;
  [[nodiscard]] auto code(std::uint32_t channel) const -> std::uint32_t;
  [[nodiscard]] auto external(analog_channel channel) const -> std::size_t;
  void               hold_until(std::uint64_t ps);
  void               power_up();
  void               check_common();
  void               check_group();
  void               check_injected();
  void               check_channel(std::uint32_t channel, std::string_view reg);
  void               begin(std::uint64_t start_ps);
  void               schedule(std::uint64_t start_ps);
  void               finish();
  void               finish_injected();
  [[nodiscard]] auto land(std::uint32_t result) -> bool;
  [[nodiscard]] auto dma_active() const -> bool;
  [[nodiscard]] auto scan_aborted() const -> bool;
  [[nodiscard]] auto guard(std::uint32_t channel, std::uint32_t result, field_ref const& enable)
      -> bool;
  void store_injected(std::size_t index, std::uint32_t result);
  void stop_engine();
  void run_due(std::uint64_t ps);
  void take_edge(bool regular, bool injected, std::uint64_t at_ps);
  void defer_edge(
      adc_l1_edge&  slot,
      std::uint32_t group,
      std::uint32_t selection,
      trigger_edge  edge,
      std::uint64_t at_ps);
  void fire_edge(std::uint32_t group);
  void drop_edge();
  void abort_regular();
  void abort();
  void signal();
  void unmodelled(bool& logged, std::string_view name, std::string_view what);
  void forbidden(bool& logged, std::string_view name, std::string message);

  register_file&                                  regs_;
  supply_options                                  supply_;
  adc_l1_variant                                  variant_;
  std::uint32_t                                   bonded_channels_{};
  std::uint32_t                                   bonded_channels_b_{};
  core::clock_domain                              hsi_;
  core::clock_domain                              pclk2_;
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
  field_ref                                       trigger_select_;
  field_ref                                       injected_trigger_;
  field_ref                                       continuous_;
  field_ref                                       end_selection_;
  field_ref                                       align_;
  field_ref                                       dma_;
  field_ref                                       delay_;
  std::optional<field_ref>                        bank_;
  field_ref                                       scan_;
  field_ref                                       watchdog_interrupt_;
  field_ref                                       resolution_;
  field_ref                                       discontinuous_;
  field_ref                                       discontinuous_count_;
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
  field_ref                                       injected_started_;
  field_ref                                       injected_end_;
  field_ref                                       injected_select_;
  field_ref                                       injected_discontinuous_;
  field_ref                                       injected_watchdog_;
  field_ref                                       watchdog_single_;
  field_ref                                       watchdog_channel_;
  field_ref                                       watchdog_flag_;
  field_ref                                       high_;
  field_ref                                       low_;
  field_ref                                       dma_continue_;
  dma_signal                                      request_;
  field_ref                                       injected_length_;
  std::array<field_ref, injected_ranks>           injected_sequence_;
  std::array<field_ref, injected_ranks>           offsets_;
  std::array<field_ref, injected_ranks>           injected_data_;
  std::uint32_t                                   clearable_{};
  std::array<bool, selections>                    selection_logged_{};
  std::array<bool, selections>                    injected_selection_logged_{};
  bool                                            injected_refused_logged_{};
  bool                                            auto_start_logged_{};
  bool                                            auto_trigger_logged_{};
  bool                                            auto_discontinuous_logged_{};
  bool                                            auto_delay_logged_{};
  bool                                            both_discontinuous_logged_{};
  bool                                            discontinuous_continuous_logged_{};
  bool                                            power_down_logged_{};
  bool                                            start_refused_logged_{};
  bool                                            length_logged_{};
  bool                                            prescaler_logged_{};
  bool                                            no_clock_logged_{};
  std::array<bool, channels>                      channel_logged_{};
};

constexpr std::array<write_entry<adc_l1_model>, 24> banked_writes{
    {
     {"SR", &adc_l1_model::sr_written},
     {"CR1", &adc_l1_model::cr1_written},
     {"CR2", &adc_l1_model::cr2_written},
     {"SMPR1", &adc_l1_model::plain_written},
     {"SMPR2", &adc_l1_model::plain_written},
     {"SMPR3", &adc_l1_model::plain_written},
     {"JOFR1", nullptr},
     {"JOFR2", nullptr},
     {"JOFR3", nullptr},
     {"JOFR4", nullptr},
     {"SQR1", &adc_l1_model::sequence_written},
     {"SQR2", &adc_l1_model::sequence_written},
     {"SQR3", &adc_l1_model::sequence_written},
     {"SQR4", &adc_l1_model::sequence_written},
     {"SQR5", &adc_l1_model::sequence_written},
     {"JSQR", &adc_l1_model::injected_sequence_written},
     {"JDR1", nullptr},
     {"JDR2", nullptr},
     {"JDR3", nullptr},
     {"JDR4", nullptr},
     {"HTR", nullptr},
     {"LTR", nullptr},
     {"DR", nullptr},
     {"SMPR0", &adc_l1_model::plain_written},
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
      variant_{variant}, hsi_{ctx.clock.domain("hsi")}, pclk2_{ctx.clock.domain("pclk2")},
      line_{irq("ADC1_IRQn")}, status_{reg("SR")}, common_status_{reg("CSR")},
      ready_{field("SR", "ADONS")}, overrun_{field("SR", "OVR")}, started_{field("SR", "STRT")},
      end_of_conversion_{field("SR", "EOC")}, power_{field("CR2", "ADON")},
      start_{field("CR2", "SWSTART")}, injected_start_{field("CR2", "JSWSTART")},
      trigger_{field("CR2", "EXTEN")}, trigger_select_{field("CR2", "EXTSEL")},
      injected_trigger_{field("CR2", "JEXTEN")}, continuous_{field("CR2", "CONT")},
      end_selection_{field("CR2", "EOCS")}, align_{field("CR2", "ALIGN")},
      dma_{field("CR2", "DMA")}, delay_{field("CR2", "DELS")}, scan_{field("CR1", "SCAN")},
      watchdog_interrupt_{field("CR1", "AWDIE")}, resolution_{field("CR1", "RES")},
      discontinuous_{field("CR1", "DISCEN")}, discontinuous_count_{field("CR1", "DISCNUM")},
      auto_injected_{field("CR1", "JAUTO")}, watchdog_{field("CR1", "AWDEN")},
      power_down_idle_{field("CR1", "PDI")}, power_down_delay_{field("CR1", "PDD")},
      length_{field("SQR1", "L")}, data_{field("DR", "RegularDATA")},
      sensors_{field("CCR", "TSVREFE")}, prescaler_{field("CCR", "ADCPRE")},
      request_{ctx.peers, "adc_dma"}
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
  injected_started_       = field("SR", "JSTRT");
  injected_end_           = field("SR", "JEOC");
  injected_select_        = field("CR2", "JEXTSEL");
  injected_discontinuous_ = field("CR1", "JDISCEN");
  injected_watchdog_      = field("CR1", "JAWDEN");
  watchdog_single_        = field("CR1", "AWDSGL");
  watchdog_channel_       = field("CR1", "AWDCH");
  watchdog_flag_          = field("SR", "AWD");
  high_                   = field("HTR", "HT");
  low_                    = field("LTR", "LT");
  dma_continue_           = field("CR2", "DDS");
  injected_length_        = field("JSQR", "JL");
  for(std::size_t rank = 0; rank < injected_ranks; ++rank)
  {
    injected_sequence_[rank] = field("JSQR", std::format("JSQ{}", rank + 1));
    offsets_[rank] = field(std::format("JOFR{}", rank + 1), std::format("JOFFSET{}", rank + 1));
    injected_data_[rank] = field(std::format("JDR{}", rank + 1), "JDATA");
  }
  interrupts_ = {
      {{end_of_conversion_, field("CR1", "EOCIE")},
       {injected_end_, field("CR1", "JEOCIE")},
       {watchdog_flag_, watchdog_interrupt_},
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
  bonded_channels_b_ = desc.bonded_channels_b.value_or(variant.bank_b_only);
  if((bonded_channels_b_ & ~variant.bank_b_only) != 0)
  {
    throw config_error(
        desc.name, 0, "bonded_channels_b", "a mask of bank B channels on a pad of their own");
  }
  require_channel_pins(desc, variant.external, pad_role::positive);
  require_channel_pins(desc, variant.bank_b_only, pad_role::bank_b);
}

void adc_l1_model::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void adc_l1_model::on_bus_reset()
{
  auto const was = listened();
  notify(was);
  restore_reset_values();
  retune(was);
}

void adc_l1_model::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  abort();
  drop_edge();
  ++state_.ready_generation;
  state_.ready_waiting = false;
  state_.unread        = false;
  state_.dma_done      = false;
  request_.resolve();
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

auto adc_l1_model::injected_length() const -> std::size_t
{
  return get(regs_, scan_) == 0 ? 1 : get(regs_, injected_length_) + 1u;
}

auto adc_l1_model::channel_at(std::size_t rank) const -> std::uint32_t
{
  return get(regs_, *sequence_[rank]);
}

auto adc_l1_model::injected_channel(std::size_t rank) const -> std::uint32_t
{
  return get(regs_, injected_sequence_[injected_ranks - 1u - get(regs_, injected_length_) + rank]);
}

auto adc_l1_model::current_channel() const -> std::uint32_t
{
  return state_.injecting ? injected_channel(state_.injected_rank) : channel_at(state_.rank);
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
  return std::uint64_t{sampling(current_channel())} + sar_cycles[get(regs_, resolution_)];
}

auto adc_l1_model::sample_end_ps() const -> std::uint64_t
{
  return state_.sample_end_ps;
}

auto adc_l1_model::engine_busy() const -> bool
{
  return state_.converting or state_.waiting_clock or state_.start_pending;
}

auto adc_l1_model::regular_busy() const -> bool
{
  return (engine_busy() and not state_.injecting) or state_.resume_frozen or state_.regular_resume;
}

auto adc_l1_model::readable(std::uint32_t channel) const -> bool
{
  return (variant_.external >> channel & 1u) != 0;
}

auto adc_l1_model::calibration(std::uint32_t at, std::string_view name) const -> std::uint16_t
{
  auto const word = factory_halfword(ctx_.part, at);
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
  auto const              shift      = 2u * state_.resolution;
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
  auto const own_pad = bank_b() and (variant_.bank_b_only >> channel & 1u) != 0;
  auto const scaled  = std::uint64_t{(own_pad ? state_.input_b_uv : state_.input_uv)[channel]}
                     * full_scale
                     / (std::uint64_t{supply_.vdda_mv} * 1000u);
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(full_scale - 1u, scaled)) >> shift;
}

auto adc_l1_model::external(analog_channel channel) const -> std::size_t
{
  auto const number = channel.number;
  auto const listed = number < channels;
  if(channel.bank == analog_bank::a)
  {
    if(not listed or (variant_.external >> number & 1u) == 0)
    {
      throw config_error(
          desc_.name, 0, "channel",
          std::format("an external channel of mask {:#010x}, not {}", variant_.external, number));
    }
    return number;
  }
  if(listed and (variant_.bank_b_only >> number & 1u) != 0)
  {
    return number;
  }
  if(listed and variant_.banks and (variant_.external >> number & 1u) != 0)
  {
    throw config_error(
        desc_.name, 0, "channel",
        std::format(
            "a bank B channel on a pad of its own, mask {:#010x}, not {}b, which shares the pad "
            "of bank A channel {}",
            variant_.bank_b_only, number, number));
  }
  throw config_error(
      desc_.name, 0, "channel",
      std::format(
          "a bank B channel on a pad of its own, mask {:#010x}, not {}b", variant_.bank_b_only,
          number));
}

void adc_l1_model::drive(analog_channel channel, std::uint32_t microvolts)
{
  auto const at = external(channel);
  if(state_.converting)
  {
    hold_until(ctx_.clock.time().ps);
  }
  (channel.bank == analog_bank::a ? state_.input_uv : state_.input_b_uv)[at] = microvolts;
}

auto adc_l1_model::input(analog_channel channel) const -> std::uint32_t
{
  auto const at = external(channel);
  return (channel.bank == analog_bank::a ? state_.input_uv : state_.input_b_uv)[at];
}

void adc_l1_model::hold_until(std::uint64_t ps)
{
  if(not state_.held and sample_end_ps() <= ps)
  {
    state_.held_code = code(current_channel());
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
    release_freeze();
  }
  return value;
}

auto adc_l1_model::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const word = offset & ~3u;
  if(word != trigger_.offset)
  {
    return base::on_write(regs, offset, size, old, value, ctx);
  }
  auto const next = merge_write(regs_.peek(word), offset, size, value);
  auto const was  = listened();
  auto const will = selected(next, get(regs_, ready_) != 0);
  check_selection(was, will);
  notify(was);
  if(will != was)
  {
    notify(will);
  }
  auto const result = base::on_write(regs, offset, size, old, value, ctx);
  retune(was);
  return result;
}

auto adc_l1_model::selected(std::uint32_t control, bool ready) const -> trigger_selection
{
  if(power_.of(control) == 0 or not ready)
  {
    return {};
  }
  trigger_selection out;
  if(trigger_.of(control) != 0)
  {
    out.regular = trigger_select_.of(control);
  }
  if(injected_trigger_.of(control) != 0)
  {
    out.injected = injected_select_.of(control);
  }
  return out;
}

auto adc_l1_model::listened() const -> trigger_selection
{
  return selected(regs_.peek(trigger_.offset), get(regs_, ready_) != 0);
}

auto adc_l1_model::can_listen(std::string_view signal) const -> bool
{
  return not signal.empty()
     and (std::ranges::contains(regular_triggers, signal) or std::ranges::contains(injected_triggers, signal));
}

auto adc_l1_model::listens(std::string_view signal) const -> bool
{
  auto const now = listened();
  return (now.regular and regular_triggers[*now.regular] == signal)
      or (now.injected and injected_triggers[*now.injected] == signal);
}

void adc_l1_model::check_selection(trigger_selection const& was, trigger_selection const& will)
{
  auto const check = [this](
                         std::span<std::string_view const, selections> table,
                         std::span<bool, selections> logged, std::string_view name,
                         std::uint32_t selection) {
    auto const signal = table[selection];
    if(signal.empty())
    {
      forbidden(
          logged[selection], "CR2",
          std::format(
              "{} {:#06b}: a reserved trigger selection, no conversion starts", name, selection));
      return;
    }
    if(trigger_source_of(ctx_.peers, signal) == nullptr)
    {
      unmodelled(
          logged[selection], "CR2",
          std::format("{} {:#06b}: trigger {}, which no model produces,", name, selection, signal));
    }
  };
  if(will.regular and will.regular != was.regular)
  {
    check(regular_triggers, selection_logged_, "EXTSEL", *will.regular);
  }
  if(will.injected and will.injected != was.injected)
  {
    check(injected_triggers, injected_selection_logged_, "JEXTSEL", *will.injected);
  }
}

void adc_l1_model::retune(trigger_selection const& was)
{
  auto const now = listened();
  if(now == was)
  {
    return;
  }
  notify(was);
  notify(now);
}

void adc_l1_model::notify(trigger_selection const& selection)
{
  if(selection.regular and not regular_triggers[*selection.regular].empty())
  {
    static_cast<void>(interest_changed(ctx_.peers, regular_triggers[*selection.regular]));
  }
  if(selection.injected and not injected_triggers[*selection.injected].empty())
  {
    static_cast<void>(interest_changed(ctx_.peers, injected_triggers[*selection.injected]));
  }
}

void adc_l1_model::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  auto const now     = listened();
  auto const detects = [edge](std::uint32_t polarity) {
    return edge == trigger_edge::rising ? (polarity & exten_rising) != 0
                                        : (polarity & exten_falling) != 0;
  };
  auto const injected = now.injected
                    and injected_triggers[*now.injected] == signal
                    and detects(get(regs_, injected_trigger_))
                    and get(regs_, auto_injected_) == 0;
  auto const regular =
      now.regular and regular_triggers[*now.regular] == signal and detects(get(regs_, trigger_));
  if(not injected and not regular)
  {
    return;
  }
  auto const ahead          = at.ps > ctx_.clock.time().ps;
  auto const defer_regular  = ahead and regular and not state_.regular_edge.pending;
  auto const defer_injected = ahead and injected and not state_.injected_edge.pending;
  if(defer_regular)
  {
    defer_edge(state_.regular_edge, 0, *now.regular, edge, at.ps);
  }
  if(defer_injected)
  {
    defer_edge(state_.injected_edge, 1, *now.injected, edge, at.ps);
  }
  if((regular and not defer_regular) or (injected and not defer_injected))
  {
    take_edge(regular and not defer_regular, injected and not defer_injected, at.ps);
  }
}

void adc_l1_model::take_edge(bool regular, bool injected, std::uint64_t at_ps)
{
  auto const hz    = adc_hz();
  auto const start = at_ps + (hz == 0 ? 0 : cycles_ps(latency_halves, 2 * hz));
  run_due(std::min(at_ps, ctx_.clock.time().ps));
  if(injected
     and regular
     and not state_.injecting
     and not injected_blocked(at_ps)
     and not regular_busy()
     and not blocked(at_ps))
  {
    check_injected();
    check_group();
  }
  if(injected)
  {
    injected_start(start, at_ps, false);
  }
  if(regular)
  {
    regular_start(start, at_ps, false);
  }
}

void adc_l1_model::defer_edge(
    adc_l1_edge&  slot,
    std::uint32_t group,
    std::uint32_t selection,
    trigger_edge  edge,
    std::uint64_t at_ps)
{
  slot.pending   = true;
  slot.falling   = edge == trigger_edge::falling;
  slot.selection = static_cast<std::uint8_t>(selection);
  slot.ps        = at_ps;
  slot.queued    = ctx_.events.schedule_at(vtime{at_ps}, edge_due, group, ctx_.clock) + 1;
}

void adc_l1_model::fire_edge(std::uint32_t group)
{
  auto& regular_slot  = state_.regular_edge;
  auto& injected_slot = state_.injected_edge;
  auto& slot          = group == 0 ? regular_slot : injected_slot;
  auto& other         = group == 0 ? injected_slot : regular_slot;
  if(not slot.pending)
  {
    return;
  }
  slot.pending = false;
  slot.queued  = 0;
  auto const joint =
      other.pending
      and other.ps == slot.ps
      and other.falling == slot.falling
      and regular_triggers[regular_slot.selection] == injected_triggers[injected_slot.selection];
  if(joint)
  {
    if(auto const seq = std::exchange(other.queued, 0); seq != 0)
    {
      ctx_.events.cancel(seq - 1);
    }
    other.pending = false;
  }
  auto const now     = listened();
  auto const detects = [](std::uint32_t polarity, bool falling) {
    return (polarity & (falling ? exten_falling : exten_rising)) != 0;
  };
  auto const regular = (group == 0 or joint)
                   and now.regular == std::optional<std::uint32_t>{regular_slot.selection}
                   and detects(get(regs_, trigger_), regular_slot.falling);
  auto const injected = (group == 1 or joint)
                    and now.injected == std::optional<std::uint32_t>{injected_slot.selection}
                    and detects(get(regs_, injected_trigger_), injected_slot.falling)
                    and get(regs_, auto_injected_) == 0;
  if(injected or regular)
  {
    take_edge(regular, injected, slot.ps);
  }
}

void adc_l1_model::drop_edge()
{
  for(auto* const slot : {&state_.regular_edge, &state_.injected_edge})
  {
    if(auto const seq = std::exchange(slot->queued, 0); seq != 0)
    {
      ctx_.events.cancel(seq - 1);
    }
    slot->pending = false;
  }
}

auto adc_l1_model::blocked(std::uint64_t ps) const -> bool
{
  return state_.frozen
      or ps < state_.delay_until_ps
      or (get(regs_, overrun_) != 0 and dma_active());
}

auto adc_l1_model::injected_blocked(std::uint64_t ps) const -> bool
{
  return state_.injected_frozen or ps < state_.injected_delay_until_ps;
}

void adc_l1_model::release_freeze()
{
  if(not std::exchange(state_.frozen, false))
  {
    return;
  }
  if(std::exchange(state_.resume_frozen, false))
  {
    resume_regular(ctx_.clock.time().ps);
  }
}

void adc_l1_model::resume_regular(std::uint64_t start_ps)
{
  if(state_.injecting)
  {
    state_.regular_resume = true;
    return;
  }
  begin(start_ps);
}

void adc_l1_model::sr_written(std::uint32_t old, std::uint32_t now)
{
  auto const kept = old & (now | ~clearable_);
  regs_.poke(status_.offset, kept);
  if(end_of_conversion_.of(now) == 0)
  {
    state_.unread = false;
  }
  signal();
  if(injected_end_.of(old) != 0 and injected_end_.of(kept) == 0)
  {
    state_.injected_frozen = false;
  }
  if(end_of_conversion_.of(old) != 0 and end_of_conversion_.of(kept) == 0)
  {
    release_freeze();
  }
}

void adc_l1_model::cr1_written(std::uint32_t, std::uint32_t)
{
  signal();
}

void adc_l1_model::cr2_written(std::uint32_t old, std::uint32_t now)
{
  if(dma_.of(now) != dma_.of(old))
  {
    state_.dma_done = false;
  }
  signal();
  if(power_.of(now) == 0 and power_.of(old) != 0)
  {
    update(regs_, ready_, 0);
    update(regs_, start_, 0);
    update(regs_, injected_start_, 0);
    ++state_.ready_generation;
    state_.ready_waiting = false;
    abort();
    signal();
    return;
  }
  if(power_.of(now) != 0 and power_.of(old) == 0)
  {
    power_up();
  }
  if(start_.of(now) != 0 and start_.of(old) == 0)
  {
    software_start();
  }
  if(injected_start_.of(now) != 0 and injected_start_.of(old) == 0)
  {
    injected_software_start();
  }
}

void adc_l1_model::software_start()
{
  if(get(regs_, ready_) == 0)
  {
    update(regs_, start_, 0);
    forbidden(
        start_refused_logged_, "CR2",
        "SWSTART with ADONS clear, forbidden by RM0038 12.15.3: ignored");
    return;
  }
  auto const now = ctx_.clock.time().ps;
  regular_start(now, now, true);
}

void adc_l1_model::injected_software_start()
{
  if(get(regs_, ready_) == 0)
  {
    update(regs_, injected_start_, 0);
    forbidden(
        injected_refused_logged_, "CR2",
        "JSWSTART with ADONS clear, forbidden by RM0038 12.15.3: ignored");
    return;
  }
  if(get(regs_, auto_injected_) != 0)
  {
    update(regs_, injected_start_, 0);
    forbidden(
        auto_start_logged_, "CR2",
        "JSWSTART with JAUTO, against RM0038 12.3.9: ignored, the injected group follows the "
        "regular group");
    return;
  }
  auto const now = ctx_.clock.time().ps;
  injected_start(now, now, true);
}

void adc_l1_model::regular_start(std::uint64_t start_ps, std::uint64_t edge_ps, bool software)
{
  if(regular_busy() or blocked(edge_ps))
  {
    if(software and not(state_.waiting_clock and not state_.injecting))
    {
      update(regs_, start_, 0);
    }
    return;
  }
  try
  {
    arm_group();
  }
  catch(...)
  {
    if(software)
    {
      update(regs_, start_, 0);
    }
    throw;
  }
  if(state_.injecting)
  {
    state_.regular_resume = true;
    return;
  }
  begin(start_ps);
}

void adc_l1_model::injected_start(std::uint64_t start_ps, std::uint64_t edge_ps, bool software)
{
  if(state_.injecting or injected_blocked(edge_ps))
  {
    if(software and not(state_.waiting_clock and state_.injecting))
    {
      update(regs_, injected_start_, 0);
    }
    return;
  }
  try
  {
    check_injected();
  }
  catch(...)
  {
    if(software)
    {
      update(regs_, injected_start_, 0);
    }
    throw;
  }
  suspend_regular();
  start_injected(start_ps);
}

void adc_l1_model::arm_group()
{
  check_group();
  if(get(regs_, discontinuous_) == 0 or state_.rank >= length())
  {
    state_.rank = 0;
  }
  state_.subgroup_left = static_cast<std::uint8_t>(get(regs_, discontinuous_count_) + 1u);
}

void adc_l1_model::start_injected(std::uint64_t start_ps)
{
  if(get(regs_, injected_discontinuous_) == 0
     or get(regs_, auto_injected_) != 0
     or state_.injected_rank >= injected_length())
  {
    state_.injected_rank = 0;
  }
  state_.injected_left = static_cast<std::uint8_t>(get(regs_, discontinuous_count_) + 1u);
  state_.injecting     = true;
  begin(start_ps);
}

void adc_l1_model::suspend_regular()
{
  if(not engine_busy())
  {
    return;
  }
  stop_engine();
  state_.regular_resume = true;
}

void adc_l1_model::sequence_written(std::uint32_t, std::uint32_t)
{
  if(regular_busy())
  {
    abort_regular();
    update(regs_, start_, 0);
    return;
  }
  state_.rank = 0;
}

void adc_l1_model::injected_sequence_written(std::uint32_t, std::uint32_t)
{
  if(not state_.injecting)
  {
    state_.injected_rank = 0;
    return;
  }
  stop_engine();
  state_.injecting     = false;
  state_.injected_rank = 0;
  update(regs_, injected_start_, 0);
  if(std::exchange(state_.regular_resume, false))
  {
    begin(std::max(ctx_.clock.time().ps, state_.delay_until_ps));
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

void adc_l1_model::check_common()
{
  if(get(regs_, prescaler_) == 3)
  {
    forbidden(prescaler_logged_, "CCR", "ADCPRE 11: a reserved prescaler, divided by 4");
  }
  if(get(regs_, power_down_idle_) != 0 or get(regs_, power_down_delay_) != 0)
  {
    unmodelled(power_down_logged_, "CR1", "PDI and PDD: the automatic power-down");
  }
  if(get(regs_, discontinuous_) != 0 and get(regs_, injected_discontinuous_) != 0)
  {
    forbidden(
        both_discontinuous_logged_, "CR1",
        "DISCEN with JDISCEN, against RM0038 12.3.10: each group follows its own bit");
  }
}

void adc_l1_model::check_group()
{
  check_common();
  if(get(regs_, discontinuous_) != 0 and get(regs_, continuous_) != 0)
  {
    forbidden(
        discontinuous_continuous_logged_, "CR1",
        "DISCEN with CONT: converted as DISCEN alone, inferred");
  }
  auto const ranks = variant_.banks ? max_ranks : max_ranks - 1;
  if(get(regs_, scan_) != 0 and get(regs_, length_) + 1u > ranks)
  {
    forbidden(
        length_logged_, "SQR1",
        std::format(
            "L {:#07b}: {} conversions, above the {} ranks of the category: {} converted",
            get(regs_, length_), get(regs_, length_) + 1u, ranks, ranks));
  }
  for(std::size_t rank = 0; rank < length(); ++rank)
  {
    check_channel(channel_at(rank), "SQR1");
  }
  if(get(regs_, auto_injected_) == 0)
  {
    return;
  }
  if(get(regs_, discontinuous_) != 0 or get(regs_, injected_discontinuous_) != 0)
  {
    forbidden(
        auto_discontinuous_logged_, "CR1",
        "JAUTO with DISCEN or JDISCEN, against RM0038 12.3.9: the whole injected group follows "
        "the last regular rank");
  }
  if(get(regs_, injected_trigger_) != 0)
  {
    forbidden(
        auto_trigger_logged_, "CR2",
        "JEXTEN with JAUTO, against RM0038 12.3.9: the injected triggers are ignored");
  }
  if(get(regs_, delay_) != 0)
  {
    unmodelled(auto_delay_logged_, "CR2", "DELS with JAUTO: the delay of auto-injected sequences");
  }
  check_injected();
}

void adc_l1_model::check_injected()
{
  check_common();
  for(std::size_t rank = 0; rank < injected_length(); ++rank)
  {
    check_channel(injected_channel(rank), "JSQR");
  }
}

void adc_l1_model::check_channel(std::uint32_t channel, std::string_view name)
{
  if(channel == sensor_channel or channel == vrefint_channel)
  {
    if(get(regs_, sensors_) != 0)
    {
      static_cast<void>(calibration(
          channel == sensor_channel ? variant_.ts_cal1 : variant_.vrefint_cal,
          channel == sensor_channel ? "TS_CAL1" : "VREFINT_CAL"));
    }
    return;
  }
  if(readable(channel))
  {
    return;
  }
  auto const what = channel == vcomp_channel
                      ? std::string{"channel 26, VCOMP,"}
                      : std::format("channel {}, absent from this category,", channel);
  unmodelled(channel_logged_[channel], name, std::format("{} read as 0", what));
}

void adc_l1_model::begin(std::uint64_t start_ps)
{
  if(start_ps > ctx_.clock.time().ps)
  {
    state_.start_pending = true;
    state_.start_ps      = start_ps;
    ++state_.generation;
    state_.queued =
        ctx_.events.schedule_at(vtime{start_ps}, start_due, state_.generation, ctx_.clock) + 1;
    return;
  }
  start_now(start_ps);
}

void adc_l1_model::start_now(std::uint64_t start_ps)
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
  state_.resolution    = static_cast<std::uint8_t>(get(regs_, resolution_));
  state_.sample_end_ps = start_ps + cycles_ps(sampling(current_channel()), state_.clock_hz);
  state_.end_ps        = start_ps + cycles_ps(conversion_cycles(), state_.clock_hz);
  state_.held          = false;
  state_.waiting_clock = false;
  state_.converting    = true;
  ++state_.generation;
  update(regs_, state_.injecting ? injected_start_ : start_, 0);
  update(regs_, state_.injecting ? injected_started_ : started_, 1);
  state_.queued =
      ctx_.events.schedule_at(vtime{state_.end_ps}, conversion_end, state_.generation, ctx_.clock)
      + 1;
}

void adc_l1_model::hold(std::uint64_t end_ps, bool& frozen, std::uint64_t& until_ps)
{
  auto const delay = get(regs_, delay_);
  auto const pclk  = ctx_.clock.hz(pclk2_);
  if(delay == 1)
  {
    frozen = true;
  }
  else if(delay >= 2 and pclk != 0)
  {
    until_ps = end_ps
             + cycles_ps(delay_pclk[delay] + sync_pclk, pclk)
             + cycles_ps(sync_adc_cycles, state_.clock_hz);
  }
}

void adc_l1_model::finish()
{
  if(state_.injecting)
  {
    finish_injected();
    return;
  }
  auto const end     = state_.end_ps;
  auto const result  = state_.held ? state_.held_code : code(current_channel());
  state_.converting  = false;
  auto const crossed = guard(current_channel(), result, watchdog_);
  if(not land(result) and dma_active())
  {
    abort_regular();
    signal();
    return;
  }
  auto const cut  = crossed and scan_aborted();
  auto const last = cut or std::size_t{state_.rank} + 1u >= length();
  auto const each = get(regs_, end_selection_) != 0;
  if(each or last)
  {
    update(regs_, end_of_conversion_, 1);
  }
  state_.rank     = last ? std::uint8_t{0} : static_cast<std::uint8_t>(state_.rank + 1u);
  auto const more = cut                             ? false
                  : get(regs_, discontinuous_) != 0 ? not last and --state_.subgroup_left > 0
                                                    : not last or get(regs_, continuous_) != 0;
  if(get(regs_, delay_) == 1)
  {
    if(each or last)
    {
      state_.frozen = true;
    }
  }
  else if(not each or last)
  {
    hold(end, state_.frozen, state_.delay_until_ps);
  }
  if(last and not cut and get(regs_, auto_injected_) != 0)
  {
    if(more and state_.frozen)
    {
      state_.resume_frozen = true;
    }
    else
    {
      state_.regular_resume = more;
    }
    start_injected(end);
  }
  else if(more)
  {
    if(state_.frozen)
    {
      state_.resume_frozen = true;
    }
    else
    {
      begin(std::max(end, state_.delay_until_ps));
    }
  }
  signal();
}

void adc_l1_model::finish_injected()
{
  auto const end    = state_.end_ps;
  auto const result = state_.held ? state_.held_code : code(current_channel());
  state_.converting = false;
  auto const index  = std::size_t{state_.injected_rank};
  auto const cut    = guard(current_channel(), result, injected_watchdog_) and scan_aborted();
  auto const last   = cut or index + 1u >= injected_length();
  store_injected(index, result);
  state_.injected_rank = last ? std::uint8_t{0} : static_cast<std::uint8_t>(index + 1u);
  auto const whole = get(regs_, injected_discontinuous_) == 0 or get(regs_, auto_injected_) != 0;
  auto const more  = cut ? false : whole ? not last : not last and --state_.injected_left > 0;
  if(last)
  {
    update(regs_, injected_end_, 1);
    hold(end, state_.injected_frozen, state_.injected_delay_until_ps);
  }
  if(more)
  {
    begin(end);
    signal();
    return;
  }
  state_.injecting = false;
  if(std::exchange(state_.regular_resume, false))
  {
    begin(std::max(end, state_.delay_until_ps));
  }
  signal();
}

auto adc_l1_model::land(std::uint32_t result) -> bool
{
  auto const tracked = get(regs_, end_selection_) != 0 or dma_active();
  if(tracked and state_.unread)
  {
    update(regs_, overrun_, 1);
    return false;
  }
  if(get(regs_, align_) != 0)
  {
    auto const resolution = std::uint32_t{state_.resolution};
    result <<= resolution == 3 ? 2u : 4u + 2u * resolution;
  }
  update(regs_, data_, result);
  state_.unread = tracked;
  return true;
}

auto adc_l1_model::dma_active() const -> bool
{
  return get(regs_, dma_) != 0 and not state_.dma_done;
}

auto adc_l1_model::guard(std::uint32_t channel, std::uint32_t result, field_ref const& enable)
    -> bool
{
  if(get(regs_, enable) == 0
     or (get(regs_, watchdog_single_) != 0 and get(regs_, watchdog_channel_) != channel))
  {
    return false;
  }
  auto const value = result << (2u * state_.resolution);
  if(value > get(regs_, high_) or value < get(regs_, low_))
  {
    update(regs_, watchdog_flag_, 1);
    return true;
  }
  return false;
}

auto adc_l1_model::scan_aborted() const -> bool
{
  return get(regs_, scan_) != 0 and get(regs_, watchdog_interrupt_) != 0;
}

void adc_l1_model::transfer_completed(dma_controller const&, std::uint32_t)
{}

void adc_l1_model::count_ended(dma_controller const& by, std::uint32_t request)
{
  if(not request_.carries(by, request) or get(regs_, dma_) == 0 or get(regs_, dma_continue_) != 0)
  {
    return;
  }
  state_.dma_done = true;
  signal();
}

void adc_l1_model::store_injected(std::size_t index, std::uint32_t result)
{
  auto const resolution = std::uint32_t{state_.resolution};
  auto const offset     = get(regs_, offsets_[index]) >> (2u * resolution);
  auto       value      = static_cast<std::int32_t>(result) - static_cast<std::int32_t>(offset);
  if(get(regs_, align_) != 0)
  {
    value *= std::int32_t{1} << ((resolution == 3 ? 2u : 4u + 2u * resolution) - 1u);
  }
  update(regs_, injected_data_[index], static_cast<std::uint32_t>(value) & 0xFFFFu);
}

void adc_l1_model::run_due(std::uint64_t ps)
{
  for(;;)
  {
    auto const ends   = state_.converting and state_.end_ps <= ps;
    auto const starts = state_.start_pending and state_.start_ps <= ps;
    if(not ends and not starts)
    {
      return;
    }
    if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
    {
      ctx_.events.cancel(seq - 1);
    }
    if(ends)
    {
      finish();
    }
    else
    {
      state_.start_pending = false;
      start_now(state_.start_ps);
    }
  }
}

void adc_l1_model::stop_engine()
{
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
  ++state_.generation;
  state_.converting    = false;
  state_.waiting_clock = false;
  state_.start_pending = false;
}

void adc_l1_model::abort_regular()
{
  if(not state_.injecting)
  {
    stop_engine();
  }
  state_.regular_resume = false;
  state_.frozen         = false;
  state_.resume_frozen  = false;
  state_.delay_until_ps = 0;
  state_.rank           = 0;
}

void adc_l1_model::abort()
{
  stop_engine();
  state_.injecting               = false;
  state_.regular_resume          = false;
  state_.frozen                  = false;
  state_.resume_frozen           = false;
  state_.delay_until_ps          = 0;
  state_.rank                    = 0;
  state_.injected_frozen         = false;
  state_.injected_delay_until_ps = 0;
  state_.injected_rank           = 0;
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
  request_.set(dma_active() and state_.unread);
}

void adc_l1_model::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == ready_end)
  {
    if(payload == state_.ready_generation and get(regs_, power_) != 0)
    {
      auto const was  = listened();
      auto const will = selected(regs_.peek(trigger_.offset), true);
      if(will != was)
      {
        notify(will);
      }
      update(regs_, ready_, 1);
      check_selection(was, listened());
      retune(was);
    }
    return;
  }
  if(kind == edge_due)
  {
    fire_edge(static_cast<std::uint32_t>(payload));
    return;
  }
  if(kind == start_due and payload == state_.generation and state_.start_pending)
  {
    state_.queued        = 0;
    state_.start_pending = false;
    start_now(state_.start_ps);
    return;
  }
  if(kind == conversion_end and payload == state_.generation and state_.converting)
  {
    state_.queued = 0;
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
  warn_once_or_refuse(logged, name, ctx_.strict_peripherals ? reg(name).offset : 0u, [&] {
    return std::format("{} not modelled", what);
  });
}

void adc_l1_model::forbidden(bool& logged, std::string_view name, std::string message)
{
  warn_once_or_refuse(
      logged, name, ctx_.strict_peripherals ? reg(name).offset : 0u, std::move(message));
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
