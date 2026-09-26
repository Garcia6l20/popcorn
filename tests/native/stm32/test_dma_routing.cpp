#include "dma.hpp"
#include "dma_signal.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr popcorn::address source      = 0x2001'0000;
constexpr popcorn::address destination = 0x2001'1000;
constexpr std::uint32_t    length      = 16;

class requester final : public periph::model
{
public:
  explicit requester(periph::model_context& ctx)
      : context{ctx}, tx{ctx.peers, "usart1_tx_dma"}, unknown_signal{ctx.peers, "nope_dma"},
        unknown_controller{ctx.peers, "usart1_tx_dma", "GPDMA9"}
  {}

  periph::model_context& context;
  stm32::dma_signal      tx;
  stm32::dma_signal      unknown_signal;
  stm32::dma_signal      unknown_controller;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {
    tx.resolve();
    unknown_signal.resolve();
    unknown_controller.resolve();
  }
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

auto machine() -> stm32_fixture
{
  auto options = popcorn::testing::stm32_options();
  options.models.emplace(
      "USART1",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<requester>(ctx); });
  stm32_fixture f{u575(), "rcc_ll", std::move(options)};
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  return f;
}

auto usart(stm32_fixture const& f) -> requester&
{
  auto* const found = f.model_of<requester>("USART1");
  REQUIRE(found != nullptr);
  return *found;
}

void idle_for(stm32_fixture const& f, std::chrono::nanoseconds span)
{
  auto const now = f.target->virtual_clock().time();
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000},
                     .until_time = vtime{now.ps + static_cast<std::uint64_t>(span.count()) * 1000}})
          .reason
      == stop_reason::time);
}

void write(stm32_fixture const& f, std::string_view reg, std::uint32_t value)
{
  auto&      regs = f.target->peripheral("GPDMA1");
  auto const found =
      std::ranges::find(regs.description().registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != regs.description().registers.end());
  REQUIRE(
      f.target->model("GPDMA1")
          ->on_write(
              regs, found->offset, 4, regs.peek(found->offset), value, {.pc = 0, .now = icount{0}})
          .has_value());
}

auto bit(std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const& unit = *std::ranges::find(
      u575().peripherals, std::string_view{"GPDMA1"}, &popcorn::peripheral_desc::name);
  auto const found = std::ranges::find(unit.registers, reg, &popcorn::register_desc::name);
  auto const bits  = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << bits->lsb;
}

void start_channel(stm32_fixture const& f)
{
  for(std::uint32_t index = 0; index < length; ++index)
  {
    REQUIRE(f.target->mem().write(source + index, 1, (index * 13u + 5u) & 0xffu));
    REQUIRE(f.target->mem().write(destination + index, 1, 0));
  }
  write(f, "GPDMA_C0TR1", bit("GPDMA_C0TR1", "SINC") | bit("GPDMA_C0TR1", "DINC"));
  write(f, "GPDMA_C0TR2", 25u * bit("GPDMA_C0TR2", "REQSEL") | bit("GPDMA_C0TR2", "DREQ"));
  write(f, "GPDMA_C0BR1", length);
  write(f, "GPDMA_C0SAR", source);
  write(f, "GPDMA_C0DAR", destination);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
}

auto moved(stm32_fixture const& f) -> std::uint32_t
{
  return length - f.field("GPDMA1", "GPDMA_C0BR1", "BNDT");
}

auto copied(stm32_fixture const& f) -> bool
{
  for(std::uint32_t index = 0; index < length; ++index)
  {
    auto const byte = f.target->mem().read(destination + index, 1);
    REQUIRE(byte.has_value());
    if(*byte != ((index * 13u + 5u) & 0xffu))
    {
      return false;
    }
  }
  return true;
}
}

TEST_CASE("the GPDMA request table gives RM0456 Table 137 numbers")
{
  auto        f          = machine();
  auto* const controller = dynamic_cast<stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  CHECK(controller->request_of("spi1_rx_dma") == 6u);
  CHECK(controller->request_of("usart1_tx_dma") == 25u);
  CHECK(controller->request_of("dcmi_dma") == 86u);
  CHECK(controller->request_of("pssi_dma") == 86u);
  CHECK(controller->request_of("jpeg_tx_dma") == 125u);
  CHECK_FALSE(controller->request_of("nope_dma").has_value());
}

TEST_CASE("a requester's signal resolves through peers, unknown names stay unrouted")
{
  auto f = machine();
  CHECK(usart(f).tx.routed());
  CHECK_FALSE(usart(f).unknown_signal.routed());
  CHECK_FALSE(usart(f).unknown_controller.routed());
}

TEST_CASE("a hardware request moves data while high and pauses while low")
{
  auto f = machine();
  start_channel(f);
  idle_for(f, std::chrono::microseconds{20});
  CHECK(moved(f) == 0);
  usart(f).tx.set(true);
  CHECK(moved(f) == 0);
  idle_for(f, std::chrono::nanoseconds{4'500});
  auto const part = moved(f);
  CHECK(part > 0);
  CHECK(part < length);
  usart(f).tx.set(false);
  idle_for(f, std::chrono::microseconds{20});
  CHECK(moved(f) == part);
  usart(f).tx.set(true);
  idle_for(f, std::chrono::microseconds{40});
  CHECK(moved(f) == length);
  CHECK(copied(f));
  CHECK(f.field("GPDMA1", "GPDMA_C0SR", "TCF") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C0CR", "EN") == 0);
}

TEST_CASE("a snapshot taken with the request high restores the request level with the controller")
{
  auto f = machine();
  start_channel(f);
  usart(f).tx.set(true);
  idle_for(f, std::chrono::nanoseconds{3'500});
  REQUIRE(moved(f) > 0);
  auto const saved = f.target->snapshot();
  idle_for(f, std::chrono::microseconds{40});
  REQUIRE(moved(f) == length);
  f.target->restore(saved);
  REQUIRE(moved(f) < length);
  idle_for(f, std::chrono::microseconds{40});
  CHECK(moved(f) == length);
  CHECK(copied(f));
}

TEST_CASE("a channel suspended during a request stops, then completes on resume")
{
  auto f = machine();
  start_channel(f);
  usart(f).tx.set(true);
  idle_for(f, std::chrono::nanoseconds{3'500});
  auto const part = moved(f);
  REQUIRE(part > 0);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN") | bit("GPDMA_C0CR", "SUSP"));
  CHECK(f.field("GPDMA1", "GPDMA_C0SR", "SUSPF") == 1);
  idle_for(f, std::chrono::microseconds{20});
  CHECK(moved(f) == part);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  idle_for(f, std::chrono::microseconds{40});
  CHECK(moved(f) == length);
  CHECK(copied(f));
}

TEST_CASE("a signal restored into a fresh machine that was never reset routes on first use")
{
  auto f = machine();
  start_channel(f);
  usart(f).tx.set(true);
  idle_for(f, std::chrono::nanoseconds{3'500});
  usart(f).tx.set(false);
  auto const part  = moved(f);
  auto const saved = f.target->snapshot();

  auto options = popcorn::testing::stm32_options();
  options.models.emplace(
      "USART1",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<requester>(ctx); });
  popcorn::periph::stm32::register_builtin_models();
  core::machine fresh{u575(), popcorn::arch::armv8m::factory(), std::move(options)};
  fresh.load(popcorn::testing::elf("rcc_ll"));
  fresh.restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  auto* const restored = dynamic_cast<requester*>(fresh.model("USART1"));
  REQUIRE(restored != nullptr);
  restored->tx.set(true);
  auto const now = fresh.virtual_clock().time();
  REQUIRE(fresh
              .run_slice(
                  {.max_instructions = icount{1'000'000'000},
                   .wall_budget      = std::chrono::seconds{60},
                   .until_time       = vtime{now.ps + 40'000'000}})
              .stopped.has_value());
  auto& regs = fresh.peripheral("GPDMA1");
  auto  bndt = std::ranges::find(
                   regs.description().registers, std::string_view{"GPDMA_C0BR1"},
                   &popcorn::register_desc::name)
                   ->offset;
  CHECK(part > 0);
  CHECK((regs.peek(bndt) & 0xffffu) == 0);
}
