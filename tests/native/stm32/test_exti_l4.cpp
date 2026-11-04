#include "exti_classic.hpp"
#include "factories.hpp"
#include "stm32_fixture.hpp"
#include "variants.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;

constexpr auto          firmware  = std::string_view{"l476_l4_exti_ll"};
constexpr std::uint32_t exti      = 0x4001'0400;
constexpr std::uint32_t imr1      = exti + 0x00;
constexpr std::uint32_t emr1      = exti + 0x04;
constexpr std::uint32_t rtsr1     = exti + 0x08;
constexpr std::uint32_t ftsr1     = exti + 0x0C;
constexpr std::uint32_t swier1    = exti + 0x10;
constexpr std::uint32_t pr1       = exti + 0x14;
constexpr std::uint32_t imr2      = exti + 0x20;
constexpr std::uint32_t rtsr2     = exti + 0x28;
constexpr std::uint32_t ftsr2     = exti + 0x2C;
constexpr std::uint32_t swier2    = exti + 0x30;
constexpr std::uint32_t pr2       = exti + 0x34;
constexpr std::uint32_t syscfg    = 0x4001'0000;
constexpr std::uint32_t memrmp    = syscfg + 0x00;
constexpr std::uint32_t exticr1   = syscfg + 0x08;
constexpr std::uint32_t skr       = syscfg + 0x24;
constexpr std::uint32_t apb2rstr  = 0x4002'1000 + 0x40;
constexpr std::uint32_t syscfgrst = 1u << 0;
constexpr std::uint32_t exti0     = 6;
constexpr std::uint32_t pvd_pvm   = 1;

auto parked(
    part_description const& desc    = l476rc(),
    core::machine_options   options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{desc, firmware, std::move(options)};
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  return f;
}

auto read(stm32_fixture const& f, std::uint32_t at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void write(stm32_fixture const& f, std::uint32_t at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}

auto cpu_store(stm32_fixture const& f, std::uint32_t at, std::uint32_t value) -> popcorn::stop
{
  constexpr std::uint32_t code = 0x2000'0100;
  REQUIRE(f.target->mem().write(code, 2, 0x6001).has_value());
  REQUIRE(f.target->mem().write(code + 2, 2, 0xbe00).has_value());
  f.set("r0", at);
  f.set("r1", value);
  f.set("pc", code | 1u);
  return f.run_to_stop({.max_instructions = icount{4}});
}

auto irq_pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  return (read(f, 0xE000'E200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
}

void clear_pending(stm32_fixture const& f, std::uint32_t irq)
{
  write(f, 0xE000'E280u + 4 * (irq / 32), 1u << (irq % 32));
}

auto port(stm32_fixture const& f, std::string_view name) -> stm32::gpio_port&
{
  auto* const found = f.model_of<stm32::gpio_port>(name);
  REQUIRE(found != nullptr);
  return *found;
}

constexpr auto watched = std::to_array<std::uint32_t>({1, 2, 3, 6, 7, 8, 9, 10, 23, 40, 41, 64});

auto pending_irqs(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const irq : watched)
  {
    if(irq_pending(f, irq))
    {
      out.push_back(irq);
    }
  }
  return out;
}

void clear_watched(stm32_fixture const& f)
{
  for(auto const irq : watched)
  {
    clear_pending(f, irq);
  }
}

auto with_register_fields(std::string_view reg, auto const& edit) -> part_description
{
  auto part       = l476rc();
  auto peripheral = std::ranges::find(
      part.peripherals, std::string_view{"EXTI"}, &popcorn::peripheral_desc::name);
  REQUIRE(peripheral != part.peripherals.end());
  auto found = std::ranges::find(peripheral->registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != peripheral->registers.end());
  edit(*found);
  return part;
}
}

TEST_CASE("RM0351 14.3: PA0 rises into EXTI0 on NVIC 6, SWIER2 line 35 enters PVD_PVM on NVIC 1")
{
  stm32_fixture f{l476rc(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  REQUIRE(f.log() == std::vector<std::uint32_t>{0, 0xff82'0001, 0x8f});
  port(f, "GPIOA").drive(0, true);
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0, 0xff82'0001, 0x8f, 0xe001, 0xb3, 0x3508, 0, 0});
}

TEST_CASE("RM0351 9.2.3: PH0 takes line 0 at code 7, code 7 on line 2 selects no port")
{
  auto const f = parked();
  write(f, exticr1, 0xffff);
  CHECK(read(f, exticr1) == 0x7777u);
  auto const* const selector = f.model_of<stm32::exti_selector>("SYSCFG");
  REQUIRE(selector != nullptr);
  CHECK(selector->selected_port(0) == std::optional<std::uint8_t>{'H' - 'A'});
  CHECK(selector->selected_port(1) == std::optional<std::uint8_t>{'H' - 'A'});
  CHECK(selector->selected_port(2) == std::nullopt);
  CHECK(selector->selected_port(3) == std::nullopt);
  write(f, exticr1, 0x0607);
  CHECK(selector->selected_port(2) == std::optional<std::uint8_t>{'G' - 'A'});
  write(f, 0x4800'0000, 0xabff'fffc);
  write(f, 0x4800'1c00, 0);
  write(f, imr1, 0xff82'0005);
  write(f, rtsr1, 0x5);
  port(f, "GPIOA").drive(0, true);
  CHECK(read(f, pr1) == 0);
  port(f, "GPIOH").drive(0, true);
  CHECK(read(f, pr1) == 1u);
  CHECK(irq_pending(f, exti0));
}

TEST_CASE("RM0351 14.5: IMR1 and IMR2 reset to the direct lines, the edge registers lack them")
{
  auto const f = parked();
  f.target->reset(popcorn::reset_kind::power_on);
  CHECK(read(f, imr1) == 0xff82'0000u);
  CHECK(read(f, imr2) == 0x0000'0087u);
  for(auto const at : {rtsr1, ftsr1})
  {
    write(f, at, ~std::uint32_t{});
    CHECK(read(f, at) == 0x007d'ffffu);
  }
  for(auto const at : {rtsr2, ftsr2})
  {
    write(f, at, 0xff);
    CHECK(read(f, at) == 0x78u);
  }
  write(f, imr2, ~std::uint32_t{});
  CHECK(read(f, imr2) == 0xffu);
  write(f, imr1, 0);
  CHECK(read(f, imr1) == 0);
  write(f, imr2, 0);
  write(f, swier1, ~std::uint32_t{});
  CHECK(read(f, pr1) == 0);
  write(f, swier2, ~std::uint32_t{});
  CHECK(read(f, pr2) == 0);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(read(f, imr1) == 0xff82'0000u);
  CHECK(read(f, imr2) == 0x0000'0087u);
  CHECK(read(f, rtsr2) == 0);
  CHECK(read(f, swier1) == 0);
}

TEST_CASE("RM0351 Table 61: every EXTI line 0 to 39 raises its NVIC line only, a direct line none")
{
  auto const expected = std::to_array<std::optional<std::uint32_t>>({
      6, 7,  8,  9,  10, 23, 23, 23, 23, 23, 40, 40, 40, 40, 40, 40, 1, {}, 41, 2,
      3, 64, 64, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, 1,  1, 1,  1,  {},
  });
  auto const f        = parked();
  write(f, imr1, ~std::uint32_t{});
  write(f, imr2, 0xff);
  for(std::uint32_t line = 0; line < expected.size(); ++line)
  {
    CAPTURE(line);
    auto const swier = line < 32 ? swier1 : swier2;
    auto const pr    = line < 32 ? pr1 : pr2;
    auto const bit   = 1u << (line % 32);
    write(f, swier, bit);
    CHECK(read(f, pr) == (expected[line] ? bit : 0u));
    CHECK(read(f, swier) == (expected[line] ? bit : 0u));
    CHECK(
        pending_irqs(f)
        == (expected[line] ? std::vector<std::uint32_t>{*expected[line]}
                           : std::vector<std::uint32_t>{}));
    write(f, pr, bit);
    CHECK(read(f, pr) == 0);
    CHECK(read(f, swier) == 0);
    clear_watched(f);
  }
}

TEST_CASE("RM0351 Table 61: PVD_PVM stays raised while line 16 or a line 35 to 38 is pending")
{
  auto const f = parked();
  write(f, imr1, ~std::uint32_t{});
  write(f, imr2, 0xff);
  write(f, swier2, 1u << 3);
  CHECK(read(f, pr2) == 1u << 3);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, swier1, 1u << 16);
  write(f, pr2, 1u << 3);
  clear_pending(f, pvd_pvm);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, swier2, 1u << 6);
  write(f, pr1, 1u << 16);
  clear_pending(f, pvd_pvm);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, pr2, 1u << 6);
  clear_pending(f, pvd_pvm);
  CHECK_FALSE(irq_pending(f, pvd_pvm));
  CHECK(pending_irqs(f).empty());
}

TEST_CASE(
    "RM0351 14.3: a direct line latches nothing and raises no NVIC line, its IMR bit is storage")
{
  auto const f     = parked();
  auto*      input = f.model_of<stm32::exti_line_input>("EXTI");
  REQUIRE(input != nullptr);
  write(f, imr1, 0xff82'0000u | (1u << 18));
  write(f, rtsr1, 1u << 18);
  for(auto const line : {std::uint8_t{17}, std::uint8_t{23}, std::uint8_t{31}})
  {
    CAPTURE(line);
    input->line_changed(line, true);
    input->line_changed(line, false);
  }
  input->line_changed(39, true);
  input->line_changed(40, true);
  CHECK(read(f, pr1) == 0);
  CHECK(read(f, pr2) == 0);
  CHECK(pending_irqs(f).empty());
  input->line_changed(18, true);
  CHECK(read(f, pr1) == 1u << 18);
  CHECK(pending_irqs(f) == std::vector<std::uint32_t>{41});
  write(f, rtsr2, 1u << 4);
  write(f, imr2, 1u << 4);
  input->line_changed(36, true);
  CHECK(read(f, pr2) == 1u << 4);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, imr1, 1u << 23);
  CHECK(read(f, imr1) == 1u << 23);
  write(f, emr1, 1u << 23);
  CHECK(read(f, emr1) == 1u << 23);
}

TEST_CASE("a description whose edge register has a direct line's bit, or lacks a line, throws")
{
  auto const tr17 = with_register_fields("RTSR1", [](popcorn::register_desc& reg) {
    auto added = reg.fields.front();
    added.name = "TR17";
    added.lsb  = 17;
    reg.fields.push_back(added);
  });
  try
  {
    stm32_fixture f{tr17, firmware};
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& error)
  {
    CHECK(std::string_view{error.what()}.contains("RTSR1"));
  }
  auto const no_pr38 = with_register_fields("PR2", [](popcorn::register_desc& reg) {
    std::erase_if(reg.fields, [](auto const& field) { return field.name == "PIF38"; });
  });
  CHECK_THROWS_WITH_AS(
      (stm32_fixture{no_pr38, firmware}), doctest::Contains("PR2"), popcorn::config_error);
  auto const imr2_short = with_register_fields("IMR2", [](popcorn::register_desc& reg) {
    std::erase_if(reg.fields, [](auto const& field) { return field.name == "MR39"; });
  });
  CHECK_THROWS_WITH_AS(
      (stm32_fixture{imr2_short, firmware}), doctest::Contains("IMR2"), popcorn::config_error);
}

TEST_CASE(
    "RM0351 9.2: the L4 SYSCFG registers outside EXTICR are storage, one warning each, "
    "model_error under strict, SYSCFGRST keeps MEMRMP")
{
  {
    auto const                    f = parked();
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(cpu_store(f, skr, 0xca).reason == stop_reason::user);
    REQUIRE(cpu_store(f, skr, 0x53).reason == stop_reason::user);
    REQUIRE(cpu_store(f, memrmp, 0x1).reason == stop_reason::user);
    REQUIRE(cpu_store(f, exticr1, 0x2).reason == stop_reason::user);
    REQUIRE(logged.size() == 2);
    CHECK(logged[0].reg == "SKR");
    CHECK(logged[1].reg == "MEMRMP");
    CHECK(logged[1].kind == core::log_kind::model_warning);
    write(f, apb2rstr, syscfgrst);
    write(f, apb2rstr, 0);
    CHECK(read(f, exticr1) == 0);
    CHECK(f.target->peripheral("SYSCFG").peek(0) == 0x1u);
  }
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const f               = parked(l476rc(), std::move(options));
  CHECK_THROWS_AS((void)cpu_store(f, skr, 0xca), popcorn::model_error);
  CHECK(f.target->peripheral("SYSCFG").peek(0x24) == 0);
}

TEST_CASE("the L4 EXTI row serves the L476, the L1 variant does not attach to its description")
{
  auto const f = parked();
  CHECK(f.model_of<stm32::exti_line_input>("EXTI") != nullptr);
  stm32::register_builtin_models();
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(stm32::board{}, l476rc());
  options.models.insert_or_assign("EXTI", stm32::exti_classic_factory(stm32::l1_exti_24));
  CHECK_THROWS_AS(
      (core::machine{l476rc(), popcorn::arch::armv8m::factory(), std::move(options)}),
      popcorn::config_error);
}
