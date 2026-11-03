#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint32_t compare_match  = 1u << 0;
constexpr std::uint32_t reload_match   = 1u << 1;
constexpr std::uint32_t compare_ok     = 1u << 3;
constexpr std::uint32_t reload_ok      = 1u << 4;
constexpr std::uint32_t counter_top    = 0xFFFF;
constexpr std::uint64_t frozen         = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t enable_bit     = 1u << 0;
constexpr std::uint32_t single_start   = 1u << 1;
constexpr std::uint32_t continuous_run = 1u << 2;
constexpr std::uint32_t tick_event     = 0;

enum class term : std::uint8_t
{
  compare_written,
  reload_written,
  compare,
  reload,
};

struct lptim_state
{
  std::uint64_t anchor_ps{};
  std::uint64_t anchor_frac{};
  std::uint64_t hz{};
  std::uint64_t settled_ps{};
  std::uint64_t floor_ps{};
  std::uint64_t compare_held{};
  std::uint64_t reload_held{};
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

  friend auto operator==(lptim_state const&, lptim_state const&) -> bool = default;
};

/**
 * @brief LPTIM of the STM32L4, RM0351 section 34, clocked by RCC's `lptim1` or `lptim2` domain
 *        from CCIPR. The 16-bit counter is derived from the kernel clock, the prescaler and the
 *        virtual time of its last anchor, never ticked. CNTSTRT or SNGSTRT written with ENABLE
 *        already 1 starts it from 0 the variant's start cycles after the write, RM0351 34.4.6,
 *        in continuous mode: it counts 0 to ARR and wraps, RM0351 34.4.7. A write setting them
 *        with ENABLE 0 is discarded, RM0351 34.4.6, also when the same write sets ENABLE,
 *        inferred. They read 0 at once, inferred, and a start while the counter runs changes
 *        nothing. CMPM and ARRM set the variant's event delay after the counter reaches CMP and
 *        ARR, one kernel clock on the STM32L4 by ES0250 2.18.3, also with PRESC above 0,
 *        inferred. CMP above ARR never matches, and a counter above a lowered ARR runs to
 *        0xFFFF before it wraps, inferred. A CMP or ARR write sets CMPOK or ARROK, and takes
 *        effect, the variant's OK cycles later, 3 kernel clocks inferred: RM0351 34.4.10 states
 *        a latency with no number. A second write before the OK flag replaces the pending
 *        value, inferred: RM0351 34.4.10 calls it unpredictable. Counter ticks up to the OK
 *        compare with the old values, and a match of such a tick still sets its flag at its
 *        due after the OK, inferred. Terms due at the same time apply in the order CMPOK,
 *        ARROK, CMPM, ARRM, inferred.
 *        A flag that sets while its IER bit is 1 arms the LPTIM IRQ until ICR clears it, so an
 *        IER bit set after its flag raises nothing, RM0351 34.6. ENABLE written to 0 stops the
 *        counter at 0 and keeps the flags and a pending CMP or ARR write, inferred. The counter
 *        counts the kernel clock whatever CFGR selects besides PRESC, and IER, CFGR, OR, CMP and
 *        ARR take every write.
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

  void                       plain_written(std::uint32_t old, std::uint32_t now);
  void                       clear_written(std::uint32_t old, std::uint32_t now);
  void                       control_written(std::uint32_t old, std::uint32_t now);
  template <term Which> void value_written(std::uint32_t old, std::uint32_t now);

private:
  struct due_term
  {
    std::uint64_t due;
    term          which;
  };

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
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
  void start(std::uint64_t ps);
  void stop();
  void refresh();
  void reschedule();

  lptim_variant      variant_;
  register_file&     regs_;
  reg_ref            isr_;
  reg_ref            ier_;
  reg_ref            cr_;
  reg_ref            cnt_;
  field_ref          prescaler_;
  core::clock_domain kernel_;
  irq_line*          line_ = nullptr;
};

constexpr std::array<write_entry<lptim>, 8> lptim_writes{
    {
     {"ISR", &lptim::plain_written},
     {"ICR", &lptim::clear_written},
     {"IER", &lptim::plain_written},
     {"CFGR", &lptim::plain_written},
     {"CR", &lptim::control_written},
     {"CMP", &lptim::value_written<term::compare_written>},
     {"ARR", &lptim::value_written<term::reload_written>},
     {"CNT", &lptim::plain_written},
     }
};

constexpr std::array<write_entry<lptim>, 1> lptim_option_writes{{{"OR", &lptim::plain_written}}};

lptim::lptim(model_context& ctx, peripheral_desc const& desc, lptim_variant const& variant)
    : dispatch_model{ctx, desc, "LPTIM", lptim_writes}, variant_{variant}, regs_{ctx.regs},
      isr_{reg("ISR")}, ier_{reg("IER")}, cr_{reg("CR")}, cnt_{reg("CNT")},
      prescaler_{field("CFGR", "PRESC")}, kernel_{ctx.clock.domain(lower(desc.name))},
      line_{&irq(desc.name + "_IRQn")}
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
  auto const compare = next.which == term::compare_written;
  raise_flag(compare ? compare_ok : reload_ok);
  if(auto const ticks = ticks_at(next.due))
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

void lptim::start(std::uint64_t ps)
{
  state_.hz = ctx_.clock.hz(kernel_);
  if(state_.hz == 0)
  {
    state_.anchor_ps   = ps;
    state_.anchor_frac = 0;
  }
  else
  {
    set_anchor(wide{ps} * state_.hz + wide{variant_.start_cycles} * ps_per_second);
  }
  state_.anchor_count = 0;
  state_.floor_ps     = ps;
  state_.running      = 1;
}

void lptim::stop()
{
  state_.running      = 0;
  state_.anchor_count = 0;
  state_.compare_held = 0;
  state_.reload_held  = 0;
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

void lptim::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const t = now_ps();
  catch_up(t);
  regs_.poke(cr_.offset, now & enable_bit);
  auto const was_on = (old & enable_bit) != 0;
  auto const is_on  = (now & enable_bit) != 0;
  if(not was_on and is_on)
  {
    state_.hz = ctx_.clock.hz(kernel_);
  }
  else if(was_on and not is_on)
  {
    stop();
  }
  auto const starting = now & (single_start | continuous_run);
  if(was_on and is_on and starting != 0)
  {
    if(state_.running == 0)
    {
      start(t);
    }
  }
  refresh();
  reschedule();
}

template <term Which> void lptim::value_written(std::uint32_t, std::uint32_t now)
{
  constexpr auto compare = Which == term::compare_written;
  auto const     t       = now_ps();
  catch_up(t);
  auto& due = compare ? state_.compare_due : state_.reload_due;
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
