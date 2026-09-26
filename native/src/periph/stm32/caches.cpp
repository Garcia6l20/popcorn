#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/types.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t invalidate_done = 0;
constexpr std::uint32_t command_done    = 1;

struct cache_state
{
  std::array<std::uint32_t, 2> generation{};

  friend auto operator==(cache_state const&, cache_state const&) -> bool = default;
};

void drive(irq_line& line, bool level)
{
  if(level)
  {
    line.raise();
  }
  else
  {
    line.lower();
  }
}

class icache final : public dispatch_model<icache, cache_state>
{
public:
  icache(model_context& ctx, peripheral_desc const& desc, cache_options const& options);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  void cr_written(std::uint32_t old, std::uint32_t now);
  void ier_written(std::uint32_t old, std::uint32_t now);
  void fcr_written(std::uint32_t old, std::uint32_t now);

private:
  void invalidate();
  void interrupt();

  cache_options const& options_;
  register_file&       regs_;
  field_ref            enable_;
  field_ref            invalidate_;
  field_ref            busy_;
  field_ref            busy_end_;
  field_ref            error_;
  field_ref            busy_end_irq_;
  field_ref            error_irq_;
  field_ref            clear_busy_end_;
  field_ref            clear_error_;
  reg_ref              fcr_;
  irq_line&            line_;
};

constexpr std::array<write_entry<icache>, 3> icache_writes{
    {
     {"ICACHE_CR", &icache::cr_written},
     {"ICACHE_IER", &icache::ier_written},
     {"ICACHE_FCR", &icache::fcr_written},
     }
};

icache::icache(model_context& ctx, peripheral_desc const& desc, cache_options const& options)
    : dispatch_model{ctx, desc, "ICACHE", icache_writes}, options_{options}, regs_{ctx.regs},
      enable_{field("ICACHE_CR", "EN")}, invalidate_{field("ICACHE_CR", "CACHEINV")},
      busy_{field("ICACHE_SR", "BUSYF")}, busy_end_{field("ICACHE_SR", "BSYENDF")},
      error_{field("ICACHE_SR", "ERRF")}, busy_end_irq_{field("ICACHE_IER", "BSYENDIE")},
      error_irq_{field("ICACHE_IER", "ERRIE")}, clear_busy_end_{field("ICACHE_FCR", "CBSYENDF")},
      clear_error_{field("ICACHE_FCR", "CERRF")}, fcr_{reg("ICACHE_FCR")}, line_{irq("ICACHE_IRQn")}
{}

void icache::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  invalidate();
}

void icache::invalidate()
{
  update(regs_, busy_, 1);
  schedule_in(options_.busy_delay, invalidate_done, ++state_.generation[invalidate_done]);
}

void icache::interrupt()
{
  drive(
      line_, (get(regs_, busy_end_) & get(regs_, busy_end_irq_))
                 != 0
                 or (get(regs_, error_) & get(regs_, error_irq_))
                 != 0);
}

void icache::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != invalidate_done or payload != state_.generation[invalidate_done])
  {
    return;
  }
  update(regs_, busy_, 0);
  update(regs_, busy_end_, 1);
  interrupt();
}

void icache::cr_written(std::uint32_t old, std::uint32_t now)
{
  if(invalidate_.of(now) != 0 or (enable_.of(old) != 0 and enable_.of(now) == 0))
  {
    update(regs_, invalidate_, 0);
    invalidate();
  }
}

void icache::ier_written(std::uint32_t, std::uint32_t)
{
  interrupt();
}

void icache::fcr_written(std::uint32_t, std::uint32_t now)
{
  if(clear_busy_end_.of(now) != 0)
  {
    update(regs_, busy_end_, 0);
  }
  if(clear_error_.of(now) != 0)
  {
    update(regs_, error_, 0);
  }
  regs_.poke(fcr_.offset, 0);
  interrupt();
}

class dcache final : public dispatch_model<dcache, cache_state>
{
public:
  dcache(model_context& ctx, peripheral_desc const& desc, cache_options const& options);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  void cr_written(std::uint32_t old, std::uint32_t now);
  void ier_written(std::uint32_t old, std::uint32_t now);
  void fcr_written(std::uint32_t old, std::uint32_t now);

private:
  void invalidate();
  void interrupt();

  cache_options const& options_;
  register_file&       regs_;
  field_ref            enable_;
  field_ref            invalidate_;
  field_ref            command_;
  field_ref            start_;
  field_ref            busy_;
  field_ref            busy_end_;
  field_ref            error_;
  field_ref            command_busy_;
  field_ref            command_end_;
  field_ref            busy_end_irq_;
  field_ref            error_irq_;
  field_ref            command_end_irq_;
  field_ref            clear_busy_end_;
  field_ref            clear_error_;
  field_ref            clear_command_end_;
  reg_ref              fcr_;
  irq_line&            line_;
};

constexpr std::array<write_entry<dcache>, 3> dcache_writes{
    {
     {"DCACHE_CR", &dcache::cr_written},
     {"DCACHE_IER", &dcache::ier_written},
     {"DCACHE_FCR", &dcache::fcr_written},
     }
};

dcache::dcache(model_context& ctx, peripheral_desc const& desc, cache_options const& options)
    : dispatch_model{ctx, desc, "DCACHE", dcache_writes}, options_{options}, regs_{ctx.regs},
      enable_{field("DCACHE_CR", "EN")}, invalidate_{field("DCACHE_CR", "CACHEINV")},
      command_{field("DCACHE_CR", "CACHECMD")}, start_{field("DCACHE_CR", "STARTCMD")},
      busy_{field("DCACHE_SR", "BUSYF")}, busy_end_{field("DCACHE_SR", "BSYENDF")},
      error_{field("DCACHE_SR", "ERRF")}, command_busy_{field("DCACHE_SR", "BUSYCMDF")},
      command_end_{field("DCACHE_SR", "CMDENDF")}, busy_end_irq_{field("DCACHE_IER", "BSYENDIE")},
      error_irq_{field("DCACHE_IER", "ERRIE")}, command_end_irq_{field("DCACHE_IER", "CMDENDIE")},
      clear_busy_end_{field("DCACHE_FCR", "CBSYENDF")}, clear_error_{field("DCACHE_FCR", "CERRF")},
      clear_command_end_{field("DCACHE_FCR", "CCMDENDF")}, fcr_{reg("DCACHE_FCR")},
      line_{irq("DCACHE1_IRQn")}
{}

void dcache::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  invalidate();
}

void dcache::invalidate()
{
  update(regs_, busy_, 1);
  schedule_in(options_.busy_delay, invalidate_done, ++state_.generation[invalidate_done]);
}

void dcache::interrupt()
{
  drive(
      line_, (get(regs_, busy_end_) & get(regs_, busy_end_irq_))
                 != 0
                 or (get(regs_, error_) & get(regs_, error_irq_))
                 != 0
                 or (get(regs_, command_end_) & get(regs_, command_end_irq_))
                 != 0);
}

void dcache::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind > command_done or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == invalidate_done)
  {
    update(regs_, busy_, 0);
    update(regs_, busy_end_, 1);
  }
  else if(kind == command_done)
  {
    update(regs_, command_busy_, 0);
    update(regs_, command_end_, 1);
  }
  interrupt();
}

void dcache::cr_written(std::uint32_t old, std::uint32_t now)
{
  if(invalidate_.of(now) != 0 or (enable_.of(old) != 0 and enable_.of(now) == 0))
  {
    update(regs_, invalidate_, 0);
    invalidate();
  }
  if(start_.of(now) != 0)
  {
    update(regs_, start_, 0);
    auto const command = command_.of(now);
    auto const idle    = get(regs_, busy_) == 0 and get(regs_, command_busy_) == 0;
    if(enable_.of(now) != 0 and idle and command >= 1 and command <= 3)
    {
      update(regs_, command_busy_, 1);
      schedule_in(options_.busy_delay, command_done, ++state_.generation[command_done]);
    }
  }
}

void dcache::ier_written(std::uint32_t, std::uint32_t)
{
  interrupt();
}

void dcache::fcr_written(std::uint32_t, std::uint32_t now)
{
  if(clear_busy_end_.of(now) != 0)
  {
    update(regs_, busy_end_, 0);
  }
  if(clear_error_.of(now) != 0)
  {
    update(regs_, error_, 0);
  }
  if(clear_command_end_.of(now) != 0)
  {
    update(regs_, command_end_, 0);
  }
  regs_.poke(fcr_.offset, 0);
  interrupt();
}
}

auto icache_factory(cache_options const& options) -> model_factory
{
  return [&options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<icache>(ctx, desc, options);
  };
}

auto dcache_factory(cache_options const& options) -> model_factory
{
  return [&options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<dcache>(ctx, desc, options);
  };
}
}
