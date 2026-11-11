#include "lptim_v2_rows.hpp"
#include "model_base.hpp"
#include "secure_config.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>

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
using wide = unsigned __int128;

constexpr std::uint32_t compare_match = 1u << 0;
constexpr std::uint32_t reload_match  = 1u << 1;
constexpr std::uint32_t compare_ok    = 1u << 3;
constexpr std::uint32_t reload_ok     = 1u << 4;
constexpr std::uint32_t update_event  = 1u << 7;
constexpr std::uint32_t repetition_ok = 1u << 8;
constexpr std::uint32_t interrupts_ok = 1u << 24;
constexpr std::uint32_t modelled_enables =
    compare_match | reload_match | compare_ok | reload_ok | update_event | repetition_ok;
constexpr std::uint32_t counter_top      = 0xFFFF;
constexpr std::uint32_t enable_bit       = 1u << 0;
constexpr std::uint32_t single_start     = 1u << 1;
constexpr std::uint32_t continuous_run   = 1u << 2;
constexpr std::uint32_t counter_reset    = 1u << 3;
constexpr std::uint32_t reset_after_read = 1u << 4;
constexpr std::uint32_t tick_event       = 0;
constexpr std::uint64_t start_cycles     = 3;
constexpr std::uint64_t ok_cycles        = 3;
constexpr std::uint64_t bus_cycles       = 4;
constexpr std::uint64_t reset_cycles     = 3;

enum class term : std::uint8_t
{
  compare_written,
  reload_written,
  interrupts_written,
  repetition_written,
  counter_reset,
  compare,
  reload,
  update_flag,
  update,
  mode_switch,
  output,
};

enum class use : std::uint8_t
{
  external_clock,
  trigger,
  encoder,
  channel_output,
  capture,
  compare2,
  interrupts,
  count,
};

constexpr auto unmodelled_table = std::to_array<std::string_view>({
    "CKSEL 1 or COUNTMODE 1, a count of input 1 edges, at a counter start: not modelled, the "
    "counter counts the kernel clock",
    "TRIGEN not 00, a start by an external trigger, at a CNTSTRT or SNGSTRT write: not modelled, "
    "no trigger starts the counter",
    "ENC 1, the encoder mode, at a counter start: not modelled, the counter counts the "
    "kernel clock up",
    "CC2E 1 with channel 2 as output, the channel 2 PWM output, at a counter start: not "
    "modelled, channel 2 drives no signal",
    "CC1SEL or CC2SEL 1, the input capture mode, at a counter start: not modelled, channel 1 "
    "compares CCR1 and nothing is captured",
    "a CCR2 write, the channel 2 compare: not modelled, CMP2OK and CC2IF never set",
    "a DIER enable of EXTTRIG, UP, DOWN, CC2IF, an over-capture flag or a DMA request: not "
    "modelled, the model never sets these flags or requests",
});
static_assert(unmodelled_table.size() == static_cast<std::size_t>(use::count));

struct lptim_v2_state
{
  std::uint64_t anchor_ps{};
  std::uint64_t anchor_frac{};
  std::uint64_t hz{};
  std::uint64_t enable_ps{};
  std::uint64_t settled_ps{};
  std::uint64_t floor_ps{};
  std::uint64_t compare_due{};
  std::uint64_t update_flag_due{};
  std::uint64_t reload_due{};
  std::uint64_t interrupts_due{};
  std::uint64_t repetition_due{};
  std::uint64_t reset_due{};
  std::uint64_t switch_due{};
  std::uint64_t out_ps{};
  std::uint64_t paused_units{};
  std::uint64_t generation{};
  std::uint64_t queued{};
  std::uint32_t anchor_count{};
  std::uint32_t isr{};
  std::uint32_t armed{};
  std::uint32_t compare{};
  std::uint32_t reload{1};
  std::uint32_t compare_pending{};
  std::uint32_t reload_pending{};
  std::uint32_t interrupts{};
  std::uint32_t interrupts_pending{};
  std::uint32_t repetition{};
  std::uint32_t repetition_reload{};
  std::uint32_t repetition_pending{};
  std::uint32_t compare_preload{};
  std::uint32_t reload_preload{};
  std::uint8_t  preloaded{};
  std::uint8_t  running{};
  std::uint8_t  single{};
  std::uint8_t  paused_counting{};
  std::uint8_t  switch_single{};
  std::uint8_t  raw{};
  std::uint8_t  out{};
  std::uint8_t  latched{};
  std::uint8_t  started_once{};

  friend auto operator==(lptim_v2_state const&, lptim_v2_state const&) -> bool = default;
};

/**
 * @brief LPTIM of the STM32U5, RM0456 section 58, clocked by RCC's `lptim1` to `lptim4` domain
 *        from CCIPR1 and CCIPR3. The 16-bit counter is derived from the kernel clock, the
 *        prescaler PRESC and the virtual time of its last anchor, never ticked. CNTSTRT and
 *        SNGSTRT written with ENABLE already 1 start it from 0 at the later of the write plus 3
 *        kernel clocks, RM0456 58.4.7, and ENABLE plus two prescaled clocks, RM0456 58.4.13, the
 *        prescaled reading inferred. A write setting them with ENABLE 0 is discarded, RM0456
 *        58.4.7, also when the same write sets ENABLE, inferred. They read 0 at once, inferred.
 *        The counter counts 0 to ARR and overflows to 0. The repetition counter, loaded with
 *        REP at the start, inferred, counts the overflows down, and an overflow with it at 0 is
 *        the update event: it transfers the preloaded values and reloads REP, RM0456 58.4.16.
 *        One-shot mode stops at 0 at the update event, RM0456 58.4.8. CC1IF and ARRM set one
 *        counter cycle after the counter reaches CCR1 and ARR, so CC1IF reads CNT CCR1 + 1 and ARRM
 *        reads CNT 0, RM0456 58.4.19. A counter cycle is one prescaled tick. UE sets one counter
 *        cycle after the update event, the instant inferred from the same note, which lists the
 *        update event flag. CCR1 above ARR never matches, CCR1 equal to ARR sets CC1IF with ARRM,
 *        and a counter above a lowered ARR runs to 0xFFFF before it overflows, inferred. A CCR1
 *        value the counter holds at the instant the value takes effect matches one counter cycle
 *        later, a smaller one in the next period, inferred. A match flag in flight when its value
 *        takes effect at or after its match instant and before the flag sets at that instant,
 *        inferred. COUNTRST and a RSTARE read lose a flag in flight. A CCR1 or ARR write
 *        with ENABLE 1 sets CMP1OK or ARROK 3 kernel clocks later, inferred: RM0456 58.4.11 states
 *        a latency with no number. With PRELOAD 0, or with the counter stopped, the value takes
 *        effect at the flag, and counter ticks up to it compare with the old values, inferred.
 *        With PRELOAD 1 and the counter started, from the CNTSTRT or SNGSTRT write on, inferred, it
 *        takes effect at the first update event at or after the flag, RM0456 58.4.11. ENABLE
 *        written to 0 and the one-shot stop apply such a value at once, inferred. An RCR write
 *        with ENABLE 1 sets REPOK 4 APB clocks plus 3 kernel clocks later, RM0456 58.4.16, the
 *        APB clock taken at the write, inferred. The repetition counter takes the new REP at the
 *        first update event at or after REPOK, PRELOAD 0 or 1, RM0456 58.4.16, inferred
 *        for PRELOAD 0, where RM0456 58.4.11 states an immediate RCR update. A flag due
 *        at the instant of an update event applies before it: the update and a one-shot
 *        stop use the old value and the new one counts from then, and a REPOK at that
 *        instant reloads the new REP, inferred. An ARR write clears UE, RM0456 58.7.2.
 *        A DIER write with ENABLE 1 sets DIEROK, and takes effect, 3 kernel clocks
 *        later, inferred: RM0456 58.7.8 states the flag with no latency. Each DIER bit
 *        the write changes then clears its ISR flag, ES0499 2.17.3, at that instant,
 *        inferred. Terms due at the same time apply in the order CC1IF, ARRM, CMP1OK, ARROK,
 *        DIEROK, REPOK, COUNTRST, UE, update event, mode switch, inferred. A match flag due with a
 *        value that takes effect at the same instant belongs to the old value.
 *        COUNTRST written with ENABLE 1 resets CNT 3 kernel clocks later and reads 1 until then,
 *        RM0456 58.4.14 and 58.7.11, the count restarting at that instant with the update event
 *        due then dropped, inferred. A COUNTRST write with ENABLE 0 is discarded, RM0456 58.7.11.
 *        With RSTARE 1 a CPU or DMA read of CNT returns the count and resets CNT, RM0456 58.4.14,
 *        the count restarting at the read, inferred. A debugger read leaves CNT, inferred.
 *        RSTARE written with ENABLE 0 is discarded, RM0456 58.7.11.
 *        A flag that sets while its DIER bit is in effect arms the LPTIM IRQ until ICR clears
 *        it, so a DIER bit set after its flag raises nothing, RM0456 58.6. ENABLE written to 0
 *        stops the counter at 0, keeps the flags and a pending CCR1, ARR, RCR or DIER write and
 *        drops a pending COUNTRST and the repetition count, inferred.
 *        RM0456 58.7.10 allows CFGR writes only with ENABLE 0, and 58.7.8, 58.7.12, 58.7.13 and
 *        58.7.16 DIER, CCR1, ARR and RCR writes only with ENABLE 1, and RM0456 58.4.11, 58.4.16
 *        and 58.7.8 a second CCR1, ARR, RCR or DIER write only after its OK flag. The model keeps
 *        the register for a write outside its rule, inferred, the second write excepted, whose
 *        value replaces the pending one outside strict_peripherals, inferred: each logs one
 *        model_warning and raises model_error under strict_peripherals. CFGR2 is storage: a
 *        firmware write that sets a bit logs one model_warning and raises model_error under
 *        strict_peripherals before it is stored, a write of the reset value 0, as HAL_LPTIM_Init
 *        and DeInit make, passes.
 *        SNGSTRT written while the counter runs in continuous mode makes it stop at a later
 *        update event, and CNTSTRT written in one-shot mode makes it go on counting, RM0456
 *        58.4.8. The new mode reaches the counter 3 kernel clocks after the write, inferred, so
 *        an update event before then keeps the old mode, and a CNTSTRT reaching a counter that
 *        the one-shot stop stopped starts it. The same mode written again while the counter runs
 *        is discarded, inferred. A second CNTSTRT or SNGSTRT before the first reaches the counter
 *        replaces it, logs one model_warning and raises model_error under strict_peripherals,
 *        inferred.
 *        A kernel rate change keeps CNT, the kernel clocks already spent in the current tick or
 *        the start latency, and the kernel clocks left before each pending OK flag, COUNTRST and
 *        mode switch, inferred. A stopped kernel clock freezes all of them until it runs again,
 *        inferred. LPTIMxRST resets the registers and the state and withdraws the pending event.
 *        The output of channel 1, or of LPTIM4, is the trigger signal `lptim<n>_ch1`, or
 *        `lptim4_out`, and drives no pin. OC1REF is high while CNT is above CCR1, so the output
 *        rises at CNT CCR1 + 1 with CC1IF, CCR1 0 at CNT 1, and falls at CNT 0 with ARRM, RM0456
 *        58.4.19 and Figures 747 and 748. A signal edge carries the whole ps at or after its
 *        instant, so a kernel clock that is not a whole ps puts it up to 1 ps late. CCR1 at or
 *        above ARR keeps it inactive, RM0456 58.4.10 requires ARR above CCR1. A CCR1 value that
 *        takes effect moves the level at its CMP1OK, inferred. A one-pulse run gives one pulse.
 *        With WAVE 1 it rises once and keeps its level until ENABLE is written to 0, and a CNTSTRT
 *        or SNGSTRT after the first start is discarded until then, the software restart inferred.
 *        The level is OC1REF xor CC1P bit 0 with CC1E 1 and CC1SEL 0, 0 otherwise, inferred, and
 *        WAVPOL on LPTIM4. A polarity or CC1E change takes effect at the register write, also
 *        with ENABLE 0, RM0456 58.4.10.
 *        ENABLE written to 0 and LPTIMxRST drop the level to inactive at the write, inferred. A
 *        sink that starts listening sees the edges from then on. The functions below
 *        are logged as not modelled: each logs one model_warning at its first use and
 *        raises model_error under strict_peripherals at every use, after the write has
 *        taken the effect stated. At a counter start: CKSEL or COUNTMODE, ENC on an
 *        encoder instance, CC2E with the channel as output and CC1SEL or CC2SEL on a
 *        capture instance, also at a CCMR1 write while the counter runs. At the write:
 *        CNTSTRT or SNGSTRT with TRIGEN not 00, which leaves the counter stopped, CCR2
 *        and a DIER bit other than CC1IE, ARRMIE, CMP1OKIE, ARROKIE, UEIE and REPOKIE.
 *        Deviation: CNT reads exact, without the double-read rule of RM0456 58.7.14.
 */
class lptim_v2 final : public dispatch_model<lptim_v2, lptim_v2_state>,
                       public clock_listener,
                       public bus_reset_listener,
                       public trigger_source
{
public:
  lptim_v2(model_context& ctx, peripheral_desc const& desc, lptim_v2_variant const& variant);

  void               reset(register_file& regs, reset_kind kind) override;
  void               load(std::span<std::byte const> in) override;
  void               on_bus_reset() override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;
  void               on_clock_change() override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     access) -> std::expected<void, bus_error> override;
  [[nodiscard]] auto storage_only(std::uint32_t offset) const -> bool override;

  void                       plain_written(std::uint32_t old, std::uint32_t now);
  void                       clear_written(std::uint32_t old, std::uint32_t now);
  void                       interrupts_written(std::uint32_t old, std::uint32_t now);
  void                       configuration_written(std::uint32_t old, std::uint32_t now);
  void                       control_written(std::uint32_t old, std::uint32_t now);
  void                       channels_written(std::uint32_t old, std::uint32_t now);
  void                       repetition_written(std::uint32_t old, std::uint32_t now);
  void                       compare2_written(std::uint32_t old, std::uint32_t now);
  template <term Which> void value_written(std::uint32_t old, std::uint32_t now);

private:
  struct due_term
  {
    std::uint64_t due;
    term          which;
  };

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto first_wrap() const -> std::uint64_t;
  [[nodiscard]] auto position(std::uint64_t ticks) const -> std::uint32_t;
  [[nodiscard]] auto ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto count_at(std::uint64_t ps) const -> std::uint32_t;
  [[nodiscard]] auto ceil_ps(wide at) const -> std::uint64_t;
  [[nodiscard]] auto ticks_to(std::uint32_t value) const -> std::uint64_t;
  [[nodiscard]] auto match_due(std::uint32_t value) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto next_term() const -> std::optional<due_term>;
  [[nodiscard]] auto in_flight(std::uint32_t value, std::uint64_t ps) const -> bool;
  [[nodiscard]] auto kernel_due(std::uint64_t ps, std::uint64_t cycles) const -> std::uint64_t;
  [[nodiscard]] auto repetition_ok_due(std::uint64_t ps) const -> std::uint64_t;
  [[nodiscard]] auto period() const -> std::uint64_t;
  [[nodiscard]] auto overflows(std::uint64_t ticks) const -> std::uint64_t;
  [[nodiscard]] auto repetition_after(std::uint64_t count) const -> std::uint32_t;
  [[nodiscard]] auto update_ticks() const -> std::uint64_t;
  [[nodiscard]] auto update_due() const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto preload() const -> bool;
  [[nodiscard]] auto wave() const -> bool;
  [[nodiscard]] auto raw_at(std::uint64_t ps) const -> bool;
  [[nodiscard]] auto drive(bool raw) const -> bool;
  [[nodiscard]] auto out_due() const -> std::optional<std::uint64_t>;

  void               set_anchor(wide at);
  void               reanchor(std::uint64_t ps, std::uint64_t ticks);
  [[nodiscard]] auto settle_to(std::uint64_t ps) -> bool;
  void               catch_up(std::uint64_t ps);
  void               apply(due_term const& next);
  void               raise_flag(std::uint32_t flag);
  void               start(std::uint64_t ps, bool single);
  void               begin(wide at, std::uint64_t ps, bool single);
  void               switch_mode(std::uint64_t ps, bool single);
  void               stop();
  void               transfer_preloaded();
  void               restart_count(std::uint64_t ps);
  void               unmodelled(use which, std::string_view reg);
  void               refuse_at_start();
  void               refuse_channels();
  void               sync_out(std::uint64_t ps);
  void               refresh();
  void               reschedule();
  void               rebase(std::uint64_t ps, std::uint64_t hz);
  void               restore_reset_values();

  lptim_v2_variant                          variant_;
  register_file&                            regs_;
  reg_ref                                   isr_;
  reg_ref                                   icr_;
  reg_ref                                   dier_;
  reg_ref                                   cfgr_;
  reg_ref                                   cr_;
  reg_ref                                   ccr1_;
  reg_ref                                   arr_;
  reg_ref                                   cnt_;
  reg_ref                                   inputs_;
  reg_ref                                   rcr_;
  field_ref                                 prescaler_;
  field_ref                                 clock_select_;
  field_ref                                 count_mode_;
  field_ref                                 encoder_;
  field_ref                                 preload_;
  field_ref                                 wave_;
  field_ref                                 polarity_;
  field_ref                                 channel_polarity_;
  field_ref                                 trigger_enable_;
  std::array<field_ref, 2>                  channel_enable_{};
  std::array<field_ref, 2>                  channel_select_{};
  core::clock_domain                        kernel_;
  core::clock_domain                        bus_;
  irq_line*                                 line_ = nullptr;
  trigger_output                            out_;
  std::optional<bool>                       listened_;
  std::array<bool, unmodelled_table.size()> unmodelled_logged_{};
  std::array<bool, 5>                       locked_logged_{};
  std::array<bool, 4>                       pending_logged_{};
  bool                                      switch_logged_ = false;
  storage_notes                             notes_;
};

lptim_v2::lptim_v2(model_context& ctx, peripheral_desc const& desc, lptim_v2_variant const& variant)
    : dispatch_model{ctx, desc, "LPTIM", {}}, variant_{variant}, regs_{ctx.regs},
      isr_{reg(variant.status)}, icr_{reg(variant.clear)}, dier_{reg(variant.interrupts)},
      cfgr_{reg("CFGR")}, cr_{reg("CR")}, ccr1_{reg("CCR1")}, arr_{reg("ARR")}, cnt_{reg("CNT")},
      inputs_{reg("CFGR2")}, rcr_{reg("RCR")}, prescaler_{field("CFGR", "PRESC")},
      clock_select_{field("CFGR", "CKSEL")}, count_mode_{field("CFGR", "COUNTMODE")},
      encoder_{variant.encoder ? field("CFGR", "ENC") : field_ref{}},
      preload_{field("CFGR", "PRELOAD")}, wave_{field("CFGR", "WAVE")},
      polarity_{variant.channels == 0 ? field("CFGR", "WAVPOL") : field_ref{}},
      trigger_enable_{field("CFGR", "TRIGEN")}, kernel_{ctx.clock.domain(lower(desc.name))},
      bus_{ctx.clock.domain(variant.bus_clock)}, line_{&irq(desc.name + "_IRQn")},
      out_{ctx.peers, lower(desc.name) + (variant.channels != 0 ? "_ch1" : "_out")}
{
  auto const writes = std::to_array<write_entry<lptim_v2>>({
      {variant.status,     &lptim_v2::plain_written                       },
      {variant.clear,      &lptim_v2::clear_written                       },
      {variant.interrupts, &lptim_v2::interrupts_written                  },
      {"CFGR",             &lptim_v2::configuration_written               },
      {"CR",               &lptim_v2::control_written                     },
      {"CCR1",             &lptim_v2::value_written<term::compare_written>},
      {"ARR",              &lptim_v2::value_written<term::reload_written> },
      {"CNT",              &lptim_v2::plain_written                       },
      {"CFGR2",            nullptr                                        },
      {"RCR",              &lptim_v2::repetition_written                  },
  });
  bind(writes);
  if(variant.channels != 0)
  {
    auto const channel_writes = std::to_array<write_entry<lptim_v2>>({
        {"CCMR1", &lptim_v2::channels_written},
        {"CCR2",  &lptim_v2::compare2_written},
    });
    bind(channel_writes);
    channel_enable_   = {field("CCMR1", "CC1E"), field("CCMR1", "CC2E")};
    channel_select_   = {field("CCMR1", "CC1SEL"), field("CCMR1", "CC2SEL")};
    channel_polarity_ = field("CCMR1", "CC1P");
  }
  state_.reload = describe("ARR").reset;
  state_.hz     = ctx.clock.hz(kernel_);
}

auto lptim_v2::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto lptim_v2::enabled() const -> bool
{
  return (regs_.peek(cr_.offset) & enable_bit) != 0;
}

auto lptim_v2::anchor() const -> wide
{
  return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
}

auto lptim_v2::tick_unit() const -> wide
{
  return (wide{1} << get(regs_, prescaler_)) * ps_per_second;
}

void lptim_v2::set_anchor(wide at)
{
  state_.anchor_ps   = static_cast<std::uint64_t>(at / state_.hz);
  state_.anchor_frac = static_cast<std::uint64_t>(at % state_.hz);
}

auto lptim_v2::first_wrap() const -> std::uint64_t
{
  auto const from = state_.anchor_count;
  return from <= state_.reload ? state_.reload + 1u - from : counter_top + 1u - from;
}

auto lptim_v2::position(std::uint64_t ticks) const -> std::uint32_t
{
  auto const wrap = first_wrap();
  if(ticks < wrap)
  {
    return static_cast<std::uint32_t>(state_.anchor_count + ticks);
  }
  return static_cast<std::uint32_t>((ticks - wrap) % (std::uint64_t{state_.reload} + 1u));
}

auto lptim_v2::ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>
{
  auto const at = wide{ps} * state_.hz;
  if(state_.running == 0 or state_.hz == 0 or at < anchor())
  {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>((at - anchor()) / tick_unit());
}

auto lptim_v2::count_at(std::uint64_t ps) const -> std::uint32_t
{
  auto const ticks = ticks_at(ps);
  return ticks ? position(*ticks) : state_.anchor_count;
}

auto lptim_v2::ceil_ps(wide at) const -> std::uint64_t
{
  return static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz);
}

auto lptim_v2::ticks_to(std::uint32_t value) const -> std::uint64_t
{
  auto const from   = state_.anchor_count;
  auto const period = std::uint64_t{state_.reload} + 1u;
  return from <= state_.reload ? (value >= from ? value - from : period - from + value)
                               : first_wrap() + value;
}

auto lptim_v2::match_due(std::uint32_t value) const -> std::optional<std::uint64_t>
{
  if(state_.running == 0 or state_.hz == 0 or value > state_.reload)
  {
    return std::nullopt;
  }
  auto const step    = wide{std::uint64_t{state_.reload} + 1u} * tick_unit();
  auto const floor   = wide{state_.floor_ps} * state_.hz;
  auto const settled = wide{state_.settled_ps} * state_.hz;
  auto       at      = anchor() + wide{ticks_to(value) + 1u} * tick_unit();
  if(at <= floor)
  {
    at += ((floor - at) / step + 1) * step;
  }
  if(at <= settled)
  {
    at += ((settled - at) / step + 1) * step;
  }
  return ceil_ps(at);
}

auto lptim_v2::kernel_due(std::uint64_t ps, std::uint64_t cycles) const -> std::uint64_t
{
  auto const units = wide{cycles} * ps_per_second;
  return state_.hz == 0 ? static_cast<std::uint64_t>(units) : ceil_ps(wide{ps} * state_.hz + units);
}

auto lptim_v2::repetition_ok_due(std::uint64_t ps) const -> std::uint64_t
{
  auto const cycles = wide{ok_cycles} * ps_per_second;
  if(state_.hz == 0)
  {
    return static_cast<std::uint64_t>(cycles);
  }
  auto const bus = ctx_.clock.hz(bus_);
  auto const crossed =
      bus == 0
          ? ps
          : ps + static_cast<std::uint64_t>((wide{bus_cycles} * ps_per_second + bus - 1) / bus);
  return ceil_ps(wide{crossed} * state_.hz + cycles);
}

auto lptim_v2::period() const -> std::uint64_t
{
  return std::uint64_t{state_.reload} + 1u;
}

auto lptim_v2::overflows(std::uint64_t ticks) const -> std::uint64_t
{
  auto const wrap = first_wrap();
  return ticks < wrap ? 0 : 1 + (ticks - wrap) / period();
}

auto lptim_v2::repetition_after(std::uint64_t count) const -> std::uint32_t
{
  if(count <= state_.repetition)
  {
    return static_cast<std::uint32_t>(state_.repetition - count);
  }
  auto const cycle = std::uint64_t{state_.repetition_reload} + 1u;
  return static_cast<std::uint32_t>(
      state_.repetition_reload - (count - state_.repetition - 1u) % cycle);
}

auto lptim_v2::update_ticks() const -> std::uint64_t
{
  return first_wrap() + std::uint64_t{state_.repetition} * period();
}

auto lptim_v2::update_due() const -> std::optional<std::uint64_t>
{
  if(state_.running == 0 or state_.hz == 0)
  {
    return std::nullopt;
  }
  auto const step =
      wide{std::uint64_t{state_.repetition_reload} + 1u} * wide{period()} * tick_unit();
  auto const settled = wide{state_.settled_ps} * state_.hz;
  auto       at      = anchor() + wide{update_ticks()} * tick_unit();
  if(at <= settled)
  {
    at += ((settled - at) / step + 1) * step;
  }
  return ceil_ps(at);
}

auto lptim_v2::preload() const -> bool
{
  return get(regs_, preload_) != 0;
}

auto lptim_v2::wave() const -> bool
{
  return get(regs_, wave_) != 0;
}

auto lptim_v2::drive(bool raw) const -> bool
{
  if(variant_.channels == 0)
  {
    return raw != (get(regs_, polarity_) != 0);
  }
  return get(regs_, channel_enable_[0]) != 0
     and get(regs_, channel_select_[0]) == 0
     and raw != ((get(regs_, channel_polarity_) & 1u) != 0);
}

auto lptim_v2::raw_at(std::uint64_t ps) const -> bool
{
  if(state_.latched != 0)
  {
    return true;
  }
  if(state_.running == 0)
  {
    return false;
  }
  if(state_.hz == 0)
  {
    return state_.raw != 0;
  }
  auto const ticks = ticks_at(ps);
  if(not ticks)
  {
    return false;
  }
  auto const count = position(*ticks);
  return state_.compare < count and count <= state_.reload;
}

auto lptim_v2::out_due() const -> std::optional<std::uint64_t>
{
  if(state_.running == 0
     or state_.hz == 0
     or state_.latched != 0
     or not(listened_.value_or(false) or wave()))
  {
    return std::nullopt;
  }
  auto const step  = wide{std::uint64_t{state_.reload} + 1u} * tick_unit();
  auto const after = wide{state_.out_ps} * state_.hz;
  auto       best  = std::optional<std::uint64_t>{};
  for(auto const value : {state_.compare, state_.reload})
  {
    if(value > state_.reload)
    {
      continue;
    }
    auto at = anchor() + wide{ticks_to(value) + 1u} * tick_unit();
    if(at <= after)
    {
      at += ((after - at) / step + 1) * step;
    }
    best = std::min(best.value_or(ceil_ps(at)), ceil_ps(at));
  }
  return best;
}

void lptim_v2::sync_out(std::uint64_t ps)
{
  auto const raw = raw_at(ps);
  if(raw and wave() and state_.running != 0)
  {
    state_.latched = 1;
  }
  state_.raw       = raw ? 1 : 0;
  auto const level = static_cast<std::uint8_t>(drive(raw) ? 1u : 0u);
  if(level != state_.out)
  {
    state_.out = level;
    if(listened_.value_or(false))
    {
      out_.edge(level != 0 ? trigger_edge::rising : trigger_edge::falling, ps);
    }
  }
  state_.out_ps = std::max(state_.out_ps, ps);
}

void lptim_v2::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  listened_.reset();
}

void lptim_v2::interest_changed()
{
  catch_up(now_ps());
  refresh();
  listened_ = out_.wanted();
  reschedule();
}

auto lptim_v2::produces(std::string_view signal) const -> bool
{
  return signal == out_.signal();
}

auto lptim_v2::level(std::string_view signal) const -> std::optional<bool>
{
  if(signal != out_.signal())
  {
    return std::nullopt;
  }
  return drive(raw_at(now_ps()));
}

auto lptim_v2::next_term() const -> std::optional<due_term>
{
  auto best = std::optional<due_term>{};
  if(state_.hz == 0)
  {
    return best;
  }
  auto offer = [&](std::optional<std::uint64_t> due, term which) {
    if(due and *due != 0 and (not best or *due < best->due))
    {
      best = due_term{*due, which};
    }
  };
  if((state_.isr & compare_match) == 0)
  {
    offer(match_due(state_.compare), term::compare);
  }
  if((state_.isr & reload_match) == 0)
  {
    offer(match_due(state_.reload), term::reload);
  }
  offer(state_.compare_due, term::compare_written);
  offer(state_.reload_due, term::reload_written);
  offer(state_.interrupts_due, term::interrupts_written);
  offer(state_.repetition_due, term::repetition_written);
  offer(state_.reset_due, term::counter_reset);
  offer(state_.update_flag_due, term::update_flag);
  if(state_.single != 0 or state_.preloaded != 0 or (state_.isr & update_event) == 0)
  {
    offer(update_due(), term::update);
  }
  offer(state_.switch_due, term::mode_switch);
  offer(out_due(), term::output);
  return best;
}

void lptim_v2::raise_flag(std::uint32_t flag)
{
  state_.isr |= flag;
  if((state_.interrupts & flag) != 0)
  {
    state_.armed |= flag;
  }
}

void lptim_v2::apply(due_term const& next)
{
  if(next.which == term::compare or next.which == term::reload)
  {
    raise_flag(next.which == term::compare ? compare_match : reload_match);
    return;
  }
  if(next.which == term::update)
  {
    if(auto const ticks = ticks_at(next.due))
    {
      reanchor(next.due, *ticks);
    }
    if((state_.isr & update_event) == 0 and state_.update_flag_due == 0)
    {
      state_.update_flag_due = ceil_ps(wide{next.due} * state_.hz + tick_unit());
    }
    transfer_preloaded();
    if(state_.single != 0)
    {
      stop();
    }
    return;
  }
  if(next.which == term::update_flag)
  {
    state_.update_flag_due = 0;
    raise_flag(update_event);
    return;
  }
  if(next.which == term::counter_reset)
  {
    state_.reset_due = 0;
    regs_.poke(cr_.offset, regs_.peek(cr_.offset) & ~counter_reset);
    restart_count(next.due);
    return;
  }
  if(next.which == term::repetition_written)
  {
    state_.repetition_due = 0;
    raise_flag(repetition_ok);
    auto const underflow     = settle_to(next.due);
    state_.repetition_reload = state_.repetition_pending;
    if(underflow and state_.running != 0)
    {
      state_.repetition = state_.repetition_reload;
    }
    return;
  }
  if(next.which == term::output)
  {
    return;
  }
  if(next.which == term::mode_switch)
  {
    state_.switch_due = 0;
    if(state_.running != 0)
    {
      static_cast<void>(settle_to(next.due));
    }
    if(state_.running != 0)
    {
      state_.single = state_.switch_single;
    }
    else if(state_.hz != 0)
    {
      begin(wide{next.due} * state_.hz, 0, state_.switch_single != 0);
    }
    return;
  }
  if(next.which == term::interrupts_written)
  {
    auto const changed    = state_.interrupts ^ state_.interrupts_pending;
    state_.interrupts     = state_.interrupts_pending;
    state_.interrupts_due = 0;
    state_.isr &= ~changed;
    state_.armed &= ~changed;
    raise_flag(interrupts_ok);
    return;
  }
  auto const compare = next.which == term::compare_written;
  raise_flag(compare ? compare_ok : reload_ok);
  if(state_.running != 0 and preload())
  {
    state_.preloaded |= compare ? 1u : 2u;
    (compare ? state_.compare_preload : state_.reload_preload) =
        compare ? state_.compare_pending : state_.reload_pending;
    (compare ? state_.compare_due : state_.reload_due) = 0;
    return;
  }
  auto const flag = compare ? compare_match : reload_match;
  if((state_.isr & flag) == 0 and in_flight(compare ? state_.compare : state_.reload, next.due))
  {
    raise_flag(flag);
  }
  static_cast<void>(settle_to(next.due));
  if(compare)
  {
    state_.compare     = state_.compare_pending;
    state_.compare_due = 0;
  }
  else
  {
    state_.reload     = state_.reload_pending;
    state_.reload_due = 0;
  }
}

auto lptim_v2::in_flight(std::uint32_t value, std::uint64_t ps) const -> bool
{
  auto const due = match_due(value);
  return due and *due > ps and wide{*due} * state_.hz - tick_unit() <= wide{ps} * state_.hz;
}

auto lptim_v2::settle_to(std::uint64_t ps) -> bool
{
  if(update_due() == ps)
  {
    apply(due_term{ps, term::update});
    return true;
  }
  if(auto const ticks = ticks_at(ps))
  {
    reanchor(ps, *ticks);
  }
  return false;
}

void lptim_v2::reanchor(std::uint64_t ps, std::uint64_t ticks)
{
  state_.repetition   = repetition_after(overflows(ticks));
  state_.anchor_count = position(ticks);
  state_.floor_ps     = ps;
  set_anchor(anchor() + wide{ticks} * tick_unit());
}

void lptim_v2::catch_up(std::uint64_t ps)
{
  auto const wanted = out_.wanted();
  listened_         = listened_.value_or(wanted) and wanted;
  while(auto const next = next_term())
  {
    if(next->due > ps)
    {
      break;
    }
    apply(*next);
    sync_out(next->due);
  }
  state_.settled_ps = std::max(state_.settled_ps, ps);
  sync_out(ps);
  listened_ = wanted;
}

void lptim_v2::start(std::uint64_t ps, bool single)
{
  if(state_.hz == 0)
  {
    state_.paused_counting = 0;
    state_.paused_units    = start_cycles * ps_per_second;
    state_.anchor_count    = 0;
    state_.repetition      = state_.repetition_reload;
    state_.floor_ps        = ps;
    state_.running         = 1;
    state_.single          = single ? 1 : 0;
    return;
  }
  begin(wide{ps} * state_.hz + wide{start_cycles} * ps_per_second, ps, single);
}

void lptim_v2::begin(wide at, std::uint64_t ps, bool single)
{
  auto const ready = wide{state_.enable_ps} * state_.hz + 2 * tick_unit();
  set_anchor(std::max(at, ready));
  state_.anchor_count = 0;
  state_.repetition   = state_.repetition_reload;
  state_.floor_ps     = ps;
  state_.running      = 1;
  state_.single       = single ? 1 : 0;
}

void lptim_v2::switch_mode(std::uint64_t ps, bool single)
{
  if(state_.switch_due != 0)
  {
    warn_once_or_refuse(
        switch_logged_, "CR", cr_.offset,
        "a CNTSTRT or SNGSTRT write before the previous one reaches the counter: the later one "
        "replaces it");
  }
  else if(state_.running != 0 and (state_.single != 0) == single)
  {
    return;
  }
  auto const cycles = wide{start_cycles} * ps_per_second;
  state_.switch_due =
      state_.hz == 0 ? static_cast<std::uint64_t>(cycles) : ceil_ps(wide{ps} * state_.hz + cycles);
  state_.switch_single = single ? 1 : 0;
}

void lptim_v2::stop()
{
  transfer_preloaded();
  state_.running      = 0;
  state_.single       = 0;
  state_.anchor_count = 0;
}

void lptim_v2::transfer_preloaded()
{
  if((state_.preloaded & 1u) != 0)
  {
    state_.compare = state_.compare_preload;
  }
  if((state_.preloaded & 2u) != 0)
  {
    state_.reload = state_.reload_preload;
  }
  state_.preloaded = 0;
}

void lptim_v2::restart_count(std::uint64_t ps)
{
  auto const ticks = ticks_at(ps);
  if(not ticks)
  {
    return;
  }
  reanchor(ps, *ticks);
  set_anchor(wide{ps} * state_.hz);
  state_.anchor_count = 0;
}

void lptim_v2::unmodelled(use which, std::string_view reg_name)
{
  auto const index = static_cast<std::size_t>(which);
  warn_once_or_refuse(
      unmodelled_logged_[index], reg_name, reg(reg_name).offset,
      std::string{unmodelled_table[index]});
}

void lptim_v2::refuse_channels()
{
  if(variant_.channels == 0)
  {
    return;
  }
  auto const as_input = [&](std::size_t channel) {
    return variant_.capture and get(regs_, channel_select_[channel]) != 0;
  };
  if(not as_input(1) and get(regs_, channel_enable_[1]) != 0)
  {
    unmodelled(use::channel_output, "CCMR1");
  }
  if(as_input(0) or as_input(1))
  {
    unmodelled(use::capture, "CCMR1");
  }
}

void lptim_v2::refuse_at_start()
{
  if(get(regs_, clock_select_) != 0 or get(regs_, count_mode_) != 0)
  {
    unmodelled(use::external_clock, "CFGR");
  }
  if(variant_.encoder and get(regs_, encoder_) != 0)
  {
    unmodelled(use::encoder, "CFGR");
  }
  refuse_channels();
}

void lptim_v2::refresh()
{
  sync_out(now_ps());
  regs_.poke(isr_.offset, state_.isr);
  regs_.poke(cnt_.offset, count_at(now_ps()));
  if(state_.armed != 0)
  {
    line_->raise();
  }
  else
  {
    line_->lower();
  }
}

void lptim_v2::reschedule()
{
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
  auto const next = next_term();
  if(not next)
  {
    return;
  }
  state_.queued =
      ctx_.events.schedule_at(vtime{next->due}, tick_event, ++state_.generation, ctx_.clock) + 1;
}

void lptim_v2::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void lptim_v2::on_bus_reset()
{
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
  if(state_.out != 0 and out_.wanted())
  {
    out_.edge(trigger_edge::falling, now_ps());
  }
  restore_reset_values();
}

void lptim_v2::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  state_.reload         = describe("ARR").reset;
  state_.hz             = ctx_.clock.hz(kernel_);
  state_.settled_ps     = now_ps();
  line_->lower();
}

void lptim_v2::rebase(std::uint64_t ps, std::uint64_t hz)
{
  auto       dues = std::array{&state_.compare_due,    &state_.reload_due, &state_.interrupts_due,
                               &state_.repetition_due, &state_.reset_due,  &state_.switch_due,
                               &state_.update_flag_due};
  auto const old  = state_.hz;
  if(old != 0)
  {
    auto const at = wide{ps} * old;
    for(auto* const due : dues)
    {
      if(*due != 0)
      {
        *due = static_cast<std::uint64_t>(std::max(wide{*due} * old, at) - at);
      }
    }
    state_.paused_counting = 0;
    state_.paused_units    = 0;
    if(state_.running != 0 and at >= anchor())
    {
      auto const since = at - anchor();
      state_.repetition =
          repetition_after(overflows(static_cast<std::uint64_t>(since / tick_unit())));
      state_.anchor_count    = position(static_cast<std::uint64_t>(since / tick_unit()));
      state_.paused_counting = 1;
      state_.paused_units    = static_cast<std::uint64_t>(since % tick_unit());
    }
    else if(state_.running != 0)
    {
      state_.paused_units = static_cast<std::uint64_t>(anchor() - at);
    }
  }
  state_.hz = hz;
  if(hz == 0)
  {
    return;
  }
  auto const at = wide{ps} * hz;
  for(auto* const due : dues)
  {
    if(*due != 0)
    {
      *due = std::max<std::uint64_t>(ceil_ps(at + *due), ps + 1);
    }
  }
  if(state_.running != 0)
  {
    auto const units = wide{state_.paused_units};
    set_anchor(state_.paused_counting != 0 ? (at > units ? at - units : 0) : at + units);
    if(state_.paused_counting != 0)
    {
      state_.floor_ps = ps;
    }
  }
  state_.paused_counting = 0;
  state_.paused_units    = 0;
}

void lptim_v2::on_clock_change()
{
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == state_.hz)
  {
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  rebase(t, hz);
  refresh();
  reschedule();
}

void lptim_v2::plain_written(std::uint32_t, std::uint32_t)
{}

void lptim_v2::clear_written(std::uint32_t, std::uint32_t now)
{
  catch_up(now_ps());
  state_.isr &= ~now;
  state_.armed &= ~now;
  regs_.poke(icr_.offset, 0);
  refresh();
  reschedule();
}

void lptim_v2::interrupts_written(std::uint32_t old, std::uint32_t now)
{
  if(not enabled())
  {
    regs_.poke(dier_.offset, old);
    warn_once_or_refuse(
        locked_logged_[0], variant_.interrupts, dier_.offset,
        "a DIER write with CR.ENABLE 0: the register keeps its value");
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  if(state_.interrupts_due != 0)
  {
    regs_.poke(dier_.offset, old);
    warn_once_or_refuse(
        pending_logged_[0], variant_.interrupts, dier_.offset,
        "a DIER write before DIEROK: the new value replaces the pending one");
    regs_.poke(dier_.offset, now);
  }
  state_.interrupts_pending = now;
  state_.interrupts_due     = kernel_due(t, ok_cycles);
  refresh();
  reschedule();
  if((now & ~modelled_enables) != 0)
  {
    unmodelled(use::interrupts, variant_.interrupts);
  }
}

void lptim_v2::configuration_written(std::uint32_t old, std::uint32_t now)
{
  if(not enabled())
  {
    if(old != now)
    {
      refresh();
      reschedule();
    }
    return;
  }
  if(old == now)
  {
    return;
  }
  regs_.poke(cfgr_.offset, old);
  warn_once_or_refuse(
      locked_logged_[1], "CFGR", cfgr_.offset,
      "a CFGR write with CR.ENABLE 1: the register keeps its value");
}

void lptim_v2::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const t = now_ps();
  catch_up(t);
  auto const was_on         = (old & enable_bit) != 0;
  auto const is_on          = (now & enable_bit) != 0;
  auto const rstare_allowed = was_on ? reset_after_read : (old & reset_after_read);
  if(was_on and is_on and (now & counter_reset) != 0 and state_.reset_due == 0)
  {
    state_.reset_due = kernel_due(t, reset_cycles);
  }
  if(not was_on and is_on)
  {
    state_.enable_ps = t;
  }
  else if(was_on and not is_on)
  {
    state_.switch_due   = 0;
    state_.reset_due    = 0;
    state_.latched      = 0;
    state_.started_once = 0;
    stop();
  }
  regs_.poke(
      cr_.offset,
      (now & (enable_bit | rstare_allowed)) | (state_.reset_due != 0 ? counter_reset : 0u));
  auto const starting = now & (single_start | continuous_run);
  auto       started  = false;
  auto       armed    = false;
  if(was_on and is_on and starting != 0)
  {
    auto const single = (starting & continuous_run) == 0;
    if(state_.running != 0 or state_.switch_due != 0)
    {
      switch_mode(t, single);
    }
    else if(get(regs_, trigger_enable_) != 0)
    {
      armed = true;
    }
    else if(not(wave() and state_.started_once != 0))
    {
      start(t, single);
      state_.started_once = 1;
      started             = true;
    }
  }
  refresh();
  reschedule();
  if(armed)
  {
    unmodelled(use::trigger, "CFGR");
  }
  if(started)
  {
    refuse_at_start();
  }
}

void lptim_v2::channels_written(std::uint32_t old, std::uint32_t now)
{
  auto const at = reg("CCMR1").offset;
  regs_.poke(at, old);
  catch_up(now_ps());
  regs_.poke(at, now);
  refresh();
  reschedule();
  if(state_.running != 0)
  {
    refuse_channels();
  }
}

void lptim_v2::repetition_written(std::uint32_t old, std::uint32_t now)
{
  if(not enabled())
  {
    regs_.poke(rcr_.offset, old);
    warn_once_or_refuse(
        locked_logged_[4], "RCR", rcr_.offset,
        "a RCR write with CR.ENABLE 0: the register keeps its value");
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  if(state_.repetition_due != 0)
  {
    regs_.poke(rcr_.offset, old);
    warn_once_or_refuse(
        pending_logged_[3], "RCR", rcr_.offset,
        "a RCR write before REPOK: the new value replaces the pending one");
    regs_.poke(rcr_.offset, now);
  }
  state_.repetition_pending = now & 0xFFu;
  state_.repetition_due     = repetition_ok_due(t);
  refresh();
  reschedule();
}

void lptim_v2::compare2_written(std::uint32_t, std::uint32_t)
{
  unmodelled(use::compare2, "CCR2");
}

template <term Which> void lptim_v2::value_written(std::uint32_t old, std::uint32_t now)
{
  constexpr auto compare = Which == term::compare_written;
  auto const     at      = compare ? ccr1_ : arr_;
  auto const     name    = compare ? std::string_view{"CCR1"} : std::string_view{"ARR"};
  auto const     slot    = compare ? 1uz : 2uz;
  if(not compare)
  {
    catch_up(now_ps());
    state_.isr &= ~update_event;
    state_.armed &= ~update_event;
    refresh();
    reschedule();
  }
  if(not enabled())
  {
    regs_.poke(at.offset, old);
    warn_once_or_refuse(
        locked_logged_[slot + 1], name, at.offset,
        std::format("a {} write with CR.ENABLE 0: the register keeps its value", name));
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  auto& due = compare ? state_.compare_due : state_.reload_due;
  if(due != 0)
  {
    regs_.poke(at.offset, old);
    warn_once_or_refuse(
        pending_logged_[slot], name, at.offset,
        std::format("a {} write before its OK flag: the new value replaces the pending one", name));
    regs_.poke(at.offset, now);
  }
  (compare ? state_.compare_pending : state_.reload_pending) = now & counter_top;
  due                                                        = kernel_due(t, ok_cycles);
  refresh();
  reschedule();
}

auto lptim_v2::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     access) -> std::expected<std::uint32_t, bus_error>
{
  auto const t = now_ps();
  catch_up(t);
  refresh();
  auto const value = dispatch_model::on_read(regs, offset, size, access);
  if((offset & ~3u) == cnt_.offset
     and access.by != initiator::debugger
     and (regs_.peek(cr_.offset) & reset_after_read) != 0
     and state_.running != 0)
  {
    restart_count(t);
    refresh();
    reschedule();
  }
  return value;
}

auto lptim_v2::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) == inputs_.offset
     and merge_write(regs.peek(inputs_.offset), offset, size, value) != describe("CFGR2").reset)
  {
    notes_.written(ctx_, desc_, "LPTIM input multiplexer", inputs_.offset, access, {});
  }
  return dispatch_model::on_write(regs, offset, size, old, value, access);
}

auto lptim_v2::storage_only(std::uint32_t offset) const -> bool
{
  return offset == inputs_.offset;
}

void lptim_v2::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != tick_event or payload != state_.generation)
  {
    return;
  }
  state_.queued = 0;
  catch_up(now_ps());
  refresh();
  reschedule();
}
}

auto lptim_v2_factory(lptim_v2_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<lptim_v2>(ctx, desc, variant);
  };
}

namespace
{
constexpr auto lptim1_instance = std::to_array<std::string_view>({"LPTIM1"});
constexpr auto lptim2_instance = std::to_array<std::string_view>({"LPTIM2"});
constexpr auto lptim3_instance = std::to_array<std::string_view>({"LPTIM3"});
constexpr auto lptim4_instance = std::to_array<std::string_view>({"LPTIM4"});
constexpr auto lptim_v2_table  = std::to_array<builtin>({
    row_of<^^lptim_v2_factory, ^^u5_lptim1>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim1_instance,
         .families        = family::u5}),
    row_of<^^lptim_v2_factory, ^^u5_lptim2>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim2_instance,
         .families        = family::u5}),
    row_of<^^lptim_v2_factory, ^^u5_lptim3>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim3_instance,
         .families        = family::u5}),
    row_of<^^lptim_v2_factory, ^^u5_lptim4>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim4_instance,
         .families        = family::u5}),
});
}

auto lptim_v2_rows() -> std::span<builtin const>
{
  return lptim_v2_table;
}
}
