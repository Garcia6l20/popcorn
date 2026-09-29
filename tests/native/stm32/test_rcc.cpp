#include "model_base.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
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
  auto const found = std::ranges::find(rcc.registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != rcc.registers.end());
  return found->offset;
}

auto bit_of(std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const& rcc = *std::ranges::find(
      u575().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  auto const found = std::ranges::find(rcc.registers, reg, &popcorn::register_desc::name);
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

TEST_CASE("every reset kind leaves its own flag in RCC_CSR, RMVF clears them")
{
  template for(constexpr auto kind :
               std::define_static_array(std::meta::enumerators_of(^^popcorn::reset_kind)))
  {
    auto f = idle_fixture();
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
