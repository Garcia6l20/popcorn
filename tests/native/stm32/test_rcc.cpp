#include "kernel_gate.hpp"
#include "model_base.hpp"
#include "rcc_resets.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <meta>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint64_t mhz = 1'000'000;

auto fixed_board() -> stm32::board const&
{
  static stm32::board const board{.rcc = {.fixed_core_clock = true}};
  return board;
}

class listener final : public periph::model, public stm32::clock_listener
{
public:
  explicit listener(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;
  int                    changes{};

  [[nodiscard]] auto hz(std::string_view name) const -> std::uint64_t
  {
    return context.clock.hz(context.clock.domain(name));
  }

  void on_clock_change() override
  {
    ++changes;
  }
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, reset_kind) override
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
};

class reset_counter final : public periph::model, public periph::bus_reset_listener
{
public:
  int resets{};

  void on_bus_reset() override
  {
    ++resets;
  }
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, reset_kind) override
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
};

class write_counter final : public periph::model, public periph::bus_reset_listener
{
public:
  int resets{};
  int writes{};
  int held_writes{};

  void on_bus_reset() override
  {
    ++resets;
  }
  void on_held_write(std::uint32_t, periph::access_ctx) override
  {
    ++held_writes;
  }
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {"POL"};
  }
  void reset(periph::register_file&, reset_kind) override
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
    ++writes;
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

class kind_recorder final : public periph::model
{
public:
  std::vector<reset_kind> kinds;
  int                     writes{};

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {"POL"};
  }
  void reset(periph::register_file&, reset_kind kind) override
  {
    kinds.push_back(kind);
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
    ++writes;
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

struct reset_target
{
  std::string_view instance;
  std::string_view reg;
  std::string_view bit;
};

constexpr std::array<reset_target, 8> reset_targets{
    {
     {"CRC", "RCC_AHB1RSTR", "CRCRST"},
     {"HASH", "RCC_AHB2RSTR1", "HASHRST"},
     {"OCTOSPI1", "RCC_AHB2RSTR2", "OCTOSPI1RST"},
     {"ADC4", "RCC_AHB3RSTR", "ADC4RST"},
     {"TIM2", "RCC_APB1RSTR1", "TIM2RST"},
     {"I2C4", "RCC_APB1RSTR2", "I2C4RST"},
     {"TIM1", "RCC_APB2RSTR", "TIM1RST"},
     {"LPUART1", "RCC_APB3RSTR", "LPUART1RST"},
     }
};

struct probe_state
{
  friend auto operator==(probe_state const&, probe_state const&) -> bool = default;
};

class guarded_probe final : public stm32::dispatch_model<guarded_probe, probe_state>,
                            public periph::bus_reset_listener
{
public:
  guarded_probe(periph::model_context& ctx, popcorn::peripheral_desc const& desc);

  int guarded{};

  void reset(periph::register_file&, reset_kind) override
  {}
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void on_bus_reset() override
  {}
  void on_held_write(std::uint32_t offset, periph::access_ctx ctx) override
  {
    static_cast<void>(guard(offset & ~3u, ctx, true));
  }
  auto guard(std::uint32_t, periph::access_ctx const&, bool write) -> stm32::bus_guard
  {
    guarded += write ? 1 : 0;
    return {};
  }
};

constexpr std::array<stm32::write_entry<guarded_probe>, 1> guarded_writes{{{"POL", nullptr}}};

guarded_probe::guarded_probe(periph::model_context& ctx, popcorn::peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "probe", guarded_writes}
{}

auto with_listener(core::machine_options options = popcorn::testing::stm32_options())
    -> core::machine_options
{
  options.models.emplace(
      "CRS",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<listener>(ctx); });
  return options;
}

auto probe(stm32_fixture const& f) -> listener&
{
  auto* const found = f.model_of<listener>("CRS");
  REQUIRE(found != nullptr);
  return *found;
}

auto offset_of(std::string_view reg) -> std::uint32_t
{
  auto const& rcc = *std::ranges::find(
      u575().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  auto const found = popcorn::find_register(rcc, reg);
  REQUIRE(found != nullptr);
  return found->offset;
}

auto bit_of(std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const& rcc = *std::ranges::find(
      u575().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  auto const found = popcorn::find_register(rcc, reg);
  auto const bits  = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << bits->lsb;
}

void write(stm32_fixture const& f, std::string_view reg, std::uint32_t value)
{
  auto&      regs = f.target->peripheral("RCC");
  auto const at   = offset_of(reg);
  REQUIRE(f.target->model("RCC")
              ->on_write(regs, at, 4, regs.peek(at), value, {.pc = 0, .now = icount{0}})
              .has_value());
}

auto peek(stm32_fixture const& f, std::string_view reg) -> std::uint32_t
{
  return f.target->peripheral("RCC").peek(offset_of(reg));
}

void idle_until(stm32_fixture const& f, std::chrono::nanoseconds from_now)
{
  auto const now     = f.target->virtual_clock().time();
  auto const stopped = f.run_to_stop(
      {.max_instructions = icount{1'000'000'000},
       .until_time       = vtime{now.ps + static_cast<std::uint64_t>(from_now.count()) * 1000}});
  REQUIRE(stopped.reason == stop_reason::time);
}

auto idle_fixture(core::machine_options options = with_listener()) -> stm32_fixture
{
  stm32_fixture f{u575(), "rcc_ll", std::move(options)};
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  return f;
}

auto booted_log(core::machine_options options = with_listener()) -> std::vector<std::uint32_t>
{
  stm32_fixture f{u575(), "rcc_ll", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f.log();
}
}

TEST_CASE("LL clock setup reaches 160 MHz on PLL1 with every ready flag after its delay")
{
  stm32_fixture f{u575(), "rcc_ll", with_listener()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 10);
  CHECK(log[0] == 0);
  for(std::size_t step = 1; step < 3; ++step)
  {
    CAPTURE(step);
    CHECK(log[step] > 0);
    CHECK(log[step] <= 400);
  }
  for(std::size_t step = 3; step < 7; ++step)
  {
    CAPTURE(step);
    CHECK(log[step] > 0);
    CHECK(log[step] <= 16'000);
  }
  CHECK(log[7] == 160 * mhz);
  CHECK(log[8] == 160 * mhz);
  CHECK(log[9] == 160 * mhz);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 3);
  CHECK(f.target->virtual_clock().instruction_hz() == 160 * mhz);
  auto const& clocks = probe(f);
  CHECK(clocks.hz("sysclk") == 160 * mhz);
  CHECK(clocks.hz("hclk") == 160 * mhz);
  CHECK(clocks.hz("pclk2") == 160 * mhz);
  CHECK(clocks.hz("usart1") == 160 * mhz);
  CHECK(clocks.hz("usart2") == 0);
  CHECK(clocks.hz("hse") == 16 * mhz);
  CHECK(clocks.hz("lse") == 32'768);
  CHECK(clocks.hz("pll2_p") == 48 * mhz);
  CHECK(clocks.hz("pll3_p") == 24 * mhz);
  CHECK(clocks.hz("tim1") == 0);
  CHECK(booted_log() == log);
}

TEST_CASE("a fixed core clock keeps the instruction rate while the domains follow the tree")
{
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(fixed_board(), u575());
  stm32_fixture f{u575(), "rcc_ll", with_listener(std::move(options))};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.target->virtual_clock().instruction_hz() == 4 * mhz);
  CHECK(probe(f).hz("sysclk") == 160 * mhz);
}

TEST_CASE("the RCC factory copies its options: a board changed afterwards does not reach it")
{
  stm32::board board{.rcc = {.fixed_core_clock = true}};
  auto         options       = popcorn::testing::stm32_options();
  options.models             = stm32::factories(board, u575());
  board.rcc.fixed_core_clock = false;
  stm32_fixture f{u575(), "rcc_ll", with_listener(std::move(options))};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.target->virtual_clock().instruction_hz() == 4 * mhz);
  CHECK(probe(f).hz("sysclk") == 160 * mhz);
}

TEST_CASE("machines built from factories outlive the board they were made from")
{
  auto options = popcorn::testing::stm32_options();
  {
    stm32::board const board{.rcc = {.fixed_core_clock = true}};
    options.models = stm32::factories(board, u575());
  }
  stm32_fixture f{u575(), "rcc_ll", with_listener(std::move(options))};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.target->virtual_clock().instruction_hz() == 4 * mhz);
  CHECK(probe(f).hz("sysclk") == 160 * mhz);
}

TEST_CASE("every machine reset kind leaves its own flag in RCC_CSR, a peripheral reset none")
{
  template for(constexpr auto kind :
               std::define_static_array(std::meta::enumerators_of(^^popcorn::reset_kind)))
  {
    auto f = idle_fixture();
    if constexpr([:kind:] == reset_kind::peripheral)
    {
      auto const before = peek(f, "RCC_CSR");
      CHECK_THROWS_AS(f.target->reset([:kind:]), popcorn::config_error);
      popcorn::testing::rcc_reset_pulse(f, "RCC_AHB2RSTR1", "RNGRST");
      CHECK(peek(f, "RCC_CSR") == before);
      continue;
    }
    f.target->reset([:kind:]);
    CAPTURE(std::meta::identifier_of(kind));
    auto const csr    = peek(f, "RCC_CSR");
    auto const pin    = bit_of("RCC_CSR", "PINRSTF");
    auto const bor    = bit_of("RCC_CSR", "BORRSTF");
    auto       expect = std::uint32_t{};
    switch([:kind:])
    {
      case reset_kind::power_on:
      case reset_kind::brown_out:
        expect = bor | pin;
        break;
      case reset_kind::pin:
        expect = pin | bor;
        break;
      case reset_kind::software:
        expect = bit_of("RCC_CSR", "SFTRSTF") | bor | pin;
        break;
      case reset_kind::watchdog:
        expect = bit_of("RCC_CSR", "IWDGRSTF") | bor | pin;
        break;
      case reset_kind::window_watchdog:
        expect = bit_of("RCC_CSR", "WWDGRSTF") | bor | pin;
        break;
      case reset_kind::low_power:
        expect = bit_of("RCC_CSR", "LPWRRSTF") | bor | pin;
        break;
      case reset_kind::option_load:
        expect = bit_of("RCC_CSR", "OBLRSTF") | bor | pin;
        break;
      case reset_kind::peripheral:
        break;
    }
    CHECK((csr & 0xfe00'0000u) == expect);
    write(f, "RCC_CSR", csr | bit_of("RCC_CSR", "RMVF"));
    CHECK((peek(f, "RCC_CSR") & 0xff80'0000u) == 0);
    f.target->reset(reset_kind::software);
    CHECK((peek(f, "RCC_CSR") & 0xfe00'0000u) == bit_of("RCC_CSR", "SFTRSTF"));
  }
}

TEST_CASE("HSE off then on inside the delay readies once, the stale event is ignored")
{
  auto f = idle_fixture();
  write(f, "RCC_CR", peek(f, "RCC_CR") | bit_of("RCC_CR", "HSEON"));
  idle_until(f, std::chrono::microseconds{60});
  write(f, "RCC_CR", peek(f, "RCC_CR") & ~bit_of("RCC_CR", "HSEON"));
  write(f, "RCC_CR", peek(f, "RCC_CR") | bit_of("RCC_CR", "HSEON"));
  idle_until(f, std::chrono::microseconds{50});
  CHECK(f.field("RCC", "RCC_CR", "HSERDY") == 0);
  idle_until(f, std::chrono::microseconds{60});
  CHECK(f.field("RCC", "RCC_CR", "HSERDY") == 1);
  CHECK(probe(f).hz("hse") == 16 * mhz);
}

TEST_CASE("the backup domain keeps LSE ready across every reset but power-on")
{
  auto f = idle_fixture();
  write(f, "RCC_BDCR", bit_of("RCC_BDCR", "LSEON"));
  idle_until(f, std::chrono::microseconds{110});
  REQUIRE(f.field("RCC", "RCC_BDCR", "LSERDY") == 1);
  auto& clocks  = probe(f);
  auto  changes = clocks.changes;
  f.target->reset(reset_kind::software);
  CHECK(clocks.changes == changes);
  CHECK(f.field("RCC", "RCC_BDCR", "LSEON") == 1);
  CHECK(f.field("RCC", "RCC_BDCR", "LSERDY") == 1);
  CHECK(clocks.hz("lse") == 32'768);
  f.target->reset(reset_kind::power_on);
  CHECK(peek(f, "RCC_BDCR") == 0);
  CHECK(clocks.hz("lse") == 0);
}

TEST_CASE("a clock listener hears each published change once and never from reset")
{
  auto  f       = idle_fixture();
  auto& clocks  = probe(f);
  auto  changes = clocks.changes;
  auto  cfgr2   = peek(f, "RCC_CFGR2");
  write(f, "RCC_CFGR2", cfgr2 | (8u << 0));
  CHECK(clocks.changes == changes + 1);
  CHECK(clocks.hz("hclk") == 2 * mhz);
  write(f, "RCC_CFGR2", cfgr2 | (8u << 0));
  CHECK(clocks.changes == changes + 1);
  f.target->reset(reset_kind::pin);
  CHECK(clocks.changes == changes + 1);
  CHECK(clocks.hz("hclk") == 4 * mhz);
}

TEST_CASE("a snapshot taken while PLL1 locks restores to the same run, in place and serialized")
{
  stm32_fixture f{u575(), "rcc_ll", with_listener()};
  while(f.field("RCC", "RCC_CR", "PLL1ON") == 0)
  {
    REQUIRE_FALSE(f.run({.max_instructions = icount{8}}).stopped.has_value());
  }
  REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 0);
  auto const saved = f.target->snapshot();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const expected = f.log();
  f.target->restore(saved);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.log() == expected);

  auto const    bytes = core::encode_snapshot(saved.data());
  stm32_fixture other{u575(), "rcc_ll", with_listener()};
  other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  REQUIRE(other.run_to_stop().reason == stop_reason::user);
  CHECK(other.log() == expected);
}

TEST_CASE("the RCC model state at the 160 MHz point saves the bytes captured before the template")
{
  constexpr auto expected = std::to_array<unsigned char>(
      {0xcd, 0x00, 0x00, 0x00, 0x04, 0x67, 0x65, 0x6e, 0x65, 0x72, 0x61, 0x74, 0x69, 0x6f, 0x6e,
       0x00, 0x8b, 0x00, 0x00, 0x00, 0x12, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x12, 0x31, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x32, 0x00,
       0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x33, 0x00, 0x01, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x12, 0x35, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x36, 0x00, 0x01,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x37, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x12, 0x38, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12,
       0x39, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x31, 0x30, 0x00, 0x02,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x31, 0x31, 0x00, 0x02, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x70, 0x65, 0x6e, 0x64, 0x69, 0x6e, 0x67, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x6c, 0x73, 0x69, 0x5f, 0x66, 0x6f, 0x72,
       0x63, 0x65, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x68, 0x73, 0x69, 0x31, 0x36, 0x5f,
       0x68, 0x65, 0x6c, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  stm32_fixture f{u575(), "rcc_ll", with_listener()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(probe(f).hz("sysclk") == 160 * mhz);
  std::vector<std::byte> bytes;
  f.target->model("RCC")->save(bytes);
  CHECK(std::ranges::equal(bytes, expected, {}, [](std::byte b) {
    return std::to_integer<unsigned char>(b);
  }));
}

TEST_CASE("timer domains run at twice their APB clock when the APB prescaler divides")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  write(f, "RCC_APB1ENR1", bit_of("RCC_APB1ENR1", "TIM2EN"));
  CHECK(clocks.hz("tim2") == 4 * mhz);
  CHECK(clocks.hz("tim1") == 0);
  write(f, "RCC_CFGR2", peek(f, "RCC_CFGR2") | (4u << 4));
  CHECK(clocks.hz("pclk1") == 2 * mhz);
  CHECK(clocks.hz("tim2") == 4 * mhz);
  write(f, "RCC_CFGR2", (peek(f, "RCC_CFGR2") & ~(7u << 4)) | (5u << 4));
  CHECK(clocks.hz("pclk1") == 1 * mhz);
  CHECK(clocks.hz("tim2") == 2 * mhz);
}

TEST_CASE("PLL1 P, Q and R each divide the VCO by their own divider and follow their own enable")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  REQUIRE(clocks.hz("sysclk") == 4 * mhz);
  write(
      f, "RCC_PLL1CFGR",
      bit_of("RCC_PLL1CFGR", "PLL1SRC")
          | bit_of("RCC_PLL1CFGR", "PLL1PEN")
          | bit_of("RCC_PLL1CFGR", "PLL1QEN")
          | bit_of("RCC_PLL1CFGR", "PLL1REN"));
  write(
      f, "RCC_PLL1DIVR",
      7u * bit_of("RCC_PLL1DIVR", "PLL1N")
          | 1u * bit_of("RCC_PLL1DIVR", "PLL1P")
          | 3u * bit_of("RCC_PLL1DIVR", "PLL1Q")
          | 7u * bit_of("RCC_PLL1DIVR", "PLL1R"));
  write(f, "RCC_CR", peek(f, "RCC_CR") | bit_of("RCC_CR", "PLL1ON"));
  idle_until(f, std::chrono::microseconds{150});
  REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
  CHECK(clocks.hz("pll1_p") == 16 * mhz);
  CHECK(clocks.hz("pll1_q") == 8 * mhz);
  CHECK(clocks.hz("pll1_r") == 4 * mhz);
  write(f, "RCC_PLL1CFGR", peek(f, "RCC_PLL1CFGR") & ~bit_of("RCC_PLL1CFGR", "PLL1PEN"));
  CHECK(clocks.hz("pll1_p") == 0);
  CHECK(clocks.hz("pll1_q") == 8 * mhz);
  CHECK(clocks.hz("pll1_r") == 4 * mhz);
}

TEST_CASE("USART1SEL picks the USART1 kernel clock between pclk2 and sysclk")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  write(f, "RCC_APB2ENR", bit_of("RCC_APB2ENR", "USART1EN"));
  write(f, "RCC_CFGR2", peek(f, "RCC_CFGR2") | 4u * bit_of("RCC_CFGR2", "PPRE2"));
  REQUIRE(clocks.hz("sysclk") == 4 * mhz);
  REQUIRE(clocks.hz("pclk2") == 2 * mhz);
  CHECK(clocks.hz("usart1") == 2 * mhz);
  write(f, "RCC_CCIPR1", 1u * bit_of("RCC_CCIPR1", "USART1SEL"));
  CHECK(clocks.hz("usart1") == 4 * mhz);
}

TEST_CASE("a U575 RCC description without PPRE2 or PPRE3 is a config_error")
{
  for(auto const name : {"PPRE2", "PPRE3"})
  {
    CAPTURE(name);
    auto desc = u575();
    auto rcc  = std::ranges::find(
        desc.peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
    REQUIRE(rcc != desc.peripherals.end());
    auto removed = std::size_t{};
    for(auto& reg : rcc->registers)
    {
      removed += std::erase_if(
          reg.fields, [&](popcorn::field_desc const& bits) { return bits.name == name; });
    }
    REQUIRE(removed > 0);
    CHECK_THROWS_AS(
        stm32_fixture(std::move(desc), "rcc_ll", with_listener()), popcorn::config_error);
  }
}

TEST_CASE("the oscillator driving the system clock cannot be switched off")
{
  auto                          f = idle_fixture();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  write(f, "RCC_CR", peek(f, "RCC_CR") & ~bit_of("RCC_CR", "MSISON"));
  CHECK(f.field("RCC", "RCC_CR", "MSISON") == 1);
  CHECK(f.field("RCC", "RCC_CR", "MSISRDY") == 1);
  REQUIRE(logged.size() == 1);
  CHECK(logged.front().message.contains("MSISON = 0"));
  write(f, "RCC_CR", peek(f, "RCC_CR") & ~bit_of("RCC_CR", "MSISON"));
  CHECK(logged.size() == 1);
  write(f, "RCC_CR", peek(f, "RCC_CR") & ~bit_of("RCC_CR", "MSIKON"));
  CHECK(f.field("RCC", "RCC_CR", "MSIKON") == 0);
  CHECK(f.field("RCC", "RCC_CR", "MSIKRDY") == 0);
  CHECK(probe(f).hz("msik") == 0);
}

TEST_CASE("PLL1 driving the system clock keeps its configuration against a source write")
{
  stm32_fixture f{u575(), "rcc_ll", with_listener()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  write(f, "RCC_PLL1CFGR", peek(f, "RCC_PLL1CFGR") & ~(bit_of("RCC_PLL1CFGR", "PLL1SRC") * 3u));
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1SRC") == 1);
  write(f, "RCC_PLL1DIVR", 0);
  CHECK(f.field("RCC", "RCC_PLL1DIVR", "PLL1N") == 79);
  write(f, "RCC_PLL1CFGR", peek(f, "RCC_PLL1CFGR") & ~bit_of("RCC_PLL1CFGR", "PLL1REN"));
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1REN") == 1);
  CHECK(probe(f).hz("sysclk") == 160 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 160 * mhz);
  REQUIRE(logged.size() == 1);
  CHECK(logged.front().kind == core::log_kind::model_warning);
  CHECK(logged.front().peripheral == "RCC");
  CHECK(logged.front().reg == "RCC_PLL1CFGR");
  CHECK(logged.front().message.contains("stop the core clock"));
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  idle_until(f, std::chrono::microseconds{10});
}

TEST_CASE("a switch to a source that is not ready happens after its ready event")
{
  auto f = idle_fixture();
  write(f, "RCC_CFGR1", bit_of("RCC_CFGR1", "SW"));
  idle_until(f, std::chrono::microseconds{150});
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 0);
  write(f, "RCC_CR", peek(f, "RCC_CR") | bit_of("RCC_CR", "HSION"));
  idle_until(f, std::chrono::microseconds{150});
  CHECK(f.field("RCC", "RCC_CR", "HSIRDY") == 1);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 0);
  idle_until(f, std::chrono::microseconds{60});
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 1);
  CHECK(probe(f).hz("sysclk") == 16 * mhz);
}

TEST_CASE("RTCSEL locks once set and BDRST clears the backup domain until released")
{
  auto       f   = idle_fixture();
  auto const lse = bit_of("RCC_BDCR", "RTCSEL");
  write(f, "RCC_BDCR", bit_of("RCC_BDCR", "LSEON") | lse);
  write(f, "RCC_BDCR", bit_of("RCC_BDCR", "LSEON") | 2u * lse);
  CHECK(f.field("RCC", "RCC_BDCR", "RTCSEL") == 1);
  write(f, "RCC_BDCR", peek(f, "RCC_BDCR") | bit_of("RCC_BDCR", "BDRST"));
  CHECK(peek(f, "RCC_BDCR") == bit_of("RCC_BDCR", "BDRST"));
  write(f, "RCC_BDCR", 0);
  write(f, "RCC_BDCR", 2u * lse);
  CHECK(f.field("RCC", "RCC_BDCR", "RTCSEL") == 2);
}

TEST_CASE("a ready delay pending across a core clock change ends at its virtual time")
{
  auto f = idle_fixture();
  write(f, "RCC_CR", peek(f, "RCC_CR") | bit_of("RCC_CR", "HSION"));
  auto const start = f.target->virtual_clock().time().ps;
  idle_until(f, std::chrono::microseconds{10});
  write(
      f, "RCC_ICSCR1",
      (peek(f, "RCC_ICSCR1") & ~(0xFu << 28)) | (1u << 28) | bit_of("RCC_ICSCR1", "MSIRGSEL"));
  REQUIRE(f.target->virtual_clock().core_hz() == 24 * mhz);
  auto const stopped =
      f.run_to_stop({.max_instructions = icount{100'000'000}, .stop_when = [&] {
                       return (peek(f, "RCC_CR") & bit_of("RCC_CR", "HSIRDY")) != 0;
                     }});
  REQUIRE(stopped.reason == stop_reason::user);
  auto const took = f.target->virtual_clock().time().ps - start;
  CHECK(took >= 100 * mhz);
  CHECK(took < 100 * mhz + 1'000'000 / 24);
}

TEST_CASE("every U575 instance with an RCC reset bit resolves it, the renamed ones included")
{
  auto const& rcc = *std::ranges::find(
      u575().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  std::vector<std::string> unresolved;
  for(auto const& peripheral : u575().peripherals)
  {
    if(not peripheral.alias_of and not stm32::reset_bit_of(rcc, peripheral.name))
    {
      unresolved.push_back(peripheral.name);
    }
  }
  std::ranges::sort(unresolved);
  CHECK(unresolved == std::vector<std::string>{"DBGMCU",       "DCACHE",       "DLYBOS1",
                                               "DLYBOS2",      "DLYBSD1",      "DLYBSD2",
                                               "EXTI",         "FDCAN1_RAM",   "FLASH",
                                               "GTZC1_MPCBB1", "GTZC1_MPCBB2", "GTZC1_MPCBB3",
                                               "GTZC1_TZIC",   "GTZC1_TZSC",   "GTZC2_MPCBB4",
                                               "GTZC2_TZIC",   "GTZC2_TZSC",   "ICache",
                                               "IWDG",         "PWR",          "RCC",
                                               "RTC",          "TAMP",         "WWDG"});
  auto const described = [&](std::string_view field) {
    return std::ranges::any_of(rcc.registers, [&](popcorn::register_desc const& reg) {
      return reg.name.contains("RSTR")
         and std::ranges::contains(reg.fields, field, &popcorn::field_desc::name);
    });
  };
  auto renamed = std::size_t{};
  for(auto const& [instance, field] : stm32::reset_bit_names)
  {
    CAPTURE(instance);
    CHECK(stm32::reset_bit_name(instance) == field);
    if(not described(field))
    {
      continue;
    }
    ++renamed;
    auto const found = stm32::reset_bit_of(rcc, instance);
    REQUIRE(found.has_value());
    CHECK(found->field == field);
  }
  CHECK(renamed == 11);
  for(auto const& [instance, field] : {
          std::pair{"UCPD1",  "UCPD1RST" },
          std::pair{"FDCAN1", "FDCAN1RST"}
  })
  {
    CAPTURE(instance);
    auto const found = stm32::reset_bit_of(rcc, instance);
    REQUIRE(found.has_value());
    CHECK(found->field == field);
  }
  auto const rng = stm32::reset_bit_of(rcc, "RNG");
  REQUIRE(rng.has_value());
  CHECK(rng->reg == popcorn::find_register(rcc, "RCC_AHB2RSTR1")->name);
}

TEST_CASE("each RSTR register notifies its instance's model on a 0 to 1 write only")
{
  auto options = popcorn::testing::stm32_options();
  for(auto const& target : reset_targets)
  {
    options.models.insert_or_assign(
        std::string{target.instance},
        [](periph::model_context&, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> { return std::make_shared<reset_counter>(); });
  }
  stm32_fixture const f{u575(), "rcc_ll", std::move(options)};
  auto const          counts = [&] {
    std::vector<int> out;
    for(auto const& target : reset_targets)
    {
      auto* const counter = f.model_of<reset_counter>(target.instance);
      REQUIRE(counter != nullptr);
      out.push_back(counter->resets);
    }
    return out;
  };
  for(std::size_t index = 0; index < reset_targets.size(); ++index)
  {
    auto const& target = reset_targets[index];
    CAPTURE(target.instance);
    auto const bit = bit_of(target.reg, target.bit);
    write(f, target.reg, bit);
    CHECK(peek(f, target.reg) == bit);
    write(f, target.reg, bit);
    write(f, target.reg, 0);
    CHECK(peek(f, target.reg) == 0);
    std::vector<int> expected(reset_targets.size(), 0);
    for(std::size_t done = 0; done <= index; ++done)
    {
      expected[done] = 1;
    }
    CHECK(counts() == expected);
  }
  write(f, "RCC_AHB2RSTR1", bit_of("RCC_AHB2RSTR1", "GPIOARST"));
  CHECK(peek(f, "RCC_AHB2RSTR1") == bit_of("RCC_AHB2RSTR1", "GPIOARST"));
  CHECK(counts() == std::vector<int>(reset_targets.size(), 1));
}

TEST_CASE("ADC12RST notifies both the ADC1 and the ADC12 models")
{
  auto options = popcorn::testing::stm32_options();
  for(auto const instance : {"ADC1", "ADC12"})
  {
    options.models.insert_or_assign(
        instance,
        [](periph::model_context&, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> { return std::make_shared<reset_counter>(); });
  }
  stm32_fixture const f{u575(), "rcc_ll", std::move(options)};
  write(f, "RCC_AHB2RSTR1", bit_of("RCC_AHB2RSTR1", "ADC12RST"));
  for(auto const instance : {"ADC1", "ADC12"})
  {
    CAPTURE(instance);
    auto* const counter = f.model_of<reset_counter>(instance);
    REQUIRE(counter != nullptr);
    CHECK(counter->resets == 1);
  }
}

TEST_CASE("a guarded listener runs its bus guard on a write ignored while held in reset")
{
  auto options = popcorn::testing::stm32_options();
  options.models.insert_or_assign(
      "CRC",
      [](periph::model_context& ctx, popcorn::peripheral_desc const& desc)
          -> std::shared_ptr<periph::model> { return std::make_shared<guarded_probe>(ctx, desc); });
  stm32_fixture const f{u575(), "rcc_ll", std::move(options)};
  auto* const         probe = f.model_of<guarded_probe>("CRC");
  REQUIRE(probe != nullptr);
  auto const& desc = f.target->peripheral("CRC").description();
  auto const* pol  = popcorn::find_register(desc, "POL");
  REQUIRE(pol != nullptr);
  REQUIRE(f.target->mem().write(desc.base + pol->offset, 4, 0x1021).has_value());
  CHECK(probe->guarded == 1);
  write(f, "RCC_AHB1RSTR", bit_of("RCC_AHB1RSTR", "CRCRST"));
  REQUIRE(f.target->mem().write(desc.base + pol->offset, 4, 0x8005).has_value());
  CHECK(probe->guarded == 2);
  CHECK(f.target->peripheral("CRC").peek(pol->offset) == 0x1021);
}

TEST_CASE("RCC claims each RSTR register once, with and without TrustZone")
{
  for(bool const trustzone : {false, true})
  {
    CAPTURE(trustzone);
    auto options = popcorn::testing::stm32_options();
    if(trustzone)
    {
      REQUIRE(stm32::configure_option_bytes(u575(), {.tzen = true}, options).has_value());
    }
    stm32_fixture const f{u575(), "rcc_ll", std::move(options)};
    auto const          claims = f.target->model("RCC")->claims();
    for(auto const name : stm32::reset_registers)
    {
      CAPTURE(name);
      auto const* const found =
          popcorn::find_register(f.target->peripheral("RCC").description(), name);
      REQUIRE(found != nullptr);
      CHECK(std::ranges::count(claims, found->name) == 1);
    }
  }
}

TEST_CASE("an RCC reset returns a modelled register to its reset value and holds writes off")
{
  struct target
  {
    std::string_view instance;
    std::string_view reg;
    std::string_view reset_reg;
    std::string_view reset_bit;
    std::uint32_t    value;
    std::uint32_t    reset;
    bool             storage = false;
  };
  constexpr std::array<target, 17> targets{
      {
       {"CRC", "POL", "RCC_AHB1RSTR", "CRCRST", 0x1021, 0x04C1'1DB7},
       {"HASH", "IMR", "RCC_AHB2RSTR1", "HASHRST", 0x1, 0},
       {"SYSCFG", "SYSCFG_FPUIMR", "RCC_APB3RSTR", "SYSCFGRST", 0, 0x1F, true},
       {"SYSCFG", "SYSCFG_CSLCKR", "RCC_APB3RSTR", "SYSCFGRST", 0x1, 0x1, true},
       {"SYSCFG", "SYSCFG_CNSLCKR", "RCC_APB3RSTR", "SYSCFGRST", 0x1, 0x1, true},
       {"SYSCFG", "SYSCFG_CFGR2", "RCC_APB3RSTR", "SYSCFGRST", 0x1, 0x1, true},
       {"ADC1", "ADC_IER", "RCC_AHB2RSTR1", "ADC12RST", 0x1, 0},
       {"ADC1", "ADC_SMPR1", "RCC_AHB2RSTR1", "ADC12RST", 0x7, 0},
       {"GPDMA1", "GPDMA_PRIVCFGR", "RCC_AHB1RSTR", "GPDMA1RST", 0x1, 0},
       {"TIM2", "AF2", "RCC_APB1RSTR1", "TIM2RST", 0x1, 0},
       {"SAI1", "ACR2", "RCC_APB2RSTR", "SAI1RST", 0x1, 0},
       {"MDF1", "BSMX0CR", "RCC_AHB1RSTR", "MDF1RST", 0x1, 0},
       {"ADF1", "ADF_BSMX0CR", "RCC_AHB3RSTR", "ADF1RST", 0x1, 0},
       {"ADC12", "ADC12_CCR", "RCC_AHB2RSTR1", "ADC12RST", 0x0004'0000, 0},
       {"GPDMA1", "GPDMA_C0LLR", "RCC_AHB1RSTR", "GPDMA1RST", 0x4, 0},
       {"LPDMA1", "LPDMA_C0LLR", "RCC_AHB3RSTR", "LPDMA1RST", 0x4, 0},
       {"TIM2", "ARR", "RCC_APB1RSTR1", "TIM2RST", 0x100, 0xFFFF'FFFF},
       }
  };
  auto const f = idle_fixture();
  for(auto const& t : targets)
  {
    CAPTURE(t.instance);
    CAPTURE(t.reg);
    auto const& desc  = f.target->peripheral(t.instance).description();
    auto const* found = popcorn::find_register(desc, t.reg);
    REQUIRE(found != nullptr);
    auto const at    = desc.base + found->offset;
    auto&      regs  = f.target->peripheral(t.instance);
    auto const store = [&](std::uint32_t value) {
      if(t.storage)
      {
        regs.poke(found->offset, value);
        return;
      }
      REQUIRE(f.target->mem().write(at, 4, value).has_value());
    };
    auto const load = [&] {
      if(t.storage)
      {
        return regs.peek(found->offset);
      }
      auto const value = f.target->mem().read(at, 4);
      REQUIRE(value.has_value());
      return static_cast<std::uint32_t>(*value);
    };
    store(t.value);
    REQUIRE(load() == t.value);
    write(f, t.reset_reg, bit_of(t.reset_reg, t.reset_bit));
    CHECK(load() == t.reset);
    if(not t.storage)
    {
      store(t.value);
      CHECK(load() == t.reset);
    }
    write(f, t.reset_reg, 0);
    store(t.value);
    CHECK(load() == t.value);
  }
}

TEST_CASE("a part whose RCC lacks ADC12RST is a config_error naming that bit")
{
  auto       description = u575();
  auto const rcc         = std::ranges::find(
      description.peripherals, std::string{"RCC"}, &popcorn::peripheral_desc::name);
  REQUIRE(rcc != description.peripherals.end());
  auto const resets =
      std::ranges::find(rcc->registers, std::string{"AHB2RSTR1"}, &popcorn::register_desc::name);
  REQUIRE(resets != rcc->registers.end());
  REQUIRE(
      std::erase_if(
          resets->fields, [](popcorn::field_desc const& field) { return field.name == "ADC12RST"; })
      == 1);
  CHECK_THROWS_WITH_AS(
      (stm32_fixture{description, "rcc_ll"}),
      doctest::Contains("peripherals.RCC.registers.RCC_xxxRSTR.fields.ADC12RST"),
      popcorn::config_error);
}

TEST_CASE("LPTIM1RST resets an unmodelled peripheral's storage and holds its writes")
{
  auto const  f        = idle_fixture();
  auto const& desc     = f.target->peripheral("LPTIM1").description();
  auto const* cfgr     = popcorn::find_register(desc, "CFGR");
  auto&       services = probe(f).context.services;
  REQUIRE(cfgr != nullptr);
  auto const store = [&](std::uint32_t value, popcorn::initiator by = popcorn::initiator::cpu) {
    REQUIRE(services.write(desc.base + cfgr->offset, 4, value, by).has_value());
  };
  auto const load = [&] { return f.target->peripheral("LPTIM1").peek(cfgr->offset); };
  store(0x10);
  REQUIRE(load() == 0x10);
  popcorn::testing::rcc_reset_write(f, "RCC_APB3RSTR", "LPTIM1RST", 1);
  CHECK(load() == 0);
  for(auto const by :
      {popcorn::initiator::cpu, popcorn::initiator::debugger, popcorn::initiator::dma})
  {
    CAPTURE(static_cast<int>(by));
    store(0x10, by);
    CHECK(load() == 0);
  }
  auto const read = services.read(desc.base + cfgr->offset, 4, popcorn::initiator::cpu);
  REQUIRE(read.has_value());
  CHECK(*read == 0);
  f.target->peripheral("LPTIM1").poke(cfgr->offset, 0x5);
  services.hold_in_reset("LPTIM1", true);
  CHECK(load() == 0x5);
  popcorn::testing::rcc_reset_write(f, "RCC_APB3RSTR", "LPTIM1RST", 0);
  store(0x10);
  CHECK(load() == 0x10);

  popcorn::testing::rcc_reset_write(f, "RCC_APB3RSTR", "LPTIM1RST", 1);
  f.target->peripheral("LPTIM1").poke(cfgr->offset, 0x5);
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::software);
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  store(0x20);
  CHECK(load() == 0x20);
  f.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(load() == 0x5);
  store(0x30);
  CHECK(load() == 0x5);
}

TEST_CASE("an RCC whose reset register has another family's name binds it and resets its bits")
{
  auto       description = u575();
  auto const rcc         = std::ranges::find(
      description.peripherals, std::string{"RCC"}, &popcorn::peripheral_desc::name);
  REQUIRE(rcc != description.peripherals.end());
  auto const reg =
      std::ranges::find(rcc->registers, std::string{"APB1RSTR1"}, &popcorn::register_desc::name);
  REQUIRE(reg != rcc->registers.end());
  reg->name    = "APB1LRSTR";
  reg->aliases = std::vector<std::string>{"RCC_APB1LRSTR"};
  stm32_fixture const f{description, "rcc_ll"};
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  auto const& tim2 = f.target->peripheral("TIM2").description();
  auto const* arr  = popcorn::find_register(tim2, "ARR");
  REQUIRE(arr != nullptr);
  REQUIRE(f.target->mem().write(tim2.base + arr->offset, 4, 0x100).has_value());
  REQUIRE(f.target->peripheral("TIM2").peek(arr->offset) == 0x100);
  popcorn::testing::rcc_reset_pulse(f, "RCC_APB1LRSTR", "TIM2RST");
  CHECK(f.target->peripheral("TIM2").peek(arr->offset) == 0xFFFF'FFFF);
}

TEST_CASE("a peripheral held in reset before the machine has its peripherals is a config_error")
{
  auto options = with_listener();
  options.models.insert_or_assign(
      "CORDIC",
      [](periph::model_context& ctx,
         popcorn::peripheral_desc const&) -> std::shared_ptr<periph::model> {
        ctx.services.hold_in_reset("LPTIM1", true);
        return std::make_shared<reset_counter>();
      });
  CHECK_THROWS_AS((stm32_fixture{u575(), "rcc_ll", std::move(options)}), popcorn::config_error);
}

TEST_CASE("a plain model held in reset gets on_held_write instead of on_write, once per write")
{
  auto options = popcorn::testing::stm32_options();
  options.models.insert_or_assign(
      "CRC",
      [](periph::model_context&, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<write_counter>(); });
  stm32_fixture const f{u575(), "rcc_ll", std::move(options)};
  auto* const         counter = f.model_of<write_counter>("CRC");
  REQUIRE(counter != nullptr);
  auto const& desc = f.target->peripheral("CRC").description();
  auto const* pol  = popcorn::find_register(desc, "POL");
  REQUIRE(pol != nullptr);
  auto const store = [&] {
    REQUIRE(f.target->mem().write(desc.base + pol->offset, 4, 0x1021).has_value());
  };
  store();
  REQUIRE(counter->writes == 1);
  popcorn::testing::rcc_reset_write(f, "RCC_AHB1RSTR", "CRCRST", 1);
  CHECK(counter->resets == 1);
  store();
  store();
  CHECK(counter->writes == 1);
  CHECK(counter->held_writes == 2);
  popcorn::testing::rcc_reset_write(f, "RCC_AHB1RSTR", "CRCRST", 1);
  CHECK(counter->resets == 1);
  popcorn::testing::rcc_reset_write(f, "RCC_AHB1RSTR", "CRCRST", 0);
  store();
  CHECK(counter->writes == 2);
}

TEST_CASE("a model that is no bus reset listener gets reset with the peripheral kind, writes held")
{
  auto options = popcorn::testing::stm32_options();
  options.models.insert_or_assign(
      "CRC",
      [](periph::model_context&, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<kind_recorder>(); });
  stm32_fixture const f{u575(), "rcc_ll", std::move(options)};
  auto* const         model = f.model_of<kind_recorder>("CRC");
  REQUIRE(model != nullptr);
  model->kinds.clear();
  auto const& desc = f.target->peripheral("CRC").description();
  auto const* pol  = popcorn::find_register(desc, "POL");
  REQUIRE(pol != nullptr);
  popcorn::testing::rcc_reset_write(f, "RCC_AHB1RSTR", "CRCRST", 1);
  CHECK(model->kinds == std::vector{reset_kind::peripheral});
  REQUIRE(f.target->mem().write(desc.base + pol->offset, 4, 0x1021).has_value());
  CHECK(model->writes == 0);
  popcorn::testing::rcc_reset_write(f, "RCC_AHB1RSTR", "CRCRST", 0);
  REQUIRE(f.target->mem().write(desc.base + pol->offset, 4, 0x1021).has_value());
  CHECK(model->writes == 1);
}
