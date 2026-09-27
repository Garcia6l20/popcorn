#include "test_model.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/services.hpp>

#include <format>
#include <stdexcept>
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
  note("reset");
  if(raise_in_reset)
  {
    context_.irqs.front().raise();
  }
}

auto test_model::on_read(
    periph::register_file& regs,
    std::uint32_t          offset,
    std::uint8_t           size,
    periph::access_ctx     ctx) -> std::expected<std::uint32_t, bus_error>
{
  if(record)
  {
    accesses.push_back(
        {.offset = offset, .size = size, .write = false, .old = 0, .value = 0, .ctx = ctx});
  }
  if(fail_reads)
  {
    return std::unexpected(bus_error::denied);
  }
  if(throw_reads)
  {
    throw std::runtime_error{std::format("read {}", accesses.size())};
  }
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
    std::uint8_t           size,
    std::uint32_t          old,
    std::uint32_t          value,
    periph::access_ctx     ctx) -> std::expected<void, bus_error>
{
  if(record)
  {
    accesses.push_back(
        {.offset = offset, .size = size, .write = true, .old = old, .value = value, .ctx = ctx});
  }
  if(fail_writes)
  {
    return std::unexpected(bus_error::denied);
  }
  note(std::format("write {:#x}", offset));
  if(size == 4)
  {
    regs.poke(offset, value);
  }
  if(stop_on_write)
  {
    context_.request_stop(stop_reason::user, "model stop");
  }
  if(offset == ctrl_offset and (value & 1u) != 0)
  {
    std::ignore = context_.events.schedule(
        icount{context_.clock.now().value + go_delay.value_or(delay)}, 0, 0);
    if(patch_on_go and not context_.services.write(*patch_on_go, 2, patch_value, initiator::dma))
    {
      throw std::runtime_error{"test_model: DMA patch refused"};
    }
    if(halve_rate_on_go)
    {
      context_.clock.set_core_hz(context_.clock.core_hz() / 2);
    }
  }
  return {};
}

void test_model::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(reschedule_now)
  {
    std::ignore = context_.events.schedule(context_.clock.now(), kind, payload);
    return;
  }
  fired_at = context_.clock.now();
  note("event", event_pc);
  if(data_on_event)
  {
    context_.regs.poke(data_offset, 1);
  }
  context_.irqs.front().raise();
}

void test_model::save(std::vector<std::byte>& out) const
{
  out.push_back(std::byte{context_.irqs.front().level() ? std::uint8_t{1} : std::uint8_t{0}});
}

void test_model::load(std::span<std::byte const>)
{
  if(throw_load)
  {
    throw std::runtime_error{"test_model: load refused"};
  }
}

void test_model::note(std::string message, address pc) const
{
  if(log_messages)
  {
    context_.services.log(
        {.kind       = popcorn::core::log_kind::model_warning,
         .pc         = pc,
         .peripheral = "TESTDEV",
         .message    = std::move(message)});
  }
}

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
