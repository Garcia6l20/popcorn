#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::config_error;
using popcorn::part_description;
using popcorn::peripheral_desc;
using popcorn::register_desc;

constexpr popcorn::address code_at     = 0x2000'0100;
constexpr std::uint32_t    dbgmcu_base = 0xE004'2000;
constexpr std::uint32_t    dbgmcu_cr   = dbgmcu_base + 0x04;
constexpr std::uint32_t    exti_base   = 0x4001'0400;
constexpr std::uint32_t    uart4_base  = 0x4000'4C00;

auto word(std::string name, std::uint32_t offset) -> register_desc
{
  register_desc reg;
  reg.name       = std::move(name);
  reg.offset     = offset;
  reg.width      = 32;
  reg.reset_mask = 0xFFFF'FFFFu;
  reg.fields     = {
      {.name = "VAL", .width = 32}
  };
  return reg;
}

auto block(
    std::string   name,
    std::string   ip,
    std::string   version,
    std::uint32_t base,
    std::uint32_t reg) -> peripheral_desc
{
  peripheral_desc unit;
  unit.name       = std::move(name);
  unit.ip         = std::move(ip);
  unit.ip_version = std::move(version);
  unit.base       = base;
  unit.size       = 0x400;
  unit.registers  = {word("R0", 0), word("R1", reg)};
  return unit;
}

auto toy(std::string part) -> part_description
{
  auto desc                = popcorn::testing::m33();
  desc.part                = std::move(part);
  desc.core.cpu            = "CM3";
  desc.core.revision       = "r2p1";
  desc.core.fpu            = false;
  desc.core.dsp            = false;
  desc.core.nvic_prio_bits = 4;
  desc.core.num_irqs       = 8;
  desc.peripherals.clear();
  desc.peripherals.push_back(block("EXTI", "EXTI", "", exti_base, 4));
  desc.peripherals.push_back(block("UART4", "UART", "sci2_v1_2_Cube", uart4_base, 4));
  desc.peripherals.push_back(block("DBGMCU", "DBGMCU", "", dbgmcu_base, 4));
  auto idcode   = word("IDCODE", 0);
  idcode.fields = {
      {.name = "DEV_ID", .width = 12},
      {.name = "REV_ID", .lsb = 16, .width = 16}
  };
  auto cr   = word("CR", 4);
  cr.fields = {
      {.name = "DBG_SLEEP", .width = 1},
      {.name = "DBG_STOP", .lsb = 1, .width = 1},
      {.name = "DBG_STANDBY", .lsb = 2, .width = 1},
      {.name = "TRACE_IOEN", .lsb = 5, .width = 1},
      {.name = "TRACE_MODE", .lsb = 6, .width = 2}
  };
  desc.peripherals.back().registers = {idcode, cr};
  return desc;
}

auto machine_of(part_description desc) -> core::machine
{
  stm32::register_builtin_models();
  return core::machine{
      std::move(desc), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
}
}

TEST_CASE("a Machine over an STM32L1 part attaches no model to the storage rows")
{
  auto target = machine_of(toy("STM32L152RETx"));
  CHECK(target.model("EXTI") == nullptr);
  CHECK(target.model("UART4") == nullptr);
  CHECK(target.model("DBGMCU") == nullptr);
}

TEST_CASE("an STM32L1 DBGMCU is storage until its model, a CPU store to CR reads back whole")
{
  auto target = machine_of(toy("STM32L152RETx"));
  REQUIRE(target.mem().write(code_at, 2, 0x6001).has_value());
  REQUIRE(target.mem().write(code_at + 2, 2, 0x6802).has_value());
  auto const set = [&](std::string_view name, std::uint64_t value) {
    target.regs().write(*target.regs().find(name), value);
  };
  target.reset();
  set("r0", dbgmcu_cr);
  set("r1", 0xFFFF'FFFFu);
  set("pc", code_at | 1u);
  CHECK_FALSE(target.run_slice({.max_instructions = popcorn::icount{2}}).stopped.has_value());
  CHECK(target.regs().read(*target.regs().find("r2")) == 0xFFFF'FFFFu);
}

TEST_CASE("factories() holds no entry for a storage row")
{
  stm32::register_builtin_models();
  auto const all = stm32::factories(stm32::board{}, toy("STM32L152RETx"));
  CHECK_FALSE(all.contains("EXTI"));
  CHECK_FALSE(all.contains("UART4"));
  CHECK_FALSE(all.contains("DBGMCU"));
}

TEST_CASE("an instance of a known IP version on a part outside every row glob is refused")
{
  auto const desc = toy("STM32Z999xx");
  CHECK_THROWS_AS((void)machine_of(desc), config_error);
  CHECK_THROWS_AS((void)stm32::factories(stm32::board{}, desc), config_error);
}

TEST_CASE("option_bytes.tzen on an STM32L1 part is refused")
{
  auto       options = popcorn::testing::stm32_options();
  auto const refused = stm32::configure_option_bytes(toy("STM32L152RETx"), {.tzen = true}, options);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().key == "option_bytes.tzen");
}

TEST_CASE("the shipped U5 and H5 parts attach the scoped models")
{
  stm32::register_builtin_models();
  auto const attached = [](part_description desc, std::initializer_list<std::string_view> names) {
    core::machine target{
        std::move(desc), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
    for(auto const name : names)
    {
      CAPTURE(name);
      CHECK(target.model(name) != nullptr);
    }
  };
  attached(
      popcorn::testing::description_from("POPCORN_U575_2M_DESC"),
      {"EXTI", "DBGMCU", "CRS", "ADC12", "UART4"});
  attached(
      popcorn::testing::description_from("POPCORN_H573_DESC"),
      {"EXTI", "DBGMCU", "CRS", "ADCC", "UART4"});
}
