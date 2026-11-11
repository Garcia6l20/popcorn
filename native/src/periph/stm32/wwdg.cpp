#include "debug_freeze.hpp"
#include "flash_option.hpp"
#include "model_base.hpp"
#include "wwdg_rows.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/core_hold.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
constexpr std::uint32_t option_start    = event_kinds;
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
  std::uint8_t                           held{};
  std::uint64_t                          held_ps{};

  friend auto operator==(wwdg_state const&, wwdg_state const&) -> bool = default;
};

/**
 * @brief WWDG of the STM32L4, RM0351 section 37, the STM32L1, RM0038 section 22, the STM32U5,
 *        RM0456 section 62, and the STM32H5, RM0481 section 50. The 7-bit down-counter is derived
 *        from the virtual time of its last reload, never ticked. The divider by 4096 and by
 *        2^WDGTB runs free from the `wwdg` clock domain of the RCC, PCLK1 while WWDGEN is set,
 *        also with WDGA 0, RM0351 37.3.3. WDGTB is the description field, 2 bits on the L1 and
 *        L4, 3 bits on the U5 and H5. A `CR` write reloads T and leaves the divider alone. `CR`
 *        reads return the counter. With WDGA 1, the counter going from 0x40 to 0x3F requests a
 *        window watchdog reset, T6 clear in a `CR` write requests it at once, and so does a `CR`
 *        write while the counter value before the write is above W, RM0351 37.3 and Figure 387.
 *        WDGA and EWI stay set until a reset. EWIF sets when the counter reaches 0x40, also with
 *        EWI clear, RM0351 37.5.3, and clears when written 0. RM0351 37.2 conditions the early
 *        wake-up interrupt on "if enabled and the watchdog activated", and the model sets EWIF
 *        only with WDGA 1, inferred. The WWDG IRQ follows EWIF and EWI. A rate change of the clock
 *        keeps the partial division, a gated clock freezes the counter and the divider. With
 *        `wwdg_variant::hardware_option` 0, `FLASH_OPTR.WWDG_SW` on the L4 and U5 and
 *        `FLASH_OPTSR_CUR.WWDG_SW` on the H5, the WWDG is activated at every reset, read once every
 *        model has reset, and WDGA stays set. RM0481 50.3 Table 551 footnote 4: "When WWDG_SW is
 *        set in HW mode the WWDG is running as soon as the CPU is in Run or Sleep modes". While the
 *        machine holds its core with DBGMCU `DBG_WWDG_STOP` set, the counter and the divider stop
 *        at the hold and resume at the release with their phase kept, the bit read at the hold. A
 *        reset during the hold takes the hold again at the reset instant while the bit is set.
 *        A system reset keeps the bit and the counter stays frozen, a power-on reset clears it and
 *        the counter runs, DBGMCU being reset by a power-on reset only. Inferred, board checks: the
 *        divider starts at 0 at a reset and at a clock enable, a WDGTB change keeps the divider
 *        count and the counter value, a `CR` write with T 0x40 sets no EWIF, EWIF sets only
 *        with WDGA 1, the window compare applies to the write that sets WDGA, the hardware mode
 *        starts the counter at the reset instant, and the RCC sets WWDGEN in hardware mode on
 *        the H5. A per-peripheral reset, `APB1RSTR.WWDGRST` of the L1, bumps the generation of
 *        the pending events, so a stale deadline acts on nothing.
 *
 * @limits wwdg WWDG
 *
 * @group all rule
 * - The STM32L1, RM0038 22, the STM32L4, RM0351 37, the STM32U5, RM0456 62, and the STM32H5, RM0481
 *   50, share one model. WDGTB is 2 bits at 8:7 on the L1 and L4 and 3 bits at 13:11 on the U5 and
 *   H5. The counter derives from the `wwdg` clock domain of the RCC, PCLK1 while WWDGEN is set. A
 *   gated clock freezes the counter and the divider.
 * - The divider by 4096 and 2^WDGTB runs free also with WDGA 0, RM0351 37.3.3. A `CR` write reloads
 *   T and leaves the divider phase alone. A PCLK1 rate change keeps the partial division, less than
 *   one PCLK1 cycle of phase is dropped.
 * - A `CR` write resets when the counter before the write is above W, RM0351 37.3 and Figure 387.
 *   The written T plays no part. T6 clear in a write with WDGA 1 resets at once.
 * - WDGA and EWI are set-only until a reset, RM0351 37.5.1 and 37.5.2. `CR` reads return the
 *   counter.
 * - EWIF sets when the counter reaches 0x40, also with EWI clear, RM0351 37.5.3, and clears when
 *   written 0.
 * - The counter stops while the machine holds its core with `DBG_WWDG_STOP` set in DBGMCU, and
 *   resumes at the release with its phase kept. The bit is read at the hold. A reset during the
 *   hold takes the hold again at the reset instant while `DBG_WWDG_STOP` is set. A system reset
 *   keeps `DBG_WWDG_STOP`, so the counter stays frozen from the reset instant, and a power-on reset
 *   clears it, so the counter runs. DBGMCU is reset by a power-on reset only: RM0351 48.16.4 (POR,
 *   system reset not affected), RM0456 75.12.4, RM0481 66.12.4 and RM0038 30.16.4.
 *
 * @group u5,h5,l4 rule
 * - With `FLASH_OPTR.WWDG_SW` 0 (STM32L4 and STM32U5) or `FLASH_OPTSR_CUR.WWDG_SW` 0 (STM32H5) the
 *   WWDG is activated at every reset and WDGA cannot clear, read once every model has reset. The
 *   RCC sets WWDGEN at the same instant, RCC_APB1ENR1 WWDGEN "can also be set by hardware if the
 *   WWDG_SW option bit is reset" (RM0351, RM0456), and keeps WWDGSMEN at 1, "forced to 1 by
 *   hardware when the hardware WWDG option is activated". On the STM32H5 RM0481 50.3 Table 551
 *   footnote 4 says "When WWDG_SW is set in HW mode the WWDG is running as soon as the CPU is in
 *   Run or Sleep modes", and the H5 RCC sets WWDGEN like the L4 and U5, inferred: RM0481
 *   RCC_APB1LENR WWDGEN reads "Set and reset by software" and no manual gives the hardware set. The
 *   counter then runs from the reset with T 0x7F, W 0x7F and WDGTB 0. The STM32U5 description gives
 *   FLASH_OPTR the production value 0x1FEFF8AA, WWDG_SW 1.
 *
 * @group all rule "Inferred rules"
 * - Inferred, no manual states them: the divider starts at 0 at a reset and at a clock enable, a
 *   WDGTB change keeps the divider count and the counter value, a `CR` write with T 0x40 sets no
 *   EWIF, EWIF sets only with WDGA 1 (RM0351 37.2 conditions the interrupt on WDGA, 37.5.3 says
 *   nothing), the window compare applies to the write that sets WDGA, the hardware mode starts the
 *   counter at the reset instant, and on the STM32H5 the RCC sets WWDGEN in hardware mode.
 *
 * @group l1,l4 rule
 * - The STM32L1 `APB1RSTR.WWDGRST` returns the registers to their reset values and restarts the
 *   divider. On the STM32L4 only a system reset resets the WWDG.
 *
 * @group all note
 * - ES0565 2.2.16, clearing WWDG_SW breaks debug authentication or STiRoT, is a firmware-side
 *   hazard with no counterpart in the model. ES0125, ES0224, ES0240, ES0241, ES0242, ES0578, ES0250
 *   and ES0499 name no WWDG item.
 */
class wwdg final : public dispatch_model<wwdg, wwdg_state>,
                   public clock_listener,
                   public core_hold_listener
{
public:
  wwdg(model_context& ctx, peripheral_desc const& desc, wwdg_variant const& variant);

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_clock_change() override;
  void on_core_hold(bool held) override;

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

  register_file&             regs_;
  reg_ref                    control_;
  field_ref                  counter_;
  field_ref                  activation_;
  field_ref                  window_;
  field_ref                  timebase_;
  field_ref                  early_enable_;
  field_ref                  early_flag_;
  reg_ref                    config_;
  core::clock_domain         domain_;
  irq_line&                  line_;
  bool                       freeze_logged_ = false;
  std::optional<option_bits> option_;
};

constexpr std::array<write_entry<wwdg>, 3> wwdg_writes{
    {
     {"CR", &wwdg::control_written},
     {"CFR", &wwdg::config_written},
     {"SR", &wwdg::status_written},
     }
};

wwdg::wwdg(model_context& ctx, peripheral_desc const& desc, wwdg_variant const& variant)
    : dispatch_model{ctx, desc, "WWDG", wwdg_writes}, regs_{ctx.regs}, control_{reg("CR")},
      counter_{field("CR", "T")}, activation_{field("CR", "WDGA")}, window_{field("CFR", "W")},
      timebase_{field("CFR", "WDGTB")}, early_enable_{field("CFR", "EWI")},
      early_flag_{field("SR", "EWIF")}, config_{reg("CFR")}, domain_{ctx.clock.domain("wwdg")},
      line_{irq("WWDG_IRQn")}
{
  if(not variant.hardware_option.reg.empty())
  {
    option_ = option_bits_of(
        ctx, variant.hardware_option, "the hardware watchdog option the WWDG reads at reset");
  }
}

auto wwdg::now_ps() const -> std::uint64_t
{
  return state_.held != 0 ? state_.held_ps : ctx_.clock.time().ps;
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
  if(state_.held != 0)
  {
    return;
  }
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
  if(option_)
  {
    ctx_.events.schedule_at(ctx_.clock.time(), option_start, 0, ctx_.clock);
  }
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
  if(kind == option_start)
  {
    auto const word = ctx_.services.read(option_->at, 4, initiator::debugger);
    if(word and (*word & option_->mask) == 0 and get(regs_, activation_) == 0)
    {
      update(regs_, activation_, 1);
      reschedule();
    }
    return;
  }
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

void wwdg::on_core_hold(bool held)
{
  if(held)
  {
    if(not debug_frozen(freeze_logged_) or state_.held != 0)
    {
      return;
    }
    state_.held_ps = ctx_.clock.time().ps;
    state_.held    = 1;
    for(std::uint32_t kind = 0; kind < event_kinds; ++kind)
    {
      withdraw(kind);
      ++state_.generation[kind];
    }
    return;
  }
  if(state_.held == 0)
  {
    return;
  }
  auto const span = ctx_.clock.time().ps - state_.held_ps;
  state_.held     = 0;
  state_.held_ps  = 0;
  state_.anchor_ps += span;
  reschedule();
}
}

auto wwdg_factory(wwdg_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<wwdg>(ctx, desc, variant);
  };
}

namespace
{
constexpr auto wwdg_row_table = std::to_array<builtin>({
    row_of<^^wwdg_factory, ^^l1_wwdg>(
        {.ip = "WWDG", .ip_version_glob = "wwdg1_v1*", .families = family::l1}),
    row_of<^^wwdg_factory, ^^l4_wwdg>(
        {.ip = "WWDG", .ip_version_glob = "wwdg1_v1*", .families = family::l4}),
    row_of<^^wwdg_factory, ^^u5_wwdg>(
        {.ip = "WWDG", .ip_version_glob = "wwdg1_v2*", .families = family::u5}),
    row_of<^^wwdg_factory, ^^h5_wwdg>(
        {.ip = "WWDG", .ip_version_glob = "wwdg1_v2*", .families = family::h5}),
});
}

auto wwdg_rows() -> std::span<builtin const>
{
  return wwdg_row_table;
}
}
