#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint32_t compare_match   = 1u << 0;
constexpr std::uint32_t reload_match    = 1u << 1;
constexpr std::uint32_t compare_ok      = 1u << 3;
constexpr std::uint32_t reload_ok       = 1u << 4;
constexpr std::uint32_t counter_top     = 0xFFFF;
constexpr std::uint64_t frozen          = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t enable_bit      = 1u << 0;
constexpr std::uint32_t single_start    = 1u << 1;
constexpr std::uint32_t continuous_run  = 1u << 2;
constexpr std::uint32_t tick_event      = 0;
constexpr std::size_t   unmodelled_uses = 8;

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
  compare,
  reload,
  wrap,
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
  std::uint64_t generation{};
  std::uint64_t queued{};
  std::uint32_t anchor_count{};
  std::uint32_t isr{};
  std::uint32_t armed{};
  std::uint32_t compare{};
  std::uint32_t reload{1};
  std::uint32_t compare_pending{};
  std::uint32_t reload_pending{};
  std::uint8_t  running{};
  std::uint8_t  single{};

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
     {"CFGR", "TRIGEN", "TRIGEN not 00, the start on an external trigger edge"},
     {"CFGR", "CKSEL", "CKSEL 1, the clock from input 1"},
     {"CFGR", "COUNTMODE", "COUNTMODE 1, the count of input 1 edges"},
     {"CFGR", "TIMOUT", "TIMOUT 1, the timeout restart"},
     {"CFGR", "WAVE", "WAVE 1, the set-once mode"},
     {"CFGR", "PRELOAD", "PRELOAD 1, CMP and ARR applied at the period end"},
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
 *        the same time apply in the order CMPOK, ARROK, CMPM, ARRM, wrap, inferred.
 *        A flag that sets while its IER bit is 1 arms the LPTIM IRQ until ICR clears it, so an
 *        IER bit set after its flag raises nothing, RM0351 34.6. ENABLE written to 0 stops the
 *        counter at 0 and keeps the flags and a pending CMP or ARR write, inferred.
 *        RM0351 34.4.12 and 34.7 allow CFGR, IER and OR writes only with ENABLE 0 and CMP and ARR
 *        writes only with ENABLE 1, and RM0351 34.4.10 a second CMP or ARR write only after its
 *        OK flag. The model keeps the register for a write outside its rule, the second write
 *        excepted, which replaces the pending value: each logs one model_warning and raises
 *        model_error under strict_peripherals, inferred. CNTSTRT or SNGSTRT while the counter
 *        runs, and a start with a CFGR or OR function the model lacks, the external trigger,
 *        the external clock, the input count, the timeout, set-once, PRELOAD 1, the encoder and
 *        the COMP inputs, log once each and raise under strict_peripherals: the counter then
 *        keeps its mode, counts the kernel clock and applies CMP and ARR at their OK flags.
 *        Deviations: CNT reads exact, without the double-read rule of RM0351 34.7.8. The kernel
 *        rate is read at ENABLE and at each start.
 */
class lptim final : public dispatch_model<lptim, lptim_state>
{
public:
  lptim(model_context& ctx, peripheral_desc const& desc, lptim_variant const& variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
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
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto first_wrap() const -> std::uint64_t;
  [[nodiscard]] auto position(std::uint64_t ticks) const -> std::uint32_t;
  [[nodiscard]] auto ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto count_at(std::uint64_t ps) const -> std::uint32_t;
  [[nodiscard]] auto ceil_ps(wide at) const -> std::uint64_t;
  [[nodiscard]] auto match_at(std::uint32_t value) const -> std::optional<wide>;
  [[nodiscard]] auto match_due(std::uint32_t value, std::uint64_t held) const
      -> std::optional<std::uint64_t>;
  [[nodiscard]] auto next_term() const -> std::optional<due_term>;

  void set_anchor(wide at);
  void hold_in_flight(std::uint64_t ps);
  void catch_up(std::uint64_t ps);
  void apply(due_term const& next);
  void raise_flag(std::uint32_t flag);
  void start(std::uint64_t ps, bool single);
  void stop();
  void refuse_unmodelled();
  void refresh();
  void reschedule();

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
  core::clock_domain                kernel_;
  irq_line*                         line_ = nullptr;
  std::array<bool, unmodelled_uses> unmodelled_logged_{};
  std::array<bool, 3>               locked_logged_{};
  bool                              value_logged_   = false;
  bool                              pending_logged_ = false;
  bool                              restart_logged_ = false;
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
      kernel_{ctx.clock.domain(lower(desc.name))}, line_{&irq(desc.name + "_IRQn")}
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
}

auto lptim::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
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

auto lptim::match_at(std::uint32_t value) const -> std::optional<wide>
{
  if(state_.running == 0 or state_.hz == 0 or value > state_.reload)
  {
    return std::nullopt;
  }
  auto const from   = state_.anchor_count;
  auto const period = std::uint64_t{state_.reload} + 1u;
  auto const first  = from <= state_.reload ? (value >= from ? value - from : period - from + value)
                                            : first_wrap() + value;
  auto const step   = wide{period} * tick_unit();
  auto const floor  = wide{state_.floor_ps} * state_.hz;
  auto       tick   = anchor() + wide{first} * tick_unit();
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

auto lptim::next_term() const -> std::optional<due_term>
{
  auto best  = std::optional<due_term>{};
  auto offer = [&](std::optional<std::uint64_t> due, term which) {
    if(due and *due != 0 and *due != frozen and (not best or *due < best->due))
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
  if(state_.single != 0 and state_.running != 0 and state_.hz != 0)
  {
    offer(
        state_.wrap_held != 0 ? state_.wrap_held
                              : ceil_ps(anchor() + wide{first_wrap()} * tick_unit()),
        term::wrap);
  }
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
  if(next.which == term::compare or next.which == term::reload)
  {
    raise_flag(next.which == term::compare ? compare_match : reload_match);
    return;
  }
  if(next.which == term::wrap)
  {
    stop();
    return;
  }
  auto const compare = next.which == term::compare_written;
  raise_flag(compare ? compare_ok : reload_ok);
  if(auto const ticks = ticks_at(next.due); ticks and state_.single != 0 and *ticks >= first_wrap())
  {
    hold_in_flight(next.due);
    state_.wrap_held = next.due;
    state_.floor_ps  = next.due;
  }
  else if(ticks)
  {
    hold_in_flight(next.due);
    state_.anchor_count = position(*ticks);
    state_.floor_ps     = next.due;
    set_anchor(anchor() + wide{*ticks} * tick_unit());
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

void lptim::catch_up(std::uint64_t ps)
{
  while(auto const next = next_term())
  {
    if(next->due > ps)
    {
      break;
    }
    apply(*next);
  }
  state_.settled_ps = std::max(state_.settled_ps, ps);
}

void lptim::start(std::uint64_t ps, bool single)
{
  state_.hz = ctx_.clock.hz(kernel_);
  if(state_.hz == 0)
  {
    state_.anchor_ps   = ps;
    state_.anchor_frac = 0;
  }
  else
  {
    auto const written = wide{ps} * state_.hz + wide{variant_.start_cycles} * ps_per_second;
    auto const ready   = wide{state_.enable_ps} * state_.hz + 2 * tick_unit();
    set_anchor(std::max(written, ready));
  }
  state_.anchor_count = 0;
  state_.floor_ps     = ps;
  state_.running      = 1;
  state_.single       = single ? 1 : 0;
}

void lptim::stop()
{
  state_.running      = 0;
  state_.single       = 0;
  state_.anchor_count = 0;
  state_.compare_held = 0;
  state_.reload_held  = 0;
  state_.wrap_held    = 0;
}

void lptim::refuse_unmodelled()
{
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

void lptim::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  state_.reload         = describe("ARR").reset;
  line_->lower();
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
    state_.hz        = ctx_.clock.hz(kernel_);
  }
  else if(was_on and not is_on)
  {
    stop();
  }
  auto const starting = now & (single_start | continuous_run);
  if(was_on and is_on and starting != 0)
  {
    if(state_.running != 0)
    {
      warn_once_or_refuse(
          restart_logged_, "CR", cr_.offset,
          "CNTSTRT or SNGSTRT while the counter runs: the mode switch is not modelled, the "
          "counter keeps its mode");
    }
    else
    {
      refuse_unmodelled();
      start(t, (starting & continuous_run) == 0);
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
  auto const hz                                              = ctx_.clock.hz(kernel_);
  due = hz == 0 ? frozen
                : static_cast<std::uint64_t>(
                      (wide{t} * hz + wide{variant_.ok_cycles} * ps_per_second + hz - 1) / hz);
  refresh();
  reschedule();
}

auto lptim::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  catch_up(now_ps());
  refresh();
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
