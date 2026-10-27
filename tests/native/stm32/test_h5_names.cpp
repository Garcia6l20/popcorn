#include "factories.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/flash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::part_description;
using popcorn::peripheral_desc;
using popcorn::register_desc;
using popcorn::testing::h573;
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

constexpr std::uint32_t loop_at = 0x2000'0100;

auto h5(
    part_description      desc    = popcorn::testing::h573(),
    core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{std::move(desc), "h573_boot_ll", std::move(options)};
  REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
  f.set("pc", loop_at | 1u);
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

TEST_CASE("RM0481 57.7: the H573 LPUART takes the register views and the line LPUART1_IRQn")
{
  auto const line = irq_number(h573(), "LPUART1_IRQn");
  auto const f    = h5();
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

TEST_CASE(
    "RM0456 67.7 and RM0481 57.7: a description that disagrees with the LPUART variant throws")
{
  auto h5_plain                                     = h573();
  described(h5_plain, "LPUART", "CR1_enabled").name = "CR1";
  CHECK_THROWS_AS(h5(h5_plain), popcorn::config_error);
  auto u5_views                              = u575();
  described(u5_views, "LPUART1", "CR1").name = "CR1_enabled";
  CHECK_THROWS_AS(idle(u5_views), popcorn::config_error);
}

TEST_CASE("RM0481 53.9: the H573 I2C attaches without AUTOCR")
{
  auto const f = h5();
  REQUIRE(f.target->model("I2C1") != nullptr);
  CHECK(std::ranges::contains(claims(f, "I2C1"), std::string{"TIMINGR"}));
  CHECK_FALSE(std::ranges::contains(claims(f, "I2C1"), std::string{"AUTOCR"}));
  write(f, "I2C1", "TIMINGR", 0x1042'0f13);
  CHECK(read(f, "I2C1", "TIMINGR") == 0x1042'0f13);
  auto const whole = idle(u575());
  CHECK(std::ranges::contains(claims(whole, "I2C1"), std::string{"AUTOCR"}));
}

TEST_CASE(
    "RM0456 65.7.14 and RM0481 53.9: a description that disagrees with the I2C variant throws")
{
  auto u5_without = u575();
  REQUIRE(
      std::erase_if(
          unit(u5_without, "I2C1").registers,
          [](register_desc const& reg) { return reg.name == "AUTOCR"; })
      == 1);
  CHECK_THROWS_AS(idle(u5_without), popcorn::config_error);
  auto h5_with = h573();
  auto u5_copy = u575();
  unit(h5_with, "I2C1").registers.push_back(described(u5_copy, "I2C1", "AUTOCR"));
  CHECK_THROWS_AS(h5(h5_with), popcorn::config_error);
}

TEST_CASE(
    "RM0481 51.6.8 and 52.7.10: the H573 RTC and TAMP take PRIVCFGR as their privilege register")
{
  auto const plain = h5();
  CHECK(std::ranges::contains(claims(plain, "RTC"), std::string{"PRIVCFGR"}));
  CHECK(std::ranges::contains(claims(plain, "TAMP"), std::string{"PRIVCFGR"}));
  CHECK_FALSE(std::ranges::contains(claims(plain, "RTC"), std::string{"PRIVCR"}));
  auto const u5 = idle(u575());
  CHECK(std::ranges::contains(claims(u5, "RTC"), std::string{"PRIVCR"}));
  CHECK(std::ranges::contains(claims(u5, "TAMP"), std::string{"PRIVCR"}));
}

TEST_CASE("RM0481 51.6.8: an H573 RTC or TAMP without PRIVCFGR throws")
{
  for(auto const name : {"RTC", "TAMP"})
  {
    CAPTURE(name);
    auto desc                              = h573();
    described(desc, name, "PRIVCFGR").name = "PRIVCR";
    CHECK_THROWS_AS(h5(desc), popcorn::config_error);
  }
}

TEST_CASE(
    "RM0481 51.6.8 and 52.7.10: under TrustZone the first write to PRIVCFGR of the H573 RTC and "
    "TAMP logs one warning each, without TrustZone none")
{
  for(auto const trustzone : {false, true})
  {
    CAPTURE(trustzone);
    auto desc                              = u575();
    described(desc, "RTC", "PRIVCR").name  = "PRIVCFGR";
    described(desc, "TAMP", "PRIVCR").name = "PRIVCFGR";
    auto options                           = popcorn::testing::stm32_options();
    if(trustzone)
    {
      REQUIRE(stm32::configure_option_bytes(desc, {.tzen = true}, options).has_value());
    }
    options.models.insert_or_assign("RTC", stm32::rtc_factory(std::nullopt, stm32::h5_rtc));
    options.models.insert_or_assign("TAMP", stm32::tamp_factory(stm32::h5_rtc));
    auto const                    f = idle(desc, options);
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    for(auto const round : {0, 1})
    {
      CAPTURE(round);
      write(f, "RTC", "PRIVCFGR", 0x1);
      write(f, "TAMP", "PRIVCFGR", 0x1);
    }
    auto const count = [&](std::string_view peripheral) {
      return std::ranges::count_if(logged, [&](core::log_record const& record) {
        return record.kind == core::log_kind::model_warning and record.peripheral == peripheral;
      });
    };
    CHECK(count("RTC") == (trustzone ? 1 : 0));
    CHECK(count("TAMP") == (trustzone ? 1 : 0));
  }
}

TEST_CASE("RM0456 64.6.7 and RM0481 52.7.10: a TAMP without SECCFGR throws")
{
  for(auto const& make : {+[] { return u575(); }, +[] { return h573(); }})
  {
    auto desc = make();
    CAPTURE(desc.part);
    auto& registers = unit(desc, "TAMP").registers;
    REQUIRE(
        std::erase_if(registers, [](register_desc const& reg) { return reg.name == "SECCFGR"; })
        == 1);
    CHECK_THROWS_AS(
        stm32_fixture(
            desc, desc.part == "STM32H573AIIx" ? "h573_boot_ll" : "rcc_ll",
            popcorn::testing::stm32_options()),
        popcorn::config_error);
  }
}

TEST_CASE("the registry and factories() pick the same model for every unit of the U575 and H573")
{
  for(auto const& make : {+[] { return u575(); }, +[] { return h573(); }})
  {
    auto desc = make();
    CAPTURE(desc.part);
    auto const  firmware = desc.part == "STM32H573AIIx" ? "h573_boot_ll" : "rcc_ll";
    auto const  registry = stm32_fixture{desc, firmware, popcorn::testing::stm32_options()};
    auto const  rows     = stm32::factories(stm32::board{}, desc);
    std::size_t modelled = 0;
    for(auto const& peripheral : desc.peripherals)
    {
      if(peripheral.alias_of)
      {
        continue;
      }
      CAPTURE(peripheral.name);
      auto const* const model = registry.target->model(peripheral.name);
      CHECK(
          (model != nullptr or peripheral.name.starts_with("GTZC"))
          == rows.contains(peripheral.name));
      if(model == nullptr)
      {
        continue;
      }
      ++modelled;
      auto options   = popcorn::testing::stm32_options();
      options.models = {
          {peripheral.name, rows.at(peripheral.name)}
      };
      stm32_fixture const single{desc, firmware, options};
      auto const* const   other = single.target->model(peripheral.name);
      REQUIRE(other != nullptr);
      CHECK(typeid(*other) == typeid(*model));
      CHECK(other->claims() == model->claims());
    }
    CHECK(modelled > 20);
  }
}

TEST_CASE("a part no row of a versioned IP serves throws in both selection paths")
{
  auto desc = h573();
  desc.part = "STM32X999";
  CHECK_THROWS_AS((void)stm32::factories(stm32::board{}, desc), popcorn::config_error);
  CHECK_THROWS_AS(
      stm32_fixture(desc, "h573_boot_ll", popcorn::testing::stm32_options()),
      popcorn::config_error);
}

TEST_CASE("the variant checks name the register and say which side disagrees")
{
  auto without = h573();
  REQUIRE(
      std::erase_if(
          unit(without, "TAMP").registers,
          [](register_desc const& reg) { return reg.name == "PRIVCFGR"; })
      == 1);
  try
  {
    h5(without);
    FAIL("no throw");
  }
  catch(popcorn::config_error const& error)
  {
    std::string const text = error.what();
    INFO(text);
    CHECK(text.find("peripherals.TAMP.registers.PRIVCFGR") != std::string::npos);
    CHECK(text.find("needs this register, the description lacks it") != std::string::npos);
  }
  auto with    = h573();
  auto u5_copy = u575();
  unit(with, "I2C1").registers.push_back(described(u5_copy, "I2C1", "AUTOCR"));
  try
  {
    h5(with);
    FAIL("no throw");
  }
  catch(popcorn::config_error const& error)
  {
    std::string const text = error.what();
    INFO(text);
    CHECK(text.find("peripherals.I2C1.registers.I2C_AUTOCR") != std::string::npos);
    CHECK(text.find("has no such register, the description has one") != std::string::npos);
  }
}

TEST_CASE("RM0481 56.8: the USART variant takes the FIFO presence apart from the register views")
{
  auto  without = h573();
  auto& cr1     = described(without, "USART1", "CR1_enabled").fields;
  REQUIRE(std::erase_if(cr1, [](popcorn::field_desc const& f) { return f.name == "FIFOEN"; }) == 1);
  CHECK_THROWS_AS(h5(without), popcorn::config_error);
  auto options = popcorn::testing::stm32_options();
  options.models.insert_or_assign(
      "USART1", stm32::usart_factory(stm32::board{}, {.mode_views = true, .fifo = false}));
  auto const f = h5(without, options);
  REQUIRE(f.target->model("USART1") != nullptr);
  CHECK_THROWS_AS(
      h5(h573(),
         [] {
           auto o = popcorn::testing::stm32_options();
           o.models.insert_or_assign(
               "USART1", stm32::usart_factory(stm32::board{}, {.mode_views = true, .fifo = false}));
           return o;
         }()),
      popcorn::config_error);
}

TEST_CASE("RM0481 59.6.2: the H573 SAI takes SAIEN as the block enable")
{
  auto const f = h5();
  REQUIRE(f.target->model("SAI1") != nullptr);
  write(f, "SAI1", "ACR1", bit_of(f, "SAI1", "ACR1", "SAIEN"));
  CHECK((read(f, "SAI1", "ACR1") & bit_of(f, "SAI1", "ACR1", "SAIEN")) != 0);
}

TEST_CASE(
    "RM0456 69.6.2 and RM0481 59.6.2: a description that disagrees with the SAI variant throws")
{
  auto u5_renamed = u575();
  for(auto const reg : {"ACR1", "BCR1"})
  {
    rename_field(described(u5_renamed, "SAI1", reg), "SAIAEN", "SAIEN");
  }
  CHECK_THROWS_AS(idle(u5_renamed), popcorn::config_error);
}

TEST_CASE("RM0481 9.6: the H573 DCACHE raises the line DCACHE1_IRQn")
{
  auto const line = irq_number(h573(), "DCACHE1_IRQn");
  auto const f    = h5();
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
