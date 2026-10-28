#include "factories.hpp"
#include "frame_signal.hpp"
#include "model_base.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
enum counter_phase : std::uint8_t
{
  phase_idle,
  phase_down,
  phase_up,
  phase_stopped,
};

struct crs_state
{
  std::uint64_t sync_ps{};
  std::uint64_t generation{};
  std::uint64_t queued{};
  std::uint8_t  frames{};
  std::uint8_t  phase{};

  friend auto operator==(crs_state const&, crs_state const&) -> bool = default;
};

constexpr std::uint32_t usb_sof_source  = 2;
constexpr std::uint32_t counter_event   = 0;
constexpr std::uint64_t ps_per_3_cycles = 62'500;
constexpr std::uint64_t max_interval_ps = 1'000'000'000'000;
constexpr std::uint32_t warning_factor  = 3;
constexpr std::uint32_t outrange_factor = 128;

struct flag_pair
{
  field_ref enable;
  field_ref flag;
};

/**
 * @brief The clock recovery system of the STM32U5 and STM32H5, RM0456 section 12 and RM0481
 *        section 12, one IP on both. A SYNC event is a write of 1 to `CRS_CR.SWSYNC`, or a USB
 *        start of frame while `SYNCSRC` is 2 and the 2^SYNCDIV divider, which counts the SOFs,
 *        reaches its end. Only `SWSYNC` and the USB SOF give a SYNC, and only while `CEN` is set.
 *
 *        The frequency error counter runs at the HSI48, exactly 48 MHz, RM0456 12.4.4 and RM0481
 *        12.4.4. The first SYNC after `CEN` is set loads `RELOAD` and sets no flag. Each SYNC
 *        loads `RELOAD`, and the counter reaches 0 `RELOAD` + 1 cycles later, so the RM0456
 *        12.4.6 formula `RELOAD = fTARGET / fSYNC - 1` gives an error of 0. There it sets
 *        `ESYNCF` and counts up to 128 x `FELIM`, where it sets `SYNCMISS` and `ERRF` and stops.
 *        Both are scheduled events. A later SYNC latches the count in `FECAP` and the direction in
 *        `FEDIR`, 1 while counting down, and classes the error by RM0456 12.4.5 and 12.7.3:
 *        `SYNCOKF` below 3 x `FELIM`, `SYNCWARNF` below 128 x `FELIM`, else `SYNCERR` and `ERRF`
 *        while counting down. `FECAP` holds at most `RELOAD`, the value loaded, when two SYNCs land
 * in one HSI48 cycle. A SYNC that finds the counter stopped latches 128 x `FELIM` and adds no flag
 * to the `SYNCMISS` of the stop. `TRIM` holds its value with or without `AUTOTRIMEN`, so `TRIMOVF`
 * stays 0 and the error stays what `RELOAD` and the SYNC period give. `SWSYNC` reads 0.
 *
 *        `CRS_CFGR` ignores writes while `CEN` is set, `TRIM` ignores them while `AUTOTRIMEN`
 *        is set. A write of 1 to a `CRS_ICR` bit clears the flags of the description's cross
 *        rules, and `CRS_ICR` reads 0. The IRQ line is the OR of each flag and its enable. The
 *        divider count restarts and the counter waits for its first SYNC whenever `CEN` changes.
 *        Setting `CEN` with a `SYNCSRC` other than 2 logs one warning per model life: only
 *        `SWSYNC` and the USB SOF give a SYNC. `SYNCPOL` and the RCC clock gate act on nothing.
 *        Every reset kind and the RCC reset restore the description reset values, the divider
 *        and an idle counter, and drop its pending event.
 */
class crs final : public dispatch_model<crs, crs_state>,
                  public bus_reset_listener,
                  public frame_sink
{
public:
  crs(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void frame_started(std::uint64_t at_ps) override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void clear_written(std::uint32_t old, std::uint32_t now);
  void stored(std::uint32_t, std::uint32_t)
  {}

  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void restore_reset_values();
  void sync(std::uint64_t at_ps);
  void start(std::uint64_t at_ps);
  void stop();
  void settle(std::uint64_t at_ps);
  void post();
  void withdraw();
  void refresh();
  void warn_source(std::uint32_t source);

  [[nodiscard]] auto reload() const -> std::uint64_t;
  [[nodiscard]] auto limit() const -> std::uint64_t;
  [[nodiscard]] auto due(std::uint64_t cycles) const -> std::uint64_t;
  [[nodiscard]] auto next_due() const -> std::uint64_t;
  [[nodiscard]] auto cycles_to(std::uint64_t at_ps) const -> std::uint64_t;

  register_file&         regs_;
  irq_line&              line_;
  field_ref              enable_;
  field_ref              auto_trim_;
  field_ref              software_sync_;
  field_ref              trim_;
  field_ref              divider_;
  field_ref              source_;
  field_ref              reload_;
  field_ref              error_limit_;
  field_ref              capture_;
  field_ref              direction_;
  field_ref              sync_error_;
  field_ref              sync_miss_;
  std::uint32_t          config_;
  std::uint32_t          clear_flags_;
  std::vector<cross_ref> clears_;
  flag_pair              sync_ok_;
  flag_pair              sync_warning_;
  flag_pair              error_;
  flag_pair              expected_sync_;
  bool                   source_logged_{};
};

constexpr std::array<write_entry<crs>, 4> crs_writes{
    {
     {"CR", &crs::control_written},
     {"CFGR", &crs::stored},
     {"ISR", &crs::stored},
     {"ICR", &crs::clear_written},
     }
};

crs::crs(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "CRS", crs_writes}, regs_{ctx.regs}, line_{irq("CRS_IRQn")},
      enable_{field("CR", "CEN")}, auto_trim_{field("CR", "AUTOTRIMEN")},
      software_sync_{field("CR", "SWSYNC")}, trim_{field("CR", "TRIM")},
      divider_{field("CFGR", "SYNCDIV")}, source_{field("CFGR", "SYNCSRC")},
      reload_{field("CFGR", "RELOAD")}, error_limit_{field("CFGR", "FELIM")},
      capture_{field("ISR", "FECAP")}, direction_{field("ISR", "FEDIR")},
      sync_error_{field("ISR", "SYNCERR")}, sync_miss_{field("ISR", "SYNCMISS")},
      config_{reg("CFGR").offset}, clear_flags_{reg("ICR").offset}, clears_{crosses_from("ICR")},
      sync_ok_{field("CR", "SYNCOKIE"), field("ISR", "SYNCOKF")},
      sync_warning_{field("CR", "SYNCWARNIE"), field("ISR", "SYNCWARNF")},
      error_{field("CR", "ERRIE"), field("ISR", "ERRF")},
      expected_sync_{field("CR", "ESYNCIE"), field("ISR", "ESYNCF")}
{}

auto crs::guard(std::uint32_t word, access_ctx const&, bool) -> bus_guard
{
  auto const control = regs_.peek(enable_.offset);
  if(word == config_ and enable_.of(control) != 0)
  {
    return bus_guard{.wi = ~std::uint32_t{}};
  }
  if(word == trim_.offset and auto_trim_.of(control) != 0)
  {
    return bus_guard{.wi = trim_.mask()};
  }
  return {};
}

void crs::control_written(std::uint32_t old, std::uint32_t now)
{
  if(enable_.of(old) != enable_.of(now))
  {
    state_.frames = 0;
    stop();
    if(enable_.of(now) != 0)
    {
      warn_source(get(regs_, source_));
    }
  }
  regs_.poke(software_sync_.offset, software_sync_.with(now, 0));
  if(software_sync_.of(now) != 0 and enable_.of(now) != 0)
  {
    sync(ctx_.clock.time().ps);
  }
  refresh();
}

void crs::clear_written(std::uint32_t, std::uint32_t now)
{
  apply_crosses(clears_, now);
  regs_.poke(clear_flags_, 0);
  refresh();
}

void crs::frame_started(std::uint64_t at_ps)
{
  if(get(regs_, enable_) == 0 or get(regs_, source_) != usb_sof_source)
  {
    return;
  }
  auto const period = std::uint32_t{1} << get(regs_, divider_);
  if(++state_.frames < period)
  {
    return;
  }
  state_.frames = 0;
  sync(at_ps);
  refresh();
}

void crs::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != counter_event or payload != state_.generation)
  {
    return;
  }
  state_.queued = 0;
  settle(std::max(ctx_.clock.time().ps, next_due()));
  post();
  refresh();
}

void crs::sync(std::uint64_t at_ps)
{
  if(state_.phase == phase_idle)
  {
    start(at_ps);
    return;
  }
  settle(at_ps);
  auto const elapsed  = cycles_to(at_ps);
  auto const zero     = reload() + 1;
  auto const counting = elapsed < zero;
  auto const error =
      counting ? std::min(zero - elapsed, reload()) : std::min(elapsed - zero, limit());
  update(regs_, capture_, static_cast<std::uint32_t>(error));
  update(regs_, direction_, counting ? 1 : 0);
  auto const tolerance = std::uint64_t{get(regs_, error_limit_)};
  if(error < warning_factor * tolerance)
  {
    update(regs_, sync_ok_.flag, 1);
  }
  else if(error < limit())
  {
    update(regs_, sync_warning_.flag, 1);
  }
  else if(counting)
  {
    update(regs_, sync_error_, 1);
    update(regs_, error_.flag, 1);
  }
  start(at_ps);
}

void crs::start(std::uint64_t at_ps)
{
  withdraw();
  ++state_.generation;
  state_.sync_ps = at_ps;
  state_.phase   = phase_down;
  post();
}

void crs::stop()
{
  withdraw();
  ++state_.generation;
  state_.phase = phase_idle;
}

void crs::settle(std::uint64_t at_ps)
{
  if(state_.phase == phase_down and due(reload() + 1) <= at_ps)
  {
    update(regs_, expected_sync_.flag, 1);
    state_.phase = phase_up;
  }
  if(state_.phase == phase_up and due(reload() + 1 + limit()) <= at_ps)
  {
    update(regs_, sync_miss_, 1);
    update(regs_, error_.flag, 1);
    state_.phase = phase_stopped;
  }
}

void crs::post()
{
  if(state_.phase != phase_down and state_.phase != phase_up)
  {
    return;
  }
  withdraw();
  state_.queued =
      ctx_.events.schedule_at(vtime{next_due()}, counter_event, state_.generation, ctx_.clock) + 1;
}

void crs::withdraw()
{
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
}

void crs::refresh()
{
  auto level = false;
  for(auto const* const pair : {&sync_ok_, &sync_warning_, &error_, &expected_sync_})
  {
    level = level or (get(regs_, pair->enable) != 0 and get(regs_, pair->flag) != 0);
  }
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void crs::warn_source(std::uint32_t source)
{
  if(source != usb_sof_source and not std::exchange(source_logged_, true))
  {
    warn(
        "CFGR", std::format(
                    "SYNCSRC {} selects the GPIO, LSE or a reserved sync source: popcorn gives a "
                    "SYNC only from SWSYNC and the USB SOF",
                    source));
  }
}

auto crs::reload() const -> std::uint64_t
{
  return get(regs_, reload_);
}

auto crs::limit() const -> std::uint64_t
{
  return outrange_factor * std::uint64_t{get(regs_, error_limit_)};
}

auto crs::due(std::uint64_t cycles) const -> std::uint64_t
{
  return state_.sync_ps + (cycles * ps_per_3_cycles + 2) / 3;
}

auto crs::next_due() const -> std::uint64_t
{
  return due(reload() + 1 + (state_.phase == phase_down ? 0 : limit()));
}

auto crs::cycles_to(std::uint64_t at_ps) const -> std::uint64_t
{
  auto const interval = std::min(at_ps - std::min(at_ps, state_.sync_ps), max_interval_ps);
  return interval * 3 / ps_per_3_cycles;
}

void crs::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation;
  state_                = {};
  state_.generation     = generation + 1;
  line_.lower();
}

void crs::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void crs::on_bus_reset()
{
  withdraw();
  restore_reset_values();
}
}

auto crs_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<crs>(ctx, desc);
  };
}
}
