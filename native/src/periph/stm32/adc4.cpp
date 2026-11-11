#include "adc4_rows.hpp"
#include "adc_pads.hpp"
#include "dma_signal.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t regulator_end      = 0;
constexpr std::uint32_t ready_end          = 1;
constexpr std::uint32_t calibration_end    = 2;
constexpr std::uint32_t disable_end        = 3;
constexpr std::uint32_t start_due          = 4;
constexpr std::uint32_t sampling_end       = 5;
constexpr std::uint32_t conversion_end     = 6;
constexpr std::uint32_t edge_due           = 7;
constexpr std::uint32_t channels           = 24;
constexpr std::uint32_t ranks              = 8;
constexpr std::uint32_t sequence_end       = 0xF;
constexpr std::uint32_t calibration_factor = 0;
constexpr std::uint16_t reserved_divider   = 256;
constexpr std::uint32_t watchdogs          = 3;
constexpr std::uint32_t threshold_mask     = 0xFFF;
constexpr std::uint32_t high_shift         = 16;

using wide = unsigned __int128;

[[nodiscard]] constexpr auto cycles_ps(std::uint64_t cycles, std::uint64_t hz) -> std::uint64_t
{
  return static_cast<std::uint64_t>((wide{cycles} * ps_per_second + hz - 1u) / hz);
}

struct adc4_edge
{
  bool          pending{};
  bool          falling{};
  bool          reposted{};
  std::uint8_t  selection{};
  std::uint32_t generation{};
  std::uint64_t ps{};

  friend auto operator==(adc4_edge const&, adc4_edge const&) -> bool = default;
};

struct adc4_state
{
  std::uint32_t            regulator_generation{};
  std::uint32_t            ready_generation{};
  std::uint32_t            calibration_generation{};
  std::uint32_t            disable_generation{};
  bool                     regulator_ready{};
  bool                     ready_waiting{};
  bool                     calibration_waiting{};
  bool                     disable_waiting{};
  std::uint64_t            enable_lock_ps{};
  std::uint32_t            generation{};
  bool                     start_pending{};
  bool                     converting{};
  bool                     waiting_clock{};
  std::uint8_t             rank{};
  std::uint16_t            held_code{};
  std::uint64_t            start_ps{};
  std::uint64_t            end_ps{};
  std::uint64_t            regulator_ps{};
  std::uint64_t            calibration_ps{};
  std::uint8_t             selected{};
  std::uint8_t             masked{};
  bool                     dma_stopped{};
  std::uint32_t            edge_generation{};
  std::array<adc4_edge, 2> edges{};

  friend auto operator==(adc4_state const&, adc4_state const&) -> bool = default;
};

struct adc4_pad
{
  std::uint8_t channel{};
  char         port{};
  std::uint8_t pin{};
  gpio_port*   gpio{};
};

using threshold_regs = std::array<reg_ref, watchdogs>;
using mask_regs      = std::array<reg_ref, 2>;
using flag_fields    = std::array<field_ref, watchdogs>;

struct kernel_select
{
  address       at;
  std::uint32_t mask;
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
 *        Sequence, RM0456 34.4.9: with CHSELRMOD 0 the channels whose CHSELx bit is set, from
 *        channel 0 up with SCANDIR 0 and from channel 23 down with SCANDIR 1. With CHSELRMOD 1 SQ1
 *        to SQ8 of the alternate ADC_CHSELR in order, up to the first SQx of 1111. An ADSTART write
 *        with EXTEN 00 starts the sequence: sampling starts tLATR after the write, inferred. tLATR
 *        is 4 ADC clock cycles at fADC = HCLK and HCLK / 2, and 3.75 at HCLK / 4, DS13737 Table
 *        106, for the bus clock as kernel clock, ADCDACSEL 0. Any other kernel clock takes 4
 *        cycles, inferred: RM0456 Table 330 calls its latency not deterministic. ADSTART written
 *        before ADRDY samples too, inferred. A conversion samples for SMP1 or SMP2 of ADC_SMPR, as
 *        its SMPSELx bit selects, 1.5 to 814.5 cycles, then converts for tSAR, 12.5, 10.5, 8.5 or
 *        6.5 cycles for RES 12, 10, 8 or 6 bits, Table 332. EOSMP sets at the end of sampling, EOC
 *        with the result in ADC_DR at the end of the conversion, the DR write latency taken as 0,
 *        inferred, and EOS with the last result of the sequence. The next conversion samples from
 *        the end of the previous one. CONT 1 starts the sequence over after EOS. DISCEN 1 converts
 *        one channel per ADSTART, the sequence going on at the next one. With EXTEN 00 ADSTART
 *        clears at EOS with CONT 0, and at each EOC with DISCEN 1, 34.4.13. A conversion keeps the
 *        ADC clock it started with. Without an ADC clock a conversion waits for one, then samples
 *        when the clock arrives, inferred. ADSTP drops a conversion in progress, its result lost,
 *        and the sequence starts over at the next ADSTART, 34.4.15. ADSTP clears ADSTP and ADSTART
 *        in the write, no figure gives the time, inferred. A rank past the end of a shortened
 *        sequence starts over at the first channel.
 *
 *        Results, RM0456 34.4.23: an external channel converts the analog level of its pad, the
 *        GPIO pin its description's `channel_pins` names, gpio_port::analog_level(), as V x 4096 /
 *        VDDA, floored and saturated, then shifted right two bits per RES step. The level is held
 *        at the end of sampling. ADC_DR holds the result right-aligned, or with ALIGN 1 on bit 15,
 *        a 6-bit result on bit 7, Figure 291. A CPU or DMA read of ADC_DR clears EOC. A result
 *        finding EOC set sets OVR, and replaces ADC_DR with OVRMOD 1. With OVRMOD 0 every result is
 *        lost while EOC or OVR is set: ADC_DR and EOC keep their values and OVR is set. A write
 *        changing ADC_CFGR1, ADC_SMPR, ADC_CHSELR, ADC_AWD2CR or ADC_AWD3CR with ADSTART set keeps
 *        the old register, RM0456 34.7.4 to 34.7.15, logged once for the five, a model_error under
 *        strict_peripherals. ADC_IER, ADC_CFGR2, ADC_AWDxTR and ADC_CCR accept writes while ADSTART
 *        is set. RES is accepted with ADEN 1 and ADSTART 0, where RM0456 34.4.18 asks for ADEN 0.
 *        `analog_port` drive and input act on the pad's board slot. Without a pad, drive and input
 *        throw config_error naming `channel_pins.<c>`, and the channel converts 0. A description
 *        with `channel_pins` holds a pad for every external channel, or construction throws
 *        config_error.
 *
 *        An ADSTART with no channel in the sequence is ignored, logged once, a model_error under
 *        strict_peripherals, inferred. DISCEN with CONT, forbidden by 34.4.17, converts as DISCEN
 *        alone, logged once, a model_error under strict_peripherals.
 *
 *        Hardware triggers, RM0456 34.4.16: with EXTEN not 00 an ADSTART write starts nothing,
 *        and ADC4 listens, as a trigger_sink, to the signal EXTSEL selects in RM0456 Table 329
 *        while ADEN and ADSTART are set: `tim1_trgo2`, `tim1_cc4`, `tim2_trgo`, `tim15_trgo`,
 *        `tim6_trgo`, `lptim1_ch1`, `lptim3_ch2` and `exti15` for 0 to 7. EXTEN 01 detects the
 *        rising edges, 10 the falling ones and 11 both, a timer's TRGO pulse rising at its event
 *        and falling one timer clock later. An edge while a conversion or the tLATR runs is
 *        ignored. Otherwise it starts the sequence, or the next DISCEN conversion, tLATR later,
 *        counted from the edge, inferred for the clock cases of Table 330. ADSTART stays set at
 *        the end of a sequence, 34.4.13 Note, and clears with ADSTP, which ends the listening.
 *        The `tim1_oc4` selection takes the timer's capture/compare event pulse `tim1_cc4`,
 *        inferred. An edge its source gives ahead of the machine time is taken at its own time
 *        if EXTEN and the selection still hold, after the ADC's own events due then, inferred,
 *        and a reset drops it. Two such edges are held. Any other edge is decided against the state
 *        at its arrival, so a source gives it before the conversion end it precedes, as the
 *        timers do from their own events.
 *        A selection no model produces, `tim1_trgo2`, `lptim1_ch1`, `lptim3_ch2` and `exti15`
 *        today, is logged once per selection at the ADSTART, a model_error under
 *        strict_peripherals that keeps ADC_CR, and ADSTART waits for it.
 *
 *        DMA, RM0456 34.4.23: DMAEN 1 holds the `adc4_dma` request high while EOC is set and OVR
 *        clear. A DMA read of ADC_DR clears EOC and drops it, and an overrun blocks it until
 *        software clears OVR. DMACFG 0, one-shot: when the GPDMA channel serving the request
 *        completes, the conversion in progress is dropped, the sequence resets, ADC_DR is frozen
 *        and no request rises until ADSTART rises again, ADSTART staying set, inferred. DMACFG 1,
 *        circular: the requests go on. DMAEN written with EOC set raises the request, inferred.
 *        A result arriving after the one-shot end leaves ADC_DR and EOC as they are, and the last
 *        result of a sequence sets EOS, inferred. The calibration order of the DMAEN note is not
 *        checked.
 *
 *        Analog watchdogs, RM0456 34.4.25: at the end of each conversion the 12-bit raw result,
 *        left aligned, so the result of a RES 10, 8 or 6 bit conversion shifted left 2, 4 or 6
 *        bits, is compared with ADC_AWDxTR, and AWDx sets when it is below LTx or above HTx, an LTx
 *        above HTx flagging every result. AWD1 guards every channel of the sequence with AWD1EN and
 *        AWD1SGL 0, or the channel AWD1CH names with AWD1SGL 1. AWD2 and AWD3 guard the channels
 *        set in ADC_AWD2CR and ADC_AWD3CR. A result an overrun discards is compared too. AWDx sets
 *        with EOC, no figure gives a delay, inferred. A write to ADC_AWDxTR while a conversion
 *        samples or converts masks that watchdog for that conversion, the conversion starting with
 *        its sampling, inferred, and the new thresholds apply from the next one. An AWDx flag with
 *        its AWDxIE of ADC_IER raises ADC4_IRQn. ADC_AWDx_OUT, the signal to the timers' ETR
 *        inputs, is not produced. ADC_AWD2CR and ADC_AWD3CR follow the lock of ADC_CFGR1. An AWD1CH
 *        of 18 to 21 or 24 to 31 with AWD1EN and AWD1SGL, reserved in RM0456 34.7.4, guards no
 *        channel and is logged once at ADSTART. A result arriving while ADC_DR is frozen after a
 *        one-shot DMA end is compared, and a threshold write at the exact end instant of a
 *        conversion masks the next one, both inferred.
 *
 *        Not modelled, each logged once at an ADSTART that uses it, a model_error under
 *        strict_peripherals that keeps the old ADC_CR. The wait mode WAIT, the auto-off mode
 *        AUTOFF, the oversampler OVSE and the low-frequency trigger mode LFTRIG: the conversions
 *        run without them. VREFPROT and VREFSECSMP of ADC_PWRR: tCONV stays at its DS minimum. The
 *        internal channels VIN[0], VIN[12], VIN[13], VIN[14] and VIN[21]: they convert 0.
 */
class adc4_model final : public dispatch_model<adc4_model, adc4_state>,
                         public analog_port,
                         public clock_listener,
                         public bus_reset_listener,
                         public trigger_sink,
                         public dma_requester
{
  using base = dispatch_model<adc4_model, adc4_state>;

public:
  adc4_model(
      model_context&         ctx,
      peripheral_desc const& desc,
      supply_options         supply,
      adc4_variant           variant);

  void               reset(register_file& regs, reset_kind kind) override;
  void               load(std::span<std::byte const> in) override;
  void               on_bus_reset() override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               on_clock_change() override;
  void               transfer_completed(dma_controller const& by, std::uint32_t request) override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

  void               drive(analog_channel channel, std::uint32_t microvolts) override;
  [[nodiscard]] auto input(analog_channel channel) const -> std::uint32_t override;
  [[nodiscard]] auto external_channels(analog_bank bank) const -> std::uint32_t override
  {
    return bank == analog_bank::a ? variant_.external : 0;
  }
  [[nodiscard]] auto bonded_channels(analog_bank bank) const -> std::uint32_t override
  {
    return bank == analog_bank::a ? bonded_channels_ : 0;
  }
  [[nodiscard]] auto vdda_mv() const -> std::uint32_t override
  {
    return supply_.vdda_mv;
  }

  void isr_written(std::uint32_t old, std::uint32_t now);
  void cr_written(std::uint32_t old, std::uint32_t now);
  void calfact_written(std::uint32_t old, std::uint32_t now);
  void pwrr_written(std::uint32_t old, std::uint32_t now);
  void ccr_written(std::uint32_t old, std::uint32_t now);
  void ier_written(std::uint32_t, std::uint32_t)
  {
    signal();
  }
  void cfgr1_written(std::uint32_t old, std::uint32_t now)
  {
    config_written("CFGR1", old, now);
    signal();
  }
  void smpr_written(std::uint32_t old, std::uint32_t now)
  {
    config_written("SMPR", old, now);
  }
  void chselr_written(std::uint32_t old, std::uint32_t now)
  {
    config_written("CHSELRMOD0", old, now);
  }
  void awd1tr_written(std::uint32_t, std::uint32_t)
  {
    mask_watchdog(0);
  }
  void awd2tr_written(std::uint32_t, std::uint32_t)
  {
    mask_watchdog(1);
  }
  void awd3tr_written(std::uint32_t, std::uint32_t)
  {
    mask_watchdog(2);
  }
  void awd2cr_written(std::uint32_t old, std::uint32_t now)
  {
    config_written("AWD2CR", old, now);
  }
  void awd3cr_written(std::uint32_t old, std::uint32_t now)
  {
    config_written("AWD3CR", old, now);
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
  void               config_written(std::string_view name, std::uint32_t old, std::uint32_t now);
  void               forbidden(bool& logged, std::string_view name, std::string_view message);
  [[nodiscard]] auto length() const -> std::uint32_t;
  [[nodiscard]] auto channel_at(std::uint32_t rank) const -> std::uint32_t;
  [[nodiscard]] auto unmodelled_at_start() -> std::vector<refusal>;
  [[nodiscard]] auto start_latency_ps(std::uint64_t hz) const -> std::uint64_t;
  [[nodiscard]] auto kernel_is_bus_clock() const -> bool;
  void               start_sequence(std::uint64_t from_ps);
  void               retune();
  void               fire_edge(adc4_edge& slot);
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto frozen() const -> bool;
  [[nodiscard]] auto dma_level() const -> bool;
  void               begin(std::uint64_t start_ps);
  void               end_sampling();
  void               end_conversion();
  void               abort();
  void               hold();
  void               mask_watchdog(std::uint32_t index);
  void               compare(std::uint32_t channel, std::uint32_t value);
  [[nodiscard]] auto code(std::uint32_t channel) const -> std::uint32_t;
  [[nodiscard]] auto external(analog_channel channel) const -> std::uint32_t;
  [[nodiscard]] auto pad_port(char letter) const -> gpio_port*;
  [[nodiscard]] auto driven_pad(analog_channel channel) const
      -> std::pair<gpio_port&, std::uint8_t>;
  void resolve_pads();

  register_file&               regs_;
  supply_options               supply_;
  adc4_variant                 variant_;
  std::uint32_t                bonded_channels_{};
  std::vector<adc4_pad>        pads_;
  std::optional<kernel_select> kernel_select_;
  core::clock_domain           kernel_;
  irq_line&                    line_;
  reg_ref                      isr_;
  reg_ref                      ier_;
  reg_ref                      control_;
  reg_ref                      calfact_reg_;
  reg_ref                      pwrr_;
  reg_ref                      ccr_;
  field_ref                    regulator_ready_;
  field_ref                    calibration_done_;
  field_ref                    ready_;
  field_ref                    calibrate_;
  field_ref                    regulator_;
  field_ref                    stop_;
  field_ref                    start_;
  field_ref                    disable_;
  field_ref                    enable_;
  field_ref                    data_;
  field_ref                    auto_off_;
  field_ref                    factor_;
  field_ref                    prescaler_;
  field_ref                    sampled_;
  field_ref                    converted_;
  field_ref                    overrun_;
  field_ref                    overrun_mode_;
  field_ref                    sequenced_;
  reg_ref                      config_;
  reg_ref                      sampling_;
  reg_ref                      selection_;
  reg_ref                      config2_;
  threshold_regs               thresholds_;
  mask_regs                    guard_masks_;
  flag_fields                  flags_;
  field_ref                    watchdog1_single_;
  field_ref                    watchdog1_channel_;
  field_ref                    trigger_;
  field_ref                    continuous_;
  field_ref                    discontinuous_;
  field_ref                    ordered_;
  field_ref                    backward_;
  field_ref                    resolution_;
  field_ref                    align_;
  field_ref                    wait_;
  field_ref                    dma_;
  field_ref                    watchdog1_;
  field_ref                    smp1_;
  field_ref                    smp2_;
  field_ref                    oversampler_;
  field_ref                    low_frequency_;
  field_ref                    trigger_select_;
  field_ref                    dma_mode_;
  dma_signal                   request_;
  field_ref                    vref_protection_;
  field_ref                    vref_sampling_;
  core::clock_domain           hclk_;
  std::uint32_t                clearable_{};
  std::uint32_t                set_only_{};
  bool                         regulator_refused_logged_{};
  bool                         calibration_refused_logged_{};
  bool                         enable_lost_logged_{};
  bool                         start_refused_logged_{};
  bool                         disable_refused_logged_{};
  bool                         stop_refused_logged_{};
  bool                         calfact_refused_logged_{};
  bool                         pwrr_refused_logged_{};
  bool                         prescaler_refused_logged_{};
  bool                         reserved_logged_{};
  bool                         no_clock_logged_{};
  bool                         config_refused_logged_{};
  bool                         empty_logged_{};
  bool                         discontinuous_continuous_logged_{};
  std::array<bool, 8>          trigger_logged_{};
  std::string                  trigger_message_;
  bool                         wait_logged_{};
  bool                         auto_off_logged_{};
  bool                         oversampler_logged_{};
  bool                         low_frequency_logged_{};
  bool                         watchdog_logged_{};
  bool                         vref_logged_{};
  bool                         internal_logged_{};
};

constexpr std::array<write_entry<adc4_model>, 15> writes{
    {
     {"ISR", &adc4_model::isr_written},
     {"IER", &adc4_model::ier_written},
     {"CR", &adc4_model::cr_written},
     {"CFGR1", &adc4_model::cfgr1_written},
     {"SMPR", &adc4_model::smpr_written},
     {"CHSELRMOD0", &adc4_model::chselr_written},
     {"DR", nullptr},
     {"PWR", &adc4_model::pwrr_written},
     {"CALFACT", &adc4_model::calfact_written},
     {"CCR", &adc4_model::ccr_written},
     {"AWD1TR", &adc4_model::awd1tr_written},
     {"AWD2TR", &adc4_model::awd2tr_written},
     {"AWD3TR", &adc4_model::awd3tr_written},
     {"AWD2CR", &adc4_model::awd2cr_written},
     {"AWD3CR", &adc4_model::awd3cr_written},
     }
};

adc4_model::adc4_model(
    model_context&         ctx,
    peripheral_desc const& desc,
    supply_options         supply,
    adc4_variant           variant)
    : base{ctx, desc, "ADC4", writes}, regs_{ctx.regs}, supply_{supply}, variant_{variant},
      kernel_{ctx.clock.domain(lower(desc.name))}, line_{irq(desc.name + "_IRQn")},
      isr_{reg("ISR")}, ier_{reg("IER")}, control_{reg("CR")}, calfact_reg_{reg("CALFACT")},
      pwrr_{reg("PWR")}, ccr_{reg("CCR")}, regulator_ready_{field("ISR", "LDORDY")},
      calibration_done_{field("ISR", "EOCAL")}, ready_{field("ISR", "ADRDY")},
      calibrate_{field("CR", "ADCAL")}, regulator_{field("CR", "ADVREGEN")},
      stop_{field("CR", "ADSTP")}, start_{field("CR", "ADSTART")}, disable_{field("CR", "ADDIS")},
      enable_{field("CR", "ADEN")}, data_{field("DR", "DATA")}, auto_off_{field("PWR", "AUTOFF")},
      factor_{field("CALFACT", "CALFACT")}, prescaler_{field("CCR", "PRESC")},
      sampled_{field("ISR", "EOSMP")}, converted_{field("ISR", "EOC")},
      overrun_{field("ISR", "OVR")}, overrun_mode_{field("CFGR1", "OVRMOD")},
      sequenced_{field("ISR", "EOS")}, config_{reg("CFGR1")}, sampling_{reg("SMPR")},
      selection_{reg("CHSELRMOD0")}, config2_{reg("CFGR2")},
      thresholds_{reg("AWD1TR"), reg("AWD2TR"), reg("AWD3TR")},
      guard_masks_{reg("AWD2CR"), reg("AWD3CR")},
      flags_{field("ISR", "AWD1"), field("ISR", "AWD2"), field("ISR", "AWD3")},
      watchdog1_single_{field("CFGR1", "AWD1SGL")}, watchdog1_channel_{field("CFGR1", "AWD1CH")},
      trigger_{field("CFGR1", "EXTEN")}, continuous_{field("CFGR1", "CONT")},
      discontinuous_{field("CFGR1", "DISCEN")}, ordered_{field("CFGR1", "CHSELRMOD")},
      backward_{field("CFGR1", "SCANDIR")}, resolution_{field("CFGR1", "RES")},
      align_{field("CFGR1", "ALIGN")}, wait_{field("CFGR1", "WAIT")}, dma_{field("CFGR1", "DMAEN")},
      watchdog1_{field("CFGR1", "AWD1EN")}, smp1_{field("SMPR", "SMP1")},
      smp2_{field("SMPR", "SMP2")}, oversampler_{field("CFGR2", "OVSE")},
      low_frequency_{field("CFGR2", "LFTRIG")}, trigger_select_{field("CFGR1", "EXTSEL")},
      dma_mode_{field("CFGR1", "DMACFG")}, request_{ctx.peers, lower(desc.name) + "_dma"},
      vref_protection_{field("PWR", "VREFPROT")}, vref_sampling_{field("PWR", "VREFSECSMP")},
      hclk_{ctx.clock.domain("hclk")}
{
  for(auto const& name :
      {"LDORDY", "EOCAL", "AWD3", "AWD2", "AWD1", "OVR", "EOS", "EOC", "EOSMP", "ADRDY"})
  {
    clearable_ |= field("ISR", name).mask();
  }
  set_only_ = calibrate_.mask() | stop_.mask() | start_.mask() | disable_.mask() | enable_.mask();
  bonded_channels_ = desc.bonded_channels.value_or(variant.external);
  if((bonded_channels_ & ~variant.external) != 0)
  {
    throw config_error(desc.name, 0, "bonded_channels", "a mask of external channels");
  }
  if(desc.bonded_channels_b.value_or(0) != 0)
  {
    throw config_error(desc.name, 0, "bonded_channels_b", "0 or absent, the ADC has one bank");
  }
  if(auto const rcc =
         std::ranges::find(ctx.part.peripherals, std::string_view{"RCC"}, &peripheral_desc::name);
     rcc != ctx.part.peripherals.end())
  {
    if(auto const* const found = find_register(*rcc, "RCC_CCIPR3"); found != nullptr)
    {
      if(auto const bits =
             std::ranges::find(found->fields, std::string_view{"ADCDACSEL"}, &field_desc::name);
         bits != found->fields.end())
      {
        kernel_select_ = kernel_select{
            rcc->base + found->offset,
            static_cast<std::uint32_t>(((std::uint64_t{1} << bits->width) - 1) << bits->lsb)};
      }
    }
  }
  require_channel_pins(desc, variant.external, pad_role::positive);
  for(auto const& pad : desc.channel_pins.value_or(std::vector<adc_pad_desc>{}))
  {
    if(pad.role == pad_role::positive and not pad.port.empty())
    {
      pads_.push_back({.channel = pad.channel, .port = pad.port.front(), .pin = pad.pin});
    }
  }
}

void adc4_model::reset(register_file&, reset_kind)
{
  restore_reset_values();
  resolve_pads();
}

void adc4_model::load(std::span<std::byte const> in)
{
  base::load(in);
  resolve_pads();
}

void adc4_model::resolve_pads()
{
  for(auto& pad : pads_)
  {
    pad.gpio = pad_port(pad.port);
  }
}

auto adc4_model::pad_port(char letter) const -> gpio_port*
{
  return dynamic_cast<gpio_port*>(ctx_.peers.find(std::format("GPIO{}", letter)));
}

auto adc4_model::external(analog_channel channel) const -> std::uint32_t
{
  if(channel.bank != analog_bank::a
     or channel.number >= channels
     or (variant_.external >> channel.number & 1u) == 0)
  {
    throw config_error(
        desc_.name, 0, "channel",
        std::format(
            "an external channel of mask {:#010x}, not {}{}", variant_.external, channel.number,
            channel.bank == analog_bank::b ? "b" : ""));
  }
  return channel.number;
}

auto adc4_model::driven_pad(analog_channel channel) const -> std::pair<gpio_port&, std::uint8_t>
{
  auto const  at   = external(channel);
  auto const  pad  = std::ranges::find(pads_, at, &adc4_pad::channel);
  auto* const port = pad == pads_.end()   ? nullptr
                   : pad->gpio != nullptr ? pad->gpio
                                          : pad_port(pad->port);
  if(port == nullptr)
  {
    throw config_error(
        desc_.name, 0, std::format("channel_pins.{}", at),
        std::format("a positive pad of channel {} on a GPIO port of the machine", at));
  }
  return {*port, pad->pin};
}

void adc4_model::drive(analog_channel channel, std::uint32_t microvolts)
{
  auto const [port, pin] = driven_pad(channel);
  resolve_pads();
  port.drive_analog(pin, microvolts);
}

auto adc4_model::input(analog_channel channel) const -> std::uint32_t
{
  auto const [port, pin] = driven_pad(channel);
  return port.analog_drive(pin).value_or(0);
}

auto adc4_model::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     ctx) -> std::expected<std::uint32_t, bus_error>
{
  auto const value = base::on_read(regs, offset, size, ctx);
  if((offset & ~3u) == data_.offset and ctx.by != initiator::debugger)
  {
    update(regs_, converted_, 0);
    signal();
  }
  return value;
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
  state_.edges = {};
  ++state_.edge_generation;
  state_.dma_stopped         = false;
  state_.masked              = 0;
  state_.regulator_ready     = false;
  state_.ready_waiting       = false;
  state_.calibration_waiting = false;
  state_.disable_waiting     = false;
  state_.enable_lock_ps      = 0;
  abort();
  request_.resolve();
  retune();
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
    warn("CR", "no ADC4 kernel clock: ADRDY, the calibration, ADDIS and conversions wait for it");
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
  if(rises(start_) and length() == 0)
  {
    now = start_.with(now, 0);
    if(ctx_.strict_peripherals)
    {
      regs_.poke(control_.offset, old);
    }
    forbidden(empty_logged_, "CR", "ADSTART with no channel in the sequence: the write is ignored");
  }
  if(rises(start_))
  {
    auto const unmodelled = unmodelled_at_start();
    if(not unmodelled.empty() and ctx_.strict_peripherals)
    {
      regs_.poke(control_.offset, old);
    }
    for(auto const& entry : unmodelled)
    {
      forbidden(*entry.logged, "CR", entry.message);
    }
  }
  auto const powered = rises(enable_) and regulator_.of(now) == 0;
  if(powered)
  {
    now = regulator_.with(now, 1);
  }
  if(stop_.of(now) != 0)
  {
    now = start_.with(stop_.with(now, 0), 0);
    abort();
  }
  regs_.poke(control_.offset, now);
  regulator_written(old, now);
  if(rises(start_))
  {
    state_.dma_stopped = false;
    if(get(regs_, trigger_) == 0)
    {
      start_sequence(ctx_.clock.time().ps);
    }
  }
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
  retune();
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

auto adc4_model::busy() const -> bool
{
  return state_.start_pending or state_.converting or state_.waiting_clock;
}

auto adc4_model::frozen() const -> bool
{
  return get(regs_, dma_) != 0 and get(regs_, dma_mode_) == 0 and state_.dma_stopped;
}

auto adc4_model::dma_level() const -> bool
{
  return get(regs_, dma_) != 0
     and get(regs_, converted_) != 0
     and get(regs_, overrun_) == 0
     and not frozen();
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
  request_.set(dma_level());
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
  if(kind == start_due)
  {
    if(payload == state_.generation and state_.start_pending)
    {
      state_.start_pending = false;
      begin(state_.start_ps);
    }
    return;
  }
  if(kind == sampling_end)
  {
    if(payload == state_.generation and state_.converting)
    {
      end_sampling();
    }
    return;
  }
  if(kind == conversion_end)
  {
    if(payload == state_.generation and state_.converting)
    {
      end_conversion();
    }
    return;
  }
  if(kind == edge_due)
  {
    auto const found = std::ranges::find_if(state_.edges, [payload](adc4_edge const& slot) {
      return slot.pending and slot.generation == payload;
    });
    if(found != state_.edges.end())
    {
      fire_edge(*found);
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
  if(state_.waiting_clock)
  {
    state_.waiting_clock = false;
    begin(ctx_.clock.time().ps);
  }
}

void adc4_model::config_written(std::string_view name, std::uint32_t old, std::uint32_t now)
{
  if(now == old or get(regs_, start_) == 0)
  {
    return;
  }
  regs_.poke(reg(name).offset, old);
  forbidden(
      config_refused_logged_, name,
      "ADC_CFGR1, ADC_SMPR, ADC_CHSELR, ADC_AWD2CR or ADC_AWD3CR written with ADSTART 1, RM0456 "
      "34.7.4 to 34.7.15: the write is ignored");
}

auto adc4_model::length() const -> std::uint32_t
{
  auto const selected = regs_.peek(selection_.offset);
  if(get(regs_, ordered_) == 0)
  {
    return static_cast<std::uint32_t>(std::popcount(selected & ((1u << channels) - 1u)));
  }
  for(std::uint32_t rank = 0; rank < ranks; ++rank)
  {
    if((selected >> (4 * rank) & 0xFu) == sequence_end)
    {
      return rank;
    }
  }
  return ranks;
}

auto adc4_model::channel_at(std::uint32_t rank) const -> std::uint32_t
{
  auto const selected = regs_.peek(selection_.offset);
  if(get(regs_, ordered_) != 0)
  {
    return selected >> (4 * rank) & 0xFu;
  }
  auto       left     = selected & ((1u << channels) - 1u);
  auto const backward = get(regs_, backward_) != 0;
  for(std::uint32_t skip = 0; skip < rank; ++skip)
  {
    left &= backward ? ~(1u << (31 - std::countl_zero(left))) : left - 1u;
  }
  return backward ? 31u - static_cast<std::uint32_t>(std::countl_zero(left))
                  : static_cast<std::uint32_t>(std::countr_zero(left));
}

auto adc4_model::unmodelled_at_start() -> std::vector<refusal>
{
  std::vector<refusal> out;
  auto const           add = [&](bool when, bool& logged, std::string_view message) {
    if(when)
    {
      out.push_back({&logged, message});
    }
  };
  add(get(regs_, discontinuous_) != 0 and get(regs_, continuous_) != 0,
      discontinuous_continuous_logged_,
      "DISCEN with CONT, forbidden by RM0456 34.4.17: converted as DISCEN alone");
  auto const selection = get(regs_, trigger_select_);
  trigger_message_     = std::format(
      "EXTSEL {}: the trigger {} is produced by no model, ADSTART waits for it", selection,
      variant_.trigger_signals[selection]);
  add(get(regs_, trigger_) != 0
          and trigger_source_of(ctx_.peers, variant_.trigger_signals[selection]) == nullptr,
      trigger_logged_[selection], trigger_message_);
  add(get(regs_, wait_) != 0, wait_logged_, "WAIT: the wait mode not modelled");
  add(get(regs_, auto_off_) != 0, auto_off_logged_, "AUTOFF: the auto-off mode not modelled");
  add(get(regs_, oversampler_) != 0, oversampler_logged_, "OVSE: the oversampler not modelled");
  add(get(regs_, low_frequency_) != 0, low_frequency_logged_,
      "LFTRIG: the low-frequency trigger mode not modelled");
  add(get(regs_, watchdog1_) != 0
          and get(regs_, watchdog1_single_) != 0
          and (variant_.awd1_channels >> get(regs_, watchdog1_channel_) & 1u) == 0,
      watchdog_logged_,
      "AWD1CH 18 to 21 and 24 to 31 are reserved, RM0456 34.7.4: AWD1 guards no channel");
  add(get(regs_, vref_protection_) != 0 or get(regs_, vref_sampling_) != 0, vref_logged_,
      "VREFPROT or VREFSECSMP: the extra conversion cycles not modelled, tCONV at its minimum");
  auto internal = false;
  for(std::uint32_t rank = 0; rank < length(); ++rank)
  {
    internal = internal or (variant_.internal >> channel_at(rank) & 1u) != 0;
  }
  add(internal, internal_logged_,
      "an internal channel in the sequence: VIN[0], VIN[12], VIN[13], VIN[14] and VIN[21] not "
      "modelled, converted as 0");
  return out;
}

void adc4_model::start_sequence(std::uint64_t from_ps)
{
  auto const hz = adc_hz();
  if(hz == 0)
  {
    static_cast<void>(no_clock());
    state_.waiting_clock = true;
    return;
  }
  state_.start_pending = true;
  state_.start_ps      = from_ps + start_latency_ps(hz);
  ctx_.events.schedule_at(vtime{state_.start_ps}, start_due, ++state_.generation, ctx_.clock);
}

auto adc4_model::kernel_is_bus_clock() const -> bool
{
  if(not kernel_select_)
  {
    return false;
  }
  auto const word = ctx_.services.read(kernel_select_->at, 4, initiator::debugger);
  return word and (*word & kernel_select_->mask) == 0;
}

auto adc4_model::start_latency_ps(std::uint64_t hz) const -> std::uint64_t
{
  auto const quarters = kernel_is_bus_clock() and ctx_.clock.hz(hclk_) == 4 * hz
                          ? variant_.start_latency_quarters_div4
                          : variant_.start_latency_quarters;
  return cycles_ps(quarters, 4 * hz);
}

void adc4_model::begin(std::uint64_t start_ps)
{
  auto const hz = adc_hz();
  if(hz == 0)
  {
    static_cast<void>(no_clock());
    state_.waiting_clock = true;
    return;
  }
  if(state_.rank >= length())
  {
    state_.rank = 0;
  }
  auto const channel  = channel_at(state_.rank);
  auto const second   = (regs_.peek(sampling_.offset) >> (8 + channel) & 1u) != 0;
  auto const sampling = variant_.sampling_halves[get(regs_, second ? smp2_ : smp1_)];
  auto const sar      = variant_.sar_halves[get(regs_, resolution_)];
  state_.converting   = true;
  state_.masked       = 0;

  ++state_.generation;
  ctx_.events.schedule_at(
      vtime{start_ps + cycles_ps(sampling, 2 * hz)}, sampling_end, state_.generation, ctx_.clock);
  state_.end_ps = start_ps + cycles_ps(std::uint64_t{sampling} + sar, 2 * hz);
  ctx_.events.schedule_at(vtime{state_.end_ps}, conversion_end, state_.generation, ctx_.clock);
}

void adc4_model::mask_watchdog(std::uint32_t index)
{
  if(state_.converting)
  {
    state_.masked |= static_cast<std::uint8_t>(1u << index);
  }
}

void adc4_model::compare(std::uint32_t channel, std::uint32_t value)
{
  auto const single = get(regs_, watchdog1_single_) != 0;
  auto const first =
      get(regs_, watchdog1_) != 0 and (not single or get(regs_, watchdog1_channel_) == channel);
  for(std::uint32_t index = 0; index < watchdogs; ++index)
  {
    auto const guarded =
        index == 0 ? first : (regs_.peek(guard_masks_[index - 1].offset) >> channel & 1u) != 0;
    if(not guarded or (state_.masked >> index & 1u) != 0)
    {
      continue;
    }
    auto const word = regs_.peek(thresholds_[index].offset);
    if(value < (word & threshold_mask) or value > (word >> high_shift & threshold_mask))
    {
      update(regs_, flags_[index], 1);
    }
  }
}

void adc4_model::hold()
{
  state_.held_code = static_cast<std::uint16_t>(code(channel_at(state_.rank)));
}

auto adc4_model::code(std::uint32_t channel) const -> std::uint32_t
{
  auto const pad = std::ranges::find(pads_, channel, &adc4_pad::channel);
  auto const uv =
      pad == pads_.end() or pad->gpio == nullptr or (variant_.external >> channel & 1u) == 0
          ? 0u
          : pad->gpio->analog_level(pad->pin);
  auto const full = std::min<std::uint64_t>(
      std::uint64_t{uv} * 4096u / (std::uint64_t{supply_.vdda_mv} * 1000u), 4095u);
  return static_cast<std::uint32_t>(full) >> (2u * get(regs_, resolution_));
}

void adc4_model::end_sampling()
{
  hold();
  update(regs_, sampled_, 1);
  signal();
}

void adc4_model::end_conversion()
{
  auto const res    = get(regs_, resolution_);
  auto const result = get(regs_, align_) == 0
                        ? std::uint32_t{state_.held_code}
                        : std::uint32_t{state_.held_code} << (res < 3 ? 4u + 2u * res : 2u);
  compare(channel_at(state_.rank), std::uint32_t{state_.held_code} << (2u * res));
  auto const keeps =
      get(regs_, overrun_mode_) == 0 and (get(regs_, converted_) != 0 or get(regs_, overrun_) != 0);
  if(not frozen())
  {
    if(keeps or get(regs_, converted_) != 0)
    {
      update(regs_, overrun_, 1);
    }
    if(not keeps)
    {
      update(regs_, data_, result);
      update(regs_, converted_, 1);
    }
  }
  state_.converting = false;
  auto const last   = state_.rank + 1u >= length();
  state_.rank       = last ? 0 : static_cast<std::uint8_t>(state_.rank + 1u);
  if(last)
  {
    update(regs_, sequenced_, 1);
  }
  auto const discontinuous = get(regs_, discontinuous_) != 0;
  if(discontinuous or (last and get(regs_, continuous_) == 0))
  {
    if(get(regs_, trigger_) == 0)
    {
      update(regs_, start_, 0);
    }
  }
  else
  {
    begin(state_.end_ps);
  }
  signal();
}

void adc4_model::abort()
{
  ++state_.generation;
  state_.start_pending = false;
  state_.converting    = false;
  state_.waiting_clock = false;
  state_.rank          = 0;
}

void adc4_model::retune()
{
  auto const target = static_cast<std::uint8_t>(
      get(regs_, enable_) != 0 and get(regs_, start_) != 0 and get(regs_, trigger_) != 0
          ? get(regs_, trigger_select_) + 1u
          : 0u);
  if(target == state_.selected)
  {
    return;
  }
  auto const was  = state_.selected;
  auto const note = [&](std::uint8_t at) {
    if(at != 0)
    {
      static_cast<void>(interest_changed(ctx_.peers, variant_.trigger_signals[at - 1u]));
    }
  };
  note(was);
  note(target);
  state_.selected = target;
  note(was);
  note(target);
}

auto adc4_model::listens(std::string_view signal) const -> bool
{
  return state_.selected != 0 and variant_.trigger_signals[state_.selected - 1u] == signal;
}

auto adc4_model::can_listen(std::string_view signal) const -> bool
{
  return std::ranges::contains(variant_.trigger_signals, signal);
}

void adc4_model::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  auto const polarity = get(regs_, trigger_);
  auto const detected = edge == trigger_edge::rising ? (polarity & 1u) != 0 : (polarity & 2u) != 0;
  if(not detected or not listens(signal))
  {
    return;
  }
  auto const free = std::ranges::find(state_.edges, false, &adc4_edge::pending);
  if(at.ps > ctx_.clock.time().ps and free != state_.edges.end())
  {
    *free = {
        .pending    = true,
        .falling    = edge == trigger_edge::falling,
        .reposted   = false,
        .selection  = state_.selected,
        .generation = ++state_.edge_generation,
        .ps         = at.ps};
    ctx_.events.schedule_at(vtime{at.ps}, edge_due, free->generation, ctx_.clock);
    return;
  }
  if(not busy())
  {
    start_sequence(at.ps);
  }
}

void adc4_model::fire_edge(adc4_edge& slot)
{
  auto const polarity = get(regs_, trigger_);
  auto const detected = (polarity & (slot.falling ? 2u : 1u)) != 0;
  if(not detected or state_.selected != slot.selection)
  {
    slot.pending = false;
    return;
  }
  if(busy() and not std::exchange(slot.reposted, true))
  {
    ctx_.events.schedule_at(vtime{slot.ps}, edge_due, slot.generation, ctx_.clock);
    return;
  }
  slot.pending = false;
  if(not busy())
  {
    start_sequence(slot.ps);
  }
}

void adc4_model::transfer_completed(dma_controller const& by, std::uint32_t request)
{
  if(get(regs_, dma_) == 0 or get(regs_, dma_mode_) != 0 or not request_.carries(by, request))
  {
    return;
  }
  state_.dma_stopped = true;
  abort();
  signal();
}

void adc4_model::forbidden(bool& logged, std::string_view name, std::string_view message)
{
  warn_once_or_refuse(
      logged, name, ctx_.strict_peripherals ? reg(name).offset : 0u, std::string{message});
}

constexpr auto adc4_row_table = std::to_array<builtin>({
    row_of<^^adc4_factory, ^^u5_adc4, ^^board::supply>(
        {.ip = "ADC", .ip_version_glob = "aditf4_v4*", .families = family::u5}),
});
}

auto adc4_factory(supply_options supply, adc4_variant variant) -> model_factory
{
  return
      [supply, variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
        return std::make_shared<adc4_model>(ctx, desc, supply, variant);
      };
}

auto adc4_rows() -> std::span<builtin const>
{
  return adc4_row_table;
}
}
