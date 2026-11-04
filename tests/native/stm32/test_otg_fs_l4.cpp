#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
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
  auto const    reset = r.peek(gpwrdn);
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
  auto const    reset = r.peek(gpwrdn);
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
