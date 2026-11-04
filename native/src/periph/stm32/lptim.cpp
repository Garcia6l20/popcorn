#include "factories.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/core_hold.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

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

constexpr std::uint32_t compare_match    = 1u << 0;
constexpr std::uint32_t reload_match     = 1u << 1;
constexpr std::uint32_t compare_ok       = 1u << 3;
constexpr std::uint32_t external_trigger = 1u << 2;
constexpr std::uint32_t reload_ok        = 1u << 4;
constexpr std::uint32_t counter_top      = 0xFFFF;
constexpr std::uint32_t enable_bit       = 1u << 0;
constexpr std::uint32_t single_start     = 1u << 1;
constexpr std::uint32_t continuous_run   = 1u << 2;
constexpr std::uint32_t tick_event       = 0;
constexpr std::size_t   unmodelled_uses  = 4;
constexpr std::uint8_t  compare_loaded   = 1u << 0;
constexpr std::uint8_t  reload_loaded    = 1u << 1;

enum class locked : std::uint8_t
{
  interrupts,
  configuration,
  option,
};

enum class term : std::uint8_t
{
  compare_written,
  reload_written,
  mode_switch,
  compare,
  reload,
  wrap,
  output,
  trigger,
};

struct lptim_state
{
  std::uint64_t anchor_ps{};
  std::uint64_t anchor_frac{};
  std::uint64_t hz{};
  std::uint64_t enable_ps{};
  std::uint64_t settled_ps{};
  std::uint64_t floor_ps{};
  std::uint64_t compare_held{};
  std::uint64_t reload_held{};
  std::uint64_t wrap_held{};
  std::uint64_t compare_due{};
  std::uint64_t reload_due{};
  std::uint64_t switch_due{};
  std::uint64_t paused_units{};
  std::uint64_t generation{};
  std::uint64_t queued{};
  std::uint64_t out_ps{};
  std::uint64_t trigger_due{};
  std::uint32_t anchor_count{};
  std::uint32_t isr{};
  std::uint32_t armed{};
  std::uint32_t compare{};
  std::uint32_t reload{1};
  std::uint32_t compare_pending{};
  std::uint32_t reload_pending{};
  std::uint32_t compare_preload{};
  std::uint32_t reload_preload{};
  std::uint8_t  running{};
  std::uint8_t  single{};
  std::uint8_t  paused_counting{};
  std::uint8_t  switch_single{};
  std::uint8_t  preloaded{};
  std::uint8_t  out{};
  std::uint8_t  polarity{};
  std::uint8_t  latched{};
  std::uint8_t  started_once{};
  std::uint8_t  held{};
  std::uint8_t  armed_mode{};
  std::uint8_t  trigger_level{};
  std::uint8_t  trigger_rising{};

  friend auto operator==(lptim_state const&, lptim_state const&) -> bool = default;
};

struct unmodelled_use
{
  std::string_view reg;
  std::string_view field;
  std::string_view what;
};

constexpr std::array<unmodelled_use, unmodelled_uses> unmodelled_table{
    {
     {"CFGR", "CKSEL", "CKSEL 1, the clock from input 1"},
     {"CFGR", "COUNTMODE", "COUNTMODE 1, the count of input 1 edges"},
     {"CFGR", "ENC", "ENC 1, the encoder mode"},
     {"OR", "", "OR_0 or OR_1, the COMP1 and COMP2 inputs"},
     }
};

/**
 * @brief LPTIM of the STM32L4, RM0351 section 34, clocked by RCC's `lptim1` or `lptim2` domain
 *        from CCIPR. The 16-bit counter is derived from the kernel clock, the prescaler and the
 *        virtual time of its last anchor, never ticked. CNTSTRT and SNGSTRT written with ENABLE
 *        already 1 start it from 0 at the later of the write plus the variant's start cycles,
 *        RM0351 34.4.6, and ENABLE plus two prescaled clocks, RM0351 34.4.12, the second term and
 *        its prescaled reading inferred. A write setting them with ENABLE 0 is discarded, RM0351
 *        34.4.6, also when the same write sets ENABLE, inferred. They read 0 at once, inferred.
 *        Continuous mode counts 0 to ARR and wraps, one-shot mode stops at 0 at the wrap after
 *        ARR, RM0351 34.4.7. CMPM and ARRM set the variant's event delay after the counter
 *        reaches CMP and ARR, one kernel clock on the STM32L4 by ES0250 2.18.3, also with PRESC
 *        above 0, inferred. CMP above ARR never matches, and a counter above a lowered ARR runs
 *        to 0xFFFF before it wraps, inferred. A CMP or ARR write with ENABLE 1 sets CMPOK or
 *        ARROK, and takes effect, the variant's OK cycles later, 3 kernel clocks inferred:
 *        RM0351 34.4.10 states a latency with no number. Counter ticks up to the OK compare with
 *        the old values, and a match of such a tick still sets its flag at its due after the OK,
 *        inferred. An OK at the one-shot wrap keeps the stop at the wrap, inferred. Terms due at
 *        the same time apply in the order CMPOK, ARROK, CMPM, ARRM, wrap, mode switch,
 *        inferred.
 *        A flag that sets while its IER bit is 1 arms the LPTIM IRQ until ICR clears it, so an
 *        IER bit set after its flag raises nothing, RM0351 34.6. ENABLE written to 0 stops the
 *        counter at 0 and keeps the flags and a pending CMP or ARR write, inferred.
 *        RM0351 34.4.12 and 34.7 allow CFGR, IER and OR writes only with ENABLE 0 and CMP and ARR
 *        writes only with ENABLE 1, and RM0351 34.4.10 a second CMP or ARR write only after its
 *        OK flag. The model keeps the register for a write outside its rule, the second write
 *        excepted, which replaces the pending value: each logs one model_warning and raises
 *        model_error under strict_peripherals, inferred. With PRELOAD 1 and the counter
 *        started, a CMP or ARR value takes effect at the first wrap at or after its OK flag,
 *        RM0351 34.4.10, the wrap being the tick after ARR, inferred. The counter counts as
 *        started from the CNTSTRT or SNGSTRT write, its start latency included, inferred. A
 *        later write waits for its own OK and wrap. ENABLE written to 0 and the one-shot stop
 *        apply such a value at once, inferred. SNGSTRT written while the counter
 *        runs in continuous mode makes it stop at a later wrap, and CNTSTRT written in one-shot
 *        mode makes it go on counting, RM0351 34.4.7. The new mode reaches the counter the
 *        variant's start cycles after the write, inferred, so a wrap before then keeps the old
 *        mode, and a CNTSTRT reaching a counter that the one-shot wrap stopped starts it. The
 *        same mode written again while the counter runs is discarded, inferred. A second
 *        CNTSTRT or SNGSTRT before the first reaches the counter replaces it, logs one
 *        model_warning and raises model_error under strict_peripherals, inferred. A start with
 *        a CFGR or OR function the model lacks, the external clock, the input count, the
 *        encoder, the COMP inputs and the RTC and COMP triggers of TRIGSEL above 0, logs once
 *        each and raises under strict_peripherals: the counter then counts the kernel clock.
 *        With TRIGEN not 00 and TRIGSEL 0 the `<instance>_etr` pin signal starts the counter on
 *        the edges TRIGEN selects, RM0351 34.4.6: CNTSTRT or SNGSTRT arm the start and its mode,
 *        EXTTRIG sets at the edge and the count starts two prescaled clocks later. With TRGFLT
 *        not 00 a level change counts once the pin has held it for 2, 4 or 8 kernel clocks, a
 *        shorter pulse is rejected, RM0351 34.4.4 and 34.7.4, the sampling phase taken as the
 *        edge itself, inferred. An edge during a debug hold or with the kernel clock stopped
 *        starts nothing, and the level it leaves becomes the filtered level, inferred.
 *        LPTIMxRST takes the pin's level as the filtered level, inferred. A trigger while
 *        counting is ignored with no flag, RM0351 34.4.6, or with TIMOUT 1 sets EXTTRIG and
 *        restarts the count from 0 two prescaled clocks later, RM0351 34.4.8, the latency
 *        inferred. After a one-shot stop the next trigger starts a new run, RM0351 34.4.7, only
 *        the first with WAVE 1. A trigger before the arm does nothing, and a trigger term
 *        applies after any other term due at the same time, inferred.
 *        A kernel rate change keeps CNT, the kernel clocks already spent in the current tick or
 *        the start latency, and the kernel clocks left before each pending CMPOK, ARROK, match
 *        delay, mode switch and one-shot stop, inferred. A stopped kernel clock freezes all of
 *        them until it runs again, inferred. LPTIMxRST resets the registers and the state and
 *        withdraws the pending event.
 *        The output drives the `<instance>_out` trigger signal, `lptim1_out` and `lptim2_out`, and
 *        no pin. With WAVE 0 it rises one kernel clock after CNT reaches CMP and falls one kernel
 *        clock after CNT reaches ARR, as ES0250 2.18.3 Figure 3 draws it, with PRESC above 0
 *        inferred, RM0351 34.4.9 reads "exceeds". A one-shot run gives one such pulse, RM0351
 *        34.4.9. With WAVE 1 it rises once and keeps its level until ENABLE is written to 0, and a
 *        start after the first one is discarded until then, RM0351 34.4.7, the software start
 *        reading inferred. CMP at or above ARR keeps it inactive, inferred. WAVPOL inverts it at
 *        the CFGR write, also with ENABLE 0, RM0351 34.4.9. ENABLE written to 0 and LPTIMxRST drop
 *        it to the inactive level at the write, inferred. A sink that starts listening sees the
 *        edges from then on. After a snapshot restore the output asks the sinks again at its first
 *        catch-up. While the machine holds its core with DBG_LPTIM1_STOP or DBG_LPTIM2_STOP
 *        set, its counter clock stops, RM0351 34.4.14, 48.16.4 and 48.16.5, and the model
 *        freezes it as a stopped kernel clock: CNT, the kernel clocks spent in the current tick
 *        and those left before each pending term are kept and resume at the release, the
 *        latencies inferred. A kernel rate change during the hold applies from the release,
 *        inferred. The hold is kept across LPTIMxRST, RM0351 34.4.14. At a machine reset the
 *        LPTIM reads DBG_LPTIMx_STOP again, which a system reset keeps and a power-on reset
 *        clears, RM0351 48.16.4 and 48.16.5.
 *        Deviation: CNT reads exact, without the double-read rule of RM0351 34.7.8.
 */
class lptim final : public dispatch_model<lptim, lptim_state>,
                    public clock_listener,
                    public bus_reset_listener,
                    public core_hold_listener,
                    public trigger_source,
                    public trigger_sink
{
public:
  lptim(model_context& ctx, peripheral_desc const& desc, lptim_variant const& variant);

  void               reset(register_file& regs, reset_kind kind) override;
  void               load(std::span<std::byte const> in) override;
  void               on_bus_reset() override;
  void               on_clock_change() override;
  void               on_core_hold(bool held) override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  [[nodiscard]] auto takes_pin(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
      -> std::expected<std::uint32_t, bus_error> override;

  void                         plain_written(std::uint32_t old, std::uint32_t now);
  void                         clear_written(std::uint32_t old, std::uint32_t now);
  template <locked Which> void locked_written(std::uint32_t old, std::uint32_t now);
  void                         control_written(std::uint32_t old, std::uint32_t now);
  template <term Which> void   value_written(std::uint32_t old, std::uint32_t now);

private:
  struct due_term
  {
    std::uint64_t due;
    term          which;
  };

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto kernel_hz() const -> std::uint64_t;
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto first_wrap() const -> std::uint64_t;
  [[nodiscard]] auto position(std::uint64_t ticks) const -> std::uint32_t;
  [[nodiscard]] auto ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto count_at(std::uint64_t ps) const -> std::uint32_t;
  [[nodiscard]] auto ceil_ps(wide at) const -> std::uint64_t;
  [[nodiscard]] auto ticks_to(std::uint32_t value) const -> std::uint64_t;
  [[nodiscard]] auto match_at(std::uint32_t value) const -> std::optional<wide>;
  [[nodiscard]] auto wave() const -> bool;
  [[nodiscard]] auto raw_at(std::uint64_t ps) const -> bool;
  [[nodiscard]] auto out_due() const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto match_due(std::uint32_t value, std::uint64_t held) const
      -> std::optional<std::uint64_t>;
  [[nodiscard]] auto next_term() const -> std::optional<due_term>;

  void               set_anchor(wide at);
  void               hold_in_flight(std::uint64_t ps);
  void               reanchor(std::uint64_t ps, std::uint64_t ticks);
  void               align_wrap(std::uint64_t ps);
  void               catch_up(std::uint64_t ps);
  void               apply(due_term const& next);
  void               raise_flag(std::uint32_t flag);
  [[nodiscard]] auto preload() const -> bool;
  [[nodiscard]] auto trigger_mode() const -> bool;
  [[nodiscard]] auto etr_level() const -> bool;
  void               start(std::uint64_t ps, bool single);
  void               begin(wide at, std::uint64_t ps, bool single);
  void               switch_mode(std::uint64_t ps, bool single);
  void               transfer_preloaded();
  void               stop();
  void               refuse_unmodelled();
  void               refresh();
  void               reschedule();
  void               rebase(std::uint64_t ps, std::uint64_t hz);
  void               restore_reset_values();
  void               sync_out(std::uint64_t ps);

  lptim_variant                     variant_;
  register_file&                    regs_;
  reg_ref                           isr_;
  reg_ref                           ier_;
  reg_ref                           cfgr_;
  reg_ref                           cr_;
  reg_ref                           cmp_;
  reg_ref                           arr_;
  reg_ref                           cnt_;
  field_ref                         prescaler_;
  field_ref                         preload_;
  field_ref                         wave_;
  field_ref                         polarity_;
  field_ref                         trigger_enable_;
  field_ref                         trigger_select_;
  field_ref                         trigger_filter_;
  field_ref                         timeout_;
  core::clock_domain                kernel_;
  irq_line*                         line_ = nullptr;
  trigger_output                    out_;
  std::string                       etr_;
  std::array<bool, unmodelled_uses> unmodelled_logged_{};
  std::array<bool, 3>               locked_logged_{};
  bool                              value_logged_   = false;
  bool                              pending_logged_ = false;
  bool                              switch_logged_  = false;
  bool                              freeze_logged_  = false;
  bool                              select_logged_  = false;
  std::optional<bool>               listened_;
};

constexpr std::array<write_entry<lptim>, 8> lptim_writes{
    {
     {"ISR", &lptim::plain_written},
     {"ICR", &lptim::clear_written},
     {"IER", &lptim::locked_written<locked::interrupts>},
     {"CFGR", &lptim::locked_written<locked::configuration>},
     {"CR", &lptim::control_written},
     {"CMP", &lptim::value_written<term::compare_written>},
     {"ARR", &lptim::value_written<term::reload_written>},
     {"CNT", &lptim::plain_written},
     }
};

constexpr std::array<write_entry<lptim>, 1> lptim_option_writes{
    {{"OR", &lptim::locked_written<locked::option>}}};

lptim::lptim(model_context& ctx, peripheral_desc const& desc, lptim_variant const& variant)
    : dispatch_model{ctx, desc, "LPTIM", lptim_writes}, variant_{variant}, regs_{ctx.regs},
      isr_{reg("ISR")}, ier_{reg("IER")}, cfgr_{reg("CFGR")}, cr_{reg("CR")}, cmp_{reg("CMP")},
      arr_{reg("ARR")}, cnt_{reg("CNT")}, prescaler_{field("CFGR", "PRESC")},
      preload_{field("CFGR", "PRELOAD")}, wave_{field("CFGR", "WAVE")},
      polarity_{field("CFGR", "WAVPOL")}, trigger_enable_{field("CFGR", "TRIGEN")},
      trigger_select_{field("CFGR", "TRIGSEL")}, trigger_filter_{field("CFGR", "TRGFLT")},
      timeout_{field("CFGR", "TIMOUT")}, kernel_{ctx.clock.domain(lower(desc.name))},
      line_{&irq(desc.name + "_IRQn")}, out_{ctx.peers, lower(desc.name) + "_out"},
      etr_{lower(desc.name) + "_etr"}
{
  expect_register("OR", variant.option_register);
  expect_field("CFGR", "ENC", variant.encoder);
  expect_field("ISR", "UP", variant.encoder);
  expect_field("ISR", "DOWN", variant.encoder);
  expect_field("IER", "UPIE", variant.encoder);
  expect_field("IER", "DOWNIE", variant.encoder);
  expect_field("ICR", "UPCF", variant.encoder);
  expect_field("ICR", "DOWNCF", variant.encoder);
  if(variant.option_register)
  {
    bind(lptim_option_writes);
  }
  state_.reload = describe("ARR").reset;
  state_.hz     = ctx.clock.hz(kernel_);
}

auto lptim::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto lptim::kernel_hz() const -> std::uint64_t
{
  return state_.held != 0 ? 0 : ctx_.clock.hz(kernel_);
}

auto lptim::enabled() const -> bool
{
  return (regs_.peek(cr_.offset) & enable_bit) != 0;
}

auto lptim::anchor() const -> wide
{
  return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
}

auto lptim::tick_unit() const -> wide
{
  return (wide{1} << get(regs_, prescaler_)) * ps_per_second;
}

void lptim::set_anchor(wide at)
{
  state_.anchor_ps   = static_cast<std::uint64_t>(at / state_.hz);
  state_.anchor_frac = static_cast<std::uint64_t>(at % state_.hz);
}

auto lptim::first_wrap() const -> std::uint64_t
{
  auto const from = state_.anchor_count;
  return from <= state_.reload ? state_.reload + 1u - from : counter_top + 1u - from;
}

auto lptim::position(std::uint64_t ticks) const -> std::uint32_t
{
  auto const wrap = first_wrap();
  if(ticks < wrap)
  {
    return static_cast<std::uint32_t>(state_.anchor_count + ticks);
  }
  return static_cast<std::uint32_t>((ticks - wrap) % (std::uint64_t{state_.reload} + 1u));
}

auto lptim::ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>
{
  auto const at = wide{ps} * state_.hz;
  if(state_.running == 0 or state_.hz == 0 or at < anchor())
  {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>((at - anchor()) / tick_unit());
}

auto lptim::count_at(std::uint64_t ps) const -> std::uint32_t
{
  auto const ticks = ticks_at(ps);
  return ticks ? position(*ticks) : state_.anchor_count;
}

auto lptim::ceil_ps(wide at) const -> std::uint64_t
{
  return static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz);
}

auto lptim::ticks_to(std::uint32_t value) const -> std::uint64_t
{
  auto const from   = state_.anchor_count;
  auto const period = std::uint64_t{state_.reload} + 1u;
  return from <= state_.reload ? (value >= from ? value - from : period - from + value)
                               : first_wrap() + value;
}

auto lptim::match_at(std::uint32_t value) const -> std::optional<wide>
{
  if(state_.running == 0 or state_.hz == 0 or value > state_.reload)
  {
    return std::nullopt;
  }
  auto const first = ticks_to(value);
  auto const step  = wide{std::uint64_t{state_.reload} + 1u} * tick_unit();
  auto const floor = wide{state_.floor_ps} * state_.hz;
  auto       tick  = anchor() + wide{first} * tick_unit();
  if(tick <= floor)
  {
    tick += ((floor - tick) / step + 1) * step;
  }
  auto const settled = wide{state_.settled_ps} * state_.hz;
  auto       at      = tick + wide{variant_.event_delay_cycles} * ps_per_second;
  if(at <= settled)
  {
    at += ((settled - at) / step + 1) * step;
  }
  return at;
}

auto lptim::match_due(std::uint32_t value, std::uint64_t held) const -> std::optional<std::uint64_t>
{
  auto const at  = match_at(value);
  auto const due = at ? std::optional{ceil_ps(*at)} : std::nullopt;
  if(held != 0 and (not due or held < *due))
  {
    return held;
  }
  return due;
}

auto lptim::wave() const -> bool
{
  return get(regs_, wave_) != 0;
}

auto lptim::raw_at(std::uint64_t ps) const -> bool
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
    return (state_.out ^ state_.polarity) != 0;
  }
  auto const at    = wide{ps} * state_.hz;
  auto const delay = wide{variant_.event_delay_cycles} * ps_per_second;
  if(at < anchor() + delay and state_.anchor_count == 0)
  {
    return false;
  }
  auto const count =
      at < anchor() + delay
          ? state_.anchor_count - 1
          : position(static_cast<std::uint64_t>((at - delay - anchor()) / tick_unit()));
  return state_.compare <= count and count < state_.reload;
}

auto lptim::out_due() const -> std::optional<std::uint64_t>
{
  if(state_.running == 0 or state_.latched != 0 or not(listened_.value_or(false) or wave()))
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
    auto at = anchor()
            + wide{ticks_to(value)} * tick_unit()
            + wide{variant_.event_delay_cycles} * ps_per_second;
    if(at <= after)
    {
      at += ((after - at) / step + 1) * step;
    }
    best = std::min(best.value_or(ceil_ps(at)), ceil_ps(at));
  }
  return best;
}

void lptim::sync_out(std::uint64_t ps)
{
  auto const raw = raw_at(ps);
  if(raw and wave() and state_.running != 0)
  {
    state_.latched = 1;
  }
  auto const level = static_cast<std::uint8_t>((raw ? 1u : 0u) ^ state_.polarity);
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

auto lptim::next_term() const -> std::optional<due_term>
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
  offer(state_.compare_due, term::compare_written);
  offer(state_.reload_due, term::reload_written);
  if((state_.isr & compare_match) == 0)
  {
    offer(match_due(state_.compare, state_.compare_held), term::compare);
  }
  if((state_.isr & reload_match) == 0)
  {
    offer(match_due(state_.reload, state_.reload_held), term::reload);
  }
  if(state_.running != 0 and (state_.single != 0 or state_.preloaded != 0))
  {
    offer(
        state_.wrap_held != 0 ? state_.wrap_held
                              : ceil_ps(anchor() + wide{first_wrap()} * tick_unit()),
        term::wrap);
  }
  offer(state_.switch_due, term::mode_switch);
  offer(out_due(), term::output);
  offer(state_.trigger_due, term::trigger);
  return best;
}

void lptim::raise_flag(std::uint32_t flag)
{
  state_.isr |= flag;
  if(flag == compare_match)
  {
    state_.compare_held = 0;
  }
  if(flag == reload_match)
  {
    state_.reload_held = 0;
  }
  if((regs_.peek(ier_.offset) & flag) != 0)
  {
    state_.armed |= flag;
  }
}

void lptim::hold_in_flight(std::uint64_t ps)
{
  auto const now   = wide{ps} * state_.hz;
  auto const delay = wide{variant_.event_delay_cycles} * ps_per_second;
  auto const held  = [&](std::uint32_t flag, std::uint32_t value, std::uint64_t& out) {
    if((state_.isr & flag) != 0)
    {
      return;
    }
    if(auto const at = match_at(value); at and *at - delay <= now)
    {
      out = ceil_ps(*at);
    }
  };
  held(compare_match, state_.compare, state_.compare_held);
  held(reload_match, state_.reload, state_.reload_held);
}

void lptim::apply(due_term const& next)
{
  if(next.which == term::output)
  {
    return;
  }
  if(next.which == term::trigger)
  {
    state_.trigger_due   = 0;
    state_.trigger_level = state_.trigger_rising;
    auto const polarity  = get(regs_, trigger_enable_);
    auto const rising    = state_.trigger_rising != 0;
    if((polarity == 1 and not rising) or (polarity == 2 and rising))
    {
      return;
    }
    auto const at = wide{next.due} * state_.hz + 2 * tick_unit();
    if(state_.running != 0)
    {
      if(get(regs_, timeout_) == 0)
      {
        return;
      }
      raise_flag(external_trigger);
      hold_in_flight(next.due);
      begin(at, next.due, state_.single != 0);
      return;
    }
    if(state_.armed_mode == 0 or (wave() and state_.started_once != 0))
    {
      return;
    }
    raise_flag(external_trigger);
    begin(at, next.due, state_.armed_mode == 2);
    state_.started_once = 1;
    return;
  }
  if(next.which == term::compare or next.which == term::reload)
  {
    raise_flag(next.which == term::compare ? compare_match : reload_match);
    return;
  }
  if(next.which == term::wrap)
  {
    if(state_.single != 0)
    {
      stop();
      return;
    }
    set_anchor(anchor() + wide{first_wrap()} * tick_unit());
    state_.anchor_count = 0;
    transfer_preloaded();
    return;
  }
  if(next.which == term::mode_switch)
  {
    state_.switch_due = 0;
    if(trigger_mode())
    {
      state_.armed_mode = state_.switch_single != 0 ? 2 : 1;
    }
    if(state_.running != 0)
    {
      if(auto const ticks = ticks_at(next.due))
      {
        reanchor(next.due, *ticks);
      }
      state_.single = state_.switch_single;
    }
    else if(not trigger_mode())
    {
      if(not(wave() and state_.started_once != 0))
      {
        begin(wide{next.due} * state_.hz, 0, state_.switch_single != 0);
      }
    }
    return;
  }
  auto const compare = next.which == term::compare_written;
  raise_flag(compare ? compare_ok : reload_ok);
  if(state_.running != 0 and preload())
  {
    if(state_.preloaded == 0)
    {
      align_wrap(next.due);
    }
    state_.preloaded |= compare ? compare_loaded : reload_loaded;
    (compare ? state_.compare_preload : state_.reload_preload) =
        compare ? state_.compare_pending : state_.reload_pending;
    (compare ? state_.compare_due : state_.reload_due) = 0;
    return;
  }
  if(auto const ticks = ticks_at(next.due); ticks and state_.single != 0 and *ticks >= first_wrap())
  {
    hold_in_flight(next.due);
    state_.wrap_held = next.due;
    state_.floor_ps  = next.due;
  }
  else if(ticks)
  {
    reanchor(next.due, *ticks);
  }
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

void lptim::align_wrap(std::uint64_t ps)
{
  auto const ticks = ticks_at(ps);
  auto const wrap  = first_wrap();
  if(not ticks or *ticks < wrap)
  {
    return;
  }
  auto const period = std::uint64_t{state_.reload} + 1u;
  auto       count  = (*ticks - wrap) / period;
  if(ceil_ps(anchor() + wide{wrap + count * period} * tick_unit()) < ps)
  {
    ++count;
  }
  if(count == 0)
  {
    return;
  }
  set_anchor(anchor() + wide{wrap + (count - 1) * period} * tick_unit());
  state_.anchor_count = 0;
}

void lptim::reanchor(std::uint64_t ps, std::uint64_t ticks)
{
  hold_in_flight(ps);
  state_.anchor_count = position(ticks);
  state_.floor_ps     = ps;
  set_anchor(anchor() + wide{ticks} * tick_unit());
}

void lptim::catch_up(std::uint64_t ps)
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

auto lptim::preload() const -> bool
{
  return get(regs_, preload_) != 0;
}

void lptim::start(std::uint64_t ps, bool single)
{
  if(state_.hz == 0)
  {
    state_.paused_counting = 0;
    state_.paused_units    = std::uint64_t{variant_.start_cycles} * ps_per_second;
    state_.anchor_count    = 0;
    state_.floor_ps        = ps;
    state_.running         = 1;
    state_.single          = single ? 1 : 0;
    return;
  }
  begin(wide{ps} * state_.hz + wide{variant_.start_cycles} * ps_per_second, ps, single);
}

void lptim::begin(wide at, std::uint64_t ps, bool single)
{
  auto const ready = wide{state_.enable_ps} * state_.hz + 2 * tick_unit();
  set_anchor(std::max(at, ready));
  state_.anchor_count = 0;
  state_.floor_ps     = ps;
  state_.running      = 1;
  state_.single       = single ? 1 : 0;
}

void lptim::switch_mode(std::uint64_t ps, bool single)
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
  auto const cycles = wide{variant_.start_cycles} * ps_per_second;
  state_.switch_due =
      state_.hz == 0 ? static_cast<std::uint64_t>(cycles) : ceil_ps(wide{ps} * state_.hz + cycles);
  state_.switch_single = single ? 1 : 0;
}

void lptim::transfer_preloaded()
{
  if((state_.preloaded & compare_loaded) != 0)
  {
    state_.compare = state_.compare_preload;
  }
  if((state_.preloaded & reload_loaded) != 0)
  {
    state_.reload = state_.reload_preload;
  }
  state_.preloaded = 0;
}

void lptim::stop()
{
  transfer_preloaded();
  state_.running      = 0;
  state_.single       = 0;
  state_.anchor_count = 0;
  state_.compare_held = 0;
  state_.reload_held  = 0;
  state_.wrap_held    = 0;
}

void lptim::refuse_unmodelled()
{
  if(trigger_mode() and get(regs_, trigger_select_) != 0)
  {
    warn_once_or_refuse(
        select_logged_, "CFGR", cfgr_.offset,
        "TRIGSEL not 0 with TRIGEN not 00, the RTC and COMP trigger inputs, at a counter start: "
        "not modelled, no trigger starts the counter");
  }
  for(std::size_t index = 0; index < unmodelled_table.size(); ++index)
  {
    auto const& use = unmodelled_table[index];
    if(not has_register(use.reg) or (not use.field.empty() and not has_field(use.reg, use.field)))
    {
      continue;
    }
    auto const at = reg(use.reg);
    auto const set =
        use.field.empty() ? regs_.peek(at.offset) != 0 : get(regs_, field(use.reg, use.field)) != 0;
    if(set)
    {
      warn_once_or_refuse(
          unmodelled_logged_[index], use.reg, at.offset,
          std::format(
              "{} at a counter start: not modelled, the counter counts the kernel clock",
              use.what));
    }
  }
}

void lptim::refresh()
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

void lptim::reschedule()
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

void lptim::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void lptim::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  listened_.reset();
}

void lptim::on_bus_reset()
{
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
  if(state_.out != 0 and out_.wanted())
  {
    out_.edge(trigger_edge::falling, now_ps());
  }
  auto const held = state_.held;
  restore_reset_values();
  state_.held          = held;
  state_.hz            = kernel_hz();
  state_.trigger_level = etr_level() ? 1 : 0;
}

void lptim::restore_reset_values()
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

void lptim::rebase(std::uint64_t ps, std::uint64_t hz)
{
  auto dues =
      std::array{&state_.compare_due, &state_.reload_due, &state_.compare_held, &state_.reload_held,
                 &state_.wrap_held,   &state_.switch_due, &state_.trigger_due};
  auto const old = state_.hz;
  if(old != 0)
  {
    hold_in_flight(ps);
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
      auto const since       = at - anchor();
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

void lptim::on_clock_change()
{
  auto const hz = kernel_hz();
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

void lptim::on_core_hold(bool held)
{
  if(held)
  {
    if(not debug_frozen(freeze_logged_) or state_.held != 0)
    {
      return;
    }
  }
  else if(state_.held == 0)
  {
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  state_.held = held ? 1 : 0;
  rebase(t, kernel_hz());
  refresh();
  reschedule();
}

void lptim::interest_changed()
{
  catch_up(now_ps());
  refresh();
  listened_ = out_.wanted();
  reschedule();
}

auto lptim::produces(std::string_view signal) const -> bool
{
  return signal == out_.signal();
}

auto lptim::listens(std::string_view) const -> bool
{
  return false;
}

auto lptim::can_listen(std::string_view) const -> bool
{
  return false;
}

auto lptim::takes_pin(std::string_view signal) const -> bool
{
  return signal == etr_;
}

auto lptim::trigger_mode() const -> bool
{
  return get(regs_, trigger_enable_) != 0;
}

auto lptim::etr_level() const -> bool
{
  return std::ranges::any_of(ctx_.peers.all(), [this](model const* peer) {
    auto const* const port = dynamic_cast<gpio_port const*>(peer);
    return port != nullptr and port->signal_level(etr_).value_or(false);
  });
}

void lptim::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  if(signal != etr_)
  {
    return;
  }
  auto const rising = edge == trigger_edge::rising;
  auto const t      = now_ps();
  catch_up(std::min(at.ps, t));
  if(not trigger_mode() or not enabled() or get(regs_, trigger_select_) != 0 or state_.hz == 0)
  {
    state_.trigger_level = rising ? 1 : 0;
    state_.trigger_due   = 0;
    return;
  }
  state_.trigger_due = 0;
  if(rising != (state_.trigger_level != 0))
  {
    auto const code       = get(regs_, trigger_filter_);
    auto const samples    = code == 0 ? 0u : 1u << code;
    state_.trigger_rising = rising ? 1 : 0;
    state_.trigger_due    = std::max(
        ceil_ps(wide{at.ps} * state_.hz + wide{samples} * ps_per_second), state_.settled_ps);
  }
  catch_up(t);
  refresh();
  reschedule();
}

auto lptim::level(std::string_view signal) const -> std::optional<bool>
{
  if(signal != out_.signal())
  {
    return std::nullopt;
  }
  return (static_cast<std::uint8_t>(raw_at(now_ps()) ? 1u : 0u) ^ state_.polarity) != 0;
}

void lptim::plain_written(std::uint32_t, std::uint32_t)
{}

void lptim::clear_written(std::uint32_t, std::uint32_t now)
{
  catch_up(now_ps());
  state_.isr &= ~now;
  state_.armed &= ~now;
  regs_.poke(reg("ICR").offset, 0);
  refresh();
  reschedule();
}

template <locked Which> void lptim::locked_written(std::uint32_t old, std::uint32_t now)
{
  if(Which == locked::configuration and not enabled())
  {
    catch_up(now_ps());
    state_.polarity = static_cast<std::uint8_t>(get(regs_, polarity_));
    refresh();
    reschedule();
    return;
  }
  if(not enabled() or old == now)
  {
    return;
  }
  constexpr auto name = Which == locked::interrupts    ? std::string_view{"IER"}
                      : Which == locked::configuration ? std::string_view{"CFGR"}
                                                       : std::string_view{"OR"};
  auto const     at   = reg(name);
  regs_.poke(at.offset, old);
  warn_once_or_refuse(
      locked_logged_[static_cast<std::size_t>(Which)], name, at.offset,
      std::format("a {} write with CR.ENABLE 1: the register keeps its value", name));
}

void lptim::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const t = now_ps();
  catch_up(t);
  regs_.poke(cr_.offset, now & enable_bit);
  auto const was_on = (old & enable_bit) != 0;
  auto const is_on  = (now & enable_bit) != 0;
  if(not was_on and is_on)
  {
    state_.enable_ps = t;
  }
  else if(was_on and not is_on)
  {
    state_.switch_due   = 0;
    state_.trigger_due  = 0;
    state_.armed_mode   = 0;
    state_.latched      = 0;
    state_.started_once = 0;
    stop();
  }
  auto const starting = now & (single_start | continuous_run);
  if(was_on and is_on and starting != 0)
  {
    auto const single = (starting & continuous_run) == 0;
    if(state_.running != 0 or state_.switch_due != 0)
    {
      switch_mode(t, single);
    }
    else if(trigger_mode())
    {
      refuse_unmodelled();
      state_.armed_mode = single ? 2 : 1;
    }
    else if(not(wave() and state_.started_once != 0))
    {
      refuse_unmodelled();
      start(t, single);
      state_.started_once = 1;
    }
  }
  refresh();
  reschedule();
}

template <term Which> void lptim::value_written(std::uint32_t old, std::uint32_t now)
{
  constexpr auto compare = Which == term::compare_written;
  auto const     at      = compare ? cmp_ : arr_;
  auto const     name    = compare ? std::string_view{"CMP"} : std::string_view{"ARR"};
  if(not enabled())
  {
    regs_.poke(at.offset, old);
    warn_once_or_refuse(
        value_logged_, name, at.offset,
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
        pending_logged_, name, at.offset,
        std::format("a {} write before its OK flag: the new value replaces the pending one", name));
    regs_.poke(at.offset, now);
  }
  (compare ? state_.compare_pending : state_.reload_pending) = now & counter_top;
  auto const cycles = wide{variant_.ok_cycles} * ps_per_second;
  due = state_.hz == 0 ? static_cast<std::uint64_t>(cycles) : ceil_ps(wide{t} * state_.hz + cycles);
  refresh();
  reschedule();
}

auto lptim::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const listened = listened_;
  catch_up(now_ps());
  refresh();
  if(listened_ != listened)
  {
    reschedule();
  }
  return dispatch_model::on_read(regs, offset, size, access);
}

void lptim::on_event(std::uint32_t kind, std::uint64_t payload)
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

auto lptim_factory(lptim_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<lptim>(ctx, desc, variant);
  };
}
}
