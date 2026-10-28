#include "h573_secure_rig.hpp"
#include "kernel_gate.hpp"
#include "model_base.hpp"
#include "rcc_resets.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
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
#include <initializer_list>
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
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;

constexpr std::uint64_t mhz = 1'000'000;

class listener final : public periph::model
{
public:
  explicit listener(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;

  [[nodiscard]] auto hz(std::string_view name) const -> std::uint64_t
  {
    return context.clock.hz(context.clock.domain(name));
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

auto with_listener(core::machine_options options = popcorn::testing::stm32_options())
    -> core::machine_options
{
  options.models.insert_or_assign(
      "CORDIC",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<listener>(ctx); });
  return options;
}

auto probe(stm32_fixture const& f) -> listener&
{
  auto* const found = f.model_of<listener>("CORDIC");
  REQUIRE(found != nullptr);
  return *found;
}

auto rcc_of(popcorn::part_description const& part) -> popcorn::peripheral_desc const&
{
  auto const found =
      std::ranges::find(part.peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != part.peripherals.end());
  return *found;
}

auto lsb_of(std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const* const found = popcorn::find_register(rcc_of(h573()), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << bits->lsb;
}

auto offset_of(std::string_view reg) -> std::uint32_t
{
  auto const* const found = popcorn::find_register(rcc_of(h573()), reg);
  REQUIRE(found != nullptr);
  return found->offset;
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

void set(stm32_fixture const& f, std::string_view reg, std::string_view field, std::uint32_t value)
{
  auto const  lsb = lsb_of(reg, field);
  auto const* bit = popcorn::find_register(rcc_of(h573()), reg);
  auto const  it  = std::ranges::find(bit->fields, field, &popcorn::field_desc::name);
  auto const  mask =
      it->width >= 32 ? ~std::uint32_t{} : ((std::uint32_t{1} << it->width) - 1u) * lsb;
  write(f, reg, (peek(f, reg) & ~mask) | (value * lsb));
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
  stm32_fixture f{h573(), "h573_rcc_ll", std::move(options)};
  f.set("pc", popcorn::testing::sym("h573_rcc_ll", "idle"));
  return f;
}

void pll1_from(stm32_fixture const& f, std::uint32_t src, std::uint32_t m, std::uint32_t n)
{
  set(f, "RCC_PLL1CFGR", "PLL1SRC", src);
  set(f, "RCC_PLL1CFGR", "DIVM1", m);
  set(f, "RCC_PLL1DIVR", "PLL1N", n - 1);
}

constexpr auto delay = std::chrono::microseconds{150};
}

TEST_CASE("RM0481 11.8: the bootloader clock sequence reaches 250 MHz on PLL1 P, Q and R off")
{
  stm32_fixture f{h573(), "h573_rcc_ll", with_listener()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 11);
  CHECK(log[0] > 0);
  CHECK(log[1] == 0);
  CHECK(log[2] == 0);
  CHECK(log[3] == 0);
  CHECK(log[4] > 0);
  CHECK(log[5] == 0);
  CHECK(log[6] > 0);
  CHECK(log[7] == 250 * mhz);
  CHECK(log[8] == 250 * mhz);
  CHECK(log[9] == 250 * mhz);
  CHECK(log[10] == 250 * mhz);
  auto const& clocks = probe(f);
  for(auto const name : {"sysclk", "hclk", "pclk1", "pclk2", "pclk3", "pll1_p"})
  {
    CAPTURE(name);
    CHECK(clocks.hz(name) == 250 * mhz);
  }
  CHECK(clocks.hz("pll1_q") == 0);
  CHECK(clocks.hz("pll1_r") == 0);
  CHECK(clocks.hz("hsi") == 32 * mhz);
  CHECK(clocks.hz("hsi48") == 48 * mhz);
  CHECK(clocks.hz("usb") == 48 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 250 * mhz);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 3);
}

TEST_CASE("RM0481 11.8.5: SYSCLK takes the PLL1 P output while R runs at its own rate")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  pll1_from(f, 1, 1, 8);
  set(f, "RCC_PLL1DIVR", "PLL1P", 1);
  set(f, "RCC_PLL1DIVR", "PLL1R", 3);
  write(
      f, "RCC_PLL1CFGR",
      peek(f, "RCC_PLL1CFGR")
          | lsb_of("RCC_PLL1CFGR", "PLL1PEN")
          | lsb_of("RCC_PLL1CFGR", "PLL1REN"));
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
  CHECK(clocks.hz("pll1_p") == 128 * mhz);
  CHECK(clocks.hz("pll1_r") == 64 * mhz);
  set(f, "RCC_CFGR1", "SW", 3);
  idle_until(f, delay);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 3);
  CHECK(clocks.hz("sysclk") == 128 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 128 * mhz);
}

TEST_CASE("RM0481 11.8.7: PLL1PEN, PLL1QEN and PLL1REN hold while PLL1 runs, PLL2's do not")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  pll1_from(f, 1, 4, 8);
  set(f, "RCC_PLL1CFGR", "PLL1QEN", 1);
  set(f, "RCC_CR", "PLL1ON", 1);
  set(f, "RCC_PLL1CFGR", "PLL1PEN", 1);
  set(f, "RCC_PLL1CFGR", "PLL1QEN", 0);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1PEN") == 0);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1QEN") == 1);
  idle_until(f, delay);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
  set(f, "RCC_PLL1CFGR", "PLL1REN", 1);
  set(f, "RCC_PLL1CFGR", "PLL1QEN", 0);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1REN") == 0);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1QEN") == 1);
  CHECK(clocks.hz("pll1_q") > 0);
  CHECK(clocks.hz("pll1_r") == 0);
  set(f, "RCC_CR", "PLL1ON", 0);
  set(f, "RCC_PLL1CFGR", "PLL1REN", 1);
  set(f, "RCC_PLL1CFGR", "PLL1QEN", 0);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1REN") == 1);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1QEN") == 0);

  set(f, "RCC_PLL2CFGR", "PLL2SRC", 1);
  set(f, "RCC_PLL2CFGR", "DIVM2", 4);
  set(f, "RCC_PLL2DIVR", "PLL2N", 7);
  set(f, "RCC_CR", "PLL2ON", 1);
  idle_until(f, delay);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL2RDY") == 1);
  set(f, "RCC_PLL2CFGR", "PLL2QEN", 1);
  CHECK(f.field("RCC", "RCC_PLL2CFGR", "PLL2QEN") == 1);
  CHECK(clocks.hz("pll2_q") > 0);
  set(f, "RCC_PLL2CFGR", "PLL2QEN", 0);
  CHECK(clocks.hz("pll2_q") == 0);
}

TEST_CASE("H573 board: PLL1ON loads PLL1FRACN under PLL1FRACEN in either write order")
{
  struct leg
  {
    std::string_view name;
    bool             fracen_first;
    std::uint32_t    fracen;
    std::uint64_t    sysclk;
  };
  for(auto const& [name, fracen_first, fracen, sysclk] : std::to_array<leg>({
          {"FRACEN then FRACN", true,  1, 250 * mhz},
          {"FRACN then FRACEN", false, 1, 250 * mhz},
          {"FRACEN 0",          false, 0, 248 * mhz},
  }))
  {
    CAPTURE(name);
    auto        f      = idle_fixture();
    auto const& clocks = probe(f);
    pll1_from(f, 1, 2, 31);
    set(f, "RCC_PLL1DIVR", "PLL1P", 1);
    set(f, "RCC_PLL1CFGR", "PLL1PEN", 1);
    if(fracen_first)
    {
      set(f, "RCC_PLL1CFGR", "PLL1FRACEN", fracen);
    }
    set(f, "RCC_PLL1FRACR", "PLL1FRACN", 2048);
    if(not fracen_first)
    {
      set(f, "RCC_PLL1CFGR", "PLL1FRACEN", fracen);
    }
    set(f, "RCC_CR", "PLL1ON", 1);
    idle_until(f, delay);
    REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
    CHECK(clocks.hz("pll1_p") == sysclk);
  }
}

TEST_CASE("RM0481 11.4.5: a running PLL1 takes a new PLL1FRACN on a PLL1FRACEN 0 to 1 write")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  pll1_from(f, 1, 2, 31);
  set(f, "RCC_PLL1DIVR", "PLL1P", 1);
  set(f, "RCC_PLL1CFGR", "PLL1PEN", 1);
  set(f, "RCC_PLL1CFGR", "PLL1FRACEN", 1);
  set(f, "RCC_PLL1FRACR", "PLL1FRACN", 2048);
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  REQUIRE(clocks.hz("pll1_p") == 250 * mhz);
  set(f, "RCC_PLL1FRACR", "PLL1FRACN", 4096);
  CHECK(clocks.hz("pll1_p") == 250 * mhz);
  set(f, "RCC_PLL1CFGR", "PLL1FRACEN", 0);
  CHECK(clocks.hz("pll1_p") == 250 * mhz);
  set(f, "RCC_PLL1CFGR", "PLL1FRACEN", 1);
  CHECK(clocks.hz("pll1_p") == 252 * mhz);
}

TEST_CASE("a restored PLL1 keeps its loaded fraction over a different PLL1FRACN")
{
  auto f = idle_fixture();
  probe(f);
  pll1_from(f, 1, 2, 31);
  set(f, "RCC_PLL1DIVR", "PLL1P", 1);
  set(f, "RCC_PLL1CFGR", "PLL1PEN", 1);
  set(f, "RCC_PLL1CFGR", "PLL1FRACEN", 1);
  set(f, "RCC_PLL1FRACR", "PLL1FRACN", 2048);
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  set(f, "RCC_PLL1FRACR", "PLL1FRACN", 4096);
  auto const bytes = core::encode_snapshot(f.target->snapshot().data());

  auto        other  = idle_fixture();
  auto const& clocks = probe(other);
  other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(clocks.hz("pll1_p") == 250 * mhz);
  write(other, "RCC_PLL1FRACR", peek(other, "RCC_PLL1FRACR"));
  CHECK(clocks.hz("pll1_p") == 250 * mhz);
  set(other, "RCC_PLL1CFGR", "PLL1FRACEN", 0);
  set(other, "RCC_PLL1CFGR", "PLL1FRACEN", 1);
  CHECK(clocks.hz("pll1_p") == 252 * mhz);
}

TEST_CASE("RM0481 11.8.7: PLL1SRC 10 takes CSI, DIVM1 0 keeps PLL1RDY clear")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  set(f, "RCC_CR", "CSION", 1);
  idle_until(f, delay);
  REQUIRE(clocks.hz("csi") == 4 * mhz);
  pll1_from(f, 2, 1, 8);
  set(f, "RCC_PLL1DIVR", "PLL1Q", 1);
  set(f, "RCC_PLL1CFGR", "PLL1QEN", 1);
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
  CHECK(clocks.hz("pll1_q") == 16 * mhz);
  set(f, "RCC_CR", "PLL1ON", 0);
  set(f, "RCC_PLL1CFGR", "DIVM1", 0);
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  CHECK(f.field("RCC", "RCC_CR", "PLL1RDY") == 0);
  CHECK(clocks.hz("pll1_q") == 0);
  set(f, "RCC_CR", "PLL1ON", 0);
  set(f, "RCC_PLL1CFGR", "DIVM1", 1);
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  CHECK(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
  CHECK(clocks.hz("pll1_q") == 16 * mhz);
}

TEST_CASE("H573 board: HSIDIVF sets again 1 us after an HSIDIV write")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  CHECK(clocks.hz("hsi") == 32 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 32 * mhz);
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 1);
  set(f, "RCC_CR", "HSIDIV", 0);
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 0);
  CHECK(f.field("RCC", "RCC_CR", "HSIDIV") == 0);
  CHECK(clocks.hz("hsi") == 64 * mhz);
  idle_until(f, std::chrono::nanoseconds{500});
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 0);
  idle_until(f, std::chrono::nanoseconds{600});
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 1);
  CHECK(clocks.hz("hsi") == 64 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 64 * mhz);
  write(f, "RCC_CR", peek(f, "RCC_CR") & ~lsb_of("RCC_CR", "HSIDIVF"));
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 1);
}

TEST_CASE("H573 board: HSIDIV changes while HSI feeds an enabled PLL1 off SYSCLK, holds on it")
{
  for(auto const sw : {0u, 1u, 3u})
  {
    CAPTURE(sw);
    auto  f      = idle_fixture();
    auto& clocks = probe(f);
    set(f, "RCC_CR", "CSION", 1);
    pll1_from(f, 1, 4, 32);
    set(f, "RCC_PLL1DIVR", "PLL1P", 3);
    set(f, "RCC_PLL1CFGR", "PLL1PEN", 1);
    set(f, "RCC_CR", "PLL1ON", 1);
    idle_until(f, delay);
    REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
    set(f, "RCC_CFGR1", "SW", sw);
    REQUIRE(f.field("RCC", "RCC_CFGR1", "SWS") == sw);
    set(f, "RCC_CR", "HSIDIV", 0);
    idle_until(f, delay);
    if(sw == 3)
    {
      CHECK(f.field("RCC", "RCC_CR", "HSIDIV") == 1);
      CHECK(clocks.hz("hsi") == 32 * mhz);
      CHECK(clocks.hz("pll1_p") == 64 * mhz);
    }
    else
    {
      CHECK(f.field("RCC", "RCC_CR", "HSIDIV") == 0);
      CHECK(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
      CHECK(clocks.hz("hsi") == 64 * mhz);
      CHECK(clocks.hz("pll1_p") == 128 * mhz);
    }
    CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 1);
  }
}

TEST_CASE("a snapshot taken while HSIDIVF is clear restores the pending divider event")
{
  auto f = idle_fixture();
  set(f, "RCC_CR", "HSIDIV", 0);
  REQUIRE(f.field("RCC", "RCC_CR", "HSIDIVF") == 0);
  auto const saved = f.target->snapshot();
  idle_until(f, std::chrono::nanoseconds{1100});
  REQUIRE(f.field("RCC", "RCC_CR", "HSIDIVF") == 1);
  f.target->restore(saved);
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 0);
  idle_until(f, std::chrono::nanoseconds{500});
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 0);
  idle_until(f, std::chrono::nanoseconds{600});
  CHECK(f.field("RCC", "RCC_CR", "HSIDIVF") == 1);
}

TEST_CASE("RM0481 11.8.5 TIMPRE: timer clocks at 1, 2 or 4 times the APB clock")
{
  struct row
  {
    std::uint32_t ppre1;
    std::uint32_t timpre;
    std::uint64_t pclk1;
    std::uint64_t tim2;
  };
  constexpr std::array<row, 5> rows{
      {
       {6, 0, 4 * mhz, 8 * mhz},
       {6, 1, 4 * mhz, 16 * mhz},
       {5, 1, 8 * mhz, 16 * mhz},
       {0, 0, 32 * mhz, 32 * mhz},
       {0, 1, 32 * mhz, 64 * mhz},
       }
  };
  for(auto const& [ppre1, timpre, pclk1, tim2] : rows)
  {
    CAPTURE(ppre1);
    CAPTURE(timpre);
    auto  f      = idle_fixture();
    auto& clocks = probe(f);
    set(f, "RCC_APB1LENR", "TIM2EN", 1);
    set(f, "RCC_APB2ENR", "TIM1EN", 1);
    set(f, "RCC_CFGR2", "PPRE1", ppre1);
    set(f, "RCC_CFGR1", "TIMPRE", timpre);
    CHECK(clocks.hz("pclk1") == pclk1);
    CHECK(clocks.hz("tim2") == tim2);
    CHECK(clocks.hz("tim1") == (timpre == 0 ? 32 * mhz : 64 * mhz));
  }
}

TEST_CASE("RM0481 11.8.5: SW is read through SW[1:0], 0b101 switches to CSI and SWS reads 1")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  set(f, "RCC_CR", "CSION", 1);
  idle_until(f, delay);
  set(f, "RCC_CFGR1", "SW", 5);
  idle_until(f, delay);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 1);
  CHECK(clocks.hz("sysclk") == 4 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 4 * mhz);
  idle_until(f, 3 * delay);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 1);
  CHECK(f.target->snapshot().data().events.pending.empty());
}

TEST_CASE("RM0481 11.8.42: the lpuart domain follows LPUART1SEL and LPUART1EN")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  CHECK(clocks.hz("lpuart") == 0);
  set(f, "RCC_APB3ENR", "LPUART1EN", 1);
  CHECK(clocks.hz("lpuart") == 32 * mhz);
  set(f, "RCC_CR", "CSION", 1);
  idle_until(f, delay);
  set(f, "RCC_CCIPR3", "LPUART1SEL", 4);
  CHECK(clocks.hz("lpuart") == 4 * mhz);
}

TEST_CASE("RM0481 11.8.40 to 11.8.44: UART9, SPI4, SPI5, SAI1 and USB kernel clocks")
{
  auto  f      = idle_fixture();
  auto& clocks = probe(f);
  set(f, "RCC_CFGR2", "PPRE1", 4);
  set(f, "RCC_CFGR2", "PPRE2", 5);
  set(f, "RCC_CFGR2", "PPRE3", 6);
  set(f, "RCC_APB1HENR", "UART9EN", 1);
  set(f, "RCC_APB2ENR", "SPI4EN", 1);
  set(f, "RCC_APB3ENR", "SPI5EN", 1);
  set(f, "RCC_APB2ENR", "SAI1EN", 1);
  set(f, "RCC_APB2ENR", "USBFSEN", 1);
  CHECK(clocks.hz("uart9") == 16 * mhz);
  CHECK(clocks.hz("spi4") == 8 * mhz);
  CHECK(clocks.hz("spi5") == 4 * mhz);
  CHECK(clocks.hz("sai1") == 0);
  CHECK(clocks.hz("usb") == 0);
  set(f, "RCC_CCIPR1", "UART9SEL", 3);
  CHECK(clocks.hz("uart9") == 32 * mhz);
  set(f, "RCC_CCIPR5", "SAI1SEL", 4);
  CHECK(clocks.hz("per") == 32 * mhz);
  CHECK(clocks.hz("sai1") == 32 * mhz);
  set(f, "RCC_CR", "HSI48ON", 1);
  set(f, "RCC_CCIPR4", "USBFSSEL", 3);
  CHECK(clocks.hz("usb") == 0);
  idle_until(f, delay);
  CHECK(clocks.hz("usb") == 48 * mhz);
}

TEST_CASE("RM0481 11.8.45: LSI and the RTC clock from LSI, or from HSE over RTCPRE")
{
  SUBCASE("LSI")
  {
    auto  f      = idle_fixture();
    auto& clocks = probe(f);
    set(f, "RCC_BDCR", "LSION", 1);
    idle_until(f, delay);
    CHECK(clocks.hz("lsi") == 32'000);
    write(f, "RCC_BDCR", peek(f, "RCC_BDCR") | 2u * lsb_of("RCC_BDCR", "RTCSEL"));
    CHECK(clocks.hz("rtc") == 0);
    set(f, "RCC_BDCR", "RTCEN", 1);
    CHECK(clocks.hz("rtc") == 32'000);
  }
  SUBCASE("HSE over RTCPRE")
  {
    auto  f      = idle_fixture();
    auto& clocks = probe(f);
    set(f, "RCC_CR", "HSEON", 1);
    idle_until(f, delay);
    write(f, "RCC_BDCR", 3u * lsb_of("RCC_BDCR", "RTCSEL") | lsb_of("RCC_BDCR", "RTCEN"));
    CHECK(clocks.hz("rtc") == 0);
    set(f, "RCC_CFGR1", "RTCPRE", 1);
    CHECK(clocks.hz("rtc") == 0);
    set(f, "RCC_CFGR1", "RTCPRE", 16);
    CHECK(clocks.hz("rtc") == 1 * mhz);
  }
}

TEST_CASE("RM0481 11.3.3: VSWRST clears the backup domain until released")
{
  auto f = idle_fixture();
  write(f, "RCC_BDCR", lsb_of("RCC_BDCR", "LSEON") | lsb_of("RCC_BDCR", "RTCSEL"));
  write(f, "RCC_BDCR", lsb_of("RCC_BDCR", "LSEON") | 2u * lsb_of("RCC_BDCR", "RTCSEL"));
  CHECK(f.field("RCC", "RCC_BDCR", "RTCSEL") == 1);
  write(f, "RCC_BDCR", peek(f, "RCC_BDCR") | lsb_of("RCC_BDCR", "VSWRST"));
  CHECK(peek(f, "RCC_BDCR") == lsb_of("RCC_BDCR", "VSWRST"));
  write(f, "RCC_BDCR", 0);
  write(f, "RCC_BDCR", 2u * lsb_of("RCC_BDCR", "RTCSEL"));
  CHECK(f.field("RCC", "RCC_BDCR", "RTCSEL") == 2);
}

TEST_CASE("RM0481 11.8.46: every reset kind leaves its own RCC_RSR flag, RMVF clears them")
{
  template for(constexpr auto kind :
               std::define_static_array(std::meta::enumerators_of(^^popcorn::reset_kind)))
  {
    if constexpr([:kind:] != reset_kind::peripheral)
    {
      CAPTURE(std::meta::identifier_of(kind));
      auto f = idle_fixture();
      f.target->reset([:kind:]);
      auto const pin    = lsb_of("RCC_RSR", "PINRSTF");
      auto const bor    = lsb_of("RCC_RSR", "BORRSTF");
      auto const always = pin | bor;
      auto       expect = always;
      switch([:kind:])
      {
        case reset_kind::software:
          expect |= lsb_of("RCC_RSR", "SFTRSTF");
          break;
        case reset_kind::watchdog:
          expect |= lsb_of("RCC_RSR", "IWDGRSTF");
          break;
        case reset_kind::window_watchdog:
          expect |= lsb_of("RCC_RSR", "WWDGRSTF");
          break;
        case reset_kind::low_power:
          expect |= lsb_of("RCC_RSR", "LPWRRSTF");
          break;
        default:
          break;
      }
      CHECK((peek(f, "RCC_RSR") & 0xfc00'0000u) == expect);
      write(f, "RCC_RSR", peek(f, "RCC_RSR") | lsb_of("RCC_RSR", "RMVF"));
      CHECK(peek(f, "RCC_RSR") == 0);
      f.target->reset(reset_kind::software);
      CHECK(peek(f, "RCC_RSR") == lsb_of("RCC_RSR", "SFTRSTF"));
    }
  }
}

TEST_CASE("RM0481 11.8.1: the oscillator driving SYSCLK stays on, with no log record")
{
  auto                          f = idle_fixture();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  write(f, "RCC_CR", peek(f, "RCC_CR") & ~lsb_of("RCC_CR", "HSION"));
  CHECK(f.field("RCC", "RCC_CR", "HSION") == 1);
  CHECK(f.field("RCC", "RCC_CR", "HSIRDY") == 1);
  CHECK(probe(f).hz("sysclk") == 32 * mhz);
  CHECK(logged.empty());
}

TEST_CASE("RM0481 11.8.1: the oscillator feeding an enabled PLL1 stays on while SYSCLK runs apart")
{
  struct leg
  {
    std::string_view on;
    std::uint32_t    pll_source;
    std::uint32_t    sw;
  };
  for(auto const& [on, pll_source, sw] : std::to_array<leg>({
          {"HSEON", 3, 0},
          {"HSION", 1, 1},
          {"CSION", 2, 0},
  }))
  {
    CAPTURE(on);
    auto                          f = idle_fixture();
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    auto const& clocks = probe(f);
    set(f, "RCC_CR", "CSION", 1);
    set(f, "RCC_CR", on, 1);
    idle_until(f, delay);
    set(f, "RCC_CFGR1", "SW", sw);
    REQUIRE(f.field("RCC", "RCC_CFGR1", "SWS") == sw);
    pll1_from(f, pll_source, 1, 8);
    set(f, "RCC_PLL1DIVR", "PLL1Q", 1);
    set(f, "RCC_PLL1CFGR", "PLL1QEN", 1);
    set(f, "RCC_CR", "PLL1ON", 1);
    idle_until(f, delay);
    REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
    auto const rate = clocks.hz("pll1_q");
    REQUIRE(rate > 0);
    set(f, "RCC_CR", on, 0);
    CHECK(f.field("RCC", "RCC_CR", on) == 1);
    idle_until(f, delay);
    CHECK(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
    CHECK(clocks.hz("pll1_q") == rate);
    CHECK(logged.empty());
  }
}

TEST_CASE("RM0481 11.8.1: one RCC_CR write clears HSEON and PLL1ON of a PLL1 off SYSCLK")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  set(f, "RCC_CR", "HSEON", 1);
  idle_until(f, delay);
  pll1_from(f, 3, 1, 8);
  set(f, "RCC_PLL1CFGR", "PLL1QEN", 1);
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
  write(f, "RCC_CR", peek(f, "RCC_CR") & ~(lsb_of("RCC_CR", "HSEON") | lsb_of("RCC_CR", "PLL1ON")));
  CHECK(f.field("RCC", "RCC_CR", "HSEON") == 0);
  CHECK(f.field("RCC", "RCC_CR", "HSERDY") == 0);
  CHECK(f.field("RCC", "RCC_CR", "PLL1ON") == 0);
  CHECK(f.field("RCC", "RCC_CR", "PLL1RDY") == 0);
  CHECK(clocks.hz("pll1_q") == 0);
}

TEST_CASE("RM0481 11.8.1: PLL1 driving SYSCLK keeps its configuration, with no log record")
{
  stm32_fixture f{h573(), "h573_rcc_ll", with_listener()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto const divider = peek(f, "RCC_PLL1DIVR");
  write(f, "RCC_CR", lsb_of("RCC_CR", "HSION"));
  write(
      f, "RCC_PLL1CFGR",
      peek(f, "RCC_PLL1CFGR")
          & ~(3u * lsb_of("RCC_PLL1CFGR", "PLL1SRC")
              | 63u * lsb_of("RCC_PLL1CFGR", "DIVM1")
              | lsb_of("RCC_PLL1CFGR", "PLL1PEN")));
  write(f, "RCC_PLL1DIVR", 0);
  CHECK(f.field("RCC", "RCC_CR", "PLL1ON") == 1);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1SRC") == 1);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "DIVM1") == 2);
  CHECK(f.field("RCC", "RCC_PLL1CFGR", "PLL1PEN") == 1);
  CHECK(peek(f, "RCC_PLL1DIVR") == divider);
  CHECK(f.target->virtual_clock().core_hz() == 250 * mhz);
  CHECK(logged.empty());
  f.set("pc", popcorn::testing::sym("h573_rcc_ll", "idle"));
  idle_until(f, std::chrono::microseconds{10});
}

TEST_CASE("RM0481 11.8.5: a switch to a PLL1 P output that is off keeps the core clock, warns once")
{
  auto                          f = idle_fixture();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  pll1_from(f, 1, 4, 8);
  set(f, "RCC_CR", "PLL1ON", 1);
  idle_until(f, delay);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL1RDY") == 1);
  REQUIRE(logged.empty());
  set(f, "RCC_CFGR1", "SW", 3);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 3);
  CHECK(probe(f).hz("sysclk") == 0);
  CHECK(f.target->virtual_clock().core_hz() == 32 * mhz);
  REQUIRE(logged.size() == 1);
  CHECK(logged.front().kind == core::log_kind::model_warning);
  CHECK(logged.front().reg == "RCC_CFGR1");
  CHECK(logged.front().message.contains("0 Hz"));
}

TEST_CASE("every H573 instance with an RCC reset bit resolves it, the renamed ones included")
{
  auto const&              rcc = rcc_of(h573());
  std::vector<std::string> unresolved;
  for(auto const& peripheral : h573().peripherals)
  {
    if(not peripheral.alias_of and not stm32::reset_bit_of(rcc, peripheral.name))
    {
      unresolved.push_back(peripheral.name);
    }
  }
  std::ranges::sort(unresolved);
  CHECK(
      unresolved
      == std::vector<std::string>{
          "DBGMCU", "DCACHE", "DLYBOS1", "DLYBSD1", "DLYBSD2", "EXTI", "FLASH", "GTZC1_MPCBB1",
          "GTZC1_MPCBB2", "GTZC1_MPCBB3", "GTZC1_TZIC", "GTZC1_TZSC", "ICACHE", "IWDG", "PWR",
          "RCC", "RTC", "TAMP", "WWDG"});
  for(auto const& [instance, field] : std::to_array<std::pair<std::string_view, std::string_view>>({
          {"ADC1",    "ADC12RST"   },
          {"ADC2",    "ADC12RST"   },
          {"ADCC",    "ADC12RST"   },
          {"USB",     "USBFSRST"   },
          {"DAC",     "DAC12RST"   },
          {"FDCAN1",  "FDCAN12RST" },
          {"FDCAN2",  "FDCAN12RST" },
          {"OCTOSPI", "OCTOSPI1RST"},
          {"UCPD1",   "UCPDRST"    },
          {"I3C",     "I3C1RST"    },
          {"LPUART",  "LPUART1RST" }
  }))
  {
    CAPTURE(instance);
    auto const found = stm32::reset_bit_of(rcc, instance);
    REQUIRE(found.has_value());
    CHECK(found->field == field);
  }
}

TEST_CASE("each of the seven H573 RSTR registers notifies its own instance's model")
{
  struct target
  {
    std::string_view instance;
    std::string_view reg;
    std::string_view bit;
  };
  constexpr std::array<target, 7> targets{
      {
       {"CRC", "RCC_AHB1RSTR", "CRCRST"},
       {"GPIOA", "RCC_AHB2RSTR", "GPIOARST"},
       {"OCTOSPI", "RCC_AHB4RSTR", "OCTOSPI1RST"},
       {"TIM2", "RCC_APB1LRSTR", "TIM2RST"},
       {"UART9", "RCC_APB1HRSTR", "UART9RST"},
       {"TIM1", "RCC_APB2RSTR", "TIM1RST"},
       {"I2C3", "RCC_APB3RSTR", "I2C3RST"},
       }
  };
  auto options = popcorn::testing::stm32_options();
  for(auto const& entry : targets)
  {
    options.models.insert_or_assign(
        std::string{entry.instance},
        [](periph::model_context&, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> { return std::make_shared<reset_counter>(); });
  }
  stm32_fixture f{h573(), "h573_rcc_ll", std::move(options)};
  for(auto const& entry : targets)
  {
    CAPTURE(entry.instance);
    popcorn::testing::rcc_reset_pulse(f, entry.reg, entry.bit);
    for(auto const& other : targets)
    {
      CAPTURE(other.instance);
      auto const* const counter = f.model_of<reset_counter>(other.instance);
      REQUIRE(counter != nullptr);
      auto const done = &other - targets.data() <= &entry - targets.data();
      CHECK(counter->resets == (done ? 1 : 0));
    }
  }
}

TEST_CASE("RM0481 11.8.44: the H573 RNG runs on its kernel clock and pauses without it")
{
  auto        f         = idle_fixture();
  auto const& rng       = f.target->peripheral("RNG");
  auto const  base      = rng.description().base;
  auto const  rng_write = [&](std::uint32_t offset, std::uint32_t value) {
    REQUIRE(f.target->mem().write(base + offset, 4, value).has_value());
  };
  constexpr std::uint32_t cr    = 0x00;
  constexpr std::uint32_t sr    = 0x04;
  constexpr std::uint32_t rngen = 1u << 2;
  constexpr std::uint32_t drdy  = 1u << 0;
  set(f, "RCC_CR", "HSI48ON", 1);
  idle_until(f, delay);
  set(f, "RCC_AHB2ENR", "RNGEN", 1);
  SUBCASE("HSI48 selected: words come")
  {
    rng_write(cr, rng.peek(cr) | rngen);
    idle_until(f, std::chrono::milliseconds{2});
    CHECK((rng.peek(sr) & drdy) != 0);
    popcorn::testing::rcc_reset_pulse(f, "RCC_AHB2RSTR", "RNGRST");
    CHECK(rng.peek(cr) == 0x0080'0d00);
    CHECK(rng.peek(sr) == 0);
  }
  SUBCASE("LSE selected and stopped: no word")
  {
    set(f, "RCC_CCIPR5", "RNGSEL", 2);
    rng_write(cr, rng.peek(cr) | rngen);
    idle_until(f, std::chrono::milliseconds{2});
    CHECK((rng.peek(sr) & drdy) == 0);
  }
  SUBCASE("RNGEN of AHB2ENR clear: no word")
  {
    set(f, "RCC_AHB2ENR", "RNGEN", 0);
    rng_write(cr, rng.peek(cr) | rngen);
    idle_until(f, std::chrono::milliseconds{2});
    CHECK((rng.peek(sr) & drdy) == 0);
  }
}

TEST_CASE("coverage classifies RCC_CIER as served, the model reads its enables")
{
  auto f = idle_fixture();
  f.target->enable_coverage({}, true);
  constexpr popcorn::address stub = 0x2000'0200;
  REQUIRE(f.target->mem().write(stub, 4, 0xE7FE'6802).has_value());
  auto const& desc = rcc_of(h573());
  f.set("r0", desc.base + offset_of("RCC_CIER"));
  f.set("pc", stub | 1u);
  idle_until(f, std::chrono::microseconds{1});
  CHECK(f.target->coverage().unmodelled.empty());
}

TEST_CASE(
    "RM0481 11.5.1 Table 122: each RCC_SECCFGR bit makes its RCC fields RAZ/WI to Non-secure "
    "accesses, open while the bit is clear")
{
  struct row
  {
    std::string_view bit;
    std::string_view reg;
    std::string_view field;
  };
  for(auto const& one : std::initializer_list<row>{
          {"HSISEC",      "RCC_HSICFGR",  "HSITRIM"   },
          {"HSESEC",      "RCC_CIER",     "HSERDYIE"  },
          {"CSISEC",      "RCC_CSICFGR",  "CSITRIM"   },
          {"LSISEC",      "RCC_CIER",     "LSIRDYIE"  },
          {"LSESEC",      "RCC_CIER",     "LSERDYIE"  },
          {"SYSCLKSEC",   "RCC_CFGR1",    "MCO1PRE"   },
          {"SYSCLKSEC",   "RCC_CCIPR4",   "SYSTICKSEL"},
          {"PRESCSEC",    "RCC_CFGR1",    "TIMPRE"    },
          {"PRESCSEC",    "RCC_CFGR2",    "PPRE3"     },
          {"PLL1SEC",     "RCC_PLL1CFGR", "DIVM1"     },
          {"PLL2SEC",     "RCC_PLL2DIVR", "PLL2N"     },
          {"PLL3SEC",     "RCC_PLL3CFGR", "PLL3VCOSEL"},
          {"HSI48SEC",    "RCC_CIER",     "HSI48RDYIE"},
          {"CKPERSELSEC", "RCC_CCIPR5",   "CKPERSEL"  },
  })
  {
    std::string const name = std::string{one.bit} + " " + std::string{one.field};
    CAPTURE(name);
    for(auto const secured : {false, true})
    {
      CAPTURE(secured);
      popcorn::testing::h573_secure_rig const r{"h573_rcc_ll"};
      auto const                              bits = r.bit("RCC", one.reg, one.field);
      auto const                              mask = r.bit("RCC", "RCC_SECCFGR", one.bit);
      r.write("RCC", "RCC_SECCFGR", secured ? mask : 0, true);
      auto const stored = r.peek("RCC", one.reg);
      r.write("RCC", one.reg, stored ^ bits, false);
      CHECK((r.peek("RCC", one.reg) & bits) == ((secured ? stored : stored ^ bits) & bits));
      CHECK((r.read("RCC", one.reg, false) & bits) == (secured ? 0 : (stored ^ bits) & bits));
    }
  }
}

TEST_CASE(
    "RM0481 11.5.1: a peripheral the TZSC secures has its RCC enable, sleep enable and kernel "
    "clock bits RAZ/WI to Non-secure accesses, under its SVD bit name")
{
  struct row
  {
    std::string_view sec_reg;
    std::string_view sec;
    std::string_view reg;
    std::string_view field;
  };
  for(auto const& one : std::initializer_list<row>{
          {"SECCFGR2", "LPUART1SEC",  "RCC_APB3ENR",    "LPUART1EN"  },
          {"SECCFGR2", "LPUART1SEC",  "RCC_APB3LPENR",  "LPUART1LPEN"},
          {"SECCFGR2", "LPUART1SEC",  "RCC_CCIPR3",     "LPUART1SEL" },
          {"SECCFGR3", "ADC12SEC",    "RCC_AHB2ENR",    "ADC12EN"    },
          {"SECCFGR3", "ADC12SEC",    "RCC_CCIPR5",     "ADCDACSEL"  },
          {"SECCFGR2", "UCPDSEC",     "RCC_APB1HENR",   "UCPDEN"     },
          {"SECCFGR2", "USBSEC",      "RCC_APB2LPENR",  "USBFSLPEN"  },
          {"SECCFGR1", "UART12SEC",   "RCC_CCIPR2",     "USART12SEL" },
          {"SECCFGR1", "I3C1SEC",     "RCC_APB1LLPENR", "I3C1LPEN"   },
          {"SECCFGR3", "OCTOSPI1SEC", "RCC_AHB4ENR",    "OCTOSPI1EN" },
  })
  {
    std::string const name{one.field};
    CAPTURE(name);
    popcorn::testing::h573_secure_rig const r{"h573_rcc_ll"};
    auto const                              bits = r.bit("RCC", one.reg, one.field);
    r.write("GTZC1_TZSC", one.sec_reg, r.bit("GTZC1_TZSC", one.sec_reg, one.sec), true);
    auto const stored = r.peek("RCC", one.reg);
    r.write("RCC", one.reg, stored ^ bits, false);
    CHECK((r.peek("RCC", one.reg) & bits) == (stored & bits));
    CHECK((r.read("RCC", one.reg, false) & bits) == 0);
    r.write("RCC", one.reg, stored ^ bits, true);
    CHECK((r.peek("RCC", one.reg) & bits) == ((stored ^ bits) & bits));
  }
}

TEST_CASE(
    "RM0481 11.5.1: ADCDACSEL, TIMICSEL and the FDCAN12 bits are Secure while any peripheral they "
    "serve is")
{
  struct row
  {
    std::string_view sec_reg;
    std::string_view sec;
    std::string_view reg;
    std::string_view field;
  };
  for(auto const& one : std::initializer_list<row>{
          {"SECCFGR1", "DAC1SEC",   "RCC_CCIPR5",   "ADCDACSEL" },
          {"SECCFGR3", "ADC12SEC",  "RCC_CCIPR5",   "ADCDACSEL" },
          {"SECCFGR2", "FDCAN2SEC", "RCC_CCIPR5",   "FDCAN12SEL"},
          {"SECCFGR1", "TIM12SEC",  "RCC_CCIPR1",   "TIMICSEL"  },
          {"SECCFGR2", "TIM15SEC",  "RCC_CCIPR1",   "TIMICSEL"  },
          {"SECCFGR1", "LPTIM2SEC", "RCC_CCIPR1",   "TIMICSEL"  },
          {"SECCFGR2", "FDCAN2SEC", "RCC_APB1HENR", "FDCAN12EN" },
          {"SECCFGR2", "FDCAN1SEC", "RCC_APB1HENR", "FDCAN12EN" },
  })
  {
    std::string const name = std::string{one.sec} + " " + std::string{one.field};
    CAPTURE(name);
    for(auto const secured : {false, true})
    {
      CAPTURE(secured);
      popcorn::testing::h573_secure_rig const r{"h573_rcc_ll"};
      auto const                              bits = r.bit("RCC", one.reg, one.field);
      r.write(
          "GTZC1_TZSC", one.sec_reg, secured ? r.bit("GTZC1_TZSC", one.sec_reg, one.sec) : 0, true);
      auto const stored = r.peek("RCC", one.reg);
      r.write("RCC", one.reg, stored ^ bits, false);
      CHECK((r.peek("RCC", one.reg) & bits) == ((secured ? stored : stored ^ bits) & bits));
    }
  }
}

TEST_CASE(
    "RM0481 11.5.1 and 7.11 FLASH_SECCR.INV: FLITFEN and FLITFLPEN follow the FLASH security "
    "state")
{
  popcorn::testing::h573_secure_rig const r{"h573_rcc_ll"};
  struct bit_of
  {
    std::string_view reg;
    std::uint32_t    bits;
  };
  auto const enables = std::to_array<bit_of>({
      {"RCC_AHB1ENR",   r.bit("RCC", "RCC_AHB1ENR",   "FLITFEN")  },
      {"RCC_AHB1LPENR", r.bit("RCC", "RCC_AHB1LPENR", "FLITFLPEN")},
  });
  for(auto const& [reg, bits] : enables)
  {
    std::string const name{reg};
    CAPTURE(name);
    REQUIRE((r.peek("RCC", reg) & bits) == bits);
    r.write("RCC", reg, r.peek("RCC", reg) & ~bits, false);
    CHECK((r.peek("RCC", reg) & bits) == bits);
    CHECK((r.read("RCC", reg, false) & bits) == 0);
  }
  r.write("FLASH", "SECKEYR", 0x4567'0123, true);
  r.write("FLASH", "SECKEYR", 0xcdef'89ab, true);
  auto const inv = r.bit("FLASH", "SECCR", "INV");
  r.write("FLASH", "SECCR", inv, true);
  REQUIRE((r.peek("FLASH", "SECCR") & inv) == inv);
  for(auto const& [reg, bits] : enables)
  {
    std::string const name{reg};
    CAPTURE(name);
    CHECK((r.read("RCC", reg, false) & bits) == bits);
    r.write("RCC", reg, r.peek("RCC", reg) & ~bits, false);
    CHECK((r.peek("RCC", reg) & bits) == 0);
  }
}

TEST_CASE("an H573 description without RCC_CR.HSEEXT builds at TZEN=0 and throws at TZEN=1")
{
  auto desc = h573();
  auto rcc =
      std::ranges::find(desc.peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  REQUIRE(rcc != desc.peripherals.end());
  auto cr =
      std::ranges::find(rcc->registers, std::string_view{"CR"}, &popcorn::register_desc::name);
  REQUIRE(cr != rcc->registers.end());
  REQUIRE(
      std::erase_if(cr->fields, [](popcorn::field_desc const& f) { return f.name == "HSEEXT"; })
      == 1);
  stm32::register_builtin_models();
  CHECK_NOTHROW(
      core::machine(desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()));
  CHECK_THROWS_AS(
      core::machine(
          desc, popcorn::arch::armv8m::factory(),
          popcorn::testing::h573_secure_rig::options({.tzen = true})),
      popcorn::config_error);
}

TEST_CASE(
    "RM0481 11.5.1: a Secure RTC function makes RTCPRE, RTCEN and RTCSEL RAZ/WI to Non-secure "
    "accesses, VSWRST takes the write")
{
  popcorn::testing::h573_secure_rig const r{"h573_rcc_ll"};
  r.write("PWR", "PWR_DBPCR", 1, false);
  auto const rtcpre = r.bit("RCC", "RCC_CFGR1", "RTCPRE");
  auto const backup = r.bit("RCC", "RCC_BDCR", "RTCEN") | r.bit("RCC", "RCC_BDCR", "RTCSEL");
  r.write("RTC", "SECCFGR", r.bit("RTC", "SECCFGR", "SEC"), true);
  r.write("RCC", "RCC_CFGR1", rtcpre, false);
  CHECK((r.peek("RCC", "RCC_CFGR1") & rtcpre) == 0);
  r.write("RCC", "RCC_BDCR", backup, false);
  CHECK((r.peek("RCC", "RCC_BDCR") & backup) == 0);
  r.write("RCC", "RCC_CFGR1", rtcpre, true);
  CHECK((r.read("RCC", "RCC_CFGR1", false) & rtcpre) == 0);
  CHECK((r.read("RCC", "RCC_CFGR1", true) & rtcpre) == rtcpre);
  auto const vswrst = r.bit("RCC", "RCC_BDCR", "VSWRST");
  r.write("RCC", "RCC_BDCR", vswrst, false);
  CHECK((r.peek("RCC", "RCC_BDCR") & vswrst) == vswrst);
}

TEST_CASE("RM0481 11.4.8: the CMSIS SystemInit run after the bootloader leaves HSI as SYSCLK")
{
  stm32_fixture f{h573(), "h573_rcc_ll", with_listener()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.field("RCC", "RCC_CFGR1", "SWS") == 3);
  std::vector<std::string> warnings;
  auto const               sink = f.target->add_log_sink([&](core::log_record const& record) {
    if(record.kind == core::log_kind::model_warning)
    {
      warnings.push_back(record.message);
    }
  });
  f.set("pc", popcorn::testing::sym("h573_rcc_ll", "SystemInit"));
  f.set("lr", popcorn::testing::sym("h573_rcc_ll", "idle") | 1u);
  idle_until(f, std::chrono::microseconds{10});
  auto const pc   = f.target->regs().read(*f.target->regs().find("pc"));
  auto const idle = popcorn::testing::sym("h573_rcc_ll", "idle");
  CHECK(pc >= idle);
  CHECK(pc < idle + 16);
  CHECK(f.field("RCC", "RCC_CFGR1", "SW") == 0);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 0);
  CHECK(f.field("RCC", "RCC_CR", "PLL1ON") == 0);
  CHECK(f.field("RCC", "RCC_CR", "PLL1RDY") == 0);
  CHECK(f.field("RCC", "RCC_CR", "HSIDIV") == 1);
  CHECK(probe(f).hz("sysclk") == 32 * mhz);
  CHECK(probe(f).hz("hclk") == 32 * mhz);
  CHECK(probe(f).hz("pll1_p") == 0);
  CHECK(f.target->virtual_clock().core_hz() == 32 * mhz);
  CHECK(warnings.empty());
}

TEST_CASE("H573 board: the SystemInit that clears HSIDIV keeps it until PLL1 stops driving SYSCLK")
{
  stm32_fixture f{h573(), "h573_rcc_ll", with_listener()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.field("RCC", "RCC_CFGR1", "SWS") == 3);
  std::vector<std::string> warnings;
  auto const               sink   = f.target->add_log_sink([&](core::log_record const& record) {
    if(record.kind == core::log_kind::model_warning)
    {
      warnings.push_back(record.message);
    }
  });
  auto const&              clocks = probe(f);
  auto const               bit    = [](std::string_view field) { return lsb_of("RCC_CR", field); };
  write(f, "RCC_CR", bit("HSION"));
  CHECK(f.field("RCC", "RCC_CR", "HSIDIV") == 1);
  CHECK(f.field("RCC", "RCC_CR", "PLL1ON") == 1);
  CHECK(clocks.hz("hclk") == 250 * mhz);
  write(f, "RCC_CFGR1", 0);
  CHECK(f.field("RCC", "RCC_CFGR1", "SWS") == 0);
  CHECK(clocks.hz("hclk") == 32 * mhz);
  write(f, "RCC_CFGR2", 0);
  CHECK(clocks.hz("hclk") == 32 * mhz);
  write(
      f, "RCC_CR",
      peek(f, "RCC_CR")
          & ~(bit("HSEON")
              | bit("HSECSSON")
              | bit("HSEBYP")
              | bit("HSEEXT")
              | 3u * bit("HSIDIV")
              | bit("HSIKERON")
              | bit("CSION")
              | bit("CSIKERON")
              | bit("HSI48ON")
              | bit("PLL1ON")
              | bit("PLL2ON")
              | bit("PLL3ON")));
  CHECK(f.field("RCC", "RCC_CR", "PLL1ON") == 0);
  CHECK(f.field("RCC", "RCC_CR", "PLL1RDY") == 0);
  CHECK(f.field("RCC", "RCC_CR", "HSIDIV") == 0);
  CHECK(clocks.hz("hclk") == 64 * mhz);
  write(f, "RCC_PLL1CFGR", 0);
  write(f, "RCC_PLL1DIVR", 0x01010280u);
  write(f, "RCC_PLL1FRACR", 0);
  CHECK(clocks.hz("pll1_p") == 0);
  CHECK(clocks.hz("hclk") == 64 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 64 * mhz);
  CHECK(warnings.empty());
}
