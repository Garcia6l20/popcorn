#include "test_model.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>

#include <tuple>

namespace popcorn::testing
{
auto test_model::claims() const -> std::vector<std::string>
{
  return {"DATA", "CTRL"};
}

void test_model::reset(periph::register_file& regs, reset_kind kind)
{
  regs.poke(data_offset, 0);
  regs.poke(ctrl_offset, 0);
  last_reset = kind;
  if(raise_in_reset)
  {
    context_.irqs.front().raise();
  }
}

auto test_model::on_read(
    periph::register_file& regs,
    std::uint32_t          offset,
    std::uint8_t           size,
    periph::access_ctx) -> std::expected<std::uint32_t, bus_error>
{
  auto const word = regs.peek(offset & ~std::uint32_t{3});
  if((offset & ~std::uint32_t{3}) == data_offset)
  {
    context_.irqs.front().lower();
  }
  auto const shifted = word >> (8 * (offset % 4));
  return size == 4 ? shifted : shifted & ((std::uint32_t{1} << (8 * size)) - 1);
}

auto test_model::on_write(
    periph::register_file& regs,
    std::uint32_t          offset,
    std::uint8_t,
    std::uint32_t,
    std::uint32_t value,
    periph::access_ctx) -> std::expected<void, bus_error>
{
  regs.poke(offset & ~std::uint32_t{3}, value);
  if(offset == ctrl_offset and (value & 1u) != 0)
  {
    std::ignore = context_.events.schedule(icount{context_.clock.now().value + delay}, 0, 0);
  }
  return {};
}

void test_model::on_event(std::uint32_t, std::uint64_t)
{
  context_.irqs.front().raise();
}

void test_model::save(std::vector<std::byte>& out) const
{
  out.push_back(std::byte{context_.irqs.front().level() ? std::uint8_t{1} : std::uint8_t{0}});
}

void test_model::load(std::span<std::byte const>)
{}

auto test_model::context() const noexcept -> periph::model_context&
{
  return context_;
}

auto test_model_factory() -> periph::model_factory
{
  return [](periph::model_context& ctx, peripheral_desc const&) -> std::shared_ptr<periph::model> {
    return std::make_shared<test_model>(ctx);
  };
}
}
