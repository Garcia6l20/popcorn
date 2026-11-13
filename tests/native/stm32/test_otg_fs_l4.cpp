#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/usb.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware   = std::string_view{"l476_l4_exti_ll"};
constexpr std::uint32_t otg_base   = 0x5000'0000;
constexpr std::uint32_t gotgctl    = 0x00;
constexpr std::uint32_t grstctl    = 0x10;
constexpr std::uint32_t gpwrdn     = 0x58;
constexpr std::uint32_t gadpctl    = 0x60;
constexpr std::uint32_t csrst      = 1u << 0;
constexpr std::uint32_t psrst      = 1u << 1;
constexpr std::uint32_t fcrst      = 1u << 2;
constexpr std::uint32_t rxfflsh    = 1u << 4;
constexpr std::uint32_t txfflsh    = 1u << 5;
constexpr std::uint32_t all_fifos  = 0x10u << 6;
constexpr std::uint32_t ahbidl     = 1u << 31;
constexpr std::uint32_t curmod     = 1u << 21;
constexpr std::uint32_t adpmen     = 1u << 0;
constexpr std::uint32_t reset_word = ahbidl;
constexpr std::uint32_t gahbcfg    = 0x08;
constexpr std::uint32_t gintsts    = 0x14;
constexpr std::uint32_t gintmsk    = 0x18;
constexpr std::uint32_t bvaloen    = 1u << 6;
constexpr std::uint32_t bvaloval   = 1u << 7;
constexpr std::uint32_t otgint     = 1u << 2;
constexpr std::uint32_t global_irq = 1u << 0;
constexpr std::uint32_t otg_irq    = 67;
constexpr std::uint32_t u5_otg_irq = 73;

constexpr std::uint32_t pwr_cr2       = 0x4000'7004;
constexpr std::uint32_t rcc_apb1rstr1 = 0x4002'1038;
constexpr std::uint32_t pwrrst        = 1u << 28;
constexpr std::uint32_t usv           = 1u << 10;
constexpr std::uint32_t pwrdwn        = 1u << 16;
constexpr std::uint32_t gccfg         = 0x38;
constexpr std::uint32_t dctl          = 0x804;
constexpr std::uint32_t sdis          = 1u << 1;

struct pull_up_host final : popcorn::periph::usb::host_side
{
  std::vector<bool> changes;

  void pull_up_changed(bool on, popcorn::vtime) override
  {
    changes.push_back(on);
  }

  void on_time(popcorn::vtime) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}
};

auto options(bool strict) -> core::machine_options
{
  auto out               = popcorn::testing::stm32_options();
  out.strict_peripherals = strict;
  return out;
}

struct otg_rig
{
  explicit otg_rig(bool strict = false) : f{l476rc(), firmware, options(strict)}
  {
    f.set("pc", sym(firmware, "idle"));
  }

  [[nodiscard]] auto read(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(otg_base + offset, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto peek(std::uint32_t offset) const -> std::uint32_t
  {
    return f.target->peripheral("OTG_FS").peek(offset);
  }

  void write(std::uint32_t offset, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(otg_base + offset, 4, value).has_value());
  }

  [[nodiscard]] auto cpu_store(std::uint32_t offset, std::uint32_t value) const -> popcorn::stop
  {
    constexpr std::uint32_t code = 0x2000'0100;
    REQUIRE(f.target->mem().write(code, 2, 0x6001).has_value());
    REQUIRE(f.target->mem().write(code + 2, 2, 0xbe00).has_value());
    f.set("r0", otg_base + offset);
    f.set("r1", value);
    f.set("pc", code | 1u);
    return f.run_to_stop({.max_instructions = popcorn::icount{4}});
  }

  [[nodiscard]] auto nvic_pending(std::uint32_t irq) const -> bool
  {
    auto const word = f.target->mem().read(0xE000'E200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }

  void supply(bool valid) const
  {
    auto const word = f.target->mem().read(pwr_cr2, 4);
    REQUIRE(word.has_value());
    auto const now = valid ? *word | usv : *word & ~std::uint64_t{usv};
    REQUIRE(f.target->mem().write(pwr_cr2, 4, now).has_value());
  }

  [[nodiscard]] auto port() const -> popcorn::periph::usb::device_port&
  {
    auto* const found = f.model_of<popcorn::periph::usb::device_port>("OTG_FS");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto warnings() const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [](core::log_record const& record) {
      return record.kind == core::log_kind::model_warning;
    });
  }

  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink =
      f.target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
};
}

TEST_CASE("an STM32L476 OTG_FS of otgfs1_v2_0 takes the OTG model, GRSTCTL reads AHBIDL")
{
  otg_rig const r;
  REQUIRE(r.f.target->model("OTG_FS") != nullptr);
  CHECK(r.read(grstctl) == reset_word);
}

TEST_CASE("RM0351 47.15.5: FCRST, CSRST and the flushes read 0 right after they are written")
{
  otg_rig const r;
  r.write(grstctl, fcrst | all_fifos);
  CHECK(r.read(grstctl) == (reset_word | all_fifos));
  r.write(grstctl, csrst | rxfflsh | txfflsh | all_fifos);
  CHECK(r.read(grstctl) == (reset_word | all_fifos));
}

TEST_CASE(
    "RM0351 Table 322: GRSTCTL bit 1 and GOTGCTL bit 21 read 0, the L47x/L48x core has no "
    "PSRST or CURMOD")
{
  otg_rig const r;
  r.write(grstctl, psrst);
  CHECK(r.read(grstctl) == reset_word);
  r.write(gotgctl, r.read(gotgctl) | curmod);
  CHECK((r.read(gotgctl) & curmod) == 0);
}

TEST_CASE(
    "RM0351 47.15.18 and 47.15.19: GPWRDN and GADPCTL are storage, a firmware store "
    "warns once per register")
{
  otg_rig const r;
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  auto const reset = r.peek(gpwrdn);
  REQUIRE((reset & adpmen) == 0);
  REQUIRE(r.cpu_store(gpwrdn, adpmen).reason == popcorn::stop_reason::user);
  REQUIRE(r.cpu_store(gpwrdn, adpmen).reason == popcorn::stop_reason::user);
  CHECK(r.peek(gpwrdn) == (reset | adpmen));
  CHECK(r.warnings() == 1);
  REQUIRE(r.cpu_store(gadpctl, 0).reason == popcorn::stop_reason::user);
  CHECK(r.warnings() == 2);
}

TEST_CASE(
    "under strict_peripherals a firmware GPWRDN store raises model_error "
    "and keeps the reset value")
{
  otg_rig const r{true};
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  auto const reset = r.peek(gpwrdn);
  CHECK_THROWS_AS(static_cast<void>(r.cpu_store(gpwrdn, adpmen)), popcorn::model_error);
  CHECK(r.peek(gpwrdn) == reset);
}

TEST_CASE("RM0351 Table 61: a session end with GINTMSK.OTGINT and GAHBCFG.GINTMSK pends IRQ 67")
{
  otg_rig const r;
  r.write(gintmsk, otgint);
  r.write(gotgctl, bvaloen | bvaloval);
  REQUIRE_FALSE(r.nvic_pending(otg_irq));
  r.write(gotgctl, bvaloen);
  REQUIRE((r.read(gintsts) & otgint) != 0);
  CHECK_FALSE(r.nvic_pending(otg_irq));
  r.write(gahbcfg, global_irq);
  CHECK(r.nvic_pending(otg_irq));
  CHECK_FALSE(r.nvic_pending(u5_otg_irq));
}

TEST_CASE(
    "RM0351 5.1.3 and 5.4.2: with PWR_CR2.USV 0 a connectable device stays disconnected and "
    "warns once, a USV write connects it, a PWRRST pulse disconnects it")
{
  otg_rig const r;
  auto const    host = std::make_shared<pull_up_host>();
  r.write(gccfg, pwrdwn);
  r.write(gotgctl, bvaloen | bvaloval);
  r.port().attach(host);
  r.write(dctl, r.read(dctl) & ~sdis);
  CHECK_FALSE(r.port().pulled_up());
  CHECK(host->changes.empty());
  auto const usv_warnings = [&] {
    return std::ranges::count_if(r.logged, [](core::log_record const& record) {
      return record.kind == core::log_kind::model_warning
         and record.message.find("USV") != std::string::npos;
    });
  };
  CHECK(usv_warnings() == 1);
  r.supply(true);
  CHECK(r.port().pulled_up());
  CHECK(host->changes == std::vector<bool>{true});
  r.supply(false);
  CHECK(host->changes == std::vector<bool>{true, false});
  r.supply(true);
  CHECK(host->changes == std::vector<bool>{true, false, true});
  CHECK(usv_warnings() == 1);
  REQUIRE(r.f.target->mem().write(rcc_apb1rstr1, 4, pwrrst).has_value());
  REQUIRE(r.f.target->mem().write(rcc_apb1rstr1, 4, 0).has_value());
  CHECK_FALSE(r.port().pulled_up());
  CHECK(host->changes == std::vector<bool>{true, false, true, false});
}

TEST_CASE(
    "RM0351 5.4.2: a machine reset of a connected device logs no USV warning, a later start "
    "with USV 0 warns once")
{
  otg_rig const r;
  auto const    host         = std::make_shared<pull_up_host>();
  auto const    usv_warnings = [&] {
    return std::ranges::count_if(r.logged, [](core::log_record const& record) {
      return record.kind == core::log_kind::model_warning
         and record.message.find("USV") != std::string::npos;
    });
  };
  r.supply(true);
  r.write(gccfg, pwrdwn);
  r.write(gotgctl, bvaloen | bvaloval);
  r.port().attach(host);
  r.write(dctl, r.read(dctl) & ~sdis);
  REQUIRE(r.port().pulled_up());
  r.f.target->reset();
  CHECK_FALSE(r.port().pulled_up());
  CHECK(usv_warnings() == 0);
  r.write(gccfg, pwrdwn);
  r.write(gotgctl, bvaloen | bvaloval);
  r.write(dctl, r.read(dctl) & ~sdis);
  CHECK_FALSE(r.port().pulled_up());
  CHECK(usv_warnings() == 1);
}

namespace
{
void set_rcc(otg_rig const& r, std::string_view reg, std::string_view field, std::uint32_t value)
{
  auto const& desc  = r.f.target->peripheral("RCC").description();
  auto const  found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  auto const mask = ((std::uint32_t{1} << bits->width) - 1u) << bits->lsb;
  auto const old  = r.f.target->mem().read(desc.base + found->offset, 4);
  REQUIRE(old.has_value());
  auto const word = (static_cast<std::uint32_t>(*old) & ~mask) | ((value << bits->lsb) & mask);
  REQUIRE(r.f.target->mem().write(desc.base + found->offset, 4, word).has_value());
}

void clk48_from_pll(otg_rig const& r, std::uint32_t n)
{
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  set_rcc(r, "PLLCFGR", "PLLSRC", 1);
  set_rcc(r, "PLLCFGR", "PLLM", 0);
  set_rcc(r, "PLLCFGR", "PLLN", n);
  set_rcc(r, "PLLCFGR", "PLLQ", 1);
  set_rcc(r, "PLLCFGR", "PLLQEN", 1);
  set_rcc(r, "CR", "PLLON", 1);
  r.f.run_for(100'000'000);
  set_rcc(r, "CCIPR", "CLK48SEL", 2);
}

auto clock_warnings(otg_rig const& r) -> std::ptrdiff_t
{
  return std::ranges::count_if(r.logged, [](core::log_record const& record) {
    return record.kind == core::log_kind::model_warning
       and record.message.find("48 MHz kernel clock") != std::string::npos;
  });
}
}

TEST_CASE(
    "CLK48SEL: powering the transceiver with CLK48SEL on no clock checks at each rising PWRDWN "
    "write and warns once")
{
  otg_rig const r;
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  REQUIRE(r.cpu_store(gccfg, pwrdwn).reason == popcorn::stop_reason::user);
  REQUIRE(r.cpu_store(gccfg, 0).reason == popcorn::stop_reason::user);
  REQUIRE(r.cpu_store(gccfg, pwrdwn).reason == popcorn::stop_reason::user);
  CHECK(clock_warnings(r) == 1);
}

TEST_CASE("CLK48SEL: a transceiver left powered down checks no clock")
{
  otg_rig const r;
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  REQUIRE(r.cpu_store(gccfg, 0).reason == popcorn::stop_reason::user);
  CHECK(clock_warnings(r) == 0);
}

TEST_CASE("under strict_peripherals a GCCFG PWRDWN write with no 48 MHz clock raises model_error")
{
  otg_rig const r{true};
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  CHECK_THROWS_AS(static_cast<void>(r.cpu_store(gccfg, pwrdwn)), popcorn::model_error);
}

TEST_CASE("under strict_peripherals a store to any word of the six FIFO windows is served")
{
  otg_rig const r{true};
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  for(std::uint32_t const window : {0x1000u, 0x3000u, 0x6000u})
  {
    for(std::uint32_t const word : {0u, 4u, 0x7fcu, 0xffcu})
    {
      CAPTURE(window);
      CAPTURE(word);
      CHECK_NOTHROW(static_cast<void>(r.cpu_store(window + word, 0x1234'5678)));
    }
  }
}

TEST_CASE("under strict_peripherals a store past the sixth FIFO window still raises")
{
  otg_rig const r{true};
  popcorn::testing::enable_clocks(
      *r.f.target, {
                       {"AHB2ENR", "OTGFSEN"}
  });
  CHECK_THROWS_AS(static_cast<void>(r.cpu_store(0x7000, 0)), popcorn::model_error);
}

TEST_CASE("a debugger GCCFG PWRDWN write with no 48 MHz clock is served on a strict machine")
{
  otg_rig const r{true};
  CHECK_NOTHROW(r.write(gccfg, pwrdwn));
  CHECK(clock_warnings(r) == 0);
}

TEST_CASE("USB 2.0 7.1.11: PLL Q at 48 MHz powers the transceiver silently")
{
  otg_rig const r{true};
  clk48_from_pll(r, 48);
  REQUIRE_NOTHROW(static_cast<void>(r.cpu_store(gccfg, pwrdwn)));
  CHECK(clock_warnings(r) == 0);
}

TEST_CASE("only the PWRDWN write that powers the transceiver checks the clock")
{
  otg_rig const r{true};
  clk48_from_pll(r, 48);
  REQUIRE_NOTHROW(static_cast<void>(r.cpu_store(gccfg, pwrdwn)));
  set_rcc(r, "CCIPR", "CLK48SEL", 0);
  CHECK_NOTHROW(static_cast<void>(r.cpu_store(gotgctl, 0)));
  CHECK_NOTHROW(static_cast<void>(r.cpu_store(gccfg, pwrdwn | (1u << 21))));
  CHECK(clock_warnings(r) == 0);
}

TEST_CASE("USB 2.0 7.1.11: PLL Q at 47 MHz warns, 0.25 % is the full-speed tolerance")
{
  otg_rig const r;
  clk48_from_pll(r, 47);
  REQUIRE(r.cpu_store(gccfg, pwrdwn).reason == popcorn::stop_reason::user);
  CHECK(clock_warnings(r) == 1);
}
