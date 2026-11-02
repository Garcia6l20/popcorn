#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
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

auto toy(std::string part, bool with_uart4 = true) -> part_description
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
  if(with_uart4)
  {
    desc.peripherals.push_back(block("UART4", "UART", "sci2_v1_2_Cube", uart4_base, 4));
  }
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

struct l4_storage
{
  std::string_view name;
  std::string_view ip;
  std::string_view version;
  std::uint32_t    base;
};

constexpr auto l4_storage_rows = std::to_array<l4_storage>({
    {.name = "EXTI",    .ip = "EXTI",   .version = "",               .base = 0x4001'0400},
    {.name = "UART4",   .ip = "UART",   .version = "sci2_v2_1_Cube", .base = 0x4000'4C00},
    {.name = "LPUART1", .ip = "LPUART", .version = "sci3_v1_1_Cube", .base = 0x4000'8000},
    {.name = "DBGMCU",  .ip = "DBGMCU", .version = "",               .base = 0xE004'2000},
});

auto toy_l4(std::span<l4_storage const> rows) -> part_description
{
  auto desc                = popcorn::testing::m33();
  desc.part                = "STM32L476RCTx";
  desc.core.cpu            = "CM4";
  desc.core.revision       = "r0p1";
  desc.core.trustzone      = false;
  desc.core.nvic_prio_bits = 4;
  desc.core.num_irqs       = 82;
  desc.peripherals.clear();
  for(auto const& row : rows)
  {
    desc.peripherals.push_back(
        block(std::string{row.name}, std::string{row.ip}, std::string{row.version}, row.base, 4));
  }
  return desc;
}

auto storage_row(std::string_view name) -> std::span<l4_storage const>
{
  auto const found = std::ranges::find(l4_storage_rows, name, &l4_storage::name);
  REQUIRE(found != l4_storage_rows.end());
  return {found, 1};
}

auto cpu_load(core::machine& target, std::uint32_t at) -> std::optional<std::uint64_t>
{
  REQUIRE(target.mem().write(code_at, 2, 0x6802).has_value());
  target.reset();
  target.regs().write(*target.regs().find("r0"), at);
  target.regs().write(*target.regs().find("r2"), 0xDEAD'BEEFu);
  target.regs().write(*target.regs().find("pc"), code_at | 1u);
  if(target.run_slice({.max_instructions = popcorn::icount{1}}).stopped.has_value())
  {
    return std::nullopt;
  }
  return target.regs().read(*target.regs().find("r2"));
}
}

TEST_CASE("a Machine over an STM32L1 part attaches the L1 DBGMCU model")
{
  auto target = machine_of(toy("STM32L152RETx", false));
  CHECK(target.model("DBGMCU") != nullptr);
}

TEST_CASE("a UART4 of sci2 on an STM32L1 part takes the USART model, which needs its registers")
{
  CHECK_THROWS_AS((void)machine_of(toy("STM32L152RETx")), config_error);
}

TEST_CASE("the STM32L1 DBGMCU model keeps the CR fields of a CPU store of all ones")
{
  auto target = machine_of(toy("STM32L152RETx", false));
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
  CHECK(target.regs().read(*target.regs().find("r2")) == 0xE7u);
}

TEST_CASE("factories() of an STM32L1 part holds the sci2 UART4 and the DBGMCU")
{
  stm32::register_builtin_models();
  auto const all = stm32::factories(stm32::board{}, toy("STM32L152RETx"));
  CHECK(all.contains("UART4"));
  CHECK(all.contains("DBGMCU"));
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

TEST_CASE("an STM32L4 EXTI of no IP version attaches as storage")
{
  CHECK(machine_of(toy_l4(storage_row("EXTI"))).model("EXTI") == nullptr);
}

TEST_CASE("an STM32L4 UART4 of sci2_v2_1_Cube attaches as storage")
{
  CHECK(machine_of(toy_l4(storage_row("UART4"))).model("UART4") == nullptr);
}

TEST_CASE("an STM32L4 LPUART1 of sci3_v1_1_Cube attaches as storage")
{
  CHECK(machine_of(toy_l4(storage_row("LPUART1"))).model("LPUART1") == nullptr);
}

TEST_CASE("an STM32L4 DBGMCU of no IP version attaches as storage")
{
  CHECK(machine_of(toy_l4(storage_row("DBGMCU"))).model("DBGMCU") == nullptr);
}

TEST_CASE("an STM32L4 CRS is refused, the L4 descriptions carry none")
{
  constexpr auto crs = std::to_array<l4_storage>({
      {.name = "CRS", .ip = "CRS", .version = "", .base = 0x4000'6000}
  });
  CHECK_THROWS_AS((void)machine_of(toy_l4(crs)), config_error);
}

TEST_CASE("factories() of an STM32L4 part holds none of the storage instances")
{
  stm32::register_builtin_models();
  auto const all = stm32::factories(stm32::board{}, toy_l4(l4_storage_rows));
  for(auto const& row : l4_storage_rows)
  {
    CAPTURE(row.name);
    CHECK_FALSE(all.contains(std::string{row.name}));
  }
}

TEST_CASE("RM0351 48.6.4, 48.15.3 and 48.17.9: the L4 TPIU, ETM and ROM table are RAZ/WI")
{
  auto const desc   = toy_l4(storage_row("DBGMCU"));
  auto const blocks = stm32::raz_wi_map(desc);
  REQUIRE(blocks.size() == 3);
  CHECK(blocks[0].start == 0xE004'0000u);
  CHECK(blocks[1].start == 0xE004'1000u);
  CHECK(blocks[2].start == 0xE00F'F000u);
  auto options   = popcorn::testing::stm32_options();
  options.raz_wi = blocks;
  stm32::register_builtin_models();
  core::machine target{desc, popcorn::arch::armv8m::factory(), std::move(options)};
  CHECK(cpu_load(target, 0xE004'0004u) == 0u);
  CHECK(cpu_load(target, 0xE004'1000u) == 0u);
  CHECK(cpu_load(target, 0xE00F'FFD0u) == 0u);
}

TEST_CASE("without the L4 RAZ/WI map a CPU load of the L4 TPIU faults")
{
  auto target = machine_of(toy_l4(storage_row("DBGMCU")));
  CHECK_FALSE(cpu_load(target, 0xE004'0004u).has_value());
}

TEST_CASE("option_bytes.tzen on an STM32L4 part is refused")
{
  auto       options = popcorn::testing::stm32_options();
  auto const refused =
      stm32::configure_option_bytes(toy_l4(l4_storage_rows), {.tzen = true}, options);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().key == "option_bytes.tzen");
}
