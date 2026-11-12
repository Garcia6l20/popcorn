#include "../support/inert_model.hpp"
#include "stm32_fixture.hpp"
#include "usart_rows.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/flash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
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

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(popcorn::periph::model_context& ctx) : context{ctx}
  {}

  popcorn::periph::model_context& context;
};

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
    auto options = popcorn::testing::stm32_options();
    if(trustzone)
    {
      REQUIRE(stm32::configure_option_bytes(h573(), {.tzen = true}, options).has_value());
    }
    auto const                    f = h5(h573(), options);
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

TEST_CASE(
    "the registry and factories() pick the same model for every unit of the U575 and H573, at "
    "TZEN=0 and 1")
{
  for(auto const& make : {+[] { return u575(); }, +[] { return h573(); }})
  {
    for(auto const trustzone : {false, true})
    {
      auto desc = make();
      CAPTURE(desc.part);
      CAPTURE(trustzone);
      auto const firmware = desc.part == "STM32H573AIIx" ? "h573_boot_ll" : "rcc_ll";
      auto const options  = [&] {
        auto out = popcorn::testing::stm32_options();
        if(trustzone)
        {
          REQUIRE(stm32::configure_option_bytes(desc, {.tzen = true}, out).has_value());
        }
        return out;
      };
      auto const  registry = stm32_fixture{desc, firmware, options()};
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
        if(trustzone)
        {
          CHECK((model != nullptr) == rows.contains(peripheral.name));
        }
        if(model == nullptr)
        {
          continue;
        }
        CHECK(rows.contains(peripheral.name));
        ++modelled;
        auto single_options = options();
        single_options.models.try_emplace(peripheral.name, rows.at(peripheral.name));
        stm32_fixture const single{desc, firmware, single_options};
        auto const* const   other = single.target->model(peripheral.name);
        REQUIRE(other != nullptr);
        CHECK(typeid(*other) == typeid(*model));
        CHECK(other->claims() == model->claims());
      }
      CHECK(modelled > 20);
    }
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
      "USART1", stm32::usart_factory(stm32::board{}.uarts, {.mode_views = true, .fifo = false}));
  auto const f = h5(without, options);
  REQUIRE(f.target->model("USART1") != nullptr);
  CHECK_THROWS_AS(
      h5(h573(),
         [] {
           auto o = popcorn::testing::stm32_options();
           o.models.insert_or_assign(
               "USART1",
               stm32::usart_factory(stm32::board{}.uarts, {.mode_views = true, .fifo = false}));
           return o;
         }()),
      popcorn::config_error);
}

TEST_CASE(
    "RM0481 56.8: H573 USART2 needs TXFNF, PRESC and NECF, WUFIE is a model_error under strict")
{
  auto  no_txfnf = h573();
  auto& isr      = described(no_txfnf, "USART2", "ISR_enabled").fields;
  REQUIRE(std::erase_if(isr, [](popcorn::field_desc const& f) { return f.name == "TXFNF"; }) == 1);
  CHECK_THROWS_WITH_AS(h5(no_txfnf), doctest::Contains("TXFNF"), popcorn::config_error);
  auto  no_necf = h573();
  auto& icr     = described(no_necf, "USART2", "ICR").fields;
  REQUIRE(std::erase_if(icr, [](popcorn::field_desc const& f) { return f.name == "NECF"; }) == 1);
  std::erase_if(unit(no_necf, "USART2").cross_rules, [](popcorn::cross_rule const& rule) {
    return rule.field == "NECF";
  });
  CHECK_THROWS_WITH_AS(h5(no_necf), doctest::Contains("NECF"), popcorn::config_error);
  auto  no_presc = h573();
  auto& regs     = unit(no_presc, "USART2").registers;
  REQUIRE(std::erase_if(regs, [](register_desc const& r) { return r.name == "PRESC"; }) == 1);
  CHECK_THROWS_WITH_AS(h5(no_presc), doctest::Contains("PRESC"), popcorn::config_error);
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  options.models.insert_or_assign(
      "CRC", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
        return std::make_shared<bus_probe>(ctx);
      });
  auto const  f     = h5(h573(), std::move(options));
  auto* const probe = f.model_of<bus_probe>("CRC");
  REQUIRE(probe != nullptr);
  auto const cr3 = address_of(f, "USART2", "CR3");
  CHECK_THROWS_AS(
      static_cast<void>(probe->context.services.write(
          cr3, 4, bit_of(f, "USART2", "CR3", "WUFIE"), popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(f.target->mem().read(cr3, 4).value_or(1) == 0);
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

namespace
{
struct h5_unit
{
  std::string_view name;
  bool             low_power;
};

constexpr h5_unit usart2{"USART2", false};
constexpr h5_unit uart4{"UART4", false};
constexpr h5_unit lpuart{"LPUART", true};

auto mask_of(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::string_view     field) -> std::uint32_t
{
  auto const* found = popcorn::find_register(f.target->peripheral(peripheral).description(), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return (bits->width >= 32 ? 0xFFFF'FFFFu : ((1u << bits->width) - 1u)) << bits->lsb;
}

struct lock_scene
{
  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink;
  std::string_view              unit;

  static auto options_of(bool strict) -> core::machine_options
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "CRC", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    return options;
  }

  explicit lock_scene(h5_unit const& which, bool strict = false)
      : f{h5(h573(), options_of(strict))},
        sink{f.target->add_log_sink(
            [this](core::log_record const& record) { logged.push_back(record); })},
        unit{which.name}
  {
    auto const bus   = which.low_power ? "RCC_APB3ENR" : "RCC_APB1LENR";
    auto const field = which.low_power ? std::string{"LPUART1EN"} : std::string{which.name} + "EN";
    write(f, "RCC", bus, read(f, "RCC", bus) | mask_of(f, "RCC", bus, field));
  }

  void debugger(std::string_view reg, std::uint32_t value) const
  {
    write(f, unit, reg, value);
  }

  [[nodiscard]] auto get(std::string_view reg) const -> std::uint32_t
  {
    return read(f, unit, reg);
  }

  [[nodiscard]] auto mask(std::string_view reg, std::string_view field) const -> std::uint32_t
  {
    return mask_of(f, unit, reg, field);
  }

  void cpu(std::string_view reg, std::uint32_t value) const
  {
    auto* const probe = f.model_of<bus_probe>("CRC");
    REQUIRE(probe != nullptr);
    REQUIRE(
        probe->context.services.write(address_of(f, unit, reg), 4, value, popcorn::initiator::cpu)
            .has_value());
  }

  [[nodiscard]] auto refuses(std::string_view reg, std::uint32_t value) const -> bool
  {
    auto* const probe = f.model_of<bus_probe>("CRC");
    REQUIRE(probe != nullptr);
    try
    {
      static_cast<void>(probe->context.services.write(
          address_of(f, unit, reg), 4, value, popcorn::initiator::cpu));
    }
    catch(popcorn::model_error const&)
    {
      return true;
    }
    return false;
  }

  [[nodiscard]] auto lock_records(std::string_view reg) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.reg == reg and record.message.find("written with UE 1") != std::string::npos;
    });
  }
};

struct h5_field
{
  std::string_view reg;
  std::string_view field;
  std::string_view section;
};

constexpr std::array<h5_field, 40> usart_fields{
    {{"CR1_enabled", "FIFOEN", "56.8.1"},
     {"CR1_enabled", "M1", "56.8.1"},
     {"CR1_enabled", "M0", "56.8.1"},
     {"CR1_enabled", "DEAT", "56.8.1"},
     {"CR1_enabled", "DEDT", "56.8.1"},
     {"CR1_enabled", "OVER8", "56.8.1"},
     {"CR1_enabled", "WAKE", "56.8.1"},
     {"CR1_enabled", "PCE", "56.8.1"},
     {"CR1_enabled", "PS", "56.8.1"},
     {"CR2", "ADD", "56.8.3"},
     {"CR2", "MSBFIRST", "56.8.3"},
     {"CR2", "DATAINV", "56.8.3"},
     {"CR2", "TXINV", "56.8.3"},
     {"CR2", "RXINV", "56.8.3"},
     {"CR2", "SWAP", "56.8.3"},
     {"CR2", "LINEN", "56.8.3"},
     {"CR2", "STOP", "56.8.3"},
     {"CR2", "CPOL", "56.8.3"},
     {"CR2", "CPHA", "56.8.3"},
     {"CR2", "LBCL", "56.8.3"},
     {"CR2", "LBDL", "56.8.3"},
     {"CR2", "ADDM7", "56.8.3"},
     {"CR2", "DIS_NSS", "56.8.3"},
     {"CR2", "SLVEN", "56.8.3"},
     {"CR3", "TXFTCFG", "56.8.4"},
     {"CR3", "RXFTCFG", "56.8.4"},
     {"CR3", "SCEN", "56.8.4"},
     {"CR3", "NACK", "56.8.4"},
     {"CR3", "HDSEL", "56.8.4"},
     {"CR3", "IRLP", "56.8.4"},
     {"CR3", "IREN", "56.8.4"},
     {"CR3", "DEP", "56.8.4"},
     {"CR3", "DEM", "56.8.4"},
     {"CR3", "DDRE", "56.8.4"},
     {"CR3", "OVRDIS", "56.8.4"},
     {"CR3", "ONEBIT", "56.8.4"},
     {"CR3", "CTSE", "56.8.4"},
     {"CR3", "RTSE", "56.8.4"},
     {"CR3", "WUS0", "56.8.4"},
     {"CR3", "WUS1", "56.8.4"}}
};
constexpr std::array<h5_field, 25> lpuart_fields{
    {{"CR1_enabled", "M1", "57.7.1"},   {"CR1_enabled", "M0", "57.7.1"},
     {"CR1_enabled", "DEAT", "57.7.1"}, {"CR1_enabled", "DEDT", "57.7.1"},
     {"CR1_enabled", "WAKE", "57.7.1"}, {"CR1_enabled", "PCE", "57.7.1"},
     {"CR1_enabled", "PS", "57.7.1"},   {"CR2", "MSBFIRST", "57.7.3"},
     {"CR2", "DATAINV", "57.7.3"},      {"CR2", "TXINV", "57.7.3"},
     {"CR2", "RXINV", "57.7.3"},        {"CR2", "SWAP", "57.7.3"},
     {"CR2", "STOP", "57.7.3"},         {"CR2", "ADDM7", "57.7.3"},
     {"CR3", "DEP", "57.7.4"},          {"CR3", "DEM", "57.7.4"},
     {"CR3", "DDRE", "57.7.4"},         {"CR3", "OVRDIS", "57.7.4"},
     {"CR3", "TXFTCFG", "57.7.4"},      {"CR3", "RXFTCFG", "57.7.4"},
     {"CR3", "CTSE", "57.7.4"},         {"CR3", "RTSE", "57.7.4"},
     {"CR3", "HDSEL", "57.7.4"},        {"CR3", "WUS0", "57.7.4"},
     {"CR3", "WUS1", "57.7.4"}}
};

void pin_locked(h5_unit const& which, h5_field const& entry)
{
  CAPTURE(which.name);
  CAPTURE(entry.field);
  lock_scene t{which};
  auto const mask = t.mask(entry.reg, entry.field);
  if(entry.reg == "CR1_enabled")
  {
    auto const enable = t.mask("CR1_enabled", "UE");
    t.cpu("CR1_enabled", mask | enable);
    REQUIRE(t.get("CR1_enabled") == (mask | enable));
    t.cpu("CR1_enabled", enable);
    CHECK(t.get("CR1_enabled") == (mask | enable));
    t.cpu("CR1_enabled", 0);
    CHECK(t.get("CR1_enabled") == 0);
    return;
  }
  t.cpu(entry.reg, mask);
  REQUIRE(t.get(entry.reg) == mask);
  t.cpu("CR1_enabled", t.mask("CR1_enabled", "UE"));
  t.cpu(entry.reg, 0);
  CHECK(t.get(entry.reg) == mask);
  t.cpu("CR1_enabled", 0);
  t.cpu(entry.reg, 0);
  CHECK(t.get(entry.reg) == 0);
}
}

TEST_CASE("RM0481 56.8 and 57.7: every UE-locked H5 field holds with UE 1")
{
  auto const pin_each = [](h5_unit const& which, auto const& table) {
    for(auto const& entry : table)
    {
      SUBCASE(
          std::string{which.name}
              .append(" ")
              .append(entry.section)
              .append(" ")
              .append(entry.field)
              .c_str())
      {
        pin_locked(which, entry);
      }
    }
  };
  pin_each(usart2, usart_fields);
  pin_each(lpuart, lpuart_fields);
}

TEST_CASE("RM0481 56.8.6 and 57.7.6: BRR and PRESC hold with UE 1 on USART2 and the LPUART")
{
  for(auto const& which : {usart2, lpuart})
  {
    for(auto const* reg : {"BRR", "PRESC"})
    {
      CAPTURE(which.name);
      CAPTURE(reg);
      lock_scene t{which};
      t.cpu(reg, 0x5);
      t.cpu("CR1_enabled", t.mask("CR1_enabled", "UE"));
      t.cpu(reg, 0x2);
      CHECK(t.get(reg) == 0x5);
    }
  }
}

TEST_CASE("RM0481 56.8.1 and 57.7.1: FIFOEN is held on the USART and free on the LPUART")
{
  lock_scene a{usart2};
  auto const fifo   = a.mask("CR1_enabled", "FIFOEN");
  auto const enable = a.mask("CR1_enabled", "UE");
  a.cpu("CR1_enabled", enable);
  a.cpu("CR1_enabled", enable | fifo);
  CHECK(a.get("CR1_enabled") == enable);
  lock_scene b{lpuart};
  auto const lp_fifo   = b.mask("CR1_enabled", "FIFOEN");
  auto const lp_enable = b.mask("CR1_enabled", "UE");
  b.cpu("CR1_enabled", lp_enable);
  b.cpu("CR1_enabled", lp_enable | lp_fifo);
  CHECK(b.get("CR1_enabled") == (lp_enable | lp_fifo));
}

TEST_CASE("RM0481 56.8.3: ABRMOD is locked only while ABREN is 1 on an H5 USART")
{
  lock_scene t{usart2};
  auto const abren  = t.mask("CR2", "ABREN");
  auto const mode   = t.mask("CR2", "ABRMOD") & (1u << 21);
  auto const enable = t.mask("CR1_enabled", "UE");
  t.cpu("CR1_enabled", enable);
  t.cpu("CR2", mode);
  CHECK(t.get("CR2") == mode);
  t.cpu("CR1_enabled", 0);
  t.cpu("CR2", abren | mode);
  t.cpu("CR1_enabled", enable);
  t.cpu("CR2", abren);
  CHECK(t.get("CR2") == (abren | mode));
}

TEST_CASE("RM0481 56.8.4 and 57.7.4: WUS written with UE 1 keeps its value, one lock record")
{
  for(auto const& which : {usart2, lpuart})
  {
    for(auto const* bit : {"WUS0", "WUS1"})
    {
      CAPTURE(which.name);
      CAPTURE(bit);
      lock_scene t{which};
      auto const wus = t.mask("CR3", bit);
      t.cpu("CR3", wus);
      t.cpu("CR1_enabled", t.mask("CR1_enabled", "UE"));
      t.cpu("CR3", 0);
      t.cpu("CR3", 0);
      CHECK(t.get("CR3") == wus);
      CHECK(t.lock_records("CR3") == 1);
    }
  }
}

TEST_CASE(
    "RM0481 56.8.4 and 57.7.4: a WUS write is a model_error under strict, the field reads old")
{
  for(auto const& which : {usart2, lpuart})
  {
    for(auto const* bit : {"WUS0", "WUS1"})
    {
      CAPTURE(which.name);
      CAPTURE(bit);
      lock_scene t{which, true};
      auto const wus = t.mask("CR3", bit);
      t.debugger("CR3", wus);
      t.debugger("CR1_enabled", t.mask("CR1_enabled", "UE"));
      CHECK(t.refuses("CR3", 0));
      CHECK(t.get("CR3") == wus);
    }
  }
}

TEST_CASE("RM0481 57.7.3: ADD with UE 1 is kept with RE 1 and taken with RE 0 on the H5 LPUART")
{
  lock_scene t{lpuart};
  auto const add    = t.mask("CR2", "ADD");
  auto const enable = t.mask("CR1_enabled", "UE");
  auto const rx     = t.mask("CR1_enabled", "RE");
  auto const low    = add & (0x5Au << 24);
  auto const high   = add & (0x11u << 24);
  t.cpu("CR1_enabled", enable);
  t.cpu("CR2", low);
  CHECK(t.get("CR2") == low);
  CHECK(t.lock_records("CR2") == 0);
  t.cpu("CR1_enabled", enable | rx);
  t.cpu("CR2", high);
  CHECK(t.get("CR2") == low);
  CHECK(t.lock_records("CR2") == 1);
  t.cpu("CR1_enabled", enable);
  t.cpu("CR2", high);
  CHECK(t.get("CR2") == high);
}

TEST_CASE("RM0481 56.8.3: ADD with UE 1 is kept on an H5 USART even with RE 0")
{
  lock_scene t{usart2};
  auto const add    = t.mask("CR2", "ADD");
  auto const enable = t.mask("CR1_enabled", "UE");
  t.cpu("CR2", add & (0x5Au << 24));
  t.cpu("CR1_enabled", enable);
  t.cpu("CR2", add & (0x11u << 24));
  CHECK(t.get("CR2") == (add & (0x5Au << 24)));
  CHECK(t.lock_records("CR2") == 1);
}

TEST_CASE("an H5 ADD write refused by the lock is a model_error under strict, the field reads old")
{
  lock_scene a{lpuart, true};
  auto const add    = a.mask("CR2", "ADD") & (0x5Au << 24);
  auto const enable = a.mask("CR1_enabled", "UE");
  a.debugger("CR2", add);
  a.debugger("CR1_enabled", enable | a.mask("CR1_enabled", "RE"));
  CHECK(a.refuses("CR2", 0));
  CHECK(a.get("CR2") == add);
  lock_scene b{usart2, true};
  b.debugger("CR2", add);
  b.debugger("CR1_enabled", enable);
  CHECK(b.refuses("CR2", 0));
  CHECK(b.get("CR2") == add);
}

TEST_CASE("the H5 lock warning names RM0481")
{
  for(auto const& which : {usart2, uart4, lpuart})
  {
    CAPTURE(which.name);
    lock_scene t{which};
    t.cpu("CR1_enabled", t.mask("CR1_enabled", "UE"));
    t.cpu("BRR", 0x345);
    auto const found = std::ranges::find_if(t.logged, [](core::log_record const& record) {
      return record.message.find("written with UE 1") != std::string::npos;
    });
    REQUIRE(found != t.logged.end());
    CHECK(found->message.find("RM0481") != std::string::npos);
    CHECK(found->message.find("RM0456") == std::string::npos);
  }
}

TEST_CASE("RM0481 56.8.4 and 57.7.4: setting WUS0 or WUS1 logs once and raises under strict")
{
  auto const stop_records = [](lock_scene const& t) {
    return std::ranges::count_if(t.logged, [](core::log_record const& record) {
      return record.message.find("Stop mode") != std::string::npos;
    });
  };
  for(auto const& which : {usart2, lpuart})
  {
    for(auto const* bit : {"WUS0", "WUS1"})
    {
      CAPTURE(which.name);
      CAPTURE(bit);
      lock_scene t{which};
      auto const wus = t.mask("CR3", bit);
      t.cpu("CR3", wus);
      CHECK(t.get("CR3") == wus);
      CHECK(stop_records(t) == 1);
      t.cpu("CR3", 0);
      t.cpu("CR3", wus);
      CHECK(stop_records(t) == 1);
      lock_scene c{which};
      c.debugger("CR3", wus);
      c.cpu("CR3", 0);
      CHECK(c.get("CR3") == 0);
      CHECK(stop_records(c) == 0);
      lock_scene s{which, true};
      CHECK(s.refuses("CR3", wus));
      CHECK(s.get("CR3") == 0);
    }
  }
}

TEST_CASE(
    "RM0481 56.8.4 and 57.7.4: setting WUS0 with UE 1 logs a stop-mode record and a lock record")
{
  auto const count = [](lock_scene const& t, std::string_view text) {
    return std::ranges::count_if(t.logged, [&](core::log_record const& record) {
      return record.message.find(text) != std::string::npos;
    });
  };
  for(auto const& which : {usart2, lpuart})
  {
    CAPTURE(which.name);
    lock_scene t{which};
    auto const wus0 = t.mask("CR3", "WUS0");
    auto const wus1 = t.mask("CR3", "WUS1");
    t.debugger("CR3", wus1);
    t.debugger("CR1_enabled", t.mask("CR1_enabled", "UE"));
    t.cpu("CR3", wus0 | wus1);
    CHECK(t.get("CR3") == wus1);
    CHECK(count(t, "Stop mode") == 1);
    CHECK(count(t, "written with UE 1") == 1);
    lock_scene s{which, true};
    s.debugger("CR3", wus1);
    s.debugger("CR1_enabled", s.mask("CR1_enabled", "UE"));
    CHECK(s.refuses("CR3", wus0 | wus1));
    CHECK(s.get("CR3") == wus1);
    CHECK(count(s, "Stop mode") == 1);
    CHECK(count(s, "written with UE 1") == 0);
  }
}
