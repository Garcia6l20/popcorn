#include "models.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/register_file.hpp>

#include <format>
#include <limits>
#include <span>
#include <utility>

namespace popcorn::bind
{
namespace
{
template <typename Line>
auto line_at(std::span<Line> lines, std::uint32_t index, char const* kind) -> Line&
{
  if(index >= lines.size())
  {
    throw config_error(
        "model", 0, kind, std::format("a line index below {}, not {}", lines.size(), index));
  }
  return lines[index];
}
}

model_context_view::model_context_view(
    periph::model_context&      ctx,
    std::shared_ptr<bool const> alive) noexcept
    : ctx_{&ctx}, alive_{std::move(alive)}
{}

auto model_context_view::context() const -> periph::model_context&
{
  if(not *alive_)
  {
    throw config_error("model", 0, "machine", "a model whose machine still exists");
  }
  return *ctx_;
}

auto model_context_view::description() const -> peripheral_desc
{
  return context().regs.description();
}

auto model_context_view::part() const -> mcu_description
{
  return context().part;
}

auto model_context_view::peek(std::uint32_t offset) const -> std::uint32_t
{
  auto const& regs = context().regs;
  if(offset % 4 != 0 or offset >= regs.description().size)
  {
    throw config_error(
        regs.description().name, 0, std::format("{:#x}", offset),
        std::format("a word offset below {:#x}", regs.description().size));
  }
  return regs.peek(offset);
}

void model_context_view::poke(std::uint32_t offset, std::uint32_t value)
{
  auto& regs = context().regs;
  if(offset % 4 != 0 or offset >= regs.description().size)
  {
    throw config_error(
        regs.description().name, 0, std::format("{:#x}", offset),
        std::format("a word offset below {:#x}", regs.description().size));
  }
  regs.poke(offset, value);
}

auto model_context_view::irq_count() const -> std::uint32_t
{
  return static_cast<std::uint32_t>(context().irqs.size());
}

void model_context_view::set_irq(std::uint32_t index, bool level)
{
  auto& line = line_at(context().irqs, index, "irq");
  if(level)
  {
    line.raise();
  }
  else
  {
    line.lower();
  }
}

auto model_context_view::irq_level(std::uint32_t index) const -> bool
{
  return line_at(context().irqs, index, "irq").level();
}

auto model_context_view::schedule(std::uint64_t delay_ps, std::uint32_t kind, std::uint64_t payload)
    -> std::uint64_t
{
  auto&      ctx = context();
  auto const now = ctx.clock.time();
  if(delay_ps > std::numeric_limits<std::uint64_t>::max() - now.ps)
  {
    throw config_error(
        ctx.regs.description().name, 0, "delay", "a due time within 2^64 picoseconds");
  }
  return ctx.events.schedule_at(vtime{now.ps + delay_ps}, kind, payload, ctx.clock);
}

void model_context_view::cancel(std::uint64_t handle)
{
  context().events.cancel(handle);
}

auto model_context_view::time_ps() const -> std::uint64_t
{
  return context().clock.time().ps;
}

auto model_context_view::instructions() const -> std::uint64_t
{
  return context().clock.now().value;
}

void model_context_view::request_stop(stop_reason reason, std::string detail)
{
  if(not is_enumerator(reason))
  {
    throw config_error("model", 0, "reason", "a stop reason");
  }
  context().request_stop(reason, std::move(detail));
}

auto model_context_view::peer(std::string const& peripheral) const -> reflex::py::nb::object
{
  return detail::python_object(context().peers.find(peripheral));
}
}
