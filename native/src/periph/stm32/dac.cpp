#include "analog_peers.hpp"
#include "dac_rows.hpp"
#include "dma.hpp"
#include "dma_signal.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/bus_reset.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
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
using wide = unsigned __int128;

constexpr std::size_t   channels  = 2;
constexpr std::size_t   depth     = 3;
constexpr std::uint32_t data_mask = 0xFFF;
constexpr std::uint16_t lfsr_seed = 0xAAA;

enum class sample_phase : std::uint8_t
{
  off,
  sample,
  hold,
  refresh,
};

enum class load_kind : std::uint8_t
{
  write,
  software,
  hardware,
};

enum class holding : std::uint8_t
{
  right12_1,
  left12_1,
  right8_1,
  right12_2,
  left12_2,
  right8_2,
  right12_dual,
  left12_dual,
  right8_dual,
};

struct dac_state
{
  std::uint64_t                               generation{};
  std::array<std::uint64_t, channels * depth> due_ps{};
  std::array<std::uint8_t, channels * depth>  triggered{};
  std::array<std::uint16_t, channels>         dhr{};
  std::array<std::uint8_t, channels>          requested{};
  std::array<std::uint16_t, channels>         lfsr{lfsr_seed, lfsr_seed};
  std::array<std::uint16_t, channels>         triangle{};
  std::array<std::uint8_t, channels>          falling{};
  std::array<std::uint64_t, channels>         busy_ps{};
  std::array<std::uint8_t, channels>          phase{};
  std::array<std::uint64_t, channels>         phase_ps{};
  std::array<std::uint64_t, channels>         phase_event{};
  std::array<std::uint16_t, channels>         held{};
  std::array<std::uint8_t, channels>          held_set{};
  std::array<std::uint32_t, channels>         internal{};
  std::array<std::uint8_t, channels>          internal_set{};
  std::array<std::uint8_t, channels>          pad_driven{};

  friend auto operator==(dac_state const&, dac_state const&) -> bool = default;
};

enum class rule : std::uint8_t
{
  trigger_locked,
  amplitude_locked,
  calibration_locked,
  calibration_enable,
  wave_reserved,
  mode_locked,
  sample_hold_lsi,
  sample_busy,
  hold_locked,
  refresh_locked,
  wave_untriggered,
  trigger_reserved,
  count,
};

using selection = std::array<std::optional<std::uint32_t>, channels>;

/**
 * @brief DAC1 of the STM32L4, RM0351 section 19, two 12-bit channels on the APB1 clock.
 *        A write to DAC_DHR12Rx, DAC_DHR12Lx, DAC_DHR8Rx or a dual register stores the data
 *        into the internal DHR1 and DHR2 by the shifts of RM0351 19.4.3, and every holding
 *        register then reads its view of DHR1 and DHR2, inferred. With TENx 0 DHRx loads into
 *        DAC_DORx one APB1 clock cycle after the write, RM0351 19.4.4, the cycle counted from
 *        the access time at the APB1 rate of the write, inferred. With TENx 1 and TSELx 111 a
 *        DHR write loads nothing, and each SWTRIGx write loads DORx one APB1 cycle later,
 *        RM0351 19.4.6. SWTRIGx follows the `w` access of RM0351 19.7.2 and reads 0, inferred
 *        against the 19.4.6 text of a bit reset by hardware at the load. Written with TENx 0
 *        or another TSELx it loads nothing, inferred. With TENx 1 and TSELx below 111 the
 *        channel listens, as a trigger_sink, to the signal of RM0351 Table 128 that TSELx
 *        selects: `tim6_trgo`, `tim8_trgo`, `tim7_trgo`, `tim5_trgo`, `tim2_trgo`, `tim4_trgo`
 *        or `exti9`. Each rising edge loads DORx three APB1 cycles after the edge, RM0351
 *        19.4.4 and 19.4.6, counted from the exact instant the source gives the edge, and a
 *        falling edge loads nothing. The load takes DHRx as it is at the load, inferred. The
 *        model keeps three pending loads per channel, in order, and a fourth before the first
 *        load moves the third to its own due. Three cover the APB1 timers, whose update needs
 *        ARR 1 or more, RM0351 33.4.8, so two kernel clocks, one APB1 cycle at the most. TIM8
 *        on APB2 and EXTI9 can give edges closer than that. Popcorn charges no APB wait states,
 *        so writes closer than one APB1 cycle happen only in the model. In that window a load
 *        takes DHRx as it is at the load, and the same three slots hold the writes' loads.
 *        DAC_DORx is read only. The digital interface works with ENx 0, RM0351 19.4.2 note, so
 *        loads and triggers do not depend on ENx.
 *        While ENx is 1 a DAC_CR write keeps the old TSELx, RM0351 19.4.6 note, the old MAMPx,
 *        RM0351 19.4.9, and the old CENx, RM0351 19.7.1, and DAC_MCR keeps the old MODEx while
 *        ENx or CENx is 1, RM0351 19.7.16. ENx set while CENx is 1, which RM0351 19.4.11 does
 *        not allow, and WAVEx 11, reserved by RM0351 19.7.1, keep the write. Each of these logs
 *        one model_warning per rule, and under strict_peripherals raises model_error and leaves
 *        the register as before the write.
 *        With CENx 1 CAL_FLAGx reads 1 when OTRIMx is at or above the variant's factory trim and
 *        MODEx enables the buffer, 000, 001, 100 or 101, RM0351 19.4.11 and 19.7.14, at once
 *        with no tTRIM delay. Otherwise it reads 0 with a buffer-off MODEx and keeps its value
 *        with CENx 0, inferred. The factory trim is the variant's value for each channel,
 *        inferred, as RM0351 19.7.15 gives 0x00XX 00XX. DAC_CCR resets to it. DMAUDRx clears
 *        on a write of 1. CAL_FLAGx and BWSTx are read only.
 *        With DMAENx 1 a load from a hardware trigger raises the `dac1_ch<x>_dma` request at the
 *        load instant, RM0351 19.4.7, the instant inferred. A load from a DHR write or SWTRIGx
 *        raises none, RM0351 19.4.7. The request drops when a DMA transfer serving it
 *        acknowledges it, or when DMAENx is cleared, inferred. A hardware load while the request
 *        is still high raises no new request and sets DMAUDRx, RM0351 19.4.7, decided at the
 *        load, inferred, and DORx still loads DHRx. DMAUDRx with DMAUDRIEx holds the DAC's
 *        contribution to TIM6_DAC_IRQn, position 54 of RM0351 Table 61, which TIM6 shares.
 *        WAVEx 01 adds the LFSR of RM0351 Figure 141, masked to the MAMPx + 1 low bits, to DHRx
 *        at each SWTRIGx or hardware load, then steps it, RM0351 19.4.8 and 19.4.12, from the
 *        preload 0xAAA, Figure 142 giving 0xAAA then 0xD55. WAVEx 1x adds the triangle counter,
 *        then steps it up to 2^(MAMPx + 1) - 1 and back down to 0, RM0351 19.4.9 and 19.4.12,
 *        Figure 144 giving DHRx + 0, + 1, + 2. RM0351 19.4.9 reads the increment before the add,
 *        and the model follows 19.4.12 and Figure 144. A MAMPx above 1011 acts as 1011. The sum
 *        saturates at 0xFFF, the reading of the RM0351 "without overflow", inferred. WAVEx 00
 *        resets the LFSR to 0xAAA and the triangle counter to 0, RM0351 19.4.8 and 19.4.9. The
 *        LFSR runs the 4095 states from 0xAAA and never reaches 0, so the antilock-up injection
 *        of RM0351 19.4.8 never applies. The RM0351 19.4.8 and 19.4.9 notes require TENx for a
 *        wave. A holding register write with TENx 0 and WAVEx set logs one model_warning and
 *        loads DHRx with no wave, the load inferred, and under strict_peripherals raises
 *        model_error and leaves the holding registers as before the write. A DAC_CR write that
 *        sets WAVEx with TENx 0 is accepted, as LL_DAC_Init writes WAVEx before
 *        LL_DAC_EnableTrigger sets TENx.
 *        Each accepted DAC_SHSRx write sets BWSTx, RM0351 19.7.14, and so does a DAC_MCR write
 *        that sets MODEx 1xx, the sample and hold mode, which RM0351 19.7.14 places "just after
 *        sample and hold mode enable", the MCR write inferred. BWSTx clears 3 LSI periods later,
 *        RM0351 19.7.14 "about 3 LSI periods", the 3 inferred. While the LSI is off BWSTx stays
 *        set, a count already running included, and it clears 3 LSI periods after the LSI
 *        starts. A DAC_SHSRx write while ENx and BWSTx are both 1 is ignored, RM0351 19.7.17. A
 *        THOLDx or TREFRESHx write while ENx or CENx is 1 is ignored, RM0351 19.7.19 and
 *        19.7.20. MODEx 1xx set with the LSI off is not allowed, RM0351 19.4.10. Each of these
 *        logs one model_warning per rule, and under strict_peripherals raises model_error and
 *        leaves the register as before the write. The LSI stopping while a MODEx is 1xx logs the
 *        same rule's warning once, under strict_peripherals too, as the stop comes from an RCC
 *        write whose clock change every other listener also sees. A sample and hold channel
 *        loads DAC_DORx as a normal one does, and runs the phases of RM0351 19.4.10 in LSI
 *        periods: a sample phase of TSAMPLEx + 1, RM0351 19.7.17, then hold phases of THOLDx and
 *        refresh phases of TREFRESHx in turn, RM0351 19.7.19 and 19.7.20, the alternation
 *        inferred from RM0351 19.4.10. The sample phase starts at ENx set, at each DHRx change,
 *        RM0351 19.4.10 "any change in DAC_DHRx immediately triggers a new sample phase", and at
 *        each DAC_DORx load. A write of the value DHRx already holds is no change, inferred, and
 *        the restart at the load is inferred from RM0351 19.4.10 "the DAC core converts data on a
 *        triggered conversion". Each phase length is counted from the end of the one before. A
 *        phase of length 0 is skipped, and with THOLDx and TREFRESHx both 0 the channel stays in
 *        refresh with no further phase. While the LSI is off the phase in progress holds, and it
 *        restarts at its full length when the LSI starts, inferred. ENx 0 stops the phases.
 *        With ENx 1 a channel drives the chip analog slot of its DAC_OUTx pad, PA4 or PA5, RM0351
 *        19.3, when MODEx connects the pad, 000, 001, 010, 100, 101 or 110, RM0351 Table 130. The
 *        level is DORx x VDDA / 4096 in microvolts, rounded up, so an ADC at the same VDDA converts
 *        DORx back, VDDA from the board's supply_options taken as VREF+. It follows each DAC_DORx
 *        load at once, with no tSETTLING or tWAKEUP, and the buffer state changes no level,
 *        inferred. A sample and hold channel drives the DAC_DORx value held at the end of each
 *        sample and refresh phase, through the hold phase, in which RM0351 19.4.10 tri-states the
 *        output and the hold capacitor keeps the voltage, inferred. From ENx set to the end of its
 *        first sample phase it leaves the pad, inferred. The Table 129 settling formulas do not
 *        enter the level. MODEx 011 and 111, ENx 0, DAC1RST and a machine reset release the pad.
 *        The ADC channels on the pad convert the level, and a board drive on the pad yields to it
 *        as gpio_port states. MODEx 001, 011, 101, 110 and 111 connect the channel on chip, RM0351
 *        Table 130, and the model publishes the same level, the held one in sample and hold mode,
 *        MODEx 111 holding it on the internal CLint, through internal_analog_source for ADC2
 *        channels 17 and 18 and ADC3 channels 14 and 15, RM0351 18.4.11. MODEx 000, 010, 100, ENx
 *        0, DAC1RST and a machine reset disconnect it. Each change of the published level tells the
 *        ADCs that read it before it applies.
 *        A Stop 2 entry with ENx 1, which RM0351 19.5 Table 131 forbids, reaches the model as
 *        nothing: the PWR model has no Stop entry for a peripheral to see. In Stop 0 and Stop 1
 *        a channel keeps its level and a sample and hold channel runs its phases, as RM0351
 *        Table 131 states for the sample and hold mode.
 *        DAC1RST and a machine reset restore the claimed registers, drop the pending loads and
 *        the DMA requests, and lower the interrupt.
 */
class dac final : public dispatch_model<dac, dac_state>,
                  public bus_reset_listener,
                  public trigger_sink,
                  public dma_requester,
                  public clock_listener,
                  public internal_analog_source
{
public:
  dac(model_context&         ctx,
      peripheral_desc const& desc,
      dac_variant const&     variant,
      supply_options         supply);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     access) -> std::expected<void, bus_error> override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;
  void transfer_completed(dma_controller const& by, std::uint32_t request) override;
  void acknowledged(dma_controller const& by, std::uint32_t request) override;
  void on_clock_change() override;
  [[nodiscard]] auto internal_level(std::size_t output) const
      -> std::optional<std::uint32_t> override;

  void                                control_written(std::uint32_t old, std::uint32_t now);
  void                                mode_written(std::uint32_t old, std::uint32_t now);
  void                                trim_written(std::uint32_t old, std::uint32_t now);
  void                                status_written(std::uint32_t old, std::uint32_t now);
  void                                hold_written(std::uint32_t old, std::uint32_t now);
  void                                refresh_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Channel> void sample_written(std::uint32_t old, std::uint32_t now);
  void                                trigger_written(std::uint32_t old, std::uint32_t now);
  template <holding Which> void       holding_written(std::uint32_t old, std::uint32_t now);

private:
  struct channel_fields
  {
    field_ref enable;
    field_ref trigger_enable;
    field_ref trigger_select;
    field_ref amplitude;
    field_ref wave;
    field_ref calibration;
    field_ref software;
    reg_ref   output;
    field_ref mode;
    field_ref trim;
    field_ref flag;
    field_ref underrun;
    field_ref dma;
    field_ref underrun_interrupt;
    field_ref busy;
    reg_ref   sample;
    field_ref hold;
    field_ref refresh;
  };

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto cycles_ps(std::uint64_t cycles) const -> std::uint64_t;
  [[nodiscard]] auto load_delay_ps(trigger_time const& at) const -> std::uint64_t;
  [[nodiscard]] auto software_selected(std::size_t channel) const -> bool;
  [[nodiscard]] auto listened(std::uint32_t control) const -> selection;
  void               notify(selection const& which);
  void               retune(selection const& was);
  void               store(std::size_t channel, std::uint32_t data);
  void               mirror();
  void               request(std::size_t channel, std::uint64_t due, load_kind kind);
  void               catch_up(std::size_t channel, std::uint64_t ps);
  void               transfer(std::size_t channel, load_kind kind);
  [[nodiscard]] auto wave_term(std::size_t channel) -> std::uint32_t;
  void               release(std::size_t channel);
  [[nodiscard]] auto output_level(std::size_t channel, std::uint8_t modes) const
      -> std::optional<std::uint32_t>;
  void               publish(std::size_t channel);
  void               drive_output(std::size_t channel);
  void               drive_outputs();
  void               refresh_line();
  void               start_busy(std::size_t channel);
  void               settle_busy(std::size_t channel, std::uint64_t ps);
  [[nodiscard]] auto lsi_periods_ps(std::uint64_t periods) const -> std::uint64_t;
  [[nodiscard]] auto phase_periods(std::size_t channel, sample_phase which) const -> std::uint64_t;
  void               start_sample(std::size_t channel);
  void               advance_phase(std::size_t channel, std::uint64_t ps);
  void               schedule_phase(std::size_t channel);
  void               hold_phase(std::size_t channel);
  void               resume_phase(std::size_t channel);
  void               cancel_phase(std::size_t channel);
  void               keep_locked(
      field_ref const& bits,
      std::string_view name,
      rule             which,
      std::uint32_t    old,
      std::string      message);
  void restore_reset_values();
  void refresh_flags();
  void restore_trim();
  void
      refuse(rule which, std::string_view name, reg_ref at, std::uint32_t old, std::string message);

  dac_variant                                                  variant_;
  supply_options                                               supply_;
  register_file&                                               regs_;
  reg_ref                                                      control_;
  reg_ref                                                      mode_;
  reg_ref                                                      status_;
  std::array<reg_ref, 9>                                       holding_;
  std::array<channel_fields, 2>                                channel_;
  core::clock_domain                                           bus_;
  core::clock_domain                                           lsi_;
  irq_line*                                                    line_;
  std::array<dma_signal, channels>                             requests_;
  std::array<bool, std::to_underlying(rule::count)>            rule_logged_{};
  gpio_port*                                                   pad_port_{};
  std::array<std::vector<internal_analog_listener*>, channels> readers_;
  bool                                                         readers_known_{};
};

constexpr std::array<write_entry<dac>, 13> dac_writes{
    {
     {"CR", &dac::control_written},
     {"SR", &dac::status_written},
     {"DHR12R1", &dac::holding_written<holding::right12_1>},
     {"DHR12L1", &dac::holding_written<holding::left12_1>},
     {"DHR8R1", &dac::holding_written<holding::right8_1>},
     {"DHR12R2", &dac::holding_written<holding::right12_2>},
     {"DHR12L2", &dac::holding_written<holding::left12_2>},
     {"DHR8R2", &dac::holding_written<holding::right8_2>},
     {"DHR12RD", &dac::holding_written<holding::right12_dual>},
     {"DHR12LD", &dac::holding_written<holding::left12_dual>},
     {"DHR8RD", &dac::holding_written<holding::right8_dual>},
     {"DOR1", nullptr},
     {"DOR2", nullptr},
     }
};

constexpr std::array<write_entry<dac>, 1> dac_calibration_writes{{{"CCR", &dac::trim_written}}};

constexpr std::array<write_entry<dac>, 1> dac_mode_writes{{{"MCR", &dac::mode_written}}};

constexpr std::array<write_entry<dac>, 4> dac_sample_hold_writes{
    {
     {"SHSR1", &dac::sample_written<0>},
     {"SHSR2", &dac::sample_written<1>},
     {"SHHR", &dac::hold_written},
     {"SHRR", &dac::refresh_written},
     }
};

dac::dac(
    model_context&         ctx,
    peripheral_desc const& desc,
    dac_variant const&     variant,
    supply_options         supply)
    : dispatch_model{
          ctx, desc, "DAC", dac_writes
},
      variant_{variant}, supply_{supply}, regs_{ctx.regs}, control_{reg("CR")},
      mode_{variant.mode_register ? reg("MCR") : reg_ref{}}, status_{reg("SR")},
      holding_{reg("DHR12R1"), reg("DHR12L1"), reg("DHR8R1"),  reg("DHR12R2"), reg("DHR12L2"),
               reg("DHR8R2"),  reg("DHR12RD"), reg("DHR12LD"), reg("DHR8RD")},
      bus_{ctx.clock.domain("pclk1")}, lsi_{ctx.clock.domain("lsi")}, line_{&irq(variant.irq)},
      requests_{
          dma_signal{ctx.peers, lower(desc.name) + "_ch1_dma"},
          dma_signal{ctx.peers, lower(desc.name) + "_ch2_dma"}}
{
  bind(std::array<write_entry<dac>, 1>{{{variant.swtrig_register, &dac::trigger_written}}});
  if(variant.calibration)
  {
    bind(dac_calibration_writes);
  }
  if(variant.mode_register)
  {
    bind(dac_mode_writes);
  }
  if(variant.sample_hold)
  {
    bind(dac_sample_hold_writes);
  }
  auto const present = [&](bool on, std::string_view reg_name, std::string field_name) {
    return on ? field(reg_name, field_name) : field_ref{};
  };
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const n    = index + 1;
    channel_[index] = {
        .enable             = field("CR", std::format("EN{}", n)),
        .trigger_enable     = field("CR", std::format("TEN{}", n)),
        .trigger_select     = field("CR", std::format("TSEL{}", n)),
        .amplitude          = field("CR", std::format("MAMP{}", n)),
        .wave               = field("CR", std::format("WAVE{}", n)),
        .calibration        = present(variant.calibration, "CR", std::format("CEN{}", n)),
        .software           = field(variant.swtrig_register, std::format("SWTRIG{}", n)),
        .output             = reg(std::format("DOR{}", n)),
        .mode               = present(variant.mode_register, "MCR", std::format("MODE{}", n)),
        .trim               = present(variant.calibration, "CCR", std::format("OTRIM{}", n)),
        .flag               = present(variant.calibration, "SR", std::format("CAL_FLAG{}", n)),
        .underrun           = field("SR", std::format("DMAUDR{}", n)),
        .dma                = field("CR", std::format("DMAEN{}", n)),
        .underrun_interrupt = field("CR", std::format("DMAUDRIE{}", n)),
        .busy               = present(variant.sample_hold, "SR", std::format("BWST{}", n)),
        .sample             = variant.sample_hold ? reg(std::format("SHSR{}", n)) : reg_ref{},
        .hold               = present(variant.sample_hold, "SHHR", std::format("THOLD{}", n)),
        .refresh            = present(variant.sample_hold, "SHRR", std::format("TREFRESH{}", n)),
    };
  }
  restore_trim();
}

auto dac::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto dac::cycles_ps(std::uint64_t cycles) const -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(bus_);
  return hz == 0 ? 0 : (cycles * ps_per_second + hz - 1) / hz;
}

auto dac::load_delay_ps(trigger_time const& at) const -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(bus_);
  if(hz == 0 or at.hz == 0 or at.lag == 0)
  {
    return cycles_ps(3);
  }
  auto const span = wide{3} * ps_per_second * at.hz - wide{at.lag} * hz;
  auto const unit = wide{at.hz} * hz;
  return static_cast<std::uint64_t>((span + unit - 1) / unit);
}

auto dac::listened(std::uint32_t control) const -> selection
{
  selection which;
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(channel_[channel].trigger_enable.of(control) != 0)
    {
      which[channel] = variant_.hardware_index(channel_[channel].trigger_select.of(control));
    }
  }
  return which;
}

void dac::notify(selection const& which)
{
  for(auto const& select : which)
  {
    if(select and not variant_.hardware_triggers[*select].empty())
    {
      static_cast<void>(interest_changed(ctx_.peers, variant_.hardware_triggers[*select]));
    }
  }
}

void dac::retune(selection const& was)
{
  auto const now = listened(regs_.peek(control_.offset));
  if(now != was)
  {
    notify(was);
    notify(now);
  }
}

auto dac::listens(std::string_view signal) const -> bool
{
  return std::ranges::any_of(listened(regs_.peek(control_.offset)), [&](auto const& select) {
    return select and not signal.empty() and variant_.hardware_triggers[*select] == signal;
  });
}

auto dac::can_listen(std::string_view signal) const -> bool
{
  return not signal.empty() and std::ranges::contains(variant_.hardware_triggers, signal);
}

void dac::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  if(edge != trigger_edge::rising)
  {
    return;
  }
  auto const which = listened(regs_.peek(control_.offset));
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(which[channel]
       and not signal.empty()
       and variant_.hardware_triggers[*which[channel]] == signal)
    {
      catch_up(channel, at.ps);
      request(channel, at.ps + load_delay_ps(at), load_kind::hardware);
    }
  }
}

auto dac::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) != control_.offset)
  {
    return dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  auto const config = regs_.peek(control_.offset);
  auto const was    = listened(config);
  notify(was);
  notify(listened(merge_write(config, offset, size, value)));
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  retune(was);
  return result;
}

auto dac::software_selected(std::size_t channel) const -> bool
{
  return get(regs_, channel_[channel].trigger_select) == variant_.software_trigger;
}

void dac::refuse(
    rule             which,
    std::string_view name,
    reg_ref          at,
    std::uint32_t    old,
    std::string      message)
{
  if(ctx_.strict_peripherals)
  {
    regs_.poke(at.offset, old);
  }
  warn_once_or_refuse(rule_logged_[std::to_underlying(which)], name, at.offset, std::move(message));
}

void dac::control_written(std::uint32_t old, std::uint32_t now)
{
  auto       word = now;
  auto const keep = [&](field_ref const& bits, rule which, std::string message) {
    if(bits.of(word) == bits.of(old))
    {
      return;
    }
    word = bits.with(word, bits.of(old));
    regs_.poke(control_.offset, word);
    refuse(which, "CR", control_, old, std::move(message));
  };
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    auto const  n    = index + 1;
    if(unit.enable.of(old) == 0)
    {
      continue;
    }
    keep(
        unit.trigger_select, rule::trigger_locked,
        std::format("TSEL{0} written while EN{0} is 1, RM0351 19.4.6: the old TSEL{0} kept", n));
    keep(
        unit.amplitude, rule::amplitude_locked,
        std::format("MAMP{0} written while EN{0} is 1, RM0351 19.4.9: the old MAMP{0} kept", n));
    keep(
        unit.calibration, rule::calibration_locked,
        std::format("CEN{0} written while EN{0} is 1, ignored by RM0351 19.7.1", n));
  }
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    auto const  n    = index + 1;
    if(unit.calibration.of(word) != 0 and unit.enable.of(word) != 0 and unit.enable.of(old) == 0)
    {
      refuse(
          rule::calibration_enable, "CR", control_, old,
          std::format("EN{0} set while CEN{0} is 1, not allowed by RM0351 19.4.11", n));
    }
    if(auto const hardware = variant_.hardware_index(unit.trigger_select.of(word));
       unit.trigger_enable.of(word) != 0
       and hardware
       and variant_.hardware_triggers[*hardware].empty())
    {
      refuse(
          rule::trigger_reserved, "CR", control_, old,
          std::format("TSEL{} selects a reserved trigger with TEN{} set", n, n));
    }
    if(unit.wave.of(word) == 3)
    {
      refuse(
          rule::wave_reserved, "CR", control_, old,
          std::format("WAVE{} 11, reserved by RM0351 19.7.1", n));
    }
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(get(regs_, channel_[channel].wave) == 0)
    {
      state_.lfsr[channel]     = lfsr_seed;
      state_.triangle[channel] = 0;
      state_.falling[channel]  = 0;
    }
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    auto const& unit = channel_[channel];
    if(get(regs_, unit.enable) == 0)
    {
      cancel_phase(channel);
      state_.phase[channel]    = std::to_underlying(sample_phase::off);
      state_.phase_ps[channel] = 0;
      state_.held_set[channel] = 0;
    }
    else if(unit.enable.of(old) == 0 and get(regs_, unit.mode) >= 4)
    {
      start_sample(channel);
    }
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(get(regs_, channel_[channel].dma) == 0 and not variant_.dmaen_keeps_request)
    {
      release(channel);
    }
  }
  drive_outputs();
  refresh_flags();
  refresh_line();
}

void dac::mode_written(std::uint32_t old, std::uint32_t now)
{
  auto word = now;
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    auto const  n    = index + 1;
    if(unit.mode.of(word) != unit.mode.of(old)
       and (get(regs_, unit.enable) != 0 or get(regs_, unit.calibration) != 0))
    {
      word = unit.mode.with(word, unit.mode.of(old));
      regs_.poke(mode_.offset, word);
      refuse(
          rule::mode_locked, "MCR", mode_, old,
          std::format("MODE{0} written while EN{0} or CEN{0} is 1, ignored by RM0351 19.7.16", n));
    }
  }
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    if(unit.mode.of(word) >= 4 and unit.mode.of(old) < 4 and ctx_.clock.hz(lsi_) == 0)
    {
      refuse(
          rule::sample_hold_lsi, "MCR", mode_, old,
          std::format(
              "MODE{} 1xx with the LSI off, which RM0351 19.4.10 does not allow", index + 1));
    }
  }
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    if(get(regs_, unit.mode) >= 4 and unit.mode.of(old) < 4)
    {
      start_busy(index);
    }
  }
  drive_outputs();
  refresh_flags();
}

void dac::keep_locked(
    field_ref const& bits,
    std::string_view name,
    rule             which,
    std::uint32_t    old,
    std::string      message)
{
  auto const at   = reg(name);
  auto const word = regs_.peek(at.offset);
  if(bits.of(word) == bits.of(old))
  {
    return;
  }
  regs_.poke(at.offset, bits.with(word, bits.of(old)));
  refuse(which, name, at, old, std::move(message));
}

void dac::hold_written(std::uint32_t old, std::uint32_t)
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    if(get(regs_, unit.enable) != 0 or get(regs_, unit.calibration) != 0)
    {
      keep_locked(
          unit.hold, "SHHR", rule::hold_locked, old,
          std::format(
              "THOLD{0} written while EN{0} or CEN{0} is 1, ignored by RM0351 19.7.19", index + 1));
    }
  }
}

void dac::refresh_written(std::uint32_t old, std::uint32_t)
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    if(get(regs_, unit.enable) != 0 or get(regs_, unit.calibration) != 0)
    {
      keep_locked(
          unit.refresh, "SHRR", rule::refresh_locked, old,
          std::format(
              "TREFRESH{0} written while EN{0} or CEN{0} is 1, ignored by RM0351 19.7.20",
              index + 1));
    }
  }
}

template <std::size_t Channel> void dac::sample_written(std::uint32_t old, std::uint32_t)
{
  auto const& unit = channel_[Channel];
  settle_busy(Channel, now_ps());
  if(get(regs_, unit.enable) != 0 and get(regs_, unit.busy) != 0)
  {
    regs_.poke(unit.sample.offset, old);
    refuse(
        rule::sample_busy, std::format("SHSR{}", Channel + 1), unit.sample, old,
        std::format(
            "SHSR{0} written while EN{0} and BWST{0} are 1, ignored by RM0351 19.7.17",
            Channel + 1));
    return;
  }
  start_busy(Channel);
}

void dac::start_busy(std::size_t channel)
{
  auto const& unit = channel_[channel];
  update(regs_, unit.busy, 1);
  auto const hz = ctx_.clock.hz(lsi_);
  if(hz == 0)
  {
    state_.busy_ps[channel] = 0;
    return;
  }
  state_.busy_ps[channel] = now_ps() + (3 * ps_per_second + hz - 1) / hz;
  ctx_.events.schedule_at(
      vtime{state_.busy_ps[channel]}, static_cast<std::uint32_t>(channels + channel),
      state_.generation, ctx_.clock);
}

void dac::settle_busy(std::size_t channel, std::uint64_t ps)
{
  if(state_.busy_ps[channel] != 0 and state_.busy_ps[channel] <= ps)
  {
    state_.busy_ps[channel] = 0;
    update(regs_, channel_[channel].busy, 0);
  }
}

void dac::on_clock_change()
{
  if(ctx_.clock.hz(lsi_) == 0)
  {
    for(std::size_t channel = 0; channel < channels; ++channel)
    {
      if(get(regs_, channel_[channel].busy) != 0)
      {
        state_.busy_ps[channel] = 0;
      }
      hold_phase(channel);
      if(get(regs_, channel_[channel].mode) < 4)
      {
        continue;
      }
      auto& logged = rule_logged_[std::to_underlying(rule::sample_hold_lsi)];
      if(not std::exchange(logged, true))
      {
        warn(
            "MCR", std::format(
                       "the LSI stopped with MODE{} 1xx, which RM0351 19.4.10 does not allow",
                       channel + 1));
      }
    }
    return;
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(get(regs_, channel_[channel].busy) != 0 and state_.busy_ps[channel] == 0)
    {
      start_busy(channel);
    }
    resume_phase(channel);
  }
}

void dac::trim_written(std::uint32_t, std::uint32_t)
{
  refresh_flags();
}

void dac::status_written(std::uint32_t old, std::uint32_t now)
{
  auto word = old;
  for(auto const& unit : channel_)
  {
    if(unit.underrun.of(now) != 0)
    {
      word = unit.underrun.with(word, 0);
    }
  }
  regs_.poke(status_.offset, word);
  refresh_line();
}

void dac::refresh_flags()
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    if(get(regs_, unit.calibration) == 0)
    {
      continue;
    }
    auto const buffered = (get(regs_, unit.mode) & 2u) == 0;
    auto const reached  = get(regs_, unit.trim) >= variant_.factory_trim[index];
    update(regs_, unit.flag, buffered and reached ? 1u : 0u);
  }
}

void dac::restore_trim()
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    update(regs_, channel_[index].trim, variant_.factory_trim[index]);
  }
}

void dac::trigger_written(std::uint32_t, std::uint32_t now)
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(channel_[channel].software.of(now) != 0
       and get(regs_, channel_[channel].trigger_enable) != 0
       and software_selected(channel))
    {
      request(channel, now_ps() + cycles_ps(1), load_kind::software);
    }
  }
  regs_.poke(channel_[0].software.offset, 0);
}

template <holding Which> void dac::holding_written(std::uint32_t, std::uint32_t now)
{
  using enum holding;
  std::array<std::optional<std::uint32_t>, channels> data;
  switch(Which)
  {
    case right12_1:
      data[0] = now & data_mask;
      break;
    case left12_1:
      data[0] = now >> 4 & data_mask;
      break;
    case right8_1:
      data[0] = (now & 0xFF) << 4;
      break;
    case right12_2:
      data[1] = now & data_mask;
      break;
    case left12_2:
      data[1] = now >> 4 & data_mask;
      break;
    case right8_2:
      data[1] = (now & 0xFF) << 4;
      break;
    case right12_dual:
      data = {now & data_mask, now >> 16 & data_mask};
      break;
    case left12_dual:
      data = {now >> 4 & data_mask, now >> 20 & data_mask};
      break;
    case right8_dual:
      data = {(now & 0xFF) << 4, (now >> 8 & 0xFF) << 4};
      break;
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    auto const& unit = channel_[channel];
    if(data[channel] and get(regs_, unit.trigger_enable) == 0 and get(regs_, unit.wave) != 0)
    {
      if(ctx_.strict_peripherals)
      {
        mirror();
      }
      warn_once_or_refuse(
          rule_logged_[std::to_underlying(rule::wave_untriggered)], "CR", control_.offset,
          std::format(
              "a holding register write with WAVE{0} set and TEN{0} 0, which RM0351 19.4.8 and "
              "19.4.9 require for a wave: the load adds no wave",
              channel + 1));
    }
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(not data[channel])
    {
      continue;
    }
    auto const changed = *data[channel] != state_.dhr[channel];
    store(channel, *data[channel]);
    if(changed and state_.phase[channel] != std::to_underlying(sample_phase::off))
    {
      start_sample(channel);
    }
  }
  mirror();
}

void dac::store(std::size_t channel, std::uint32_t data)
{
  state_.dhr[channel] = static_cast<std::uint16_t>(data);
  if(get(regs_, channel_[channel].trigger_enable) == 0)
  {
    request(channel, now_ps() + cycles_ps(1), load_kind::write);
  }
}

void dac::mirror()
{
  auto const                         one = std::uint32_t{state_.dhr[0]};
  auto const                         two = std::uint32_t{state_.dhr[1]};
  std::array<std::uint32_t, 9> const views{
      one,
      one << 4,
      one >> 4,
      two,
      two << 4,
      two >> 4,
      one | two << 16,
      one << 4 | two << 20,
      one >> 4 | (two >> 4) << 8};
  for(std::size_t index = 0; index < views.size(); ++index)
  {
    regs_.poke(holding_[index].offset, views[index]);
  }
}

void dac::request(std::size_t channel, std::uint64_t due, load_kind kind)
{
  auto const first = channel * depth;
  auto       slot  = first;
  while(slot + 1 < first + depth and state_.due_ps[slot] != 0)
  {
    ++slot;
  }
  state_.due_ps[slot]    = due;
  state_.triggered[slot] = std::to_underlying(kind);
  if(slot == first)
  {
    ctx_.events.schedule_at(
        vtime{due}, static_cast<std::uint32_t>(channel), state_.generation, ctx_.clock);
  }
}

void dac::catch_up(std::size_t channel, std::uint64_t ps)
{
  auto const first = channel * depth;
  while(state_.due_ps[first] != 0 and state_.due_ps[first] <= ps)
  {
    auto const kind = static_cast<load_kind>(state_.triggered[first]);
    for(auto slot = first; slot + 1 < first + depth; ++slot)
    {
      state_.due_ps[slot]    = state_.due_ps[slot + 1];
      state_.triggered[slot] = state_.triggered[slot + 1];
    }
    state_.due_ps[first + depth - 1]    = 0;
    state_.triggered[first + depth - 1] = 0;
    transfer(channel, kind);
    if(state_.due_ps[first] != 0)
    {
      ctx_.events.schedule_at(
          vtime{state_.due_ps[first]}, static_cast<std::uint32_t>(channel), state_.generation,
          ctx_.clock);
    }
  }
}

auto dac::wave_term(std::size_t channel) -> std::uint32_t
{
  auto const& unit  = channel_[channel];
  auto const  wave  = get(regs_, unit.wave);
  auto const  width = std::min<std::uint32_t>(get(regs_, unit.amplitude), 11);
  auto const  top   = (2u << width) - 1u;
  if(wave == 1)
  {
    auto const value     = std::uint32_t{state_.lfsr[channel]};
    auto const tapped    = (value ^ value >> 1 ^ value >> 4 ^ value >> 6) & 1u;
    state_.lfsr[channel] = static_cast<std::uint16_t>(value >> 1 | tapped << 11);
    return value & top;
  }
  if(wave == 0)
  {
    return 0;
  }
  auto const value = std::uint32_t{state_.triangle[channel]};
  if(state_.falling[channel] == 0)
  {
    auto const next          = std::min(value + 1, top);
    state_.triangle[channel] = static_cast<std::uint16_t>(next);
    state_.falling[channel]  = next == top ? 1 : 0;
  }
  else
  {
    auto const next          = value == 0 ? 0 : value - 1;
    state_.triangle[channel] = static_cast<std::uint16_t>(next);
    state_.falling[channel]  = next == 0 ? 0 : 1;
  }
  return value;
}

void dac::transfer(std::size_t channel, load_kind kind)
{
  auto const& unit = channel_[channel];
  auto const  term = kind == load_kind::write ? 0u : wave_term(channel);
  regs_.poke(unit.output.offset, std::min(data_mask, std::uint32_t{state_.dhr[channel]} + term));
  if(state_.phase[channel] != std::to_underlying(sample_phase::off))
  {
    start_sample(channel);
  }
  drive_output(channel);
  if(kind != load_kind::hardware or get(regs_, unit.dma) == 0)
  {
    return;
  }
  if(state_.requested[channel] != 0)
  {
    update(regs_, unit.underrun, 1);
    refresh_line();
    return;
  }
  state_.requested[channel] = 1;
  requests_[channel].set(true);
}

void dac::release(std::size_t channel)
{
  if(state_.requested[channel] == 0)
  {
    return;
  }
  state_.requested[channel] = 0;
  requests_[channel].set(false);
}

auto dac::output_level(std::size_t channel, std::uint8_t modes) const
    -> std::optional<std::uint32_t>
{
  auto const& unit = channel_[channel];
  auto const  mode = get(regs_, unit.mode);
  if(get(regs_, unit.enable) == 0 or (modes >> mode & 1u) == 0)
  {
    return std::nullopt;
  }
  if(mode >= 4 and state_.held_set[channel] == 0)
  {
    return std::nullopt;
  }
  auto const code = mode >= 4 ? std::uint64_t{state_.held[channel]}
                              : std::uint64_t{regs_.peek(unit.output.offset) & data_mask};
  return static_cast<std::uint32_t>(
      (code * supply_.vdda_mv * 1000u + data_mask) / (data_mask + 1u));
}

auto dac::internal_level(std::size_t output) const -> std::optional<std::uint32_t>
{
  if(output >= channels or state_.internal_set[output] == 0)
  {
    return std::nullopt;
  }
  return state_.internal[output];
}

void dac::publish(std::size_t channel)
{
  auto const target = output_level(channel, variant_.chip_modes);
  if(target == internal_level(channel))
  {
    return;
  }
  if(not std::exchange(readers_known_, true))
  {
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const reader = dynamic_cast<internal_analog_listener*>(peer))
      {
        for(std::size_t output = 0; output < channels; ++output)
        {
          if(reader->reads_internal(*this, output))
          {
            readers_[output].push_back(reader);
          }
        }
      }
    }
  }
  for(auto* const reader : readers_[channel])
  {
    reader->internal_changing(*this, channel);
  }
  state_.internal[channel]     = target.value_or(0);
  state_.internal_set[channel] = target ? 1 : 0;
}

void dac::drive_output(std::size_t channel)
{
  if(pad_port_ == nullptr)
  {
    pad_port_ = dynamic_cast<gpio_port*>(ctx_.peers.find(variant_.pad_port));
  }
  auto const level = output_level(channel, variant_.pad_modes);
  if(pad_port_ != nullptr and (level or state_.pad_driven[channel] != 0))
  {
    pad_port_->drive_chip_analog(variant_.pads[channel], level);
  }
  state_.pad_driven[channel] = level ? 1 : 0;
  publish(channel);
}

void dac::drive_outputs()
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    drive_output(channel);
  }
}

void dac::acknowledged(dma_controller const& by, std::uint32_t request)
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(requests_[channel].carries(by, request))
    {
      release(channel);
    }
  }
}

void dac::transfer_completed(dma_controller const&, std::uint32_t)
{}

void dac::refresh_line()
{
  auto const raised = std::ranges::any_of(channel_, [&](channel_fields const& unit) {
    return get(regs_, unit.underrun) != 0 and get(regs_, unit.underrun_interrupt) != 0;
  });
  if(raised)
  {
    line_->raise();
  }
  else
  {
    line_->lower();
  }
}

void dac::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(payload != state_.generation)
  {
    return;
  }
  if(kind < channels)
  {
    catch_up(kind, now_ps());
  }
  else if(kind < 2 * channels)
  {
    settle_busy(kind - channels, now_ps());
  }
  else if(kind < 3 * channels)
  {
    advance_phase(kind - 2 * channels, now_ps());
  }
}

auto dac::lsi_periods_ps(std::uint64_t periods) const -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(lsi_);
  return hz == 0 ? 0 : (periods * ps_per_second + hz - 1) / hz;
}

auto dac::phase_periods(std::size_t channel, sample_phase which) const -> std::uint64_t
{
  auto const& unit = channel_[channel];
  switch(which)
  {
    case sample_phase::sample:
      return std::uint64_t{regs_.peek(unit.sample.offset) & 0x3FFu} + 1;
    case sample_phase::hold:
      return get(regs_, unit.hold);
    case sample_phase::refresh:
      return get(regs_, unit.refresh);
    case sample_phase::off:
      break;
  }
  return 0;
}

void dac::start_sample(std::size_t channel)
{
  cancel_phase(channel);
  state_.phase[channel]    = std::to_underlying(sample_phase::sample);
  state_.phase_ps[channel] = 0;
  if(auto const span = lsi_periods_ps(phase_periods(channel, sample_phase::sample)); span != 0)
  {
    state_.phase_ps[channel] = now_ps() + span;
    schedule_phase(channel);
  }
}

void dac::advance_phase(std::size_t channel, std::uint64_t ps)
{
  auto moved = false;
  while(state_.phase_ps[channel] != 0 and state_.phase_ps[channel] <= ps)
  {
    auto const current = static_cast<sample_phase>(state_.phase[channel]);
    if(current != sample_phase::hold)
    {
      state_.held[channel] =
          static_cast<std::uint16_t>(regs_.peek(channel_[channel].output.offset) & data_mask);
      state_.held_set[channel] = 1;
      drive_output(channel);
    }
    auto next    = current == sample_phase::hold ? sample_phase::refresh : sample_phase::hold;
    auto periods = phase_periods(channel, next);
    if(periods == 0)
    {
      next    = next == sample_phase::hold ? sample_phase::refresh : sample_phase::hold;
      periods = phase_periods(channel, next);
    }
    auto const span       = lsi_periods_ps(periods);
    state_.phase[channel] = std::to_underlying(next);
    if(span == 0)
    {
      state_.phase_ps[channel]    = 0;
      state_.phase_event[channel] = 0;
      return;
    }
    state_.phase_ps[channel] += span;
    moved = true;
  }
  if(moved)
  {
    schedule_phase(channel);
  }
}

void dac::hold_phase(std::size_t channel)
{
  if(state_.phase_ps[channel] == 0)
  {
    return;
  }
  cancel_phase(channel);
  advance_phase(channel, now_ps());
  cancel_phase(channel);
  state_.phase_ps[channel] = 0;
}

void dac::resume_phase(std::size_t channel)
{
  auto const current = static_cast<sample_phase>(state_.phase[channel]);
  if(current == sample_phase::off or state_.phase_ps[channel] != 0)
  {
    return;
  }
  if(current == sample_phase::sample)
  {
    start_sample(channel);
    return;
  }
  if(auto const span = lsi_periods_ps(phase_periods(channel, current)); span != 0)
  {
    state_.phase_ps[channel] = now_ps() + span;
    schedule_phase(channel);
  }
}

void dac::schedule_phase(std::size_t channel)
{
  state_.phase_event[channel] =
      ctx_.events.schedule_at(
          vtime{state_.phase_ps[channel]}, static_cast<std::uint32_t>(2 * channels + channel),
          state_.generation, ctx_.clock)
      + 1;
}

void dac::cancel_phase(std::size_t channel)
{
  if(auto const seq = std::exchange(state_.phase_event[channel], 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
}

auto dac::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    catch_up(channel, now_ps());
    settle_busy(channel, now_ps());
  }
  return dispatch_model::on_read(regs, offset, size, access);
}

void dac::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void dac::on_bus_reset()
{
  auto const was = listened(regs_.peek(control_.offset));
  notify(was);
  restore_reset_values();
  retune(was);
}

void dac::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    release(channel);
    cancel_phase(channel);
  }
  auto const generation   = state_.generation + 1;
  auto const internal     = state_.internal;
  auto const internal_set = state_.internal_set;
  auto const pad_driven   = state_.pad_driven;
  state_                  = {};
  state_.generation       = generation;
  state_.internal         = internal;
  state_.internal_set     = internal_set;
  state_.pad_driven       = pad_driven;
  restore_trim();
  drive_outputs();
  refresh_line();
}
}

auto dac_factory(supply_options supply, dac_variant variant) -> model_factory
{
  return
      [variant, supply](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
        return std::make_shared<dac>(ctx, desc, variant, supply);
      };
}

namespace
{
constexpr auto dac_row_table = std::to_array<builtin>({
    row_of<^^dac_factory, ^^l4_dac, ^^board::supply>(
        {.ip = "DAC", .ip_version_glob = "dacif_v2_0_Cube", .families = family::l4}),
    row_of<^^dac_factory, ^^l1_dac, ^^board::supply>(
        {.ip = "DAC", .ip_version_glob = "dacif_v1*", .families = family::l1}),
});
}

auto dac_rows() -> std::span<builtin const>
{
  return dac_row_table;
}
}
