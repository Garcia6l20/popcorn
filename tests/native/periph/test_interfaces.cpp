#include <popcorn/periph/model.hpp>

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
namespace bson = reflex::serde::bson;

class isr_model final : public popcorn::periph::model
{
public:
  struct state
  {
    std::uint32_t   isr{};
    std::uint32_t   reads{};
    popcorn::icount last_read{};
  };

  explicit isr_model(popcorn::periph::register_file& regs) : storage{regs}
  {}

  static constexpr std::uint32_t set_tc     = 1;
  static constexpr std::uint32_t isr_offset = 0;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {"ISR"};
  }

  void reset(popcorn::periph::register_file& regs, popcorn::periph::reset_kind) override
  {
    current = {.isr = 0xc0};
    regs.poke(isr_offset, current.isr);
  }

  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx ctx) -> std::uint32_t override
  {
    ++current.reads;
    current.last_read = ctx.now;
    return current.isr;
  }

  void on_write(
      popcorn::periph::register_file& regs,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t value,
      popcorn::periph::access_ctx) override
  {
    current.isr = value;
    regs.poke(isr_offset, current.isr);
  }

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == set_tc)
    {
      current.isr |= static_cast<std::uint32_t>(payload);
      storage.poke(isr_offset, current.isr);
    }
  }

  void save(std::vector<std::byte>& out) const override
  {
    bson::serializer{out}.dump(current);
  }

  void load(std::span<std::byte const> in) override
  {
    current = bson::deserializer{in}.load<state>();
  }

  state                           current;
  popcorn::periph::register_file& storage;
};

auto uart_of_toy() -> popcorn::peripheral_desc
{
  char const* const path = std::getenv("POPCORN_TOY_DESC");
  REQUIRE(path != nullptr);
  auto toy  = popcorn::load_description(path);
  auto uart = std::ranges::find(
      toy.peripherals, std::string_view{"UART1"}, &popcorn::peripheral_desc::name);
  REQUIRE(uart != toy.peripherals.end());
  return *uart;
}
}

TEST_CASE("a model claims a register of toy.json and its state round-trips")
{
  auto const                     uart = uart_of_toy();
  popcorn::periph::register_file regs{uart};
  isr_model                      model{regs};
  for(auto const& name : model.claims())
  {
    CHECK(std::ranges::contains(uart.registers, name, &popcorn::register_desc::name));
  }

  model.reset(regs, popcorn::periph::reset_kind::power_on);
  CHECK(regs.peek(isr_model::isr_offset) == 0xc0);
  model.on_write(
      regs, isr_model::isr_offset, 4, regs.peek(isr_model::isr_offset), 0x40,
      {.pc = 0x0800'0100, .now = {10}, .by = popcorn::initiator::cpu});
  CHECK(regs.peek(isr_model::isr_offset) == 0x40);
  CHECK(model.on_read(regs, isr_model::isr_offset, 4, {.pc = 0x0800'0104, .now = {12}}) == 0x40);
  model.on_event(isr_model::set_tc, 0x80);
  CHECK(model.current.isr == 0xc0);
  CHECK(regs.peek(isr_model::isr_offset) == model.current.isr);

  std::vector<std::byte> snapshot;
  model.save(snapshot);
  REQUIRE_FALSE(snapshot.empty());

  isr_model restored{regs};
  restored.reset(regs, popcorn::periph::reset_kind::software);
  restored.load(snapshot);
  CHECK(restored.current.isr == 0xc0);
  CHECK(restored.current.reads == 1);
  CHECK(restored.current.last_read.value == 12);
}

TEST_CASE("registry and context stubs throw not_implemented")
{
  CHECK_THROWS_AS(
      popcorn::periph::register_model(
          "usart", "*",
          [](popcorn::periph::model_context& ctx,
             popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
            return std::make_shared<isr_model>(ctx.regs);
          }),
      popcorn::not_implemented);
}

static_assert(not std::is_copy_constructible_v<popcorn::periph::model>);
static_assert(not std::is_move_constructible_v<popcorn::periph::model>);

TEST_CASE("access and reset enums format by name")
{
  CHECK(std::format("{}", popcorn::initiator::dma) == "dma");
  CHECK(std::format("{}", popcorn::periph::reset_kind::window_watchdog) == "window_watchdog");
}
