#include "model_base.hpp"
#include "wwdg_rows.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint32_t expiry          = 0;
constexpr std::uint32_t early_wakeup    = 1;
constexpr std::size_t   event_kinds     = 2;
constexpr std::uint64_t divider_cycles  = 4096;
constexpr std::uint32_t counter_modulus = 128;
constexpr std::uint32_t counter_mask    = 0x7F;
constexpr std::uint32_t early_value     = 0x40;
constexpr std::uint32_t reset_value     = 0x3F;
constexpr std::uint32_t top_bit         = 0x40;

struct wwdg_state
{
  std::uint64_t                          anchor_ps{};
  std::uint64_t                          hz{};
  std::uint64_t                          cycles{};
  std::uint64_t                          base{};
  std::uint32_t                          start{counter_mask};
  std::array<std::uint32_t, event_kinds> generation{};
  std::array<std::uint64_t, event_kinds> queued{};
  std::uint8_t                           bitten{};

  friend auto operator==(wwdg_state const&, wwdg_state const&) -> bool = default;
};

/**
 * @brief WWDG of the STM32L4, RM0351 section 37, and of the STM32L1, RM0038 section 22. The
 *        7-bit down-counter is derived from the virtual time of its last reload, never ticked.
 *        The divider by 4096 and by 2^WDGTB runs free from the `wwdg` clock domain of the RCC,
 *        PCLK1 while WWDGEN is set, also with WDGA 0, RM0351 37.3.3. A `CR` write reloads T and
 *        leaves the divider alone. `CR` reads return the counter. With WDGA 1, the counter going
 *        from 0x40 to 0x3F requests a window watchdog reset, T6 clear in a `CR` write requests it
 *        at once, and so does a `CR` write while the counter value before the write is above W,
 *        RM0351 37.3 and Figure 387. WDGA and EWI stay set until a reset. EWIF sets when the
 *        counter reaches 0x40 with WDGA 1, RM0351 37.2 "if enabled and the watchdog activated",
 *        also with EWI clear, 37.5.3, and clears when written 0. The WWDG IRQ follows EWIF and
 *        EWI. A rate change of the clock keeps the partial division, a gated clock freezes the
 *        counter and the divider. Inferred, board checks: the divider starts at 0 at a reset and
 *        at a clock enable, a WDGTB change keeps the divider count and the counter value, a
 *        `CR` write with T 0x40 sets no EWIF, EWIF sets only with WDGA 1, and the window
 *        compare applies to the write that sets WDGA. The counter runs while the core is
 *        halted in a debug session. A per-peripheral reset, `APB1RSTR.WWDGRST` of the L1, bumps
 *        the generation of the pending events, so a stale deadline acts on nothing.
 */
class wwdg final : public dispatch_model<wwdg, wwdg_state>, public clock_listener
{
public:
  wwdg(model_context& ctx, peripheral_desc const& desc);

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_clock_change() override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void config_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto shift_of(std::uint32_t word) const -> std::uint32_t;
  [[nodiscard]] auto cycles_at(std::uint64_t ps) const -> std::uint64_t;
  [[nodiscard]] auto ticks_at(std::uint64_t ps, std::uint32_t shift) const -> std::uint64_t;
  [[nodiscard]] auto count_at(std::uint64_t ps, std::uint32_t shift) const -> std::uint32_t;

  void schedule_tick(std::uint32_t kind, std::uint64_t tick);
  void withdraw(std::uint32_t kind);
  void reschedule();
  void bite();
  void interrupt();

  register_file&     regs_;
  reg_ref            control_;
  field_ref          counter_;
  field_ref          activation_;
  field_ref          window_;
  field_ref          timebase_;
  field_ref          early_enable_;
  field_ref          early_flag_;
  reg_ref            config_;
  core::clock_domain domain_;
  irq_line&          line_;
};

constexpr std::array<write_entry<wwdg>, 3> wwdg_writes{
    {
     {"CR", &wwdg::control_written},
     {"CFR", &wwdg::config_written},
     {"SR", &wwdg::status_written},
     }
};

wwdg::wwdg(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "WWDG", wwdg_writes}, regs_{ctx.regs}, control_{reg("CR")},
      counter_{field("CR", "T")}, activation_{field("CR", "WDGA")}, window_{field("CFR", "W")},
      timebase_{field("CFR", "WDGTB")}, early_enable_{field("CFR", "EWI")},
      early_flag_{field("SR", "EWIF")}, config_{reg("CFR")}, domain_{ctx.clock.domain("wwdg")},
      line_{irq("WWDG_IRQn")}
{}

auto wwdg::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto wwdg::shift_of(std::uint32_t word) const -> std::uint32_t
{
  return timebase_.of(word);
}

auto wwdg::cycles_at(std::uint64_t ps) const -> std::uint64_t
{
  if(state_.hz == 0 or ps <= state_.anchor_ps)
  {
    return state_.cycles;
  }
  return state_.cycles
       + static_cast<std::uint64_t>(wide{ps - state_.anchor_ps} * state_.hz / ps_per_second);
}

auto wwdg::ticks_at(std::uint64_t ps, std::uint32_t shift) const -> std::uint64_t
{
  return cycles_at(ps) / (divider_cycles << shift);
}

auto wwdg::count_at(std::uint64_t ps, std::uint32_t shift) const -> std::uint32_t
{
  auto const elapsed =
      static_cast<std::uint32_t>((ticks_at(ps, shift) - state_.base) % counter_modulus);
  return (state_.start + counter_modulus - elapsed) % counter_modulus;
}

void wwdg::schedule_tick(std::uint32_t kind, std::uint64_t tick)
{
  withdraw(kind);
  auto const cycles = tick * (divider_cycles << shift_of(regs_.peek(config_.offset)));
  auto const needed = cycles > state_.cycles ? cycles - state_.cycles : 0;
  auto const at     = wide{needed} * ps_per_second + state_.hz - 1;
  auto const due =
      std::max(state_.anchor_ps + static_cast<std::uint64_t>(at / state_.hz), now_ps());
  state_.queued[kind] =
      ctx_.events.schedule_at(vtime{due}, kind, ++state_.generation[kind], ctx_.clock) + 1;
}

void wwdg::withdraw(std::uint32_t kind)
{
  if(auto const seq = std::exchange(state_.queued[kind], 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
}

void wwdg::reschedule()
{
  for(std::uint32_t kind = 0; kind < event_kinds; ++kind)
  {
    withdraw(kind);
    ++state_.generation[kind];
  }
  if(state_.hz == 0 or state_.bitten != 0 or get(regs_, activation_) == 0)
  {
    return;
  }
  auto const now = ticks_at(now_ps(), shift_of(regs_.peek(config_.offset)));
  if(state_.start <= reset_value)
  {
    return;
  }
  auto const last = state_.base + state_.start - reset_value;
  schedule_tick(expiry, std::max(last, now));
  if(auto const early = state_.base + state_.start - early_value;
     state_.start > early_value and early > now)
  {
    schedule_tick(early_wakeup, early);
  }
}

void wwdg::bite()
{
  if(state_.bitten != 0)
  {
    return;
  }
  state_.bitten = 1;
  reschedule();
  ctx_.services.request_reset(reset_kind::window_watchdog);
}

void wwdg::interrupt()
{
  if(get(regs_, early_flag_) != 0 and get(regs_, early_enable_) != 0)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

auto wwdg::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  if((offset & ~3u) == control_.offset)
  {
    update(regs_, counter_, count_at(now_ps(), shift_of(regs_.peek(config_.offset))));
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

void wwdg::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto generation = state_.generation;
  for(auto& value : generation)
  {
    ++value;
  }
  state_            = {};
  state_.generation = generation;
  state_.anchor_ps  = now_ps();
  state_.hz         = ctx_.clock.hz(domain_);
  line_.lower();
}

void wwdg::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const word = activation_.of(old) != 0 ? activation_.with(now, 1) : now;
  regs_.poke(control_.offset, word);
  auto const at     = now_ps();
  auto const shift  = shift_of(regs_.peek(config_.offset));
  auto const before = count_at(at, shift);
  auto const value  = counter_.of(word);
  state_.start      = value;
  state_.base       = ticks_at(at, shift);
  if(activation_.of(word) != 0 and (before > get(regs_, window_) or (value & top_bit) == 0))
  {
    bite();
    return;
  }
  reschedule();
}

void wwdg::config_written(std::uint32_t old, std::uint32_t now)
{
  auto const word = early_enable_.of(old) != 0 ? early_enable_.with(now, 1) : now;
  regs_.poke(config_.offset, word);
  auto const at = now_ps();
  state_.start  = count_at(at, shift_of(old));
  state_.base   = ticks_at(at, shift_of(word));
  interrupt();
  reschedule();
}

void wwdg::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(early_flag_.offset, early_flag_.with(old, early_flag_.of(old) & early_flag_.of(now)));
  interrupt();
}

void wwdg::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= event_kinds
     or payload != state_.generation[kind]
     or state_.bitten != 0
     or get(regs_, activation_) == 0)
  {
    return;
  }
  if(kind == early_wakeup)
  {
    update(regs_, early_flag_, 1);
    interrupt();
    return;
  }
  bite();
}

void wwdg::on_clock_change()
{
  auto const hz = ctx_.clock.hz(domain_);
  if(hz == state_.hz)
  {
    return;
  }
  auto const at    = now_ps();
  auto const shift = shift_of(regs_.peek(config_.offset));
  if(state_.hz == 0)
  {
    state_.start  = count_at(at, shift);
    state_.cycles = 0;
    state_.base   = 0;
  }
  else
  {
    state_.cycles = cycles_at(at);
  }
  state_.anchor_ps = at;
  state_.hz        = hz;
  reschedule();
}
}

auto wwdg_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<wwdg>(ctx, desc);
  };
}

namespace
{
constexpr auto wwdg_row_table = std::to_array<builtin>({
    row_of<^^wwdg_factory>(
        {.ip = "WWDG", .ip_version_glob = "wwdg1_v1*", .families = family::l1 | family::l4}),
});
}

auto wwdg_rows() -> std::span<builtin const>
{
  return wwdg_row_table;
}
}
