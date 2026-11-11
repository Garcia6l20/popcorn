#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using popcorn::icount;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint32_t csrst      = 1u << 0;
constexpr std::uint32_t psrst      = 1u << 1;
constexpr std::uint32_t fcrst      = 1u << 2;
constexpr std::uint32_t rxfflsh    = 1u << 4;
constexpr std::uint32_t txfflsh    = 1u << 5;
constexpr std::uint32_t all_fifos  = 0x10u << 6;
constexpr std::uint32_t ahbidl     = 1u << 31;
constexpr std::uint32_t reset_word = ahbidl;

struct otg_fixture : stm32_fixture
{
  otg_fixture() : stm32_fixture{u575(), "boot_ll"}
  {}

  [[nodiscard]] auto offset() const -> std::uint32_t
  {
    auto const& registers = target->peripheral("OTG_FS").description().registers;
    auto const  found =
        std::ranges::find(registers, std::string_view{"GRSTCTL"}, &popcorn::register_desc::name);
    REQUIRE(found != registers.end());
    return found->offset;
  }

  void write(std::uint32_t value) const
  {
    auto& regs = target->peripheral("OTG_FS");
    REQUIRE(
        target->model("OTG_FS")
            ->on_write(regs, offset(), 4, regs.peek(offset()), value, {.pc = 0, .now = icount{0}})
            .has_value());
  }

  [[nodiscard]] auto read() const -> std::uint32_t
  {
    auto&      regs = target->peripheral("OTG_FS");
    auto const value =
        target->model("OTG_FS")->on_read(regs, offset(), 4, {.pc = 0, .now = icount{0}});
    REQUIRE(value.has_value());
    return *value;
  }
};
}

TEST_CASE("OTG_GRSTCTL reads AHBIDL set after reset")
{
  otg_fixture const f;
  REQUIRE(f.target->model("OTG_FS") != nullptr);
  CHECK(f.read() == reset_word);
}

TEST_CASE("RXFFLSH reads 0 right after it is written, AHBIDL stays set")
{
  otg_fixture const f;
  f.write(rxfflsh);
  CHECK(f.read() == reset_word);
}

TEST_CASE("TXFFLSH reads 0 right after it is written, TXFNUM keeps the FIFO number")
{
  otg_fixture const f;
  f.write(txfflsh | all_fifos);
  CHECK(f.read() == (reset_word | all_fifos));
  f.write(f.read() | rxfflsh | txfflsh);
  CHECK(f.read() == (reset_word | all_fifos));
}

TEST_CASE("a core soft reset reads done at once, AHBIDL stays set")
{
  otg_fixture const f;
  f.write(f.read() | csrst);
  CHECK(f.read() == reset_word);
}

TEST_CASE("a partial soft reset and a frame counter reset read done at once")
{
  otg_fixture const f;
  f.write(psrst | all_fifos);
  CHECK(f.read() == (reset_word | all_fifos));
  f.write(fcrst | all_fifos);
  CHECK(f.read() == (reset_word | all_fifos));
}

TEST_CASE("a machine reset brings OTG_GRSTCTL back to its reset value")
{
  otg_fixture const f;
  f.write(all_fifos);
  REQUIRE(f.read() == (reset_word | all_fifos));
  f.target->reset();
  CHECK(f.read() == reset_word);
}

namespace
{
constexpr std::uint32_t otg_base = 0x4204'0000;
constexpr std::uint32_t gccfg    = 0x38;
constexpr std::uint32_t pwrdwn   = 1u << 16;

auto without_rcc() -> popcorn::part_description
{
  auto desc = u575();
  std::erase_if(
      desc.peripherals, [](auto const& p) { return p.name == "RCC" or p.name == "SEC_RCC"; });
  return desc;
}

struct clock_rig
{
  explicit clock_rig(bool strict = false, bool with_rcc = true)
      : f{with_rcc ? u575() : without_rcc(), "boot_ll", options(strict)}
  {
    REQUIRE(f.target->mem().write(0x2000'0100, 2, 0xe7fe).has_value());
    f.set("pc", 0x2000'0101);
    if(with_rcc)
    {
      popcorn::testing::enable_clocks(
          *f.target, {
                         {"AHB2ENR1", "OTGEN"}
      });
    }
  }

  static auto options(bool strict) -> popcorn::core::machine_options
  {
    auto out               = popcorn::testing::stm32_options();
    out.strict_peripherals = strict;
    return out;
  }

  void set(std::string_view reg, std::string_view field, std::uint32_t value) const
  {
    auto const& desc  = f.target->peripheral("RCC").description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = ((std::uint32_t{1} << bits->width) - 1u) << bits->lsb;
    auto const old  = f.target->mem().read(desc.base + found->offset, 4);
    REQUIRE(old.has_value());
    auto const word = (static_cast<std::uint32_t>(*old) & ~mask) | ((value << bits->lsb) & mask);
    REQUIRE(f.target->mem().write(desc.base + found->offset, 4, word).has_value());
  }

  void iclk_from_pll1_q(std::uint32_t n) const
  {
    set("RCC_PLL1CFGR", "PLL1SRC", 1);
    set("RCC_PLL1CFGR", "PLL1RGE", 3);
    set("RCC_PLL1CFGR", "PLL1M", 0);
    set("RCC_PLL1CFGR", "PLL1QEN", 1);
    set("RCC_PLL1DIVR", "PLL1N", n - 1);
    set("RCC_PLL1DIVR", "PLL1Q", 3);
    set("RCC_CR", "PLL1ON", 1);
    f.run_for(100'000'000);
    set("RCC_CCIPR1", "ICLKSEL", 2);
  }

  void store(std::uint32_t offset, std::uint32_t value) const
  {
    constexpr std::uint32_t code = 0x2000'0200;
    REQUIRE(f.target->mem().write(code, 2, 0x6001).has_value());
    REQUIRE(f.target->mem().write(code + 2, 2, 0xbe00).has_value());
    f.set("r0", otg_base + offset);
    f.set("r1", value);
    f.set("pc", code | 1u);
    auto const stopped = f.run_to_stop({.max_instructions = popcorn::icount{4}});
    REQUIRE(stopped.reason == popcorn::stop_reason::user);
    f.set("pc", 0x2000'0101);
  }

  void power(std::uint32_t value = pwrdwn) const
  {
    store(gccfg, value);
  }

  [[nodiscard]] auto debugger_power(std::uint32_t value = pwrdwn) const -> bool
  {
    return f.target->mem().write(otg_base + gccfg, 4, value).has_value();
  }

  void select_iclk(std::uint32_t code) const
  {
    set("RCC_CCIPR1", "ICLKSEL", code);
  }

  [[nodiscard]] auto warnings() const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [](popcorn::core::log_record const& record) {
      return record.kind == popcorn::core::log_kind::model_warning
         and record.message.find("48 MHz kernel clock") != std::string::npos;
    });
  }

  stm32_fixture                          f;
  std::vector<popcorn::core::log_record> logged;
  popcorn::core::hook_handle             sink = f.target->add_log_sink(
      [this](popcorn::core::log_record const& record) { logged.push_back(record); });
};
}

TEST_CASE("ICLKSEL on HSI48 powers the transceiver silently")
{
  clock_rig const r{true};
  r.set("RCC_CR", "HSI48ON", 1);
  r.f.run_for(100'000'000);
  r.power();
  CHECK(r.warnings() == 0);
}

TEST_CASE("ICLKSEL on a stopped HSI48 checks at each rising PWRDWN write and warns once")
{
  clock_rig const r;
  r.power();
  r.power(0);
  r.power();
  CHECK(r.warnings() == 1);
}

TEST_CASE("a transceiver left powered down checks no ICLK")
{
  clock_rig const r;
  r.power(0);
  CHECK(r.warnings() == 0);
}

TEST_CASE("under strict_peripherals a firmware PWRDWN write with ICLK stopped raises model_error")
{
  clock_rig const r{true};
  CHECK_THROWS_AS(r.power(), popcorn::model_error);
}

TEST_CASE("a debugger PWRDWN write with ICLK stopped is served on a strict machine")
{
  clock_rig const r{true};
  CHECK(r.debugger_power());
  CHECK(r.warnings() == 0);
}

TEST_CASE("only the PWRDWN write that powers the transceiver checks the clock")
{
  clock_rig const r{true};
  r.set("RCC_CR", "HSI48ON", 1);
  r.f.run_for(100'000'000);
  r.power();
  r.select_iclk(3);
  CHECK_NOTHROW(r.store(0, 0));
  CHECK_NOTHROW(r.power(pwrdwn | (1u << 21)));
  CHECK(r.warnings() == 0);
}

TEST_CASE("USB 2.0 7.1.11: PLL1 Q at 48 MHz is silent, at 47 MHz it warns")
{
  clock_rig const ok{true};
  ok.iclk_from_pll1_q(48);
  ok.power();
  CHECK(ok.warnings() == 0);
  clock_rig const off;
  off.iclk_from_pll1_q(47);
  off.power();
  CHECK(off.warnings() == 1);
}

TEST_CASE("a machine with no RCC model checks no clock")
{
  clock_rig const r{true, false};
  CHECK_NOTHROW(r.power());
  CHECK(r.warnings() == 0);
}
