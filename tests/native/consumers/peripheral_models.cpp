#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/lines.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include "common.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace popcorn::consumers::models
{
class usart_shape final : public periph::model
{
public:
  explicit usart_shape(periph::model_context& ctx) : context{ctx}, kernel{ctx.clock.domain("pclk2")}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {"ISR", "TDR"};
  }

  void reset(periph::register_file& regs, reset_kind) override
  {
    regs.poke(0x1c, 0xc0);
  }

  auto on_read(periph::register_file& regs, std::uint32_t offset, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, bus_error> override
  {
    return regs.peek(offset & ~std::uint32_t{3});
  }

  auto on_write(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t      value,
      periph::access_ctx ctx) -> std::expected<void, bus_error> override
  {
    if(ctx.by == initiator::debugger)
    {
      return std::unexpected{bus_error::denied};
    }
    regs.poke(offset, value);
    auto const hz = context.clock.hz(kernel);
    if(hz == 0)
    {
      context.request_stop(stop_reason::user, "USART clock off");
      return {};
    }
    pending = context.events.schedule(icount{context.clock.now().value + 10}, 1, value);
    context.irqs.front().raise();
    context.dma.front().request();
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t payload) override
  {
    context.events.cancel(pending);
    std::array<std::byte, 4> word{};
    std::ignore = context.services.read_block(0x2000'0000, word, initiator::dma);
    std::ignore =
        context.services.write(0x2000'0004, 4, static_cast<std::uint32_t>(payload), initiator::dma);
    context.irqs.front().lower();
    context.dma.front().release();
    std::ignore = context.irqs.front().level() or context.dma.front().level();
    context.clock.set_hz(kernel, 16'000'000);
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(std::byte{0});
  }

  void load(std::span<std::byte const>) override
  {}

  periph::model_context& context;
  core::clock_domain     kernel;
  std::uint64_t          pending{};
};

auto factory() -> periph::model_factory
{
  return [](periph::model_context& ctx, peripheral_desc const&) -> std::shared_ptr<periph::model> {
    return std::make_shared<usart_shape>(ctx);
  };
}
}

TEST_CASE("peripheral model consumer wires a model by registry and by explicit option")
{
  CHECK_THROWS_AS(
      popcorn::periph::register_model("usart", "*", popcorn::consumers::models::factory()),
      popcorn::not_implemented);

  popcorn::core::machine_options options;
  options.models.emplace("UART1", popcorn::consumers::models::factory());
  popcorn::core::machine target{
      popcorn::testing::toy_description(), popcorn::testing::unused_arch_factory(), options};
  CHECK_THROWS_AS(std::ignore = target.peripheral("UART1"), popcorn::not_implemented);
  CHECK_THROWS_AS(target.reset(popcorn::reset_kind::pin), popcorn::not_implemented);
}
