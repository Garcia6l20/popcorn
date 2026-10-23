#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/divisor.hpp>
#include <popcorn/periph/core_hold.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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

constexpr std::uint64_t ps_per_second        = 1'000'000'000'000;
constexpr std::uint32_t tick_due             = 0;
constexpr std::size_t   max_channels         = 4;
constexpr std::uint32_t master_reset         = 0;
constexpr std::uint32_t master_enable        = 1;
constexpr std::uint32_t master_update        = 2;
constexpr std::uint32_t master_compare_pulse = 3;
constexpr std::uint32_t burst_update         = 1;
constexpr std::uint32_t burst_compare1       = 2;
constexpr std::uint32_t burst_compare4       = 5;
constexpr std::uint32_t burst_com            = 6;
constexpr std::uint32_t burst_trigger        = 7;
constexpr std::uint32_t max_burst            = 26;
constexpr std::uint8_t  etr_bit              = 1u << 4;
constexpr std::uint8_t  etrp_bit             = 1u << 5;
constexpr std::size_t   etr_input            = 4;
constexpr std::size_t   inputs_count         = 5;
constexpr std::uint32_t input_due            = 1;

struct input_filter
{
  std::uint8_t dts_divider;
  std::uint8_t samples;
};

constexpr std::array<input_filter, 16> input_filters{
    {
     {0, 0},
     {0, 2},
     {0, 4},
     {0, 8},
     {2, 6},
     {2, 8},
     {4, 6},
     {4, 8},
     {8, 6},
     {8, 8},
     {16, 5},
     {16, 6},
     {16, 8},
     {32, 5},
     {32, 6},
     {32, 8},
     }
};

enum class slave_mode : std::uint8_t
{
  disabled      = 0b0000,
  encoder1      = 0b0001,
  encoder2      = 0b0010,
  encoder3      = 0b0011,
  reset         = 0b0100,
  gated         = 0b0101,
  trigger       = 0b0110,
  external1     = 0b0111,
  reset_trigger = 0b1000,
  gated_reset   = 0b1001,
};

constexpr std::array<std::pair<std::uint32_t, std::string_view>, 7> internal_triggers{
    {
     {0, "tim1"},
     {1, "tim2"},
     {2, "tim3"},
     {3, "tim4"},
     {8, "tim5"},
     {9, "tim8"},
     {10, "tim15"},
     }
};

[[nodiscard]] constexpr auto modelled(std::uint32_t mode) -> bool
{
  using enum slave_mode;
  return mode == std::to_underlying(reset)
      or mode == std::to_underlying(gated)
      or mode == std::to_underlying(trigger)
      or mode == std::to_underlying(external1)
      or mode == std::to_underlying(reset_trigger)
      or mode == std::to_underlying(gated_reset);
}

[[nodiscard]] constexpr auto gating(std::uint32_t mode) -> bool
{
  return mode == std::to_underlying(slave_mode::gated)
      or mode == std::to_underlying(slave_mode::gated_reset);
}

[[nodiscard]] constexpr auto pin_select(std::uint32_t select) -> bool
{
  return select >= 4 and select <= 7;
}

[[nodiscard]] constexpr auto encoding(std::uint32_t mode) -> bool
{
  return (mode >= std::to_underlying(slave_mode::encoder1)
          and mode <= std::to_underlying(slave_mode::encoder3))
      or mode >= 0b1010;
}

class reacting_scope
{
public:
  explicit reacting_scope(bool& flag) : flag_{flag}, was_{std::exchange(flag, true)}
  {}
  reacting_scope(reacting_scope const&)                    = delete;
  auto operator=(reacting_scope const&) -> reacting_scope& = delete;
  reacting_scope(reacting_scope&&)                         = delete;
  auto operator=(reacting_scope&&) -> reacting_scope&      = delete;
  ~reacting_scope()
  {
    flag_ = was_;
  }

private:
  bool& flag_;
  bool  was_;
};

struct tim_state
{
  std::uint64_t                           anchor_ps{};
  std::uint64_t                           anchor_frac{};
  std::uint64_t                           hz{};
  std::uint64_t                           due_ps{};
  std::uint64_t                           carry{};
  std::uint32_t                           generation{};
  std::uint32_t                           count{};
  std::uint32_t                           repetition{};
  std::uint32_t                           prescaler{};
  std::uint32_t                           reload{};
  std::uint32_t                           edges{};
  std::uint32_t                           burst_index{};
  std::array<std::uint32_t, max_channels> compare{};
  std::array<std::uint8_t, max_channels>  compare_request{};
  std::uint8_t                            update_request{};
  std::uint8_t                            down{};
  std::uint8_t                            scheduled{};
  std::uint8_t                            gate{};
  std::uint8_t                            held{};
  std::uint64_t                           held_ps{};
  std::uint8_t                            inputs_seen{};
  std::uint8_t                            inputs_filtered{};
  std::uint8_t                            etr_edges{};
  std::array<std::uint8_t, max_channels>  capture_edges{};
  std::array<std::uint32_t, inputs_count> input_generation{};
  std::array<std::uint64_t, inputs_count> input_due_ps{};
  std::array<std::uint64_t, inputs_count> input_due_seq{};

  friend auto operator==(tim_state const&, tim_state const&) -> bool = default;
};

struct channel
{
  reg_ref                   ccr;
  std::optional<field_ref>  input_select;
  std::optional<field_ref>  polarity;
  std::optional<field_ref>  polarity_n;
  std::optional<field_ref>  capture_enable;
  std::optional<field_ref>  input_filter;
  std::optional<field_ref>  capture_prescaler;
  field_ref                 select;
  std::optional<field_ref>  preload;
  field_ref                 flag;
  std::optional<field_ref>  overcapture;
  field_ref                 interrupt;
  std::optional<field_ref>  dma;
  field_ref                 generate;
  std::optional<dma_signal> signal;
};

struct interrupt_source
{
  field_ref   enable;
  field_ref   flag;
  std::size_t line;
};

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/**
 * @brief TIM1, TIM2 to TIM7 and TIM15 to TIM17 of the STM32U5, RM0456 sections 54
 *        "Advanced-control timers (TIM1/TIM8)", 55 "General-purpose timers (TIM2/TIM3/TIM4/TIM5)",
 *        56 "General purpose timers (TIM15/TIM16/TIM17)" and 57 "Basic timers (TIM6/TIM7)": the
 *        time-base unit on the instance's timer clock. The counter is derived from the virtual time
 *        of its last anchor, never ticked: up, down and the three center-aligned modes, the
 *        prescaler, auto-reload and compare preloads loaded at the update event, the repetition
 *        counter, UDIS, URS, one-pulse mode, EGR event generation. Output compare channels 1 to 4
 *        set their flags when the counter matches, output pins are not driven. The counter width is
 *        the sum of the description's CNT fields. With UDE or CCxDE the `<instance>_upd_dma` and
 *        `<instance>_ccx_dma` requests rise with their event. Each request drops when the GPDMA
 *        acknowledges a burst for it, whatever that burst read or wrote. A DMA access to the timer
 *        releases nothing. The trigger output `<instance>_trgo` gives its edges to every listening
 *        trigger_sink as CR2.MMS selects. MMS 001 is the counter enable level, CEN and, in gated
 *        modes, the gate: a rising edge when it sets, a falling edge when it clears, by a write, at
 *        the one-pulse update or at a gate edge. MMS 000, 010 and 011 are positive pulses one timer
 *        kernel clock wide, rising at the event and falling one clock later: 000 at each UG write,
 *        010 at each update event, a UG one included whatever URS, 011 at each CC1IF set by a match
 *        or a capture. Each edge carries the time of its event. While a sink listens under MMS 010
 *        or 011, every update or CC1 match is scheduled at its own time instead of being skipped.
 *        MMS 100 and above, MMS2 and TRGO2 log a warning once and pulse nothing. The slave mode
 *        controller, RM0456 55.4 "Slave mode" and TIMx_SMCR, listens as a trigger_sink to the
 *        `<instance>_trgo` that TS selects among the internal triggers ITR0 TIM1, ITR1 TIM2, ITR2
 *        TIM3, ITR3 TIM4, ITR4 TIM5, ITR5 TIM8 and ITR6 TIM15. Reset mode reinitialises the counter
 *        and its prescaler at a rising edge, with the update event URS allows. Trigger mode sets
 *        CEN at a rising edge and counts from the edge's time. Reset + trigger does both. External
 *        clock mode 1 counts one tick per PSC + 1 rising edges, at the edge's time, and never on
 *        the timer clock. Gated mode counts on the timer clock while the master's level is high, a
 *        level only an MMS 001 master gives: under a pulsing master the counter holds and one
 *        warning is logged at its first rising edge. The gate is sampled again when the timer clock
 *        comes back. Gated + reset also reinitialises the counter when the gate closes. Every slave
 *        edge sets TIF, gate openings and closings included. ITR7, ITR8, ITR11, a timer's own ITR
 *        and the reserved codes reach nothing: the slave holds and one warning is logged at the
 *        SMCR write. Timer input pins: tim_ti1 to tim_ti4 are the `<instance>_ch1` to `_ch4`
 *        signals a GPIO pin in alternate function mode gives, while TIxSEL is 0, through the GPIO
 *        peers' signal levels. Another TIxSEL selects comparators, LSE, MCO or RTC: not modelled,
 *        the input reads low, logged once at the TISEL write. CR2.TI1S XORs tim_ti1 to tim_ti3 into
 *        tim_ti1. Each input transition takes effect at the time of its pin edge, after the ICxF
 *        filter below. The encoder modes, RM0456 55.4 "Encoder interface mode", SMS 0001 to 0011
 *        and 1010 to 1111, count on tim_ti1fp1 and tim_ti2fp2, CC1P and CC2P inverting them, by
 *        Table 569 and Table 570 and the clock plus direction rules, each count pulse through the
 *        prescaler, the counter wrapping between 0 and ARR with its update events. DIR follows
 *        every transition in the quadrature modes, tim_ti1 in the clock plus direction modes, the
 *        counting line in the directional clock modes, and DIRF rises when it changes. Table 569
 *        lists "Down" for a tim_ti2fp2 falling edge in mode 0001 with tim_ti1fp1 low: counted as
 *        nothing here, as Table 549, its TIM1/TIM8 copy, gives, since mode 0001 counts tim_ti1
 *        edges only. The encoder index, ECR.IE, and the transition and index error flags are not
 *        modelled, IE logs once. tim_etr_in is the `<instance>_etr` signal while AF1.ETRSEL is 0,
 *        other values read low and log once: ETP inverts it, ETPS divides it by 1, 2, 4 or 8:
 *        tim_etrp rises at every 2^ETPS-th rising edge and falls 2^(ETPS-1) rising edges later,
 *        then the ETF filter gives tim_etrf. External clock mode 2, ECE 1, counts each tim_etrf
 *        rising edge through the prescaler while a gated slave mode's gate is open, and with ECE
 *        set external clock mode 1 counts tim_etrf only, whatever TS, its trigger edges setting TIF
 *        alone. TS 00100 tim_ti1f_ed gives a rising edge at every tim_ti1 transition, TS 00101
 *        tim_ti1fp1 and 00110 tim_ti2fp2 their active edge by CCxP and CCxNP, both edges with both
 *        set, and their level to gated mode, TS 00111 tim_etrf: each drives the slave mode
 *        controller as an ITR edge does. Gated mode on tim_ti1f_ed holds and logs once, TS 00111 on
 *        a timer without ETR reaches nothing and logs once. A CCxP or ETP write that flips the
 *        polarised level is an edge of it, and so is a CCxNP write that moves the level TS 00101 or
 *        00110 gives the slave mode controller, as the polarity mux output moves on silicon: it
 *        counts in an encoder mode, triggers the slave mode controller and is a capture's active
 *        edge when the new level is high. Deviations: an ARR written below the count lets the
 *        counter run to its maximum and wrap with no update, a count written above ARR in
 *        center-aligned mode does the same, UIFREMAP puts UIFCPY in bit 31 only where the
 *        description names a UIFCPY field, not on the 32-bit TIM2 to TIM5, and a trigger cycle
 *        between timers drops the edge that would re-enter a timer still reacting to its own event,
 *        logged once per timer, where hardware would oscillate with a few clocks of
 *        resynchronisation delay per hop. Input capture, RM0456 55.4 "Input capture mode": a
 *        channel with CCxS 01 or 10 and CCxE set captures the counter, as it stands at the edge's
 *        time, into CCRx at every 1, 2, 4 or 8 active edges of its tim_tix, ICxPSC counted from the
 *        last CCxE clear, the active edge by CCxP and CCxNP, setting CCxIF, CCxOF on a capture
 *        while CCxIF is set, and its DMA request. A CPU or DMA read of CCRx clears CCxIF, a
 *        debugger read does not. The ICxF and ETF filters: a transition passes when its input stays
 *        at the new level for N sampling periods of fCK_INT or fDTS, fDTS from CKD, and takes
 *        effect N periods after the edge, so a shorter pulse vanishes. The sampling phase is
 *        ignored, an approximation within one sampling period, and the filter settings and timer
 *        clock in force at the edge set its delay, a change before it matures keeping that delay. A
 *        transition maturing while the timer is frozen by a debug hold applies at the hold, the
 *        filter itself counting on through it as tim_ker_ck does. Not modelled, logged once when
 *        the counter starts: SMSPE, a capture on TRC, CCxS 11, dithering, CCDS. TDE logs once and
 *        raises no trigger DMA request. DMA burst mode, RM0456 55.4 "DMA burst mode" and TIMx_DCR:
 *        every CPU or DMA access to DMAR reaches the register at DBA + index words from CR1, in the
 *        same byte lane, then advances the index, which wraps after DBL + 1 accesses. A debugger
 *        access redirects without advancing, so a test reading DMAR changes nothing the firmware
 *        sees next. CPU and DMA accesses share the index, so a CPU access to DMAR during a DMA
 *        burst moves it and can stretch the burst. A target the model serves runs its write
 *        handler. Any other described register takes the access merged under its writable bits. A
 *        target with no register reads 0, ignores the write and logs once. A DCR write resets the
 *        index, and so does each event of the DBSS source with its DMA enable set, so an event
 *        mid-burst restarts the burst: both are emulation choices RM0456 leaves open. The DBSS
 *        source, update or CC1 to CC4, keeps its request high across the GPDMA's acknowledges until
 *        the index wraps, so a channel moves DBL + 1 transfers per event. COM, trigger and reserved
 *        DBSS values log once and keep one request per event. Reserved DBL values above 11010 run
 *        26 transfers and log once. Traces name DMAR, not the redirected register. While the
 *        machine holds its core with DBGMCU `DBG_<instance>_STOP` set, the counter, its prescaler
 *        phase and its events stop at the time of the hold and resume at the release. Updates and
 *        matches due up to the hold apply at the hold, before any other event still pending at that
 *        instant, lane events included: deterministic, not in event key order. The bit is read at
 *        the hold: without a DBGMCU model or field the timer runs on, a warning logged, a
 *        model_error under strict_peripherals. A slave edge that reaches a frozen timer applies at
 *        the time of the hold: a reset, a trigger, an external clock tick or a gate change made
 *        during the hold acts as if it came at the hold, and the counter resumes from that state at
 *        the release. A frozen timer dates the TRGO edges it gives during the hold at the current
 *        time, never at the hold, so a sink that runs on sees no edge in its past. A frozen sink
 *        clamps it to its own hold. Registers are looked up by their bare names, `CR1` and so on,
 *        and by `TIM1_CR1` and the other `TIM1_` names only on a description that has no `CR1`,
 *        as one written by an older popcorn before TIM1 and TIM8 registers got bare names.
 */
class tim final : public dispatch_model<tim, tim_state>,
                  public clock_listener,
                  public trigger_source,
                  public trigger_sink,
                  public dma_requester,
                  public core_hold_listener,
                  public bus_reset_listener
{
public:
  tim(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
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
  void               on_clock_change() override;
  void               on_core_hold(bool held) override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto takes_pin(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps) override;
  void acknowledged(dma_controller const& by, std::uint32_t request) override;
  void transfer_completed(dma_controller const& by, std::uint32_t request) override;

  void                           control_written(std::uint32_t old, std::uint32_t now);
  void                           master_written(std::uint32_t old, std::uint32_t now);
  void                           slave_written(std::uint32_t old, std::uint32_t now);
  void                           interrupts_written(std::uint32_t old, std::uint32_t now);
  void                           status_written(std::uint32_t old, std::uint32_t now);
  void                           event_written(std::uint32_t old, std::uint32_t now);
  void                           counter_written(std::uint32_t old, std::uint32_t now);
  void                           reload_written(std::uint32_t old, std::uint32_t now);
  void                           burst_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Ch> void compare_written(std::uint32_t old, std::uint32_t now);
  void                           plain_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values();
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto output_time(std::uint64_t at) const -> std::uint64_t;
  [[nodiscard]] auto name(std::string_view reg) const -> std::string;
  [[nodiscard]] auto optional_field(std::string_view reg, std::string_view name) const
      -> std::optional<field_ref>;
  [[nodiscard]] auto center() const -> bool;
  [[nodiscard]] auto counting_up() const -> bool;
  [[nodiscard]] auto active() const -> bool;
  [[nodiscard]] auto counting() const -> bool;
  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto internally_clocked() const -> bool;
  [[nodiscard]] auto gate_open() const -> bool;
  [[nodiscard]] auto counter_enabled() const -> bool;
  [[nodiscard]] auto burst_source() const -> std::uint32_t;
  [[nodiscard]] auto burst_transfers() const -> std::uint32_t;
  [[nodiscard]] auto burst_target(std::uint32_t offset) const -> std::uint32_t;
  [[nodiscard]] auto described(std::uint32_t word) const
      -> std::optional<std::pair<std::uint32_t, std::uint32_t>>;
  [[nodiscard]] auto claimed(std::uint32_t word) const -> bool;
  [[nodiscard]] auto holds_burst(std::uint32_t source) const -> bool;
  [[nodiscard]] auto input_level(std::uint32_t smcr) const -> std::optional<bool>;
  [[nodiscard]] auto slave_of(std::uint32_t smcr) const -> std::uint32_t;
  [[nodiscard]] auto slave() const -> std::uint32_t;
  [[nodiscard]] auto trigger_select_of(std::uint32_t smcr) const -> std::uint32_t;
  [[nodiscard]] auto input_of(std::uint32_t smcr) const -> std::optional<std::string_view>;
  [[nodiscard]] auto flagged_when(bool up) const -> bool;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto ticks_until(std::uint64_t ps) const -> std::uint64_t;
  [[nodiscard]] auto ticks_to_wrap() const -> std::uint64_t;
  [[nodiscard]] auto ticks_to_match(std::size_t ch, std::uint64_t wrap) const -> std::uint64_t;
  [[nodiscard]] auto canonical() const -> bool;
  [[nodiscard]] auto pending() const -> bool;
  [[nodiscard]] auto output(std::size_t ch) const -> bool;
  [[nodiscard]] auto compare_value(std::size_t ch) const -> std::uint32_t;
  [[nodiscard]] auto repetition_reload() const -> std::uint32_t;
  [[nodiscard]] auto wants_update() -> bool;
  [[nodiscard]] auto wants_match(std::size_t ch) -> bool;
  [[nodiscard]] auto master_mode() const -> std::uint32_t;
  [[nodiscard]] auto master_mode_of(std::uint32_t cr2) const -> std::uint32_t;
  [[nodiscard]] auto trgo_wanted(std::uint32_t mode) -> bool;
  [[nodiscard]] auto at_ps(wide at) const -> std::uint64_t;
  [[nodiscard]] auto ticks_in(wide span) const -> std::uint64_t;

  void start_at(std::uint64_t ps);
  void rebase(std::uint64_t ps, std::uint64_t hz);
  void advance(std::uint64_t ticks);
  void catch_up(std::uint64_t ps);
  void visit(std::uint64_t ticks, std::optional<std::uint64_t> at = std::nullopt);
  void wrap(std::uint64_t at);
  void external_tick(std::uint64_t at);
  void reinitialise(std::uint64_t at);
  void begin(std::uint64_t at);
  void gate(std::optional<bool> level, trigger_edge edge, bool closing_resets, std::uint64_t at);
  void slave_edge(trigger_edge edge, std::optional<bool> level, std::uint64_t at);
  void pin_trigger(std::size_t input, std::uint64_t at, bool polarity = false);
  void etr_transition(bool level, std::uint64_t at, bool clocked);
  void etr_filtered(bool level, std::uint64_t at, bool clocked);
  void filter_input(std::size_t input, bool level, std::uint64_t at, bool clocked);
  void apply_input(std::size_t input, bool level, std::uint64_t at, bool clocked);
  void capture_edge(std::size_t input, bool level, std::uint64_t at);
  void capture_count(std::size_t ch, std::uint64_t at);
  void captured_read(std::uint32_t word, access_ctx access);
  void polarity_flipped(std::size_t ch, std::uint64_t at);
  [[nodiscard]] auto filter_delay(std::size_t input) const -> std::uint64_t;
  [[nodiscard]] auto input_level_bit(std::size_t input) const -> bool;
  [[nodiscard]] auto pin_trigger_level(std::uint32_t select) const -> std::optional<bool>;
  void               enable_changed(bool was, std::uint64_t at);
  void               slave_settled(std::optional<std::string_view> before, std::uint32_t smcr);
  void               skip_groups(std::uint64_t groups);
  void               update_event(bool software, std::uint64_t at);
  void               pulse_trgo(std::uint64_t at);
  void               enable_edge(trigger_edge edge, std::uint64_t at);
  void               match(std::size_t ch, std::uint64_t at);
  void               capture(std::size_t ch, std::uint64_t at);
  void               request_update();
  void               release(dma_controller const& by, std::uint32_t request);
  void               advance_burst(access_ctx access);
  void               warn_burst_range(std::uint32_t word);
  void               settle();
  void               reschedule();
  void               warn_unsupported();
  void               warn_inputs();
  [[nodiscard]] auto input_signal(std::string_view signal) const -> bool;
  [[nodiscard]] auto pin_level(std::string_view signal) const -> bool;
  [[nodiscard]] auto raw_input(std::size_t ch) const -> bool;
  [[nodiscard]] auto effective_input(std::size_t ch) const -> bool;
  [[nodiscard]] auto filtered(std::size_t ch) const -> bool;
  [[nodiscard]] auto polarised(std::size_t ch) const -> bool;
  void               input_edge(std::uint64_t at);
  void               inputs_changed(std::uint64_t at, bool clocked);
  void               input_transition(std::size_t ch, bool level, std::uint64_t at, bool clocked);
  void               encoder_edge(std::size_t ch, std::uint64_t at);
  void               warn_selects();

  register_file&                               regs_;
  std::string                                  prefix_;
  std::uint32_t                                mask_{};
  field_ref                                    enable_;
  field_ref                                    no_update_;
  field_ref                                    update_source_;
  field_ref                                    one_pulse_;
  field_ref                                    reload_preload_;
  field_ref                                    remap_;
  std::optional<field_ref>                     direction_;
  std::optional<field_ref>                     center_mode_;
  field_ref                                    update_flag_;
  field_ref                                    update_interrupt_;
  std::optional<field_ref>                     update_dma_;
  field_ref                                    update_generate_;
  reg_ref                                      status_;
  reg_ref                                      events_;
  reg_ref                                      counter_;
  std::optional<field_ref>                     copy_;
  field_ref                                    prescaler_;
  mutable core::divisor                        hz_divisor_;
  mutable core::divisor                        unit_divisor_;
  reg_ref                                      reload_;
  std::optional<field_ref>                     repetition_;
  std::optional<dma_signal>                    update_signal_;
  std::vector<channel>                         channels_;
  std::vector<irq_line*>                       lines_;
  std::vector<interrupt_source>                sources_;
  std::vector<std::pair<field_ref, field_ref>> generated_;
  std::vector<field_ref>                       unsupported_;
  core::clock_domain                           domain_;
  std::optional<field_ref>                     burst_length_;
  std::optional<field_ref>                     burst_base_;
  std::optional<field_ref>                     burst_select_;
  std::optional<std::uint32_t>                 burst_word_;
  std::vector<std::uint32_t>                   claimed_words_;
  std::vector<std::array<std::uint32_t, 3>>    stored_words_;
  bool                                         burst_range_logged_{};
  bool                                         burst_length_logged_{};
  bool                                         burst_source_logged_{};
  std::optional<field_ref>                     master_;
  std::optional<field_ref>                     master_high_;
  std::optional<field_ref>                     master2_;
  std::optional<trigger_output>                trgo_;
  std::string                                  instance_;
  std::optional<field_ref>                     slave_;
  std::optional<field_ref>                     slave_high_;
  std::optional<field_ref>                     select_;
  std::optional<field_ref>                     select_high_;
  std::optional<field_ref>                     slave_preload_;
  std::optional<field_ref>                     external_clock2_;
  std::optional<field_ref>                     trigger_flag_;
  std::optional<field_ref>                     trigger_dma_;
  bool                                         reacting_{};
  bool                                         cycle_logged_{};
  bool                                         unrouted_logged_{};
  bool                                         gate_logged_{};
  bool                                         trigger_dma_logged_{};
  bool                                         unsupported_logged_{};
  bool                                         encoder_logged_{};
  bool                                         etr_logged_{};
  bool                                         pin_logged_{};
  bool                                         freeze_logged_{};
  bool                                         master_logged_{};
  std::vector<std::string>                     input_signals_;
  std::optional<field_ref>                     ti1_xor_;
  std::optional<field_ref>                     direction_flag_;
  std::optional<field_ref>                     index_enable_;
  std::optional<std::uint32_t>                 select_word_;
  std::optional<std::uint32_t>                 control2_word_;
  std::optional<std::uint32_t>                 index_word_;
  std::optional<std::uint32_t>                 etr_word_;
  std::optional<field_ref>                     etr_select_;
  std::optional<field_ref>                     etr_polarity_;
  std::optional<field_ref>                     etr_prescaler_;
  std::string                                  etr_signal_;
  std::optional<field_ref>                     etr_filter_;
  std::optional<field_ref>                     clock_division_;
  std::optional<std::uint32_t>                 capture_word_;
  bool                                         select_logged_{};
  mutable std::vector<gpio_port const*>        ports_;
  mutable bool                                 ports_resolved_{};
  bool                                         index_logged_{};
};

auto tim_prefix(peripheral_desc const& desc) -> std::string_view
{
  return find_register(desc, "CR1") == nullptr and find_register(desc, "TIM1_CR1") != nullptr
           ? std::string_view{"TIM1_"}
           : std::string_view{};
}

template <std::size_t... Ch>
auto compare_entries(std::string_view prefix, std::index_sequence<Ch...>)
    -> std::array<write_entry<tim>, sizeof...(Ch)>
{
  static constexpr std::array<std::string_view, 4> plain{"CCR1", "CCR2", "CCR3", "CCR4"};
  static constexpr std::array<std::string_view, 4> prefixed{
      "TIM1_CCR1", "TIM1_CCR2", "TIM1_CCR3", "TIM1_CCR4"};
  auto const& names = prefix.empty() ? plain : prefixed;
  return {{{names[Ch], &tim::compare_written<Ch>}...}};
}

auto tim_writes(peripheral_desc const& desc) -> std::vector<write_entry<tim>>
{
  static constexpr std::array<write_entry<tim>, 16> plain{
      {
       {"CR1", &tim::control_written},
       {"CR2", &tim::master_written},
       {"SMCR", &tim::slave_written},
       {"DIER", &tim::interrupts_written},
       {"SR", &tim::status_written},
       {"EGR", &tim::event_written},
       {"CNT", &tim::counter_written},
       {"PSC", &tim::plain_written},
       {"ARR", &tim::reload_written},
       {"RCR", &tim::plain_written},
       {"DCR", &tim::burst_written},
       {"DMAR", &tim::plain_written},
       {"TISEL", &tim::plain_written},
       {"ECR", &tim::plain_written},
       {"AF1", &tim::plain_written},
       {"CCER", &tim::plain_written},
       }
  };
  static constexpr std::array<write_entry<tim>, 16> prefixed{
      {
       {"TIM1_CR1", &tim::control_written},
       {"TIM1_CR2", &tim::master_written},
       {"TIM1_SMCR", &tim::slave_written},
       {"TIM1_DIER", &tim::interrupts_written},
       {"TIM1_SR", &tim::status_written},
       {"TIM1_EGR", &tim::event_written},
       {"TIM1_CNT", &tim::counter_written},
       {"TIM1_PSC", &tim::plain_written},
       {"TIM1_ARR", &tim::reload_written},
       {"TIM1_RCR", &tim::plain_written},
       {"TIM1_DCR", &tim::burst_written},
       {"TIM1_DMAR", &tim::plain_written},
       {"TIM1_TISEL", &tim::plain_written},
       {"TIM1_ECR", &tim::plain_written},
       {"TIM1_AF1", &tim::plain_written},
       {"TIM1_CCER", &tim::plain_written},
       }
  };
  auto const has    = [&](std::string_view reg) { return find_register(desc, reg) != nullptr; };
  auto const prefix = tim_prefix(desc);
  std::vector<write_entry<tim>> out;
  for(auto const& entry : prefix.empty() ? plain : prefixed)
  {
    auto const optional = entry.reg.ends_with("CR2")
                       or entry.reg.ends_with("SMCR")
                       or entry.reg.ends_with("RCR")
                       or entry.reg.ends_with("DCR")
                       or entry.reg.ends_with("DMAR")
                       or entry.reg.ends_with("TISEL")
                       or entry.reg.ends_with("ECR")
                       or entry.reg.ends_with("AF1")
                       or entry.reg.ends_with("CCER");
    if(has(entry.reg) or not optional)
    {
      out.push_back(entry);
    }
  }
  for(auto const& entry : compare_entries(prefix, std::make_index_sequence<max_channels>{}))
  {
    if(has(entry.reg))
    {
      out.push_back(entry);
    }
  }
  return out;
}

auto multiline_group(std::string_view base) -> std::string_view
{
  if(base == "U")
  {
    return "UP";
  }
  if(base == "B" or base == "B2" or base == "SB")
  {
    return "BRK";
  }
  if(base.starts_with("CC"))
  {
    return "CC";
  }
  return "TRG_COM";
}

tim::tim(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "TIM", tim_writes(desc)}, regs_{ctx.regs},
      prefix_{tim_prefix(desc)}, enable_{field(name("CR1"), "CEN")},
      no_update_{field(name("CR1"), "UDIS")}, update_source_{field(name("CR1"), "URS")},
      one_pulse_{field(name("CR1"), "OPM")}, reload_preload_{field(name("CR1"), "ARPE")},
      remap_{field(name("CR1"), "UIFREMAP")}, direction_{optional_field(name("CR1"), "DIR")},
      center_mode_{optional_field(name("CR1"), "CMS")}, update_flag_{field(name("SR"), "UIF")},
      update_interrupt_{field(name("DIER"), "UIE")},
      update_dma_{optional_field(name("DIER"), "UDE")}, update_generate_{field(name("EGR"), "UG")},
      status_{reg(name("SR"))}, events_{reg(name("EGR"))}, counter_{reg(name("CNT"))},
      copy_{optional_field(name("CNT"), "UIFCPY")}, prescaler_{field(name("PSC"), "PSC")},
      reload_{reg(name("ARR"))}, repetition_{optional_field(name("RCR"), "REP")},
      domain_{ctx.clock.domain(lower(desc.name))},
      burst_length_{optional_field(name("DCR"), "DBL")},
      burst_base_{optional_field(name("DCR"), "DBA")},
      burst_select_{optional_field(name("DCR"), "DBSS")},
      master_{optional_field(name("CR2"), "MMS")},
      master_high_{optional_field(name("CR2"), "MMS_3")},
      master2_{optional_field(name("CR2"), "MMS2")}, instance_{lower(desc.name)},
      slave_{optional_field(name("SMCR"), "SMS")}, select_{optional_field(name("SMCR"), "TS")},
      slave_preload_{optional_field(name("SMCR"), "SMSPE")},
      external_clock2_{optional_field(name("SMCR"), "ECE")},
      trigger_dma_{optional_field(name("DIER"), "TDE")}
{
  if(not master_high_)
  {
    master_high_ = optional_field(name("CR2"), "MMS_1");
  }
  for(auto const bits : {"SMS_1", "SMS_bit3", "SMS_3"})
  {
    slave_high_ = slave_high_ ? slave_high_ : optional_field(name("SMCR"), bits);
  }
  for(auto const bits : {"TS_1", "TS_4_3"})
  {
    select_high_ = select_high_ ? select_high_ : optional_field(name("SMCR"), bits);
  }
  if(slave_)
  {
    trigger_flag_ = field(name("SR"), "TIF");
  }
  std::uint32_t top = 0;
  for(auto const& bits : describe(name("CNT")).fields)
  {
    if(bits.name != "UIFCPY")
    {
      top = std::max<std::uint32_t>(top, bits.lsb + bits.width);
    }
  }
  if(top == 0)
  {
    missing(std::format("{}.CNT", name("CNT")));
  }
  mask_                = top >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << top) - 1u;
  auto const& instance = instance_;
  if(update_dma_)
  {
    update_signal_.emplace(ctx.peers, instance + "_upd_dma");
  }
  if(master_)
  {
    trgo_.emplace(ctx.peers, instance + "_trgo");
  }
  for(std::size_t ch = 0; ch < max_channels; ++ch)
  {
    auto const n   = ch + 1;
    auto const ccr = name(std::format("CCR{}", n));
    if(not has_register(ccr))
    {
      break;
    }
    auto const               mode = n <= 2 ? 1 : 2;
    auto const               pe   = std::format("OC{}PE", n);
    std::optional<field_ref> preload;
    std::string              view;
    for(auto const& candidate :
        {name(std::format("CCMR{}_Output", mode)), name(std::format("CCMR{}_ALTERNATE1", mode))})
    {
      if(has_register(candidate)
         and std::ranges::contains(describe(candidate).fields, pe, &field_desc::name))
      {
        view    = candidate;
        preload = field(candidate, pe);
        break;
      }
    }
    if(view.empty())
    {
      missing(std::format("{}.{}", name(std::format("CCMR{}", mode)), pe));
    }
    auto& c          = channels_.emplace_back();
    c.ccr            = reg(ccr);
    c.select         = field(view, std::format("CC{}S", n));
    c.preload        = preload;
    c.flag           = field(name("SR"), std::format("CC{}IF", n));
    c.overcapture    = optional_field(name("SR"), std::format("CC{}OF", n));
    c.interrupt      = field(name("DIER"), std::format("CC{}IE", n));
    c.dma            = optional_field(name("DIER"), std::format("CC{}DE", n));
    c.generate       = field(name("EGR"), std::format("CC{}G", n));
    c.input_select   = optional_field(name("TISEL"), std::format("TI{}SEL", n));
    c.polarity       = optional_field(name("CCER"), std::format("CC{}P", n));
    c.polarity_n     = optional_field(name("CCER"), std::format("CC{}NP", n));
    c.capture_enable = optional_field(name("CCER"), std::format("CC{}E", n));
    for(auto const& input_view :
        {name(std::format("CCMR{}_Input", mode)), name(std::format("CCMR{}", mode))})
    {
      if(not c.input_filter)
      {
        c.input_filter      = optional_field(input_view, std::format("IC{}F", n));
        c.capture_prescaler = optional_field(input_view, std::format("IC{}PSC", n));
      }
    }
    input_signals_.push_back(std::format("{}_ch{}", instance, n));
    if(c.dma)
    {
      c.signal.emplace(ctx.peers, std::format("{}_cc{}_dma", instance, n));
    }
  }
  auto const multiline =
      std::ranges::contains(ctx.part.irqs, std::format("{}_UP_IRQn", desc.name), &irq_desc::name);
  std::vector<std::string> groups;
  auto const               line_of = [&](std::string_view base) -> std::size_t {
    auto const irq_name = multiline ? std::format("{}_{}_IRQn", desc.name, multiline_group(base))
                                    : std::format("{}_IRQn", desc.name);
    auto const at       = std::ranges::find(groups, irq_name);
    if(at != groups.end())
    {
      return static_cast<std::size_t>(at - groups.begin());
    }
    lines_.push_back(&irq(irq_name));
    groups.push_back(irq_name);
    return lines_.size() - 1;
  };
  auto const& flags = describe(name("SR")).fields;
  for(auto const& enable : describe(name("DIER")).fields)
  {
    std::string_view const enable_name{enable.name};
    if(not enable_name.ends_with("IE"))
    {
      continue;
    }
    auto const base = enable_name.substr(0, enable_name.size() - 2);
    for(auto const& flag_name :
        {std::format("{}IF", base), std::format("{}F", base),
         base == "B" ? std::string{"B2IF"} : std::string{},
         base == "B" ? std::string{"SBIF"} : std::string{}})
    {
      if(not flag_name.empty() and std::ranges::contains(flags, flag_name, &field_desc::name))
      {
        sources_.push_back(
            {.enable = field(name("DIER"), enable_name),
             .flag   = field(name("SR"), flag_name),
             .line   = line_of(base)});
      }
    }
  }
  for(auto const& generate : describe(name("EGR")).fields)
  {
    std::string_view const generate_name{generate.name};
    if(generate_name == "UG" or generate_name.starts_with("CC") or not generate_name.ends_with("G"))
    {
      continue;
    }
    auto const flag_name = std::format("{}IF", generate_name.substr(0, generate_name.size() - 1));
    if(std::ranges::contains(flags, flag_name, &field_desc::name))
    {
      generated_.emplace_back(field(name("EGR"), generate_name), field(name("SR"), flag_name));
    }
  }
  for(auto const& [reg_name, bits] : std::array<std::pair<std::string_view, std::string_view>, 3>{
          {
           {"SMCR", "SMSPE"},
           {"CR1", "DITHEN"},
           {"CR2", "CCDS"},
           }
  })
  {
    if(auto const found = optional_field(name(reg_name), bits))
    {
      unsupported_.push_back(*found);
    }
  }
  if(has_register(name("DMAR")))
  {
    burst_word_ = reg(name("DMAR")).offset;
  }
  ti1_xor_        = optional_field(name("CR2"), "TI1S");
  direction_flag_ = optional_field(name("SR"), "DIRF");
  index_enable_   = optional_field(name("ECR"), "IE");
  if(has_register(name("TISEL")))
  {
    select_word_ = reg(name("TISEL")).offset;
  }
  if(has_register(name("CR2")))
  {
    control2_word_ = reg(name("CR2")).offset;
  }
  if(index_enable_)
  {
    index_word_ = index_enable_->offset;
  }
  etr_select_     = optional_field(name("AF1"), "ETRSEL");
  etr_polarity_   = optional_field(name("SMCR"), "ETP");
  etr_prescaler_  = optional_field(name("SMCR"), "ETPS");
  etr_filter_     = optional_field(name("SMCR"), "ETF");
  clock_division_ = optional_field(name("CR1"), "CKD");
  if(has_register(name("CCER")))
  {
    capture_word_ = reg(name("CCER")).offset;
  }
  if(etr_select_)
  {
    etr_word_   = etr_select_->offset;
    etr_signal_ = instance + "_etr";
  }
  for(auto const& claimed : claims())
  {
    claimed_words_.push_back(reg(claimed).offset);
  }
  for(auto const& described : desc.registers)
  {
    auto const    at    = reg(described.name);
    auto const    whole = field_ref{.offset = at.offset, .lsb = at.lsb, .width = at.width}.mask();
    std::uint32_t writable = described.fields.empty() ? whole : 0;
    std::uint32_t readable = described.fields.empty() ? whole : 0;
    for(auto const& bits : described.fields)
    {
      auto const mask =
          field_ref{
              .offset = at.offset,
              .lsb    = static_cast<std::uint8_t>(at.lsb + bits.lsb),
              .width  = bits.width}
              .mask();
      writable |= bits.acc != access::read_only ? mask : 0;
      readable |= bits.acc != access::write_only and bits.acc != access::write_once ? mask : 0;
    }
    auto const found =
        std::ranges::find(stored_words_, at.offset, [](auto const& w) { return w[0]; });
    if(found == stored_words_.end())
    {
      stored_words_.push_back({at.offset, writable, readable});
    }
    else
    {
      (*found)[1] |= writable;
      (*found)[2] |= readable;
    }
  }
}

auto tim::burst_source() const -> std::uint32_t
{
  return burst_select_ ? get(regs_, *burst_select_) : 0u;
}

auto tim::burst_transfers() const -> std::uint32_t
{
  return burst_length_ ? std::min(get(regs_, *burst_length_), max_burst - 1) + 1 : 1u;
}

auto tim::burst_target(std::uint32_t offset) const -> std::uint32_t
{
  auto const base = burst_base_ ? get(regs_, *burst_base_) : 0u;
  return (base + state_.burst_index) * 4 + (offset & 3u);
}

auto tim::described(std::uint32_t word) const
    -> std::optional<std::pair<std::uint32_t, std::uint32_t>>
{
  auto const found = std::ranges::find(stored_words_, word, [](auto const& w) { return w[0]; });
  if(found == stored_words_.end())
  {
    return std::nullopt;
  }
  return std::pair{(*found)[1], (*found)[2]};
}

auto tim::claimed(std::uint32_t word) const -> bool
{
  return std::ranges::contains(claimed_words_, word);
}

auto tim::holds_burst(std::uint32_t source) const -> bool
{
  return burst_source() == source and state_.burst_index != 0;
}

void tim::warn_burst_range(std::uint32_t word)
{
  if(not std::exchange(burst_range_logged_, true))
  {
    warn(
        name("DMAR"),
        std::format(
            "DMA burst access at offset {:#x}: no timer register there, reads 0 and ignores the "
            "write",
            word));
  }
}

void tim::advance_burst(access_ctx access)
{
  if(access.by != initiator::debugger)
  {
    state_.burst_index = (state_.burst_index + 1) % burst_transfers();
  }
}

auto tim::name(std::string_view reg) const -> std::string
{
  return prefix_ + std::string{reg};
}

auto tim::optional_field(std::string_view reg, std::string_view bits) const
    -> std::optional<field_ref>
{
  if(not has_register(reg)
     or not std::ranges::contains(describe(reg).fields, bits, &field_desc::name))
  {
    return std::nullopt;
  }
  return field(reg, bits);
}

[[gnu::hot]] auto tim::now_ps() const -> std::uint64_t
{
  return state_.held != 0 ? state_.held_ps : ctx_.clock.time().ps;
}

auto tim::center() const -> bool
{
  return center_mode_ and get(regs_, *center_mode_) != 0;
}

[[gnu::hot]] auto tim::counting_up() const -> bool
{
  if(center())
  {
    return state_.down == 0;
  }
  return not direction_ or get(regs_, *direction_) == 0;
}

auto tim::active() const -> bool
{
  return get(regs_, enable_) != 0 and state_.reload != 0;
}

auto tim::counting() const -> bool
{
  return running() and state_.hz != 0;
}

[[gnu::hot]] auto tim::running() const -> bool
{
  return active() and internally_clocked();
}

auto tim::internally_clocked() const -> bool
{
  auto const mode = slave();
  if(encoding(mode)
     or mode == std::to_underlying(slave_mode::external1)
     or (external_clock2_ and get(regs_, *external_clock2_) != 0))
  {
    return false;
  }
  return gate_open();
}

auto tim::gate_open() const -> bool
{
  return not gating(slave()) or state_.gate != 0;
}

auto tim::counter_enabled() const -> bool
{
  return get(regs_, enable_) != 0 and gate_open();
}

auto tim::input_level(std::uint32_t smcr) const -> std::optional<bool>
{
  if(modelled(slave_of(smcr)) and pin_select(trigger_select_of(smcr)))
  {
    return pin_trigger_level(trigger_select_of(smcr));
  }
  auto const input = input_of(smcr);
  if(not input)
  {
    return std::nullopt;
  }
  auto const  signal = std::format("{}_trgo", *input);
  auto const* source = trigger_source_of(ctx_.peers, signal);
  return source != nullptr ? source->level(signal) : std::nullopt;
}

auto tim::slave_of(std::uint32_t smcr) const -> std::uint32_t
{
  if(not slave_)
  {
    return 0;
  }
  auto const high = slave_high_ ? slave_high_->of(smcr) : 0u;
  return slave_->of(smcr) | (high << slave_->width);
}

auto tim::slave() const -> std::uint32_t
{
  return slave_ ? slave_of(regs_.peek(slave_->offset)) : 0u;
}

auto tim::trigger_select_of(std::uint32_t smcr) const -> std::uint32_t
{
  if(not select_)
  {
    return 0;
  }
  auto const high = select_high_ ? select_high_->of(smcr) : 0u;
  return select_->of(smcr) | (high << select_->width);
}

auto tim::input_of(std::uint32_t smcr) const -> std::optional<std::string_view>
{
  if(not modelled(slave_of(smcr)))
  {
    return std::nullopt;
  }
  auto const found = std::ranges::find(
      internal_triggers, trigger_select_of(smcr),
      &std::pair<std::uint32_t, std::string_view>::first);
  if(found == internal_triggers.end() or found->second == instance_)
  {
    return std::nullopt;
  }
  return found->second;
}

auto tim::flagged_when(bool up) const -> bool
{
  if(not center())
  {
    return true;
  }
  auto const mode = get(regs_, *center_mode_);
  return mode == 3 or (mode == 2) == up;
}

auto tim::anchor() const -> wide
{
  return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
}

auto tim::tick_unit() const -> wide
{
  return (wide{state_.prescaler} + 1) * ps_per_second;
}

auto tim::ticks_until(std::uint64_t ps) const -> std::uint64_t
{
  auto const at    = wide{ps} * state_.hz;
  auto const start = anchor();
  return at > start ? ticks_in(at - start) : 0;
}

auto tim::ticks_in(wide span) const -> std::uint64_t
{
  auto const unit = tick_unit();
  if(unit >> 64 != 0)
  {
    return static_cast<std::uint64_t>(span / unit);
  }
  if(unit_divisor_.value() != static_cast<std::uint64_t>(unit))
  {
    unit_divisor_ = core::divisor{static_cast<std::uint64_t>(unit)};
  }
  return unit_divisor_.quotient(span);
}

[[gnu::hot]] auto tim::ticks_to_wrap() const -> std::uint64_t
{
  std::uint64_t const count  = state_.count;
  std::uint64_t const reload = state_.reload;
  std::uint64_t const top    = mask_;
  if(center())
  {
    if(state_.down != 0)
    {
      return std::max<std::uint64_t>(count, 1);
    }
    return count < reload ? reload - count : top - count + 1;
  }
  if(not counting_up())
  {
    return count + 1;
  }
  return count <= reload ? reload - count + 1 : top - count + 1;
}

auto tim::ticks_to_match(std::size_t ch, std::uint64_t wrap) const -> std::uint64_t
{
  auto const up = counting_up();
  if(not flagged_when(up))
  {
    return wrap;
  }
  std::uint64_t const value = compare_value(ch);
  std::uint64_t const count = state_.count;
  if(up and value > count and value - count < wrap)
  {
    return value - count;
  }
  if(not up and value < count and count - value < wrap)
  {
    return count - value;
  }
  return wrap;
}

auto tim::canonical() const -> bool
{
  if(center())
  {
    return state_.count == 0;
  }
  return state_.count == (counting_up() ? 0 : state_.reload);
}

auto tim::pending() const -> bool
{
  if(state_.prescaler != get(regs_, prescaler_)
     or state_.reload != (regs_.peek(reload_.offset) & mask_))
  {
    return true;
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(state_.compare[ch] != (regs_.peek(channels_[ch].ccr.offset) & mask_))
    {
      return true;
    }
  }
  return false;
}

auto tim::output(std::size_t ch) const -> bool
{
  return get(regs_, channels_[ch].select) == 0;
}

auto tim::compare_value(std::size_t ch) const -> std::uint32_t
{
  auto const& c = channels_[ch];
  if(c.preload and get(regs_, *c.preload) != 0)
  {
    return state_.compare[ch];
  }
  return regs_.peek(c.ccr.offset) & mask_;
}

auto tim::repetition_reload() const -> std::uint32_t
{
  return repetition_ ? get(regs_, *repetition_) : 0;
}

auto tim::wants_update() -> bool
{
  if(get(regs_, no_update_) != 0)
  {
    return false;
  }
  auto const dma   = update_dma_ and get(regs_, *update_dma_) != 0 and state_.update_request == 0;
  auto const pulse = get(regs_, one_pulse_) != 0;
  return get(regs_, update_flag_) == 0 or dma or pulse or trgo_wanted(master_update);
}

auto tim::wants_match(std::size_t ch) -> bool
{
  auto const& c = channels_[ch];
  if(not output(ch) or compare_value(ch) > state_.reload)
  {
    return false;
  }
  auto const dma = c.dma and get(regs_, *c.dma) != 0 and state_.compare_request[ch] == 0;
  return get(regs_, c.flag) == 0 or dma or (ch == 0 and trgo_wanted(master_compare_pulse));
}

auto tim::master_mode() const -> std::uint32_t
{
  return master_ ? master_mode_of(regs_.peek(master_->offset)) : 0u;
}

auto tim::master_mode_of(std::uint32_t cr2) const -> std::uint32_t
{
  if(not master_)
  {
    return 0;
  }
  auto const high = master_high_ ? master_high_->of(cr2) : 0u;
  return master_->of(cr2) | (high << master_->width);
}

auto tim::output_time(std::uint64_t at) const -> std::uint64_t
{
  return state_.held != 0 ? ctx_.clock.time().ps : at;
}

void tim::pulse_trgo(std::uint64_t at)
{
  auto const hz = ctx_.clock.hz(domain_);
  trgo_->pulse(output_time(at), hz == 0 ? 0 : (ps_per_second + hz - 1) / hz);
}

void tim::enable_edge(trigger_edge edge, std::uint64_t at)
{
  if(trgo_ and master_mode() == master_enable)
  {
    trgo_->edge(edge, output_time(at));
  }
}

auto tim::trgo_wanted(std::uint32_t mode) -> bool
{
  return trgo_ and master_mode() == mode and trgo_->wanted();
}

[[gnu::hot]] auto tim::at_ps(wide at) const -> std::uint64_t
{
  if(state_.hz == 0) [[unlikely]]
  {
    return static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz);
  }
  if(hz_divisor_.value() != state_.hz)
  {
    hz_divisor_ = core::divisor{state_.hz};
  }
  return hz_divisor_.quotient(at + state_.hz - 1);
}

void tim::start_at(std::uint64_t ps)
{
  state_.anchor_ps   = ps;
  state_.anchor_frac = 0;
  state_.carry       = 0;
  state_.hz          = ctx_.clock.hz(domain_);
}

void tim::rebase(std::uint64_t ps, std::uint64_t hz)
{
  auto cycles = wide{state_.carry};
  if(state_.hz != 0)
  {
    auto const at = wide{ps} * state_.hz;
    cycles        = at > anchor() ? (at - anchor()) / ps_per_second : 0;
  }
  state_.hz = hz;
  if(hz == 0)
  {
    state_.carry = static_cast<std::uint64_t>(cycles);
    return;
  }
  state_.carry       = 0;
  auto const at      = wide{ps} * hz;
  auto const back    = cycles * ps_per_second;
  auto const start   = at > back ? at - back : 0;
  state_.anchor_ps   = static_cast<std::uint64_t>(start / hz);
  state_.anchor_frac = static_cast<std::uint64_t>(start % hz);
}

void tim::advance(std::uint64_t ticks)
{
  auto const at      = anchor() + wide{ticks} * tick_unit();
  state_.anchor_ps   = static_cast<std::uint64_t>(at / state_.hz);
  state_.anchor_frac = static_cast<std::uint64_t>(at % state_.hz);
}

[[gnu::hot]] void tim::visit(std::uint64_t ticks, std::optional<std::uint64_t> at)
{
  if(ticks == 0)
  {
    return;
  }
  auto const          up    = counting_up();
  std::uint64_t const count = state_.count;
  auto const          low   = up ? count + 1 : count - ticks;
  auto const          high  = up ? count + ticks : count - 1;
  if(flagged_when(up))
  {
    for(std::size_t ch = 0; ch < channels_.size(); ++ch)
    {
      std::uint64_t const value = compare_value(ch);
      if(output(ch) and value >= low and value <= high)
      {
        auto const after = up ? value - count : count - value;
        match(ch, at ? *at : at_ps(anchor() + wide{after} * tick_unit()));
      }
    }
  }
  state_.count = static_cast<std::uint32_t>(up ? count + ticks : count - ticks);
}

void tim::wrap(std::uint64_t at)
{
  auto const up    = counting_up();
  bool       event = true;
  auto       next  = std::uint32_t{};
  if(center())
  {
    if(state_.down != 0)
    {
      state_.down = 0;
    }
    else if(state_.count < state_.reload)
    {
      next        = state_.reload;
      state_.down = 1;
    }
    else
    {
      event = false;
    }
  }
  else if(up and state_.count > state_.reload)
  {
    event = false;
  }
  if(event)
  {
    if(state_.repetition == 0)
    {
      state_.repetition = repetition_reload();
      if(get(regs_, no_update_) == 0)
      {
        update_event(false, at);
      }
    }
    else
    {
      --state_.repetition;
    }
  }
  if(not center() and not up)
  {
    next = state_.reload;
  }
  state_.count = next;
  if(flagged_when(up))
  {
    for(std::size_t ch = 0; ch < channels_.size(); ++ch)
    {
      if(output(ch) and compare_value(ch) == next)
      {
        match(ch, at);
      }
    }
  }
}

void tim::skip_groups(std::uint64_t groups)
{
  auto const          per_group = center() ? 2u : 1u;
  auto const          wraps     = groups * per_group;
  std::uint64_t const left      = state_.repetition;
  std::uint64_t const period    = std::uint64_t{repetition_reload()} + 1;
  auto const          updates   = wraps > left;
  state_.repetition =
      static_cast<std::uint32_t>(updates ? period - 1 - (wraps - left - 1) % period : left - wraps);
  advance(
      groups * (center() ? 2 * std::uint64_t{state_.reload} : state_.reload + std::uint64_t{1}));
  auto const at = at_ps(anchor());
  if(updates and get(regs_, no_update_) == 0)
  {
    update_event(false, at);
  }
  auto const mode = center() ? get(regs_, *center_mode_) : 0;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    auto const value   = compare_value(ch);
    auto const visited = value <= state_.reload
                     and not(mode == 1 and value == state_.reload)
                     and not(mode == 2 and value == 0);
    if(output(ch) and visited)
    {
      match(ch, at);
    }
  }
}

[[gnu::hot]] void tim::catch_up(std::uint64_t ps)
{
  reacting_scope const reacting{reacting_};
  while(counting())
  {
    auto const ticks = ticks_until(ps);
    if(ticks == 0)
    {
      return;
    }
    auto const group =
        center() ? 2 * std::uint64_t{state_.reload} : state_.reload + std::uint64_t{1};
    if(canonical() and ticks >= group)
    {
      auto groups = ticks / group;
      if(get(regs_, no_update_) == 0 and (pending() or get(regs_, one_pulse_) != 0))
      {
        groups = std::min<std::uint64_t>(groups, state_.repetition / (center() ? 2 : 1));
      }
      if(trgo_wanted(master_update) or trgo_wanted(master_compare_pulse))
      {
        groups = 0;
      }
      if(groups > 0)
      {
        skip_groups(groups);
        continue;
      }
    }
    auto const to_wrap = ticks_to_wrap();
    if(ticks < to_wrap)
    {
      visit(ticks);
      advance(ticks);
      return;
    }
    visit(to_wrap - 1);
    advance(to_wrap);
    wrap(at_ps(anchor()));
  }
}

void tim::external_tick(std::uint64_t at)
{
  if(++state_.edges <= state_.prescaler)
  {
    return;
  }
  state_.edges = 0;
  if(ticks_to_wrap() > 1)
  {
    visit(1, at);
  }
  else
  {
    wrap(at);
  }
}

void tim::reinitialise(std::uint64_t at)
{
  if(trgo_ and master_mode() == master_reset)
  {
    pulse_trgo(at);
  }
  if(get(regs_, no_update_) == 0)
  {
    update_event(true, at);
  }
  state_.repetition = repetition_reload();
  state_.down       = 0;
  state_.edges      = 0;
  state_.count      = counting_up() or center() ? 0 : state_.reload;
  start_at(at);
}

void tim::begin(std::uint64_t at)
{
  if(get(regs_, enable_) != 0)
  {
    return;
  }
  auto const was = counter_enabled();
  update(regs_, enable_, 1);
  warn_unsupported();
  start_at(at);
  enable_changed(was, at);
}

void tim::gate(std::optional<bool> level, trigger_edge edge, bool closing_resets, std::uint64_t at)
{
  if(not level and edge == trigger_edge::rising and not std::exchange(gate_logged_, true))
  {
    warn(
        name("SMCR"), std::format(
                          "{}: gated mode needs a master in MMS 001, the counter holds",
                          reflex::enum_value_name(static_cast<slave_mode>(slave()))));
  }
  auto const open = level.value_or(false);
  if(open == (state_.gate != 0))
  {
    return;
  }
  auto const was = counter_enabled();
  state_.gate    = open ? 1 : 0;
  update(regs_, *trigger_flag_, 1);
  if(open)
  {
    start_at(at);
  }
  else if(closing_resets)
  {
    reinitialise(at);
  }
  enable_changed(was, at);
}

void tim::enable_changed(bool was, std::uint64_t at)
{
  auto const now = counter_enabled();
  if(now != was)
  {
    enable_edge(now ? trigger_edge::rising : trigger_edge::falling, at);
  }
}

void tim::update_event(bool software, std::uint64_t at)
{
  state_.prescaler = get(regs_, prescaler_);
  state_.reload    = regs_.peek(reload_.offset) & mask_;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    state_.compare[ch] = regs_.peek(channels_[ch].ccr.offset) & mask_;
  }
  if(not software or get(regs_, update_source_) == 0)
  {
    update(regs_, update_flag_, 1);
    if(update_dma_ and get(regs_, *update_dma_) != 0)
    {
      if(burst_source() == burst_update)
      {
        state_.burst_index = 0;
      }
      request_update();
    }
  }
  if(trgo_ and master_mode() == master_update)
  {
    pulse_trgo(at);
  }
  if(get(regs_, one_pulse_) != 0 and get(regs_, enable_) != 0)
  {
    auto const was = counter_enabled();
    update(regs_, enable_, 0);
    enable_changed(was, at);
  }
}

void tim::match(std::size_t ch, std::uint64_t at)
{
  auto& c = channels_[ch];
  update(regs_, c.flag, 1);
  if(c.dma and get(regs_, *c.dma) != 0 and burst_source() == burst_compare1 + ch)
  {
    state_.burst_index = 0;
  }
  if(c.dma and get(regs_, *c.dma) != 0 and state_.compare_request[ch] == 0)
  {
    state_.compare_request[ch] = 1;
    c.signal->set(true);
  }
  if(ch == 0 and trgo_ and master_mode() == master_compare_pulse)
  {
    pulse_trgo(at);
  }
}

void tim::capture(std::size_t ch, std::uint64_t at)
{
  auto const& c = channels_[ch];
  if(c.overcapture and get(regs_, c.flag) != 0)
  {
    update(regs_, *c.overcapture, 1);
  }
  regs_.poke(c.ccr.offset, (regs_.peek(c.ccr.offset) & ~mask_) | (state_.count & mask_));
  match(ch, at);
}

void tim::request_update()
{
  if(state_.update_request == 0)
  {
    state_.update_request = 1;
    update_signal_->set(true);
  }
}

void tim::release(dma_controller const& by, std::uint32_t request)
{
  auto const                     update = state_.update_request != 0
                                      and update_signal_->carries(by, request)
                                      and not holds_burst(burst_update);
  std::array<bool, max_channels> compare{};
  auto                           served = update;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    compare[ch] = state_.compare_request[ch] != 0
              and channels_[ch].signal->carries(by, request)
              and not holds_burst(burst_compare1 + static_cast<std::uint32_t>(ch));
    served = served or compare[ch];
  }
  if(not served)
  {
    return;
  }
  catch_up(now_ps());
  if(update)
  {
    state_.update_request = 0;
    update_signal_->set(false);
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(compare[ch])
    {
      state_.compare_request[ch] = 0;
      channels_[ch].signal->set(false);
    }
  }
  settle();
}

void tim::acknowledged(dma_controller const& by, std::uint32_t request)
{
  release(by, request);
}

void tim::transfer_completed(dma_controller const& by, std::uint32_t request)
{
  static_cast<void>(by);
  static_cast<void>(request);
}

[[gnu::hot]] void tim::settle()
{
  auto counter = (regs_.peek(counter_.offset) & ~mask_) | state_.count;
  if(copy_)
  {
    counter = copy_->with(counter, get(regs_, remap_) != 0 ? get(regs_, update_flag_) : 0);
  }
  regs_.poke(counter_.offset, counter);
  if(center() and direction_)
  {
    update(regs_, *direction_, state_.down);
  }
  std::array<bool, 4> levels{};
  for(auto const& source : sources_)
  {
    if(get(regs_, source.flag) != 0 and get(regs_, source.enable) != 0)
    {
      levels[source.line] = true;
    }
  }
  for(std::size_t line = 0; line < lines_.size(); ++line)
  {
    if(levels[line])
    {
      lines_[line]->raise();
    }
    else
    {
      lines_[line]->lower();
    }
  }
  reschedule();
}

void tim::reschedule()
{
  if(not counting() or state_.held != 0)
  {
    state_.scheduled = 0;
    return;
  }
  auto const                   to_wrap = ticks_to_wrap();
  std::optional<std::uint64_t> ticks;
  if(wants_update())
  {
    auto const period = center() ? std::uint64_t{state_.reload} : state_.reload + std::uint64_t{1};
    auto const counts = not counting_up()
                     or state_.count < state_.reload
                     or (not center() and state_.count == state_.reload);
    ticks             = to_wrap + (counts ? state_.repetition * period : 0);
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(wants_match(ch))
    {
      ticks = std::min(ticks.value_or(to_wrap), ticks_to_match(ch, to_wrap));
    }
  }
  if(not ticks)
  {
    state_.scheduled = 0;
    return;
  }
  auto const due = std::max(at_ps(anchor() + wide{*ticks} * tick_unit()), now_ps());
  if(state_.scheduled != 0 and state_.due_ps == due)
  {
    return;
  }
  state_.scheduled = 1;
  state_.due_ps    = due;
  ctx_.events.schedule_at(vtime{due}, tick_due, ++state_.generation, ctx_.clock);
}

void tim::warn_unsupported()
{
  warn_inputs();
  auto used =
      std::ranges::any_of(unsupported_, [&](field_ref bits) { return get(regs_, bits) != 0; });
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    used = used or get(regs_, channels_[ch].select) == 3;
  }
  if(used and not std::exchange(unsupported_logged_, true))
  {
    warn(
        name("CR1"),
        "the counter started with SMSPE, a capture on TRC, dithering or CCDS configured: not "
        "modelled, TRC captures nothing, the time base counts on the internal clock");
  }
}

void tim::warn_inputs()
{
  auto const once = [&](bool& logged, bool used, std::string message) {
    if(used and not std::exchange(logged, true))
    {
      warn(name("SMCR"), std::move(message));
    }
  };
  auto const mode   = slave();
  auto const select = slave_ ? trigger_select_of(regs_.peek(slave_->offset)) : 0u;
  once(
      encoder_logged_, encoding(mode) and index_enable_ and get(regs_, *index_enable_) != 0,
      "encoder index, ECR.IE: not modelled, the index input resets nothing");
  once(
      pin_logged_, modelled(mode) and select == 4 and gating(mode),
      "gated mode on TI1F_ED, TS 00100: the edge detector gives pulses, the counter holds");
}

auto tim::input_signal(std::string_view signal) const -> bool
{
  return std::ranges::contains(input_signals_, signal)
      or (not etr_signal_.empty() and signal == etr_signal_);
}

auto tim::pin_level(std::string_view signal) const -> bool
{
  if(not ports_resolved_ and not ctx_.peers.all().empty())
  {
    ports_resolved_ = true;
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto const* const port = dynamic_cast<gpio_port const*>(peer))
      {
        ports_.push_back(port);
      }
    }
  }
  return std::ranges::any_of(
      ports_, [&](gpio_port const* port) { return port->signal_level(signal).value_or(false); });
}

auto tim::raw_input(std::size_t ch) const -> bool
{
  auto const& select = channels_[ch].input_select;
  if(select and get(regs_, *select) != 0)
  {
    return false;
  }
  return pin_level(input_signals_[ch]);
}

auto tim::effective_input(std::size_t ch) const -> bool
{
  if(ch == 0 and ti1_xor_ and get(regs_, *ti1_xor_) != 0)
  {
    auto level = raw_input(0);
    for(std::size_t other = 1; other < std::min<std::size_t>(channels_.size(), 3); ++other)
    {
      level = level != raw_input(other);
    }
    return level;
  }
  return raw_input(ch);
}

auto tim::filtered(std::size_t ch) const -> bool
{
  return ((state_.inputs_filtered >> ch) & 1u) != 0;
}

auto tim::polarised(std::size_t ch) const -> bool
{
  auto const& polarity = channels_[ch].polarity;
  return filtered(ch) != (polarity and get(regs_, *polarity) != 0);
}

void tim::input_edge(std::uint64_t at)
{
  auto const           clocked = ctx_.clock.hz(domain_) != 0;
  auto const           when    = state_.held != 0 ? std::min(at, state_.held_ps) : at;
  reacting_scope const reacting{reacting_};
  if(clocked)
  {
    catch_up(std::min(when, now_ps()));
  }
  inputs_changed(at, clocked);
  settle();
}

void tim::inputs_changed(std::uint64_t at, bool clocked)
{
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    auto const level = effective_input(ch);
    auto const bit   = static_cast<std::uint8_t>(1u << ch);
    if(((state_.inputs_seen & bit) != 0) == level)
    {
      continue;
    }
    state_.inputs_seen ^= bit;
    input_transition(ch, level, at, clocked);
  }
  if(etr_select_)
  {
    auto const level = (get(regs_, *etr_select_) == 0 and pin_level(etr_signal_))
                    != (etr_polarity_ and get(regs_, *etr_polarity_) != 0);
    if(((state_.inputs_seen & etr_bit) != 0) != level)
    {
      state_.inputs_seen ^= etr_bit;
      etr_transition(level, at, clocked);
    }
  }
}

void tim::etr_transition(bool level, std::uint64_t at, bool clocked)
{
  auto const prescaled = (state_.inputs_seen & etrp_bit) != 0;
  auto const divide    = 1u << (etr_prescaler_ ? get(regs_, *etr_prescaler_) : 0u);
  if(divide == 1)
  {
    if(level != prescaled)
    {
      state_.inputs_seen ^= etrp_bit;
      filter_input(etr_input, level, at, clocked);
    }
    return;
  }
  if(not level)
  {
    return;
  }
  if(++state_.etr_edges >= divide)
  {
    state_.etr_edges = 0;
    state_.inputs_seen |= etrp_bit;
    filter_input(etr_input, true, at, clocked);
  }
  else if(state_.etr_edges == divide / 2 and prescaled)
  {
    state_.inputs_seen &= static_cast<std::uint8_t>(~etrp_bit);
    filter_input(etr_input, false, at, clocked);
  }
}

void tim::etr_filtered(bool level, std::uint64_t at, bool clocked)
{
  state_.inputs_filtered = static_cast<std::uint8_t>(
      level ? state_.inputs_filtered | etr_bit : state_.inputs_filtered & ~etr_bit);
  if(not clocked)
  {
    return;
  }
  if(level and external_clock2_ and get(regs_, *external_clock2_) != 0 and active() and gate_open())
  {
    external_tick(at);
  }
  pin_trigger(etr_input, at);
}

auto tim::pin_trigger_level(std::uint32_t select) const -> std::optional<bool>
{
  auto const fp = [&](std::size_t ch) -> std::optional<bool> {
    if(ch >= channels_.size())
    {
      return std::nullopt;
    }
    auto const& c    = channels_[ch];
    auto const  both = c.polarity_n and get(regs_, *c.polarity_n) != 0;
    return both ? filtered(ch) : polarised(ch);
  };
  switch(select)
  {
    case 5:
      return fp(0);
    case 6:
      return fp(1);
    case 7:
      return etr_select_ ? std::optional<bool>{(state_.inputs_filtered & etr_bit) != 0}
                         : std::nullopt;
    default:
      return std::nullopt;
  }
}

void tim::pin_trigger(std::size_t input, std::uint64_t at, bool polarity)
{
  if(not slave_)
  {
    return;
  }
  auto const smcr   = regs_.peek(slave_->offset);
  auto const select = trigger_select_of(smcr);
  if(not modelled(slave_of(smcr)) or not pin_select(select) or (polarity and select == 4))
  {
    return;
  }
  auto const wanted = select == 4 or select == 5 ? std::size_t{0}
                    : select == 6                ? std::size_t{1}
                                                 : etr_input;
  if(input != wanted)
  {
    return;
  }
  auto       edge  = trigger_edge::rising;
  auto const level = pin_trigger_level(select);
  if(select != 4 and level and not *level)
  {
    auto const both = select != 7
                  and channels_[wanted].polarity_n
                  and get(regs_, *channels_[wanted].polarity_n) != 0;
    edge = both ? trigger_edge::rising : trigger_edge::falling;
  }
  slave_edge(edge, select == 4 ? std::optional<bool>{false} : level, at);
}

void tim::input_transition(std::size_t ch, bool level, std::uint64_t at, bool clocked)
{
  filter_input(ch, level, at, clocked);
}

auto tim::input_level_bit(std::size_t input) const -> bool
{
  return input == etr_input ? (state_.inputs_filtered & etr_bit) != 0 : filtered(input);
}

auto tim::filter_delay(std::size_t input) const -> std::uint64_t
{
  auto const& bits = input == etr_input ? etr_filter_ : channels_[input].input_filter;
  auto const  hz   = ctx_.clock.hz(domain_);
  if(not bits or hz == 0)
  {
    return 0;
  }
  auto const filter = input_filters[get(regs_, *bits) & 15u];
  if(filter.samples == 0)
  {
    return 0;
  }
  auto const division = clock_division_ ? std::min(get(regs_, *clock_division_), 2u) : 0u;
  auto const clocks =
      filter.dts_divider == 0 ? std::uint64_t{1} : std::uint64_t{filter.dts_divider} << division;
  auto const total = wide{filter.samples} * clocks * ps_per_second;
  return static_cast<std::uint64_t>((total + hz - 1) / hz);
}

void tim::filter_input(std::size_t input, bool level, std::uint64_t at, bool clocked)
{
  if(state_.input_due_ps[input] != 0)
  {
    ctx_.events.cancel(state_.input_due_seq[input]);
    ++state_.input_generation[input];
    state_.input_due_ps[input] = 0;
  }
  if(level == input_level_bit(input))
  {
    return;
  }
  auto const delay = clocked ? filter_delay(input) : 0;
  if(delay == 0)
  {
    apply_input(input, level, at, clocked);
    return;
  }
  auto const generation       = ++state_.input_generation[input];
  state_.input_due_ps[input]  = at + delay;
  state_.input_due_seq[input] = ctx_.events.schedule_at(
      vtime{at + delay}, input_due, (std::uint64_t{input} << 32) | generation, ctx_.clock);
}

void tim::apply_input(std::size_t input, bool level, std::uint64_t edge_at, bool clocked)
{
  auto const at = state_.held != 0 ? std::min(edge_at, state_.held_ps) : edge_at;
  if(input == etr_input)
  {
    etr_filtered(level, at, clocked);
    return;
  }
  auto const bit         = static_cast<std::uint8_t>(1u << input);
  state_.inputs_filtered = static_cast<std::uint8_t>(
      level ? state_.inputs_filtered | bit : state_.inputs_filtered & ~bit);
  if(not clocked)
  {
    return;
  }
  if(input < 2 and encoding(slave()) and active())
  {
    encoder_edge(input, at);
  }
  pin_trigger(input, at);
  capture_edge(input, level, at);
}

void tim::capture_edge(std::size_t input, bool level, std::uint64_t at)
{
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    auto const& c      = channels_[ch];
    auto const  select = get(regs_, c.select);
    if(select == 0 or select == 3 or (select == 1 ? ch : ch ^ 1u) != input)
    {
      continue;
    }
    if(not c.capture_enable or get(regs_, *c.capture_enable) == 0)
    {
      continue;
    }
    auto const both    = c.polarity_n and get(regs_, *c.polarity_n) != 0;
    auto const falling = c.polarity and get(regs_, *c.polarity) != 0;
    if(not both and level == falling)
    {
      continue;
    }
    capture_count(ch, at);
  }
}

void tim::capture_count(std::size_t ch, std::uint64_t at)
{
  auto const& c     = channels_[ch];
  auto const  every = 1u << (c.capture_prescaler ? get(regs_, *c.capture_prescaler) : 0u);
  if(++state_.capture_edges[ch] < every)
  {
    return;
  }
  state_.capture_edges[ch] = 0;
  capture(ch, at);
}

void tim::polarity_flipped(std::size_t ch, std::uint64_t at)
{
  if(ch < 2 and encoding(slave()) and active())
  {
    encoder_edge(ch, at);
  }
  pin_trigger(ch, at, true);
  auto const& c      = channels_[ch];
  auto const  select = get(regs_, c.select);
  if(select == 0 or select == 3 or not c.capture_enable or get(regs_, *c.capture_enable) == 0)
  {
    return;
  }
  if(c.polarity_n and get(regs_, *c.polarity_n) != 0)
  {
    return;
  }
  auto const source = select == 1 ? ch : ch ^ 1u;
  if(filtered(source) != (c.polarity and get(regs_, *c.polarity) != 0))
  {
    capture_count(ch, at);
  }
}

void tim::encoder_edge(std::size_t ch, std::uint64_t at)
{
  using enum slave_mode;
  auto const fp1      = polarised(0);
  auto const fp2      = polarised(1);
  auto const rising   = ch == 0 ? fp1 : fp2;
  auto const opposite = ch == 0 ? fp2 : fp1;
  auto const up4      = ch == 0 ? rising != opposite : rising == opposite;
  auto const mode     = slave();
  auto       count    = false;
  auto       up       = up4;
  auto       steer    = true;
  switch(mode)
  {
    case std::to_underlying(encoder1):
      count = ch == 0;
      break;
    case std::to_underlying(encoder2):
      count = ch == 1;
      break;
    case std::to_underlying(encoder3):
      count = true;
      break;
    case 0b1010:
      count = ch == 1;
      up    = fp1;
      break;
    case 0b1011:
      count = ch == 1 and rising;
      up    = fp1;
      break;
    case 0b1100:
      count = opposite;
      up    = ch == 1;
      steer = count;
      break;
    case 0b1101:
      count = opposite and not rising;
      up    = ch == 1;
      steer = count;
      break;
    case 0b1110:
      count = ch == 0 and opposite;
      break;
    case 0b1111:
      count = ch == 1 and opposite;
      break;
    default:
      return;
  }
  if(steer and direction_)
  {
    auto const was = get(regs_, *direction_) == 0;
    update(regs_, *direction_, up ? 0 : 1);
    if(was != up and direction_flag_)
    {
      update(regs_, *direction_flag_, 1);
    }
  }
  if(count)
  {
    external_tick(at);
  }
}

void tim::warn_selects()
{
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    auto const& select = channels_[ch].input_select;
    if(select and get(regs_, *select) != 0 and not std::exchange(select_logged_, true))
    {
      warn(
          name("TISEL"),
          std::format(
              "TI{}SEL {}: internal input sources are not modelled, tim_ti{} reads low", ch + 1,
              get(regs_, *select), ch + 1));
    }
  }
  if(etr_select_ and get(regs_, *etr_select_) != 0 and not std::exchange(etr_logged_, true))
  {
    warn(
        name("AF1"), std::format(
                         "ETRSEL {}: internal ETR sources are not modelled, tim_etr_in reads low",
                         get(regs_, *etr_select_)));
  }
}

void tim::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void tim::on_bus_reset()
{
  for(std::size_t input = 0; input < inputs_count; ++input)
  {
    if(state_.input_due_ps[input] != 0)
    {
      ctx_.events.cancel(state_.input_due_seq[input]);
    }
  }
  auto const trgo_high = trgo_ and master_mode() == master_enable and counter_enabled();
  if(update_signal_)
  {
    update_signal_->set(false);
  }
  for(auto& channel : channels_)
  {
    if(channel.signal)
    {
      channel.signal->set(false);
    }
  }
  auto const held    = state_.held;
  auto const held_ps = state_.held_ps;
  auto const slave   = slave_ ? input_of(regs_.peek(slave_->offset)) : std::nullopt;
  restore_reset_values();
  state_.held    = held;
  state_.held_ps = held_ps;
  if(trgo_high)
  {
    trgo_->edge(trigger_edge::falling, output_time(now_ps()));
  }
  if(slave)
  {
    static_cast<void>(stm32::interest_changed(ctx_.peers, std::format("{}_trgo", *slave)));
  }
}

void tim::restore_reset_values()
{
  for(auto const& claimed : claims())
  {
    regs_.poke(reg(claimed).offset, describe(claimed).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  state_.reload         = regs_.peek(reload_.offset) & mask_;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    state_.compare[ch] = regs_.peek(channels_[ch].ccr.offset) & mask_;
    if(channels_[ch].signal)
    {
      channels_[ch].signal->resolve();
    }
  }
  if(update_signal_)
  {
    update_signal_->resolve();
  }
  for(auto* const line : lines_)
  {
    line->lower();
  }
}

[[gnu::hot]] auto
    tim::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
        -> std::expected<std::uint32_t, bus_error>
{
  catch_up(now_ps());
  settle();
  if(not burst_word_ or (offset & ~3u) != *burst_word_)
  {
    auto const value = dispatch_model::on_read(regs, offset, size, access);
    captured_read(offset & ~3u, access);
    return value;
  }
  auto const target = burst_target(offset);
  auto const word   = target & ~3u;
  auto       value  = std::uint32_t{};
  if(claimed(word))
  {
    value = dispatch_model::on_read(regs, target, size, access).value_or(0);
    captured_read(word, access);
  }
  else if(auto const masks = described(word))
  {
    value = read_lanes(regs.peek(word) & masks->second, target, size);
  }
  else
  {
    warn_burst_range(word);
  }
  advance_burst(access);
  return value;
}

void tim::captured_read(std::uint32_t word, access_ctx access)
{
  if(access.by == initiator::debugger)
  {
    return;
  }
  auto cleared = false;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(not output(ch) and word == channels_[ch].ccr.offset)
    {
      update(regs_, channels_[ch].flag, 0);
      cleared = true;
    }
  }
  if(cleared)
  {
    settle();
  }
}

auto tim::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  auto const burst = burst_word_ and (offset & ~3u) == *burst_word_;
  if(burst)
  {
    offset = burst_target(offset);
    if(not claimed(offset & ~3u))
    {
      auto const word = offset & ~3u;
      if(auto const masks = described(word))
      {
        auto const stored = regs.peek(word);
        auto const merged = merge_write(stored, offset, size, value);
        regs.poke(word, (merged & masks->first) | (stored & ~masks->first));
      }
      else
      {
        warn_burst_range(word);
      }
      advance_burst(access);
      return {};
    }
  }
  auto const ps     = now_ps();
  auto const word   = offset & ~3u;
  auto const smcr   = slave_ and word == slave_->offset;
  auto const before = smcr ? input_of(regs.peek(word)) : std::nullopt;
  auto const after =
      smcr ? input_of(merge_write(regs.peek(word), offset, size, value)) : std::nullopt;
  if(before)
  {
    static_cast<void>(stm32::interest_changed(ctx_.peers, std::format("{}_trgo", *before)));
  }
  if(after and after != before)
  {
    static_cast<void>(stm32::interest_changed(ctx_.peers, std::format("{}_trgo", *after)));
  }
  catch_up(ps);
  auto const was            = running();
  auto const enabled        = counter_enabled();
  auto const ccer_write     = capture_word_ and word == *capture_word_;
  auto const ccer_before    = ccer_write ? regs.peek(word) : std::uint32_t{};
  auto const trigger_before = ccer_write and slave_
                                ? pin_trigger_level(trigger_select_of(regs.peek(slave_->offset)))
                                : std::nullopt;
  auto       result         = std::expected<void, bus_error>{};
  {
    reacting_scope const reacting{reacting_};
    result = dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  if(smcr)
  {
    slave_settled(before, regs.peek(word));
    reacting_scope const reacting{reacting_};
    enable_changed(enabled, ps);
  }
  if(not was and running())
  {
    start_at(ps);
  }
  if((select_word_ and word == *select_word_) or (etr_word_ and word == *etr_word_))
  {
    warn_selects();
  }
  if(index_word_ and word == *index_word_)
  {
    warn_inputs();
  }
  if(capture_word_ and word == *capture_word_)
  {
    auto const           clocked = ctx_.clock.hz(domain_) != 0;
    reacting_scope const reacting{reacting_};
    std::uint32_t        flipped = 0;
    for(std::size_t ch = 0; ch < channels_.size(); ++ch)
    {
      auto const& c = channels_[ch];
      if(not c.capture_enable or get(regs_, *c.capture_enable) == 0)
      {
        state_.capture_edges[ch] = 0;
      }
      if(clocked and c.polarity and c.polarity->of(ccer_before) != get(regs_, *c.polarity))
      {
        flipped |= 1u << ch;
        polarity_flipped(ch, ps);
      }
    }
    auto const select = slave_ ? trigger_select_of(regs_.peek(slave_->offset)) : 0u;
    auto const source = select == 5 ? 0u : 1u;
    if(clocked
       and (select == 5 or select == 6)
       and ((flipped >> source) & 1u) == 0
       and pin_trigger_level(select) != trigger_before)
    {
      pin_trigger(source, ps, true);
    }
  }
  if((select_word_ and word == *select_word_)
     or (control2_word_ and word == *control2_word_)
     or (etr_word_ and word == *etr_word_)
     or smcr)
  {
    reacting_scope const reacting{reacting_};
    inputs_changed(ps, ctx_.clock.hz(domain_) != 0);
  }
  if(burst)
  {
    advance_burst(access);
  }
  settle();
  return result;
}

void tim::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == input_due)
  {
    auto const input = static_cast<std::size_t>(payload >> 32);
    if(input >= inputs_count
       or state_.input_due_ps[input] == 0
       or state_.input_generation[input] != static_cast<std::uint32_t>(payload))
    {
      return;
    }
    auto const at              = state_.input_due_ps[input];
    state_.input_due_ps[input] = 0;
    reacting_scope const reacting{reacting_};
    auto const           clocked = ctx_.clock.hz(domain_) != 0;
    if(clocked)
    {
      catch_up(std::min(at, now_ps()));
    }
    apply_input(input, not input_level_bit(input), at, clocked);
    settle();
    return;
  }
  if(kind != tick_due or payload != state_.generation or state_.scheduled == 0)
  {
    return;
  }
  state_.scheduled = 0;
  catch_up(now_ps());
  settle();
}

void tim::on_clock_change()
{
  auto const hz = ctx_.clock.hz(domain_);
  if(hz == state_.hz)
  {
    return;
  }
  auto const ps    = now_ps();
  auto const woken = state_.hz == 0 and hz != 0;
  catch_up(ps);
  rebase(ps, hz);
  state_.scheduled = 0;
  if(woken and gating(slave()))
  {
    auto const was  = counter_enabled();
    auto const open = input_level(regs_.peek(slave_->offset)).value_or(false);
    if(open != (state_.gate != 0))
    {
      state_.gate = open ? 1 : 0;
      if(open)
      {
        start_at(ps);
      }
      else if(slave() == std::to_underlying(slave_mode::gated_reset))
      {
        reinitialise(ps);
      }
    }
    reacting_scope const reacting{reacting_};
    enable_changed(was, ps);
  }
  settle();
}

void tim::on_core_hold(bool held)
{
  if(held)
  {
    if(not debug_frozen(freeze_logged_) or state_.held != 0)
    {
      return;
    }
    catch_up(now_ps());
    settle();
    state_.held_ps   = ctx_.clock.time().ps;
    state_.held      = 1;
    state_.scheduled = 0;
    ++state_.generation;
    return;
  }
  if(state_.held == 0)
  {
    return;
  }
  state_.anchor_ps += ctx_.clock.time().ps - state_.held_ps;
  state_.held    = 0;
  state_.held_ps = 0;
  settle();
}

void tim::interest_changed()
{
  catch_up(now_ps());
  settle();
}

auto tim::produces(std::string_view signal) const -> bool
{
  return trgo_ and trgo_->signal() == signal;
}

auto tim::level(std::string_view signal) const -> std::optional<bool>
{
  if(not produces(signal) or master_mode() != master_enable)
  {
    return std::nullopt;
  }
  return counter_enabled();
}

auto tim::takes_pin(std::string_view signal) const -> bool
{
  return input_signal(signal);
}

auto tim::listens(std::string_view signal) const -> bool
{
  auto const input = slave_ ? input_of(regs_.peek(slave_->offset)) : std::nullopt;
  return input and signal.starts_with(*input) and signal.substr(input->size()) == "_trgo";
}

void tim::triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps)
{
  if(input_signal(signal))
  {
    input_edge(at_ps);
    return;
  }
  if(not listens(signal) or ctx_.clock.hz(domain_) == 0)
  {
    return;
  }
  if(reacting_)
  {
    if(not std::exchange(cycle_logged_, true))
    {
      warn(name("SMCR"), std::format("trigger cycle through {}: edge dropped", desc_.name));
    }
    return;
  }
  reacting_scope const reacting{reacting_};
  auto const           at = state_.held != 0 ? std::min(at_ps, state_.held_ps) : at_ps;
  catch_up(std::min(at, now_ps()));
  auto const* const source = trigger_source_of(ctx_.peers, signal);
  slave_edge(edge, source != nullptr ? source->level(signal) : std::nullopt, at);
  settle();
}

void tim::slave_edge(trigger_edge edge, std::optional<bool> level, std::uint64_t at)
{
  auto const rising = edge == trigger_edge::rising;
  auto const mode   = static_cast<slave_mode>(slave());
  switch(mode)
  {
    case slave_mode::reset:
    case slave_mode::reset_trigger:
    case slave_mode::trigger:
      if(rising)
      {
        if(mode != slave_mode::trigger)
        {
          reinitialise(at);
        }
        if(mode != slave_mode::reset)
        {
          begin(at);
        }
        update(regs_, *trigger_flag_, 1);
      }
      break;
    case slave_mode::external1:
      if(rising)
      {
        update(regs_, *trigger_flag_, 1);
        if(not(external_clock2_ and get(regs_, *external_clock2_) != 0) and active())
        {
          external_tick(at);
        }
      }
      break;
    case slave_mode::gated:
    case slave_mode::gated_reset:
      gate(level, edge, mode == slave_mode::gated_reset, at);
      break;
    default:
      break;
  }
}

void tim::control_written(std::uint32_t old, std::uint32_t now)
{
  if(center_mode_ and center_mode_->of(old) == 0 and center_mode_->of(now) != 0)
  {
    state_.down = 0;
  }
  if(reload_preload_.of(now) == 0)
  {
    state_.reload = regs_.peek(reload_.offset) & mask_;
  }
  if(enable_.of(old) == 0 and enable_.of(now) != 0)
  {
    warn_unsupported();
    enable_changed(false, now_ps());
  }
  if(enable_.of(old) != 0 and enable_.of(now) == 0)
  {
    state_.edges = 0;
    enable_changed(gate_open(), now_ps());
  }
}

void tim::master_written(std::uint32_t old, std::uint32_t now)
{
  auto const mode = master_mode();
  auto const was  = master_mode_of(old);
  if(trgo_ and counter_enabled() and (was == master_enable) != (mode == master_enable))
  {
    trgo_->edge(
        mode == master_enable ? trigger_edge::rising : trigger_edge::falling,
        output_time(now_ps()));
  }
  auto const second = master2_ and master2_->of(now) != 0;
  if((mode > master_compare_pulse or second) and not std::exchange(master_logged_, true))
  {
    warn(
        name("CR2"),
        std::format(
            "MMS {:#06b}{}: master modes above 011 and TRGO2 are not modelled, no trigger "
            "pulses",
            mode, second ? " with MMS2" : ""));
  }
}

void tim::slave_written(std::uint32_t old, std::uint32_t now)
{
  if(slave_of(old) != slave_of(now))
  {
    state_.edges = 0;
  }
}

void tim::slave_settled(std::optional<std::string_view> before, std::uint32_t smcr)
{
  auto const after = input_of(smcr);
  if(before and before != after)
  {
    static_cast<void>(stm32::interest_changed(ctx_.peers, std::format("{}_trgo", *before)));
  }
  auto const signal = after ? std::format("{}_trgo", *after) : std::string{};
  auto const routed = after and stm32::interest_changed(ctx_.peers, signal);
  auto const mode   = slave_of(smcr);
  if(modelled(mode)
     and not routed
     and not(pin_select(trigger_select_of(smcr)) and (trigger_select_of(smcr) != 7 or etr_select_))
     and not std::exchange(unrouted_logged_, true))
  {
    warn(
        name("SMCR"),
        std::format(
            "SMS {:#06b} TS {}: the trigger input is not modelled, no trigger reaches the "
            "slave mode controller",
            mode, trigger_select_of(smcr)));
  }
  warn_inputs();
  state_.gate = gating(mode) and input_level(smcr).value_or(false) ? 1 : 0;
}

void tim::interrupts_written(std::uint32_t, std::uint32_t now)
{
  if(trigger_dma_ and trigger_dma_->of(now) != 0 and not std::exchange(trigger_dma_logged_, true))
  {
    warn(name("DIER"), "TDE: the trigger DMA request is not modelled");
  }
}

void tim::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(status_.offset, old & now);
}

void tim::event_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(events_.offset, 0);
  for(auto const& [generate, flag] : generated_)
  {
    if(generate.of(now) != 0)
    {
      update(regs_, flag, 1);
    }
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(channels_[ch].generate.of(now) == 0)
    {
      continue;
    }
    if(output(ch))
    {
      match(ch, now_ps());
    }
    else
    {
      capture(ch, now_ps());
    }
  }
  if(update_generate_.of(now) != 0)
  {
    reinitialise(now_ps());
  }
}

void tim::counter_written(std::uint32_t, std::uint32_t now)
{
  state_.count = now & mask_;
  if(state_.count == 0)
  {
    state_.down = 0;
  }
  else if(center() and state_.count == state_.reload)
  {
    state_.down = 1;
  }
}

void tim::reload_written(std::uint32_t, std::uint32_t)
{
  if(get(regs_, reload_preload_) == 0)
  {
    state_.reload = regs_.peek(reload_.offset) & mask_;
  }
}

template <std::size_t Ch> void tim::compare_written(std::uint32_t, std::uint32_t)
{
  auto const& c = channels_[Ch];
  if(not c.preload or get(regs_, *c.preload) == 0)
  {
    state_.compare[Ch] = regs_.peek(c.ccr.offset) & mask_;
  }
}

void tim::burst_written(std::uint32_t, std::uint32_t now)
{
  state_.burst_index = 0;
  auto const length  = burst_length_ ? burst_length_->of(now) : 0u;
  if(length > max_burst and not std::exchange(burst_length_logged_, true))
  {
    warn(
        name("DCR"),
        std::format("DBL {:#07b} is reserved: the burst runs {} transfers", length, max_burst));
  }
  auto const source = burst_select_ ? burst_select_->of(now) : 0u;
  auto const used   = length != 0 or (burst_base_ and burst_base_->of(now) != 0) or source != 0;
  auto const modelled_source = source >= burst_update and source <= burst_compare4;
  if(used and not modelled_source and not std::exchange(burst_source_logged_, true))
  {
    warn(
        name("DCR"), std::format(
                         "DBSS {:#06b}: {}, the DMA requests stay one per event", source,
                         source == burst_com       ? "COM burst requests are not modelled"
                         : source == burst_trigger ? "trigger burst requests are not modelled"
                                                   : "reserved burst source"));
  }
}

void tim::plain_written(std::uint32_t, std::uint32_t)
{}
}

auto tim_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<tim>(ctx, desc);
  };
}
}
