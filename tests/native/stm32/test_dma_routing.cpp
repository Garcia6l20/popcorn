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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

class requester final : public periph::model, public stm32::dma_requester
{
public:
  explicit requester(periph::model_context& ctx)
      : context{ctx}, tx{ctx.peers, "usart1_tx_dma"}, unknown_signal{ctx.peers, "nope_dma"}
  {}

  periph::model_context&                                              context;
  stm32::dma_signal                                                   tx;
  stm32::dma_signal                                                   unknown_signal;
  std::vector<std::pair<stm32::dma_controller const*, std::uint32_t>> served;

  void transfer_completed(stm32::dma_controller const& by, std::uint32_t request) override
  {
    if(tx.carries(by, request))
    {
      served.emplace_back(&by, request);
    }
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {
    tx.resolve();
    unknown_signal.resolve();
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
  auto&      regs  = f.target->peripheral("GPDMA1");
  auto const found = popcorn::find_register(regs.description(), reg);
  REQUIRE(found != nullptr);
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
  auto const found = popcorn::find_register(unit, reg);
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

using request_log = std::vector<std::pair<std::string, std::pair<std::uint32_t, bool>>>;

class fake_controller final : public periph::model, public stm32::dma_controller
{
public:
  fake_controller(
      std::string                  name,
      std::optional<std::uint32_t> tx,
      std::shared_ptr<request_log> log)
      : name_{std::move(name)}, tx_{tx}, log_{std::move(log)}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
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
  void on_request(std::uint32_t request, bool level) override
  {
    log_->emplace_back(name_, std::pair{request, level});
  }
  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    return signal == "usart1_tx_dma" ? tx_ : std::nullopt;
  }

private:
  std::string                  name_;
  std::optional<std::uint32_t> tx_;
  std::shared_ptr<request_log> log_;
};

struct fanned
{
  std::shared_ptr<request_log> log = std::make_shared<request_log>();
  stm32_fixture                f;

  fanned(std::optional<std::uint32_t> on_gpdma, std::optional<std::uint32_t> on_lpdma)
      : f{build(on_gpdma, on_lpdma)}
  {}

  [[nodiscard]] auto controller(std::string_view name) const -> stm32::dma_controller const&
  {
    auto* const found = f.model_of<stm32::dma_controller>(name);
    REQUIRE(found != nullptr);
    return *found;
  }

private:
  auto build(std::optional<std::uint32_t> on_gpdma, std::optional<std::uint32_t> on_lpdma)
      -> stm32_fixture
  {
    auto options = popcorn::testing::stm32_options();
    options.models.emplace(
        "USART1",
        [](periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> { return std::make_shared<requester>(ctx); });
    for(auto const& [name, number] : {
            std::pair{std::string{"GPDMA1"}, on_gpdma},
            std::pair{std::string{"LPDMA1"}, on_lpdma}
    })
    {
      options.models.emplace(
          name,
          [name, number, shared = log](periph::model_context&, popcorn::peripheral_desc const&)
              -> std::shared_ptr<periph::model> {
            return std::make_shared<fake_controller>(name, number, shared);
          });
    }
    stm32_fixture built{u575(), "rcc_ll", std::move(options)};
    built.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
    return built;
  }
};

auto described_first(std::string_view a, std::string_view b) -> bool
{
  auto const& units = u575().peripherals;
  auto const  at    = [&](std::string_view name) {
    auto const found = std::ranges::find(units, name, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    return found - units.begin();
  };
  return at(a) < at(b);
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
}

TEST_CASE("a request reaches every controller knowing its signal, each with its own number")
{
  fanned const two{25u, 1u};
  auto&        tx = usart(two.f).tx;
  REQUIRE(tx.routed());
  tx.set(true);
  tx.set(false);
  auto const gpdma_first = described_first("GPDMA1", "LPDMA1");
  auto const first =
      gpdma_first ? std::pair{std::string{"GPDMA1"}, 25u} : std::pair{std::string{"LPDMA1"}, 1u};
  auto const second =
      gpdma_first ? std::pair{std::string{"LPDMA1"}, 1u} : std::pair{std::string{"GPDMA1"}, 25u};
  CHECK(
      *two.log
      == request_log{
          {first.first,  {first.second, true}  },
          {second.first, {second.second, true} },
          {first.first,  {first.second, false} },
          {second.first, {second.second, false}}
  });

  fanned const one{std::nullopt, 1u};
  usart(one.f).tx.set(true);
  CHECK(
      *one.log
      == request_log{
          {"LPDMA1", {1u, true}}
  });

  fanned const none{std::nullopt, std::nullopt};
  CHECK_FALSE(usart(none.f).tx.routed());
  usart(none.f).tx.set(true);
  CHECK(none.log->empty());
}

TEST_CASE("a completion counts only from the controller that knows the signal under that number")
{
  SUBCASE("signal known on GPDMA1 only")
  {
    fanned const rig{25u, std::nullopt};
    auto&        dev    = usart(rig.f);
    auto const&  gpdma1 = rig.controller("GPDMA1");
    auto const&  lpdma1 = rig.controller("LPDMA1");
    dev.transfer_completed(lpdma1, 25);
    CHECK(dev.served.empty());
    dev.transfer_completed(gpdma1, 25);
    CHECK(
        dev.served
        == std::vector<std::pair<stm32::dma_controller const*, std::uint32_t>>{
            {&gpdma1, 25u}
    });
  }
  SUBCASE("signal known on both under different numbers")
  {
    fanned const rig{25u, 1u};
    auto&        dev    = usart(rig.f);
    auto const&  gpdma1 = rig.controller("GPDMA1");
    auto const&  lpdma1 = rig.controller("LPDMA1");
    dev.transfer_completed(lpdma1, 25);
    dev.transfer_completed(gpdma1, 1);
    CHECK(dev.served.empty());
    dev.transfer_completed(lpdma1, 1);
    dev.transfer_completed(gpdma1, 25);
    CHECK(
        dev.served
        == std::vector<std::pair<stm32::dma_controller const*, std::uint32_t>>{
            {&lpdma1, 1u },
            {&gpdma1, 25u}
    });
  }
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
  auto  bndt = popcorn::find_register(regs.description(), std::string_view{"GPDMA_C0BR1"})->offset;
  CHECK(part > 0);
  CHECK((regs.peek(bndt) & 0xffffu) == 0);
}
