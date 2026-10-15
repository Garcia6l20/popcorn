#include "rng.hpp"

#include "saved.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>

#include <popcorn/plugin/plugin.hpp>

#include <limits>
#include <memory>
#include <string_view>

namespace native_plugin
{
namespace
{
constexpr std::uint64_t none       = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t converted  = 1;
constexpr std::uint32_t first_seed = 1;
constexpr std::size_t   saved_size = sizeof(std::uint32_t) + sizeof(std::uint64_t);

auto xorshift32(std::uint32_t word) noexcept -> std::uint32_t
{
  word ^= word << 13;
  word ^= word >> 17;
  word ^= word << 5;
  return word;
}

auto required(popcorn::peripheral_desc const& desc, std::string_view name)
    -> popcorn::register_desc const&
{
  auto const* const reg = popcorn::find_register(desc, name);
  if(reg == nullptr)
  {
    throw popcorn::config_error(desc.name, 0, std::string{name}, "a register of the RNG");
  }
  return *reg;
}

auto mask(popcorn::register_desc const& reg, std::string_view field) -> std::uint32_t
{
  for(auto const& candidate : reg.fields)
  {
    if(candidate.name == field)
    {
      return ((std::uint64_t{1} << candidate.width) - 1) << candidate.lsb;
    }
  }
  throw popcorn::config_error(reg.name, 0, std::string{field}, "a field of the RNG");
}
}

rng::rng(
    popcorn::periph::model_context& ctx,
    popcorn::peripheral_desc const& desc,
    std::uint32_t                   seed)
    : ctx_{&ctx}, cr_{&required(desc, "CR")}, sr_{&required(desc, "SR")},
      dr_{&required(desc, "DR")}, rngen_{mask(*cr_, "RNGEN")}, ie_{mask(*cr_, "IE")},
      condrst_{mask(*cr_, "CONDRST")}, drdy_{mask(*sr_, "DRDY")}, seed_{seed},
      state_{.word = seed, .pending = none}
{}

auto rng::claims() const -> std::vector<std::string>
{
  return {cr_->name, dr_->name};
}

void rng::reset(popcorn::periph::register_file& regs, popcorn::reset_kind)
{
  stop();
  state_ = {.word = seed_, .pending = none};
  regs.poke(cr_->offset, cr_->reset);
  regs.poke(dr_->offset, dr_->reset);
}

auto rng::on_read(
    popcorn::periph::register_file& regs,
    std::uint32_t                   offset,
    std::uint8_t,
    popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error>
{
  auto const word  = offset & ~3u;
  auto       value = regs.peek(word);
  if(word == dr_->offset)
  {
    value = 0;
    if((regs.peek(sr_->offset) & drdy_) != 0)
    {
      value       = regs.peek(dr_->offset);
      state_.word = value;
      regs.poke(dr_->offset, 0);
      regs.poke(sr_->offset, regs.peek(sr_->offset) & ~drdy_);
      if((regs.peek(cr_->offset) & rngen_) != 0)
      {
        start();
      }
      follow();
    }
  }
  return value >> ((offset & 3u) * 8);
}

auto rng::on_write(
    popcorn::periph::register_file& regs,
    std::uint32_t                   offset,
    std::uint8_t                    size,
    std::uint32_t,
    std::uint32_t value,
    popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error>
{
  auto const word = offset & ~3u;
  if(word != cr_->offset)
  {
    return {};
  }
  auto const shift  = (offset & 3u) * 8;
  auto const width  = size == 4 ? ~0u : (1u << (size * 8)) - 1;
  auto const before = regs.peek(word);
  auto const after  = (before & ~(width << shift)) | ((value & width) << shift);
  regs.poke(word, after);
  if((after & condrst_) != 0)
  {
    stop();
    regs.poke(sr_->offset, 0);
    regs.poke(dr_->offset, 0);
  }
  else if((after & rngen_) == 0)
  {
    stop();
  }
  else if((before & rngen_) == 0 and (regs.peek(sr_->offset) & drdy_) == 0)
  {
    start();
  }
  follow();
  return {};
}

void rng::on_event(std::uint32_t kind, std::uint64_t)
{
  if(kind != converted)
  {
    return;
  }
  state_.pending = none;
  ctx_->regs.poke(dr_->offset, xorshift32(state_.word));
  ctx_->regs.poke(sr_->offset, ctx_->regs.peek(sr_->offset) | drdy_);
  follow();
}

void rng::save(std::vector<std::byte>& out) const
{
  append(out, state_.word);
  append(out, state_.pending);
}

void rng::load(std::span<std::byte const> in)
{
  expect_size("rng", in, saved_size);
  auto const word    = take<std::uint32_t>(in);
  auto const pending = take<std::uint64_t>(in);
  state_             = {.word = word, .pending = pending};
}

void rng::start()
{
  if(state_.pending == none)
  {
    auto const due = popcorn::vtime{ctx_->clock.time().ps + conversion_ps};
    state_.pending = ctx_->events.schedule_at(due, converted, 0, ctx_->clock);
  }
}

void rng::stop()
{
  if(state_.pending != none)
  {
    ctx_->events.cancel(state_.pending);
    state_.pending = none;
  }
}

void rng::follow()
{
  if(ctx_->irqs.empty())
  {
    return;
  }
  auto const ready   = (ctx_->regs.peek(sr_->offset) & drdy_) != 0;
  auto const enabled = (ctx_->regs.peek(cr_->offset) & ie_) != 0;
  if(ready and enabled)
  {
    ctx_->irqs.front().raise();
  }
  else
  {
    ctx_->irqs.front().lower();
  }
}
}

POPCORN_PLUGIN(plugin, r)
{
  r.add_model("rng", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const& desc) {
    return std::make_shared<native_plugin::rng>(ctx, desc, native_plugin::first_seed);
  });
}
