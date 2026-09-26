#include <popcorn/periph/model.hpp>

#include <popcorn/core/clock.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/services.hpp>

#include "../fake_services.hpp"

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

  void reset(popcorn::periph::register_file& regs, popcorn::reset_kind) override
  {
    current = {.isr = 0xc0};
    regs.poke(isr_offset, current.isr);
  }

  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    if(ctx.by == popcorn::initiator::dma)
    {
      return std::unexpected{popcorn::bus_error::denied};
    }
    ++current.reads;
    current.last_read = ctx.now;
    return current.isr;
  }

  auto on_write(
      popcorn::periph::register_file& regs,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t value,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    current.isr = value;
    regs.poke(isr_offset, current.isr);
    return {};
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

class no_peers final : public popcorn::periph::peer_directory
{
public:
  [[nodiscard]] auto find(std::string_view) const noexcept -> popcorn::periph::model* override
  {
    return nullptr;
  }
  [[nodiscard]] auto all() const noexcept -> std::span<popcorn::periph::model* const> override
  {
    return {};
  }
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

  model.reset(regs, popcorn::reset_kind::power_on);
  CHECK(regs.peek(isr_model::isr_offset) == 0xc0);
  CHECK(model
            .on_write(
                regs, isr_model::isr_offset, 4, regs.peek(isr_model::isr_offset), 0x40,
                {.pc = 0x0800'0100, .now = {10}, .by = popcorn::initiator::cpu})
            .has_value());
  CHECK(regs.peek(isr_model::isr_offset) == 0x40);
  CHECK(model.on_read(regs, isr_model::isr_offset, 4, {.pc = 0x0800'0104, .now = {12}}) == 0x40u);
  auto const refused = model.on_read(
      regs, isr_model::isr_offset, 4,
      {.pc = 0x0800'0108, .now = {14}, .by = popcorn::initiator::dma});
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == popcorn::bus_error::denied);
  model.on_event(isr_model::set_tc, 0x80);
  CHECK(model.current.isr == 0xc0);
  CHECK(regs.peek(isr_model::isr_offset) == model.current.isr);

  std::vector<std::byte> snapshot;
  model.save(snapshot);
  REQUIRE_FALSE(snapshot.empty());

  isr_model restored{regs};
  restored.reset(regs, popcorn::reset_kind::software);
  restored.load(snapshot);
  CHECK(restored.current.isr == 0xc0);
  CHECK(restored.current.reads == 1);
  CHECK(restored.current.last_read.value == 12);
}

TEST_CASE("a model factory registers for an IP and an IP version glob")
{
  CHECK_NOTHROW(
      popcorn::periph::register_model(
          "usart", "*",
          [](popcorn::periph::model_context& ctx,
             popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
            return std::make_shared<isr_model>(ctx.regs);
          }));
}

TEST_CASE("the context forwards a stop request to the machine services")
{
  auto const                      uart = uart_of_toy();
  popcorn::periph::register_file  regs{uart};
  popcorn::core::clock            clock{4'000'000};
  popcorn::core::scheduler        queue;
  popcorn::core::event_port       events{queue, 0};
  popcorn::testing::fake_services services;
  no_peers                        peers;
  popcorn::mcu_description const  part{};
  popcorn::periph::model_context  ctx{
      .regs     = regs,
      .irqs     = {},
      .dma      = {},
      .events   = events,
      .clock    = clock,
      .services = services,
      .peers    = peers,
      .part     = part};
  CHECK(ctx.peers.find("UART1") == nullptr);
  CHECK(ctx.peers.all().empty());
  ctx.request_stop(popcorn::stop_reason::user, "tx done");
  REQUIRE(services.stop.has_value());
  CHECK(services.stop->first == popcorn::stop_reason::user);
  CHECK(services.stop->second == "tx done");
}

static_assert(not std::is_copy_constructible_v<popcorn::periph::model>);
static_assert(not std::is_move_constructible_v<popcorn::periph::model>);

TEST_CASE("access and reset enums format by name")
{
  CHECK(std::format("{}", popcorn::initiator::dma) == "dma");
  CHECK(std::format("{}", popcorn::reset_kind::window_watchdog) == "window_watchdog");
}
