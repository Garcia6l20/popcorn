#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/flash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::part_description;
using popcorn::peripheral_desc;
using popcorn::register_desc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

auto unit(part_description& desc, std::string_view name) -> peripheral_desc&
{
  auto const found = std::ranges::find(desc.peripherals, name, &peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  return *found;
}

auto described(part_description& desc, std::string_view peripheral, std::string_view name)
    -> register_desc&
{
  auto& regs  = unit(desc, peripheral).registers;
  auto  found = std::ranges::find(regs, name, &register_desc::name);
  REQUIRE(found != regs.end());
  return *found;
}

void rename_field(register_desc& reg, std::string_view from, std::string_view to)
{
  auto const found = std::ranges::find(reg.fields, from, &popcorn::field_desc::name);
  REQUIRE(found != reg.fields.end());
  found->name = to;
}

auto idle(part_description desc, core::machine_options options = popcorn::testing::stm32_options())
    -> stm32_fixture
{
  stm32_fixture f{std::move(desc), "rcc_ll", std::move(options)};
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  return f;
}

auto address_of(stm32_fixture const& f, std::string_view peripheral, std::string_view reg)
    -> std::uint32_t
{
  auto const& desc  = f.target->peripheral(peripheral).description();
  auto const* found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  return static_cast<std::uint32_t>(desc.base + (found->offset & ~3u));
}

auto bit_of(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::string_view     field) -> std::uint32_t
{
  auto const* found = popcorn::find_register(f.target->peripheral(peripheral).description(), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << (bits->lsb + 8 * (found->offset % 4));
}

void write(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::uint32_t        value)
{
  REQUIRE(f.target->mem().write(address_of(f, peripheral, reg), 4, value).has_value());
}

auto read(stm32_fixture const& f, std::string_view peripheral, std::string_view reg)
    -> std::uint32_t
{
  auto const value = f.target->mem().read(address_of(f, peripheral, reg), 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  auto const word = f.target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
  REQUIRE(word.has_value());
  return (*word >> (irq % 32) & 1u) != 0;
}

auto irq_number(part_description const& desc, std::string_view name) -> std::uint16_t
{
  auto const found = std::ranges::find(desc.irqs, name, &popcorn::irq_desc::name);
  REQUIRE(found != desc.irqs.end());
  return found->number;
}

auto claims(stm32_fixture const& f, std::string_view peripheral) -> std::vector<std::string>
{
  auto const* model = f.target->model(peripheral);
  REQUIRE(model != nullptr);
  return model->claims();
}
}

TEST_CASE("RM0481 13.4: GPIO serves a port whose registers carry only per-port aliases")
{
  auto desc = u575();
  for(auto& reg : unit(desc, "GPIOA").registers)
  {
    REQUIRE(reg.aliases.has_value());
    for(auto& alias : *reg.aliases)
    {
      REQUIRE(alias.starts_with("GPIO_"));
      alias.replace(0, 5, "GPIOA_");
    }
  }
  {
    auto options = popcorn::testing::stm32_options();
    REQUIRE(stm32::configure_option_bytes(desc, {.tzen = true}, options).has_value());
    auto const secure = idle(desc, options);
    CHECK(secure.target->model("GPIOA") != nullptr);
  }
  auto const f = idle(desc);
  REQUIRE(f.target->model("GPIOA") != nullptr);
  write(f, "GPIOA", "BSRR", bit_of(f, "GPIOA", "BSRR", "BS5"));
  CHECK((read(f, "GPIOA", "ODR") & bit_of(f, "GPIOA", "ODR", "OD5")) != 0);
  write(f, "GPIOA", "BRR", bit_of(f, "GPIOA", "BRR", "BR5"));
  CHECK((read(f, "GPIOA", "ODR") & bit_of(f, "GPIOA", "ODR", "OD5")) == 0);
}

TEST_CASE("RM0481 57.7: an LPUART named LPUART with CR1 and ISR views takes its only IRQ")
{
  auto desc                              = u575();
  auto line                              = irq_number(desc, "LPUART1_IRQn");
  described(desc, "LPUART1", "CR1").name = "CR1_enabled";
  described(desc, "LPUART1", "ISR").name = "ISR_enabled";
  for(auto& rule : unit(desc, "LPUART1").cross_rules)
  {
    for(auto* const name : {&rule.reg, &rule.target_reg})
    {
      if(*name == "CR1" or *name == "ISR")
      {
        *name += "_enabled";
      }
    }
  }
  unit(desc, "LPUART1").name = "LPUART";
  for(auto& p : desc.peripherals)
  {
    if(p.alias_of == "LPUART1")
    {
      p.alias_of = "LPUART";
    }
  }
  REQUIRE(
      std::ranges::find(desc.irqs, std::string_view{"LPUART_IRQn"}, &popcorn::irq_desc::name)
      == desc.irqs.end());
  auto const f = idle(desc);
  REQUIRE(f.target->model("LPUART") != nullptr);
  CHECK_FALSE(pending(f, line));
  write(
      f, "LPUART", "CR1_enabled",
      bit_of(f, "LPUART", "CR1_enabled", "UE")
          | bit_of(f, "LPUART", "CR1_enabled", "TE")
          | bit_of(f, "LPUART", "CR1_enabled", "TCIE"));
  CHECK((read(f, "LPUART", "ISR_enabled") & bit_of(f, "LPUART", "ISR_enabled", "TEACK")) != 0);
  CHECK(pending(f, line));
}

TEST_CASE("RM0481 53.9: I2C serves an instance without AUTOCR")
{
  auto  desc = u575();
  auto& regs = unit(desc, "I2C1").registers;
  REQUIRE(std::erase_if(regs, [](register_desc const& reg) { return reg.name == "AUTOCR"; }) == 1);
  auto const f = idle(desc);
  REQUIRE(f.target->model("I2C1") != nullptr);
  CHECK(std::ranges::contains(claims(f, "I2C1"), std::string{"TIMINGR"}));
  CHECK_FALSE(std::ranges::contains(claims(f, "I2C1"), std::string{"AUTOCR"}));
  write(f, "I2C1", "TIMINGR", 0x1042'0f13);
  CHECK(read(f, "I2C1", "TIMINGR") == 0x1042'0f13);
  auto const whole = idle(u575());
  CHECK(std::ranges::contains(claims(whole, "I2C1"), std::string{"AUTOCR"}));
}

TEST_CASE("RM0481 51.6.8 and 52.7.10: RTC and TAMP take PRIVCFGR as their privilege register")
{
  auto desc                              = u575();
  described(desc, "RTC", "PRIVCR").name  = "PRIVCFGR";
  described(desc, "TAMP", "PRIVCR").name = "PRIVCFGR";
  auto const plain                       = idle(desc);
  CHECK(std::ranges::contains(claims(plain, "RTC"), std::string{"PRIVCFGR"}));
  for(auto const trustzone : {false, true})
  {
    CAPTURE(trustzone);
    auto options = popcorn::testing::stm32_options();
    if(trustzone)
    {
      REQUIRE(stm32::configure_option_bytes(desc, {.tzen = true}, options).has_value());
    }
    auto const                    f = idle(desc, options);
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    write(f, "RTC", "PRIVCFGR", 0x1);
    write(f, "TAMP", "PRIVCFGR", 0x1);
    auto const count = [&](std::string_view peripheral) {
      return std::ranges::count_if(logged, [&](core::log_record const& record) {
        return record.kind == core::log_kind::model_warning and record.peripheral == peripheral;
      });
    };
    CHECK(count("RTC") == (trustzone ? 1 : 0));
    CHECK(count("TAMP") == (trustzone ? 1 : 0));
  }
}

TEST_CASE("RM0481 59.6.2 and 59.6.10: SAI takes SAIEN as the block enable when CR1 names it so")
{
  auto desc = u575();
  for(auto const reg : {"ACR1", "BCR1"})
  {
    rename_field(described(desc, "SAI1", reg), "SAIAEN", "SAIEN");
  }
  auto const f = idle(desc);
  REQUIRE(f.target->model("SAI1") != nullptr);
  write(f, "SAI1", "ACR1", bit_of(f, "SAI1", "ACR1", "SAIEN"));
  CHECK((read(f, "SAI1", "ACR1") & bit_of(f, "SAI1", "ACR1", "SAIEN")) != 0);
}

TEST_CASE("RM0481 9.6: DCACHE raises its line when the part names it DCACHE_IRQn")
{
  auto       desc = u575();
  auto const found =
      std::ranges::find(desc.irqs, std::string_view{"DCACHE1_IRQn"}, &popcorn::irq_desc::name);
  REQUIRE(found != desc.irqs.end());
  found->name     = "DCACHE_IRQn";
  auto const line = found->number;
  auto const f    = idle(desc);
  REQUIRE(f.target->model("DCACHE") != nullptr);
  write(f, "DCACHE", "DCACHE_IER", bit_of(f, "DCACHE", "DCACHE_IER", "CMDENDIE"));
  f.run_to_stop(
      {.max_instructions = popcorn::icount{1'000'000},
       .until_time       = popcorn::vtime{f.target->virtual_clock().time().ps + 20'000'000}});
  write(
      f, "DCACHE", "DCACHE_CR",
      bit_of(f, "DCACHE", "DCACHE_CR", "EN")
          | 2u * bit_of(f, "DCACHE", "DCACHE_CR", "CACHECMD")
          | bit_of(f, "DCACHE", "DCACHE_CR", "STARTCMD"));
  f.run_to_stop(
      {.max_instructions = popcorn::icount{1'000'000},
       .until_time       = popcorn::vtime{f.target->virtual_clock().time().ps + 20'000'000}});
  CHECK((read(f, "DCACHE", "DCACHE_SR") & bit_of(f, "DCACHE", "DCACHE_SR", "CMDENDF")) != 0);
  CHECK(pending(f, line));
}
