#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint32_t aden        = 1u << 0;
constexpr std::uint32_t addis       = 1u << 1;
constexpr std::uint32_t adstart     = 1u << 2;
constexpr std::uint32_t jadstart    = 1u << 3;
constexpr std::uint32_t adstp       = 1u << 4;
constexpr std::uint32_t advregen    = 1u << 28;
constexpr std::uint32_t deeppwd     = 1u << 29;
constexpr std::uint32_t adcal       = 1u << 31;
constexpr std::uint32_t vrefen      = 1u << 22;
constexpr std::uint32_t vsensesel   = 1u << 23;
constexpr std::uint32_t cont        = 1u << 13;
constexpr std::uint32_t vrefint_cal = 6635;
constexpr std::uint32_t ts_cal1     = 4107;
constexpr std::uint64_t ns          = 1'000;
constexpr std::uint64_t us          = 1'000'000;

auto options_with(
    stm32::supply_options           supply,
    popcorn::mcu_description const& description,
    bool                            strict) -> core::machine_options
{
  stm32::board board{};
  board.supply               = supply;
  auto options               = popcorn::testing::stm32_options();
  options.models             = stm32::factories(board, description);
  options.strict_peripherals = strict;
  return options;
}

struct adc_fixture : stm32_fixture
{
  explicit adc_fixture(
      popcorn::mcu_description const& description = u575(),
      stm32::supply_options           supply      = {},
      bool                            clocked     = true,
      bool                            strict      = false)
      : stm32_fixture{description, "rcc_ll", options_with(supply, description, strict)}
  {
    set("pc", sym("rcc_ll", "idle"));
    if(clocked)
    {
      hsi16();
      put("RCC", "RCC_AHB2ENR1", "ADC12EN", 1);
    }
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> popcorn::address
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    return desc.base + found->offset;
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(target->mem().write(at(peripheral, reg), 4, value).has_value());
  }

  void
      put(std::string_view peripheral,
          std::string_view reg,
          std::string_view name,
          std::uint32_t    value) const
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    auto const  bits  = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask    = ((std::uint64_t{1} << bits->width) - 1u) << bits->lsb;
    auto const address = at(peripheral, reg);
    auto const old     = target->mem().read(address, 4);
    REQUIRE(old.has_value());
    write(
        peripheral, reg,
        static_cast<std::uint32_t>((*old & ~mask) | ((std::uint64_t{value} << bits->lsb) & mask)));
  }

  [[nodiscard]] auto read(std::string_view reg) const -> std::uint32_t
  {
    auto const value = target->mem().read(at("ADC1", reg), 4);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto cpu_read(std::string_view reg) const -> std::uint32_t
  {
    auto&      regs  = target->peripheral("ADC1");
    auto const at    = at_offset(reg);
    auto const value = target->model("ADC1")->on_read(
        regs, at, 4, {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto at_offset(std::string_view reg) const -> std::uint32_t
  {
    auto const& desc  = target->peripheral("ADC1").description();
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    return found->offset;
  }

  [[nodiscard]] auto isr(std::string_view name) const -> std::uint32_t
  {
    return field("ADC1", "ADC_ISR", name);
  }

  [[nodiscard]] auto cr(std::string_view name) const -> std::uint32_t
  {
    return field("ADC1", "ADC_CR", name);
  }

  [[nodiscard]] auto port() const -> stm32::analog_port&
  {
    auto* const found = model_of<stm32::analog_port>("ADC1");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  void settle() const
  {
    run_until(now() + 60 * us);
  }

  void hsi16() const
  {
    put("RCC", "RCC_CR", "HSION", 1);
    run_until(now() + 200 * us);
    REQUIRE(field("RCC", "RCC_CR", "HSIRDY") == 1);
    put("RCC", "RCC_CCIPR3", "ADCDACSEL", 4);
  }

  void pll2_at_72_mhz() const
  {
    write("RCC", "RCC_PLL2DIVR", (8u << 0) | (1u << 9) | (1u << 16) | (1u << 24));
    write("RCC", "RCC_PLL2CFGR", (2u << 0) | (3u << 2) | (1u << 18));
    put("RCC", "RCC_CR", "PLL2ON", 1);
    run_until(now() + 200 * us);
    REQUIRE(field("RCC", "RCC_CR", "PLL2RDY") == 1);
    put("RCC", "RCC_CCIPR3", "ADCDACSEL", 2);
  }

  void enable() const
  {
    write("ADC1", "ADC_CR", advregen);
    write("ADC1", "ADC_CR", advregen | aden);
  }

  void start(std::uint32_t channel) const
  {
    write("ADC1", "ADC_SQR1", channel << 6);
    write("ADC1", "ADC_CR", advregen | aden | adstart);
  }

  [[nodiscard]] auto convert(std::uint32_t channel) const -> std::uint32_t
  {
    write("ADC1", "ADC_SQR1", channel << 6);
    write("ADC1", "ADC_CR", advregen | adstart);
    settle();
    return cpu_read("ADC_DR");
  }
};

auto recorder(adc_fixture const& f, std::vector<core::log_record>& logged)
{
  return f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
}

auto clock_warnings(std::vector<core::log_record> const& logged) -> std::vector<core::log_record>
{
  std::vector<core::log_record> out;
  std::ranges::copy_if(logged, std::back_inserter(out), [](core::log_record const& record) {
    return record.message.contains("after PRESC");
  });
  return out;
}

struct old_adc_state
{};
}

TEST_CASE("ADC1 serves ADC_CR and ADC_DR, ADC12 serves ADC12_CCR")
{
  adc_fixture const f;
  REQUIRE(f.target->model("ADC1") != nullptr);
  REQUIRE(f.target->model("ADC12") != nullptr);
  CHECK(f.target->model("ADC4") == nullptr);
  CHECK(f.read("ADC_CR") == deeppwd);
}

TEST_CASE("LDORDY follows ADVREGEN out of deep power-down")
{
  adc_fixture const f;
  f.write("ADC1", "ADC_CR", deeppwd | advregen);
  CHECK(f.isr("LDORDY") == 0);
  f.write("ADC1", "ADC_CR", advregen);
  CHECK(f.isr("LDORDY") == 1);
  f.write("ADC1", "ADC_CR", 0);
  CHECK(f.isr("LDORDY") == 0);
}

TEST_CASE("DEEPPWD clears ADVREGEN and with it LDORDY")
{
  adc_fixture const f;
  f.write("ADC1", "ADC_CR", advregen);
  REQUIRE(f.isr("LDORDY") == 1);
  f.write("ADC1", "ADC_CR", advregen | deeppwd);
  CHECK(f.cr("ADVREGEN") == 0);
  CHECK(f.cr("DEEPPWD") == 1);
  CHECK(f.isr("LDORDY") == 0);
}

TEST_CASE("ADCAL reads 0 right after it is written")
{
  adc_fixture const f;
  f.write("ADC1", "ADC_CR", advregen);
  f.write("ADC1", "ADC_CR", advregen | adcal);
  CHECK(f.cr("ADCAL") == 0);
  CHECK(f.cr("ADVREGEN") == 1);
}

TEST_CASE("ADEN sets ADRDY once the regulator is ready, not before")
{
  adc_fixture const f;
  f.write("ADC1", "ADC_CR", aden);
  CHECK(f.isr("ADRDY") == 0);
  f.write("ADC1", "ADC_CR", addis);
  f.enable();
  CHECK(f.isr("ADRDY") == 1);
  CHECK(f.cr("ADEN") == 1);
}

TEST_CASE("ADRDY sets on the ADEN edge only, a later ADC_CR write leaves it cleared")
{
  adc_fixture const f;
  f.enable();
  REQUIRE(f.isr("ADRDY") == 1);
  f.write("ADC1", "ADC_ISR", 1u);
  REQUIRE(f.isr("ADRDY") == 0);
  f.write("ADC1", "ADC_CR", advregen | aden);
  CHECK(f.isr("ADRDY") == 0);
}

TEST_CASE("a write of 0 to a set-only ADC_CR bit leaves it, ADDIS clears ADEN")
{
  adc_fixture const f;
  f.enable();
  f.write("ADC1", "ADC_CR", advregen);
  CHECK(f.cr("ADEN") == 1);
  f.write("ADC1", "ADC_CR", advregen | addis);
  CHECK(f.cr("ADEN") == 0);
  CHECK(f.cr("ADDIS") == 0);
  CHECK(f.cr("ADVREGEN") == 1);
}

TEST_CASE("VREFINT and the temperature sensor read their factory codes once connected")
{
  adc_fixture const f;
  f.enable();
  CHECK(f.convert(0) == 0);
  CHECK(f.convert(19) == 0);
  f.write("ADC12", "ADC12_CCR", vrefen);
  CHECK(f.convert(0) == vrefint_cal);
  CHECK(f.convert(19) == 0);
  f.write("ADC12", "ADC12_CCR", vrefen | vsensesel);
  CHECK(f.convert(19) == ts_cal1);
  CHECK(f.convert(5) == 0);
  CHECK(f.convert(18) == 0);
}

TEST_CASE("a conversion sets EOC and EOS and clears ADSTART at its end, reading ADC_DR clears EOC")
{
  adc_fixture const f;
  f.write("ADC12", "ADC12_CCR", vrefen);
  f.enable();
  f.write("ADC1", "ADC_CR", advregen | adstart);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.cr("ADSTART") == 1);
  f.settle();
  CHECK(f.isr("EOC") == 1);
  CHECK(f.isr("EOS") == 1);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.read("ADC_DR") == vrefint_cal);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.cpu_read("ADC_DR") == vrefint_cal);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.isr("EOS") == 1);
}

TEST_CASE(
    "an ADSTART write with ADEN 0 or ADDIS 1 is ignored and logs once, a model_error when "
    "strict")
{
  {
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("ADC12", "ADC12_CCR", vrefen);
    f.write("ADC1", "ADC_CR", advregen | adstart);
    CHECK(f.cr("ADSTART") == 0);
    f.write("ADC1", "ADC_CR", advregen | aden | adstart);
    CHECK(f.cr("ADSTART") == 0);
    CHECK(f.cr("ADEN") == 1);
    f.settle();
    CHECK(f.isr("EOC") == 0);
    CHECK(f.read("ADC_DR") == 0);
    f.write("ADC1", "ADC_CR", advregen | aden | addis | adstart);
    CHECK(f.cr("ADSTART") == 0);
    CHECK(f.cr("ADEN") == 0);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].reg == "ADC_CR");
    CHECK(logged[0].message.contains("ADSTART"));
    f.enable();
    f.write("ADC1", "ADC_CR", advregen | aden | adstart);
    CHECK(f.cr("ADSTART") == 1);
    f.settle();
    CHECK(f.isr("EOC") == 1);
    CHECK(f.read("ADC_DR") == vrefint_cal);
    CHECK(logged.size() == 1);
  }
  adc_fixture const f{u575(), {}, true, true};
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(f.at("ADC1", "ADC_CR"), 4, advregen | adstart)),
      popcorn::model_error);
  CHECK(f.cr("ADSTART") == 0);
}

TEST_CASE("ADSTP clears ADSTART and itself")
{
  adc_fixture const f;
  f.enable();
  f.write("ADC1", "ADC_SMPR1", 7u);
  f.write("ADC1", "ADC_CR", advregen | aden | adstart);
  REQUIRE(f.cr("ADSTART") == 1);
  f.write("ADC1", "ADC_CR", advregen | aden | adstp);
  CHECK(f.cr("ADSTP") == 0);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.cr("ADEN") == 1);
  f.write("ADC1", "ADC_CR", advregen | aden | adstp);
  CHECK(f.cr("ADSTP") == 0);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.cr("ADEN") == 1);
}

TEST_CASE("RES drops two bits of the 14-bit code per step")
{
  adc_fixture const f;
  f.write("ADC12", "ADC12_CCR", vrefen);
  f.enable();
  for(std::uint32_t res = 0; res < 4; ++res)
  {
    CAPTURE(res);
    f.write("ADC1", "ADC_CFGR1", res << 2);
    CHECK(f.convert(0) == vrefint_cal >> (2 * res));
  }
}

TEST_CASE("JADSTART logs one warning and clears, continuous mode logs none")
{
  adc_fixture const             f;
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  f.enable();
  f.write("ADC1", "ADC_CFGR1", cont);
  CHECK(f.convert(0) == 0);
  CHECK(f.convert(0) == 0);
  f.write("ADC1", "ADC_CR", advregen | aden | jadstart);
  f.write("ADC1", "ADC_CR", advregen | aden | jadstart);
  CHECK(f.cr("JADSTART") == 0);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].reg == "ADC_CR");
  CHECK(logged[0].message.contains("JADSTART"));
}

TEST_CASE("CONT, DISCEN and SQR1.L log nothing, SQ1 converts first")
{
  struct setting
  {
    std::string_view reg;
    std::uint32_t    value;
  };
  for(auto const [reg, value] : {
          setting{"ADC_CFGR1", cont    },
          setting{"ADC_CFGR1", 1u << 16},
          setting{"ADC_SQR1",  1u      },
  })
  {
    CAPTURE(reg);
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("ADC12", "ADC12_CCR", vrefen);
    f.enable();
    f.write("ADC1", reg, value);
    f.write("ADC1", "ADC_CR", advregen | adstart);
    f.settle();
    CHECK(f.read("ADC_DR") == vrefint_cal);
    CHECK(logged.empty());
  }
}

TEST_CASE("a machine reset puts ADC1 back in deep power-down with the regulator off")
{
  adc_fixture const f;
  f.enable();
  f.target->reset();
  CHECK(f.read("ADC_CR") == deeppwd);
  CHECK(f.isr("LDORDY") == 0);
  CHECK(f.isr("ADRDY") == 0);
}

TEST_CASE("without the factory words a machine builds, converting VREFINT is a config_error")
{
  auto description = u575();
  for(auto& region : description.regions)
  {
    if(region.content)
    {
      std::erase_if(*region.content, [&](popcorn::region_content const& piece) {
        return region.start
             + piece.offset
            == 0x0BFA'07A5
            or region.start
             + piece.offset
            == 0x0BFA'0710;
      });
    }
  }
  adc_fixture const f{description};
  f.enable();
  CHECK(f.convert(0) == 0);
  CHECK(f.convert(5) == 0);
  f.write("ADC12", "ADC12_CCR", vrefen | vsensesel);
  auto const start = [&] {
    static_cast<void>(f.target->mem().write(f.at("ADC1", "ADC_CR"), 4, advregen | adstart));
  };
  f.write("ADC1", "ADC_SQR1", 0u);
  CHECK_THROWS_AS(start(), popcorn::config_error);
  f.write("ADC1", "ADC_SQR1", 19u << 6);
  CHECK_THROWS_AS(start(), popcorn::config_error);
}

TEST_CASE("an external channel reads its input as V x 16384 / VDDA, floored and saturated")
{
  adc_fixture const f{u575(), {.vdda_mv = 3300}};
  CHECK(f.port().vdda_mv() == 3300);
  f.enable();
  CHECK(f.convert(5) == 0);
  f.port().drive(5, 1'650'000);
  CHECK(f.port().input(5) == 1'650'000);
  CHECK(f.convert(5) == 8192);
  f.write("ADC1", "ADC_CFGR1", 1u << 2);
  CHECK(f.convert(5) == 2048);
  f.write("ADC1", "ADC_CFGR1", 0);
  f.port().drive(5, 3'400'000);
  CHECK(f.convert(5) == 16383);
  f.port().drive(5, 0);
  CHECK(f.convert(5) == 0);
  f.port().drive(17, 1'000'000);
  CHECK(f.convert(17) == 4964);
  CHECK(f.convert(1) == 0);
}

TEST_CASE("only external channels 1 to 17 take an input")
{
  adc_fixture const f;
  for(std::uint8_t const channel : {0, 18, 19, 20})
  {
    CAPTURE(channel);
    CHECK_THROWS_AS(f.port().drive(channel, 1), popcorn::config_error);
    CHECK_THROWS_AS(static_cast<void>(f.port().input(channel)), popcorn::config_error);
  }
}

TEST_CASE("VDDA scales the factory codes of VREFINT and the temperature sensor")
{
  adc_fixture const f{u575(), {.vdda_mv = 3300}};
  f.write("ADC12", "ADC12_CCR", vrefen | vsensesel);
  f.enable();
  CHECK(f.convert(0) == 6031);
  CHECK(f.convert(19) == 3733);
}

TEST_CASE("a conversion ends N x (SMP + TSAR) ADC clock cycles after ADSTART")
{
  adc_fixture const f;
  f.port().drive(5, 1'000'000);
  f.enable();
  SUBCASE("814 cycles at 14 bits, 831 cycles at 16 MHz")
  {
    f.write("ADC1", "ADC_SMPR1", 7u << 15);
    auto const start = f.now();
    f.start(5);
    f.run_until(start + 51'937'500 - us);
    CHECK(f.isr("EOC") == 0);
    CHECK(f.read("ADC_DR") == 0);
    f.run_until(start + 51'937'500);
    CHECK(f.isr("EOC") == 1);
    CHECK(f.read("ADC_DR") == 5461);
  }
  SUBCASE("8x oversampling, 36 cycles at 12 bits, 408 cycles")
  {
    f.write("ADC1", "ADC_SMPR1", 4u << 15);
    f.write("ADC1", "ADC_CFGR1", 1u << 2);
    f.write("ADC1", "ADC_CFGR2", (7u << 16) | (3u << 5) | 1u);
    auto const start = f.now();
    f.start(5);
    f.run_until(start + 25'500'000 - us);
    CHECK(f.isr("EOC") == 0);
    f.run_until(start + 25'500'000);
    CHECK(f.isr("EOC") == 1);
    CHECK(f.read("ADC_DR") == 5461 >> 2);
  }
  SUBCASE("PRESC divides the ADC clock by 16")
  {
    f.write("ADC12", "ADC12_CCR", 7u << 18);
    auto const start = f.now();
    f.start(5);
    f.run_until(start + 16 * 1'375'000 - us);
    CHECK(f.isr("EOC") == 0);
    f.run_until(start + 16 * 1'375'000);
    CHECK(f.isr("EOC") == 1);
  }
}

TEST_CASE("the oversampler sums N codes and shifts the sum right, rounding to nearest")
{
  adc_fixture const f;
  f.enable();
  f.port().drive(5, 1'000'000);
  f.write("ADC1", "ADC_CFGR2", (7u << 16) | (3u << 5) | 1u);
  CHECK(f.convert(5) == 5461);
  f.write("ADC1", "ADC_CFGR2", (7u << 16) | 1u);
  CHECK(f.convert(5) == 8 * 5461);
  f.port().drive(5, 550);
  f.write("ADC1", "ADC_CFGR2", 1u);
  CHECK(f.convert(5) == 3);
  f.write("ADC1", "ADC_CFGR2", (1u << 16) | (1u << 5) | 1u);
  CHECK(f.convert(5) == 3);
  f.port().drive(5, 200);
  f.write("ADC1", "ADC_CFGR2", (2u << 16) | (1u << 5) | 1u);
  CHECK(f.convert(5) == 2);
}

TEST_CASE("ADSTP or ADDIS during a conversion drops it")
{
  for(auto const abort : {advregen | aden | adstp, advregen | addis})
  {
    CAPTURE(abort);
    adc_fixture const f;
    f.port().drive(5, 1'000'000);
    f.enable();
    f.write("ADC1", "ADC_SMPR1", 7u << 15);
    auto const start = f.now();
    f.start(5);
    f.run_until(start + 10 * us);
    f.write("ADC1", "ADC_CR", abort);
    CHECK(f.cr("ADSTART") == 0);
    f.run_until(start + 200 * us);
    CHECK(f.isr("EOC") == 0);
    CHECK(f.read("ADC_DR") == 0);
  }
}

TEST_CASE("without a kernel clock a conversion waits for one, logged once")
{
  adc_fixture const             f{u575(), {}, false};
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  f.hsi16();
  f.port().drive(5, 1'000'000);
  f.enable();
  f.start(5);
  f.run_until(f.now() + 200 * us);
  CHECK(f.isr("EOC") == 0);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].peripheral == "ADC1");
  CHECK(logged[0].message.contains("no ADC kernel clock"));
  f.put("RCC", "RCC_AHB2ENR1", "ADC12EN", 1);
  auto const clocked = f.now();
  f.run_until(clocked + 1'375'000 - us);
  CHECK(f.isr("EOC") == 0);
  f.run_until(clocked + 1'375'000);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("ADC_DR") == 5461);
  CHECK(logged.size() == 1);
}

TEST_CASE("a snapshot mid-conversion ends it at the same time with the same code")
{
  adc_fixture const f;
  f.port().drive(5, 1'000'000);
  f.enable();
  f.write("ADC1", "ADC_SMPR1", 7u << 15);
  auto const start = f.now();
  f.start(5);
  f.run_until(start + 20 * us);
  auto const saved = f.target->snapshot();
  f.port().drive(5, 2'000'000);
  f.run_until(start + 51'937'500);
  REQUIRE(f.isr("EOC") == 1);
  CHECK(f.read("ADC_DR") == 10922);

  auto const check = [&](adc_fixture const& g) {
    CHECK(g.port().input(5) == 1'000'000);
    CHECK(g.isr("EOC") == 0);
    g.run_until(start + 51'937'500 - us);
    CHECK(g.isr("EOC") == 0);
    g.run_until(start + 51'937'500);
    CHECK(g.isr("EOC") == 1);
    CHECK(g.read("ADC_DR") == 5461);
  };
  f.target->restore(saved);
  check(f);
  adc_fixture const other;
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(other);
}

TEST_CASE("the driven input is board state: a reset keeps it and drops the conversion")
{
  adc_fixture const f;
  f.port().drive(5, 1'000'000);
  f.enable();
  f.write("ADC1", "ADC_SMPR1", 7u << 15);
  f.start(5);
  f.run_until(f.now() + 10 * us);
  f.target->reset();
  f.set("pc", sym("rcc_ll", "idle"));
  CHECK(f.port().input(5) == 1'000'000);
  f.run_until(f.now() + 200 * us);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.read("ADC_DR") == 0);
}

TEST_CASE("each ADEN edge with the ADC clock outside 5 to 55 MHz logs a warning")
{
  SUBCASE("72 MHz, once per ADEN edge")
  {
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.pll2_at_72_mhz();
    f.enable();
    auto warned = clock_warnings(logged);
    REQUIRE(warned.size() == 1);
    CHECK(warned[0].kind == core::log_kind::model_warning);
    CHECK(warned[0].peripheral == "ADC1");
    CHECK(warned[0].message.contains("72000000 Hz"));
    f.write("ADC1", "ADC_CR", advregen | aden);
    CHECK(clock_warnings(logged).size() == 1);
    f.write("ADC1", "ADC_CR", advregen | addis);
    f.enable();
    CHECK(clock_warnings(logged).size() == 2);
    f.port().drive(5, 1'000'000);
    auto const start = f.now();
    f.start(5);
    f.run_until(start + 22 * 1'000'000 / 72 + 1);
    CHECK(f.isr("EOC") == 1);
  }
  SUBCASE("72 MHz divided by 2, 16 MHz: none")
  {
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.enable();
    f.write("ADC1", "ADC_CR", advregen | addis);
    f.pll2_at_72_mhz();
    f.write("ADC12", "ADC12_CCR", 1u << 18);
    f.enable();
    CHECK(clock_warnings(logged).empty());
  }
  SUBCASE("4 MHz HCLK: below 5 MHz")
  {
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.put("RCC", "RCC_CCIPR3", "ADCDACSEL", 0);
    f.enable();
    auto const warned = clock_warnings(logged);
    REQUIRE(warned.size() == 1);
    CHECK(warned[0].message.contains("4000000 Hz"));
  }
  SUBCASE("no kernel clock: no range warning")
  {
    adc_fixture const             f{u575(), {}, false};
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.enable();
    CHECK(logged.empty());
  }
}

TEST_CASE("LSHIFT, gain compensation and a matching offset each log one warning")
{
  adc_fixture const             f;
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  f.port().drive(5, 1'000'000);
  f.enable();
  f.write("ADC1", "ADC_OFR1", (6u << 27) | 100u);
  CHECK(f.convert(5) == 5461);
  CHECK(logged.empty());
  f.write("ADC1", "ADC_OFR1", (5u << 27) | 100u);
  f.write("ADC1", "ADC_CFGR2", 1u << 28);
  f.write("ADC1", "ADC_GCOMP", 1u << 31);
  CHECK(f.convert(5) == 5461);
  CHECK(f.convert(5) == 5461);
  REQUIRE(logged.size() == 3);
  CHECK(logged[0].reg == "ADC_CFGR2");
  CHECK(logged[1].reg == "ADC_GCOMP");
  CHECK(logged[2].reg == "ADC_OFR1");
}

TEST_CASE("an ADC state saved before the inputs loads with inputs at 0 and converts again")
{
  adc_fixture const f;
  f.port().drive(5, 1'000'000);
  f.enable();
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(old_adc_state{});
  f.target->model("ADC1")->load(bytes);
  f.target->model("ADC12")->load(bytes);
  CHECK(f.port().input(5) == 0);
  f.settle();
  CHECK(f.isr("EOC") == 0);
  CHECK(f.convert(5) == 0);
  f.port().drive(5, 1'000'000);
  CHECK(f.convert(5) == 5461);
}

namespace
{
constexpr std::uint64_t slow_cycle_ps = 16 * us;
constexpr std::uint64_t slow_sample   = 22 * slow_cycle_ps;
constexpr std::uint64_t slow_window   = 5 * slow_cycle_ps;
constexpr std::uint32_t one_volt      = 5461;
constexpr std::uint32_t two_volts     = 10922;

auto slow_oversampling(adc_fixture const& f) -> std::uint64_t
{
  f.port().drive(5, 1'000'000);
  f.enable();
  f.write("ADC12", "ADC12_CCR", 11u << 18);
  f.write("ADC1", "ADC_CFGR2", (7u << 16) | 1u);
  auto const start = f.now();
  f.start(5);
  return start;
}
}

TEST_CASE("each sample holds the input at the end of its own sampling window")
{
  SUBCASE("a drive between held samples 3 and 4 mixes 4 old and 4 new codes")
  {
    adc_fixture const f;
    auto const        start = slow_oversampling(f);
    f.run_until(start + 3 * slow_sample + slow_window + 50 * us);
    f.port().drive(5, 2'000'000);
    f.run_until(start + 8 * slow_sample - us);
    CHECK(f.isr("EOC") == 0);
    f.run_until(start + 8 * slow_sample);
    REQUIRE(f.isr("EOC") == 1);
    CHECK(f.read("ADC_DR") == 4 * one_volt + 4 * two_volts);
  }
  SUBCASE("a drive inside the sampling window of sample 3 counts for it")
  {
    adc_fixture const f;
    auto const        start = slow_oversampling(f);
    f.run_until(start + 3 * slow_sample + slow_window / 2);
    f.port().drive(5, 2'000'000);
    f.run_until(start + 8 * slow_sample);
    REQUIRE(f.isr("EOC") == 1);
    CHECK(f.read("ADC_DR") == 3 * one_volt + 5 * two_volts);
  }
}

TEST_CASE("a snapshot keeps the samples held before it")
{
  adc_fixture const f;
  auto const        start = slow_oversampling(f);
  f.run_until(start + 3 * slow_sample + slow_window + 50 * us);
  f.port().drive(5, 2'000'000);
  auto const saved = f.target->snapshot();
  f.port().drive(5, 0);
  f.run_until(start + 8 * slow_sample);
  REQUIRE(f.isr("EOC") == 1);
  CHECK(f.read("ADC_DR") == 4 * one_volt);

  auto const check = [&](adc_fixture const& g) {
    CHECK(g.port().input(5) == 2'000'000);
    g.run_until(start + 8 * slow_sample);
    REQUIRE(g.isr("EOC") == 1);
    CHECK(g.read("ADC_DR") == 4 * one_volt + 4 * two_volts);
  };
  f.target->restore(saved);
  check(f);
  adc_fixture const other;
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(other);
}

TEST_CASE("a reserved PRESC divides by 256 and logs once, a model_error when strict")
{
  {
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.enable();
    f.write("ADC12", "ADC12_CCR", 12u << 18);
    auto const start = f.now();
    f.start(5);
    f.run_until(start + 256 * 1'375'000 - us);
    CHECK(f.isr("EOC") == 0);
    f.run_until(start + 256 * 1'375'000);
    CHECK(f.isr("EOC") == 1);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].peripheral == "ADC12");
    CHECK(logged[0].reg == "ADC12_CCR");
    CHECK(logged[0].message.contains("reserved"));
    f.write("ADC12", "ADC12_CCR", 15u << 18);
    f.start(5);
    f.run_until(f.now() + 256 * 1'375'000);
    CHECK(logged.size() == 1);
  }
  adc_fixture const f{u575(), {}, true, true};
  f.enable();
  f.write("ADC12", "ADC12_CCR", 12u << 18);
  f.write("ADC1", "ADC_SQR1", 5u << 6);
  CHECK_THROWS_AS(
      static_cast<void>(
          f.target->mem().write(f.at("ADC1", "ADC_CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
}

namespace
{
constexpr std::uint32_t    res12          = 1u << 2;
constexpr std::uint32_t    ovrmod         = 1u << 12;
constexpr std::uint32_t    discen         = 1u << 16;
constexpr std::uint32_t    dmngt_oneshot  = 1u;
constexpr std::uint32_t    dmngt_mdf      = 2u;
constexpr std::uint32_t    dmngt_circular = 3u;
constexpr std::uint32_t    eosmp          = 1u << 1;
constexpr std::uint32_t    ovr            = 1u << 4;
constexpr std::uint32_t    eosmpie        = 1u << 1;
constexpr std::uint32_t    eosie          = 1u << 3;
constexpr std::uint32_t    ovrie          = 1u << 4;
constexpr std::uint64_t    adc_cycle_ps   = 62'500;
constexpr std::uint32_t    adc1_irq       = 37;
constexpr std::uint32_t    dma_enable     = 1u << 0;
constexpr std::uint32_t    dma_suspend    = 1u << 2;
constexpr std::uint32_t    dma_word_copy  = 0x400A'0002;
constexpr std::uint32_t    self_link      = 0x3801'0000;
constexpr popcorn::address buffer         = 0x2001'1000;
constexpr popcorn::address node_0         = 0x2001'2000;
constexpr popcorn::address node_1         = 0x2001'2020;

constexpr std::array<std::uint32_t, 10> scan_channels{2, 3, 4, 5, 6, 14, 15, 16, 19, 0};
constexpr std::array<std::uint64_t, 10> scan_eoc_cycles{408,  816,  1224, 1888,  2296,
                                                        2704, 3112, 3520, 10152, 16784};

[[nodiscard]] constexpr auto scan_input_uv(std::uint32_t channel, std::uint32_t round = 0)
    -> std::uint32_t
{
  return channel * 100'000 + round * 10'000;
}

[[nodiscard]] constexpr auto scan_code(std::size_t rank, std::uint32_t round = 0) -> std::uint32_t
{
  auto const channel = scan_channels[rank];
  if(channel == 19)
  {
    return ts_cal1 >> 2;
  }
  if(channel == 0)
  {
    return vrefint_cal >> 2;
  }
  return static_cast<std::uint32_t>(
             std::uint64_t{scan_input_uv(channel, round)} * 16384 / 3'000'000)
      >> 2;
}

[[nodiscard]] constexpr auto scan_eoc(std::size_t rank) -> std::uint64_t
{
  return scan_eoc_cycles[rank] * adc_cycle_ps;
}

constexpr std::uint64_t scan_ps = 16784 * adc_cycle_ps;

void drive_scan_inputs(adc_fixture const& f, std::uint32_t round = 0)
{
  for(auto const channel : scan_channels)
  {
    if(channel >= 1 and channel <= 17)
    {
      f.port().drive(static_cast<std::uint8_t>(channel), scan_input_uv(channel, round));
    }
  }
}

void downstream_scan(adc_fixture const& f, std::uint32_t cfgr1)
{
  drive_scan_inputs(f);
  f.write("ADC12", "ADC12_CCR", vrefen | vsensesel);
  f.enable();
  f.write(
      "ADC1", "ADC_SMPR1",
      (7u << 0) | (4u << 6) | (4u << 9) | (4u << 12) | (5u << 15) | (4u << 18));
  f.write("ADC1", "ADC_SMPR2", (4u << 12) | (4u << 15) | (4u << 18) | (7u << 27));
  f.write("ADC1", "ADC_SQR1", 9u | (2u << 6) | (3u << 12) | (4u << 18) | (5u << 24));
  f.write("ADC1", "ADC_SQR2", 6u | (14u << 6) | (15u << 12) | (16u << 18) | (19u << 24));
  f.write("ADC1", "ADC_SQR3", 0u);
  f.write("ADC1", "ADC_CFGR1", res12 | cfgr1);
  f.write("ADC1", "ADC_CFGR2", (7u << 16) | (3u << 5) | 1u);
}

auto start_scan(adc_fixture const& f) -> std::uint64_t
{
  auto const start = f.now();
  f.write("ADC1", "ADC_CR", advregen | aden | adstart);
  return start;
}

auto pending(adc_fixture const& f) -> bool
{
  auto const word = f.target->mem().read(0xe000'e200u + 4 * (adc1_irq / 32), 4);
  REQUIRE(word.has_value());
  return (*word >> (adc1_irq % 32) & 1u) != 0;
}

void unpend(adc_fixture const& f)
{
  REQUIRE(f.target->mem().write(0xe000'e280u + 4 * (adc1_irq / 32), 4, 1u << (adc1_irq % 32)));
}

auto buffer_words(adc_fixture const& f, std::size_t count) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::size_t index = 0; index < count; ++index)
  {
    auto const word = f.target->mem().read(buffer + 4 * index, 4);
    REQUIRE(word.has_value());
    out.push_back(static_cast<std::uint32_t>(*word));
  }
  return out;
}

void downstream_dma(adc_fixture const& f)
{
  auto const source = static_cast<std::uint32_t>(f.at("ADC1", "ADC_DR"));
  auto const place  = [&](popcorn::address at, std::initializer_list<std::uint32_t> words) {
    for(auto const [index, word] : std::views::enumerate(words))
    {
      REQUIRE(f.target->mem().write(at + 4 * static_cast<std::uint32_t>(index), 4, word));
    }
  };
  place(
      node_0, {dma_word_copy, 0x8000'0000, 40, source, static_cast<std::uint32_t>(buffer),
               self_link | (node_1 & 0xfffcu)});
  place(node_1, {40, source, static_cast<std::uint32_t>(buffer), self_link | (node_1 & 0xfffcu)});
  f.write("GPDMA1", "GPDMA_C1TR1", dma_word_copy);
  f.write("GPDMA1", "GPDMA_C1TR2", 0x4000'0000);
  f.write("GPDMA1", "GPDMA_C1BR1", 0);
  f.write("GPDMA1", "GPDMA_C1LBAR", node_0 & 0xffff'0000u);
  f.write("GPDMA1", "GPDMA_C1LLR", 0xF801'0000 | (node_0 & 0xfffcu));
  f.write("GPDMA1", "GPDMA_C1CR", dma_enable);
}

void single_dma(adc_fixture const& f, std::uint32_t bytes, std::uint32_t control = dma_enable)
{
  f.write("GPDMA1", "GPDMA_C1TR1", dma_word_copy);
  f.write("GPDMA1", "GPDMA_C1TR2", 0);
  f.write("GPDMA1", "GPDMA_C1BR1", bytes);
  f.write("GPDMA1", "GPDMA_C1SAR", static_cast<std::uint32_t>(f.at("ADC1", "ADC_DR")));
  f.write("GPDMA1", "GPDMA_C1DAR", static_cast<std::uint32_t>(buffer));
  f.write("GPDMA1", "GPDMA_C1LLR", 0);
  f.write("GPDMA1", "GPDMA_C1CR", control);
}

auto dma_flag(adc_fixture const& f, std::string_view name) -> std::uint32_t
{
  return f.field("GPDMA1", "GPDMA_C1SR", name);
}

void clear_dma_flags(adc_fixture const& f)
{
  f.write("GPDMA1", "GPDMA_C1FCR", (1u << 8) | (1u << 9));
}

auto scan_codes(std::size_t first, std::size_t count, std::uint32_t round = 0)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::size_t rank = first; rank < first + count; ++rank)
  {
    out.push_back(scan_code(rank % scan_channels.size(), round));
  }
  return out;
}

struct step03_adc_state
{
  std::array<std::uint32_t, 20> input_uv{};
  std::uint32_t                 generation{};
  std::uint64_t                 start_ps{};
  std::uint64_t                 clock_hz{};
  std::uint32_t                 divider{};
  std::uint32_t                 held{};
  std::uint32_t                 sum{};
  bool                          converting{};
  bool                          waiting_clock{};
};

struct step04_adc_state
{
  std::array<std::uint32_t, 20> input_uv{};
  std::uint32_t                 generation{};
  std::uint64_t                 start_ps{};
  std::uint64_t                 clock_hz{};
  std::uint32_t                 divider{};
  std::uint32_t                 held{};
  std::uint32_t                 sum{};
  bool                          converting{};
  bool                          waiting_clock{};
  std::uint8_t                  rank{};
  std::uint8_t                  subgroup_left{};
  std::vector<std::uint32_t>    data;
  bool                          dma_stopped{};
};
}

TEST_CASE("the ten-rank scan ends each rank N x (SMP + TSAR) after the last, EOS with the tenth")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod);
  auto const start = start_scan(f);
  for(std::size_t rank = 0; rank < scan_channels.size(); ++rank)
  {
    CAPTURE(rank);
    f.run_until(start + scan_eoc(rank) - us);
    CHECK(f.isr("EOC") == 0);
    CHECK(f.isr("EOS") == 0);
    CHECK(f.cr("ADSTART") == 1);
    f.run_until(start + scan_eoc(rank));
    CHECK(f.isr("EOC") == 1);
    CHECK(f.isr("EOS") == (rank == 9 ? 1u : 0u));
    CHECK(f.cpu_read("ADC_DR") == scan_code(rank));
  }
  CHECK(f.cr("ADSTART") == 0);
  CHECK(scan_ps == 1'049 * us);
  f.run_until(start + 3 * scan_ps);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.isr("OVR") == 0);
}

TEST_CASE("CONT repeats the scan back to back, ADSTP stops it, the next ADSTART begins at rank 0")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | cont);
  auto const start = start_scan(f);
  f.run_until(start + scan_ps + scan_eoc(0));
  CHECK(f.isr("EOS") == 1);
  CHECK(f.cr("ADSTART") == 1);
  CHECK(f.read("ADC_DR") == scan_code(0));
  f.run_until(start + scan_ps + scan_eoc(3));
  CHECK(f.read("ADC_DR") == scan_code(3));
  f.run_until(start + scan_ps + scan_eoc(3) + 100 * adc_cycle_ps);
  f.write("ADC1", "ADC_CR", advregen | aden | adstp);
  CHECK(f.cr("ADSTART") == 0);
  static_cast<void>(f.cpu_read("ADC_DR"));
  f.write("ADC1", "ADC_ISR", ovr);
  f.run_until(f.now() + 2 * scan_ps);
  CHECK(f.isr("EOC") == 0);
  auto const again = start_scan(f);
  f.run_until(again + scan_eoc(0) - us);
  CHECK(f.isr("EOC") == 0);
  f.run_until(again + scan_eoc(0));
  CHECK(f.isr("EOC") == 1);
  CHECK(f.cpu_read("ADC_DR") == scan_code(0));
}

TEST_CASE(
    "DISCEN converts DISCNUM + 1 ranks per ADSTART, ADSTART clears at the subgroup's last EOC")
{
  adc_fixture const f;
  downstream_scan(f, discen | (2u << 17));
  std::vector<std::vector<std::uint32_t>> converted;
  for(std::size_t trigger = 0; trigger < 5; ++trigger)
  {
    CAPTURE(trigger);
    auto const start = start_scan(f);
    if(trigger == 0)
    {
      f.run_until(start + scan_eoc(0));
      CHECK(f.isr("EOC") == 1);
      CHECK(f.cr("ADSTART") == 1);
      f.run_until(start + scan_eoc(1));
      CHECK(f.cr("ADSTART") == 1);
      f.run_until(start + scan_eoc(2) - us);
      CHECK(f.cr("ADSTART") == 1);
      f.run_until(start + scan_eoc(2));
      CHECK(f.cr("ADSTART") == 0);
    }
    f.run_until(start + scan_ps);
    CHECK(f.cr("ADSTART") == 0);
    CHECK(f.isr("EOS") == (trigger == 3 ? 1u : 0u));
    f.write("ADC1", "ADC_ISR", 1u << 3);
    std::vector<std::uint32_t> results;
    while(f.isr("EOC") != 0)
    {
      results.push_back(f.cpu_read("ADC_DR"));
    }
    converted.push_back(results);
  }
  CHECK(
      converted
      == std::vector<std::vector<std::uint32_t>>{
          scan_codes(0, 3), scan_codes(3, 3), scan_codes(6, 3), scan_codes(9, 1),
          scan_codes(0, 3)});
}

TEST_CASE("OVRMOD 1 sets OVR at an EOC over an unread one, ADC_DR holds the latest")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod);
  auto const start = start_scan(f);
  f.run_until(start + scan_eoc(0));
  CHECK(f.isr("OVR") == 0);
  CHECK(f.read("ADC_DR") == scan_code(0));
  f.run_until(start + scan_eoc(1));
  CHECK(f.isr("OVR") == 1);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("ADC_DR") == scan_code(1));
  f.run_until(start + scan_eoc(2));
  CHECK(f.read("ADC_DR") == scan_code(2));
  CHECK(f.cpu_read("ADC_DR") == scan_code(2));
  CHECK(f.isr("EOC") == 0);
}

TEST_CASE("OVRMOD 0 keeps eight results, discards while OVR stays set, lands again once cleared")
{
  adc_fixture const f;
  downstream_scan(f, cont);
  auto const start = start_scan(f);
  f.run_until(start + scan_eoc(7));
  CHECK(f.isr("OVR") == 0);
  f.run_until(start + scan_eoc(8));
  CHECK(f.isr("OVR") == 1);
  CHECK(f.read("ADC_DR") == scan_code(0));
  CHECK(f.cpu_read("ADC_DR") == scan_code(0));
  CHECK(f.read("ADC_DR") == scan_code(1));
  f.run_until(start + scan_ps + scan_eoc(1));
  CHECK(f.isr("OVR") == 1);
  std::vector<std::uint32_t> drained;
  while(f.isr("EOC") != 0)
  {
    drained.push_back(f.cpu_read("ADC_DR"));
  }
  CHECK(drained == scan_codes(1, 7));
  f.write("ADC1", "ADC_ISR", ovr);
  f.run_until(start + scan_ps + scan_eoc(2));
  CHECK(f.isr("EOC") == 1);
  CHECK(f.isr("OVR") == 0);
  CHECK(f.cpu_read("ADC_DR") == scan_code(2));
}

TEST_CASE("EOSMP sets at the end of each sampling phase, again after a clear")
{
  adc_fixture const f;
  f.port().drive(5, 1'000'000);
  f.enable();
  f.write("ADC1", "ADC_SMPR1", 7u << 15);
  f.write("ADC1", "ADC_CFGR2", (1u << 16) | (1u << 5) | 1u);
  f.write("ADC1", "ADC_IER", eosmpie);
  auto const start = f.now();
  f.start(5);
  f.run_until(start + 814 * adc_cycle_ps - us);
  CHECK(f.isr("EOSMP") == 0);
  CHECK_FALSE(pending(f));
  f.run_until(start + 814 * adc_cycle_ps);
  CHECK(f.isr("EOSMP") == 1);
  CHECK(pending(f));
  f.run_until(start + 831 * adc_cycle_ps);
  f.write("ADC1", "ADC_ISR", eosmp);
  unpend(f);
  CHECK_FALSE(pending(f));
  f.run_until(start + (831 + 814) * adc_cycle_ps - us);
  CHECK(f.isr("EOSMP") == 0);
  f.run_until(start + (831 + 814) * adc_cycle_ps);
  CHECK(f.isr("EOSMP") == 1);
  CHECK(pending(f));
  f.run_until(start + 2 * 831 * adc_cycle_ps);
  CHECK(f.read("ADC_DR") == 5461);
}

TEST_CASE("ADC1_IRQn follows each enabled flag: OVRIE at the overrun, EOSIE at the end of scan")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod);
  f.write("ADC1", "ADC_IER", ovrie);
  auto const start = start_scan(f);
  f.run_until(start + scan_eoc(0));
  CHECK_FALSE(pending(f));
  f.run_until(start + scan_eoc(1));
  CHECK(pending(f));
  f.write("ADC1", "ADC_ISR", ovr);
  unpend(f);
  f.run_until(f.now() + us);
  CHECK_FALSE(pending(f));
  f.write("ADC1", "ADC_IER", eosie);
  f.run_until(start + scan_eoc(9) - us);
  CHECK(f.isr("OVR") == 1);
  CHECK_FALSE(pending(f));
  f.run_until(start + scan_eoc(9));
  CHECK(pending(f));
  f.write("ADC1", "ADC_ISR", 1u << 3);
  unpend(f);
  f.run_until(f.now() + us);
  CHECK_FALSE(pending(f));
}

TEST_CASE("DMNGT 10 logs one warning, requests nothing, ADC_DR keeps the result")
{
  adc_fixture const             f;
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  downstream_scan(f, ovrmod | dmngt_mdf);
  single_dma(f, 8);
  auto const start = start_scan(f);
  f.run_until(start + scan_ps + 10 * us);
  CHECK(f.read("ADC_DR") == scan_code(9));
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 8);
  start_scan(f);
  f.run_until(f.now() + scan_ps + 10 * us);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].reg == "ADC_CFGR1");
  CHECK(logged[0].message.contains("DMNGT"));
}

TEST_CASE("DMNGT 11 feeds the downstream circular list: TC per scan, HT after five words")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | dmngt_circular);
  downstream_dma(f);
  for(std::uint32_t round = 0; round < 3; ++round)
  {
    CAPTURE(round);
    drive_scan_inputs(f, round);
    auto const start = start_scan(f);
    f.run_until(start + scan_eoc(4) - us);
    CHECK(dma_flag(f, "HTF") == 0);
    f.run_until(start + scan_eoc(4) + 2 * us);
    CHECK(dma_flag(f, "HTF") == 1);
    CHECK(dma_flag(f, "TCF") == 0);
    f.run_until(start + scan_eoc(9) + 2 * us);
    CHECK(dma_flag(f, "TCF") == 1);
    CHECK(buffer_words(f, 10) == scan_codes(0, 10, round));
    CHECK(f.isr("OVR") == 0);
    CHECK(f.isr("EOC") == 0);
    clear_dma_flags(f);
  }
  CHECK(f.field("GPDMA1", "GPDMA_C1CR", "EN") == 1);
}

TEST_CASE("an overrun blocks the DMA requests until OVR is cleared, then the latest result moves")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | dmngt_circular);
  single_dma(f, 40, dma_enable | dma_suspend);
  auto const start = start_scan(f);
  f.run_until(start + scan_eoc(1));
  CHECK(f.isr("OVR") == 1);
  f.write("GPDMA1", "GPDMA_C1CR", dma_enable);
  f.run_until(f.now() + 5 * us);
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 40);
  f.write("ADC1", "ADC_ISR", ovr);
  f.run_until(f.now() + 5 * us);
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 36);
  CHECK(buffer_words(f, 2) == std::vector<std::uint32_t>{scan_code(1), 0});
  f.run_until(start + scan_eoc(2) + 2 * us);
  CHECK(buffer_words(f, 2) == scan_codes(1, 2));
}

TEST_CASE("DMNGT 01 stops at the channel's last transfer until an ADSTART 0 to 1 write")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | dmngt_oneshot);
  single_dma(f, 20);
  auto const start = start_scan(f);
  f.run_until(start + scan_eoc(4) + 2 * us);
  CHECK(dma_flag(f, "TCF") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C1CR", "EN") == 0);
  CHECK(
      buffer_words(f, 6)
      == std::vector<std::uint32_t>{
          scan_code(0), scan_code(1), scan_code(2), scan_code(3), scan_code(4), 0});
  f.run_until(start + 2 * scan_ps);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.isr("EOS") == 0);
  CHECK(f.cr("ADSTART") == 1);
  CHECK(f.read("ADC_DR") == scan_code(4));

  clear_dma_flags(f);
  f.write("GPDMA1", "GPDMA_C1BR1", 20);
  f.write("GPDMA1", "GPDMA_C1CR", dma_enable);
  f.write("ADC1", "ADC_CR", advregen | aden);
  f.run_until(f.now() + scan_ps);
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 20);
  f.write("ADC1", "ADC_CR", advregen | aden | adstp);
  f.run_until(f.now() + scan_ps);
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 20);
  auto const again = start_scan(f);
  f.run_until(again + scan_eoc(4) + 2 * us);
  CHECK(dma_flag(f, "TCF") == 1);
  auto const words = buffer_words(f, 11);
  CHECK(
      std::vector<std::uint32_t>(words.begin() + 5, words.end())
      == std::vector<std::uint32_t>{
          scan_code(0), scan_code(1), scan_code(2), scan_code(3), scan_code(4), 0});
}

TEST_CASE("a snapshot mid-scan with two results queued for the DMA resumes to the same buffer")
{
  adc_fixture const f;
  downstream_scan(f, dmngt_circular);
  downstream_dma(f);
  auto const start = start_scan(f);
  f.run_until(start + scan_eoc(3) + 2 * us);
  f.write("GPDMA1", "GPDMA_C1CR", dma_enable | dma_suspend);
  f.run_until(start + scan_eoc(5) + 100 * adc_cycle_ps);
  CHECK(
      buffer_words(f, 6)
      == std::vector<std::uint32_t>{scan_code(0), scan_code(1), scan_code(2), scan_code(3), 0, 0});
  CHECK(f.isr("EOC") == 1);
  auto const saved = f.target->snapshot();

  auto const finish = [&](adc_fixture const& g) {
    g.write("GPDMA1", "GPDMA_C1CR", dma_enable);
    g.run_until(start + scan_eoc(9) + 2 * us);
    CHECK(buffer_words(g, 10) == scan_codes(0, 10));
    CHECK(g.isr("EOS") == 1);
    CHECK(g.isr("OVR") == 0);
    CHECK(g.field("GPDMA1", "GPDMA_C1SR", "TCF") == 1);
  };
  finish(f);
  f.target->restore(saved);
  finish(f);
  adc_fixture const other;
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  finish(other);
}

TEST_CASE("an ADC state saved before the sequence loads mid-conversion as rank 0, the scan goes on")
{
  adc_fixture const f;
  f.port().drive(5, 1'000'000);
  f.port().drive(6, 2'000'000);
  f.enable();
  f.write("ADC1", "ADC_SQR1", 1u | (5u << 6) | (6u << 12));
  auto const start = f.now();
  f.write("ADC1", "ADC_CR", advregen | aden | adstart);
  f.run_until(start + 22 * adc_cycle_ps);
  REQUIRE(f.cpu_read("ADC_DR") == 5461);
  f.run_until(start + 33 * adc_cycle_ps);
  std::vector<std::byte> current;
  f.target->model("ADC1")->save(current);
  auto const now = reflex::serde::bson::deserializer{
      std::span<std::byte const>{
          current}}.load<step04_adc_state>();
  REQUIRE(now.converting);
  REQUIRE(now.rank == 1);
  step03_adc_state const old{
      .input_uv      = now.input_uv,
      .generation    = now.generation,
      .start_ps      = now.start_ps,
      .clock_hz      = now.clock_hz,
      .divider       = now.divider,
      .held          = now.held,
      .sum           = now.sum,
      .converting    = now.converting,
      .waiting_clock = now.waiting_clock};
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(old);
  f.target->model("ADC1")->load(bytes);
  f.run_until(start + 44 * adc_cycle_ps);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.isr("EOS") == 0);
  CHECK(f.cpu_read("ADC_DR") == 5461);
  f.run_until(start + 66 * adc_cycle_ps);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.isr("EOS") == 1);
  CHECK(f.cpu_read("ADC_DR") == 10922);
  CHECK(f.cr("ADSTART") == 0);
}

TEST_CASE("DISCEN with CONT converts as DISCEN alone and logs once, a model_error when strict")
{
  {
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    downstream_scan(f, discen | cont | (1u << 17));
    for(std::size_t trigger = 0; trigger < 2; ++trigger)
    {
      CAPTURE(trigger);
      auto const start = start_scan(f);
      f.run_until(start + scan_ps);
      CHECK(f.cr("ADSTART") == 0);
      std::vector<std::uint32_t> results;
      while(f.isr("EOC") != 0)
      {
        results.push_back(f.cpu_read("ADC_DR"));
      }
      CHECK(results == scan_codes(2 * trigger, 2));
    }
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].reg == "ADC_CFGR1");
    CHECK(logged[0].message.contains("DISCEN"));
  }
  adc_fixture const f{u575(), {}, true, true};
  downstream_scan(f, discen | cont);
  CHECK_THROWS_AS(
      static_cast<void>(
          f.target->mem().write(f.at("ADC1", "ADC_CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
  CHECK(f.cr("ADSTART") == 0);
}

namespace
{
constexpr std::uint64_t ms            = 1'000'000'000;
constexpr std::uint32_t exten_rising  = 1u << 10;
constexpr std::uint32_t exten_falling = 2u << 10;
constexpr std::uint32_t exten_both    = 3u << 10;
constexpr std::uint64_t tim15_period  = 500 * ms;
constexpr std::uint64_t tim15_clock   = 250'000;
constexpr std::uint64_t latency       = 3 * adc_cycle_ps;

[[nodiscard]] constexpr auto extsel(std::uint32_t selection) -> std::uint32_t
{
  return selection << 5;
}

auto tim15_trgo(
    adc_fixture const& f,
    std::uint32_t      prescaler = 124,
    std::uint32_t      reload    = 15'999,
    std::uint32_t      mode      = 2,
    bool               one_pulse = false) -> std::uint64_t
{
  f.put("RCC", "RCC_APB2ENR", "TIM15EN", 1);
  f.write("TIM15", "PSC", prescaler);
  f.write("TIM15", "ARR", reload);
  f.write("TIM15", "EGR", 1);
  f.write("TIM15", "SR", 0);
  f.put("TIM15", "CR2", "MMS", mode);
  f.put("TIM15", "CR1", "OPM", one_pulse ? 1u : 0u);
  auto const start = f.now();
  f.put("TIM15", "CR1", "CEN", 1);
  return start;
}

void read_scan(adc_fixture const& f, std::uint64_t edge)
{
  auto const update = edge + latency;
  f.run_until(update + scan_eoc(0) - us);
  CHECK(f.isr("EOC") == 0);
  for(std::size_t rank = 0; rank < scan_channels.size(); ++rank)
  {
    CAPTURE(rank);
    f.run_until(update + scan_eoc(rank));
    CHECK(f.isr("EOC") == 1);
    CHECK(f.isr("EOS") == (rank == 9 ? 1u : 0u));
    CHECK(f.cpu_read("ADC_DR") == scan_code(rank));
  }
  CHECK(f.cr("ADSTART") == 1);
  f.write("ADC1", "ADC_ISR", (1u << 3) | eosmp);
}
}

TEST_CASE("EXTSEL 14 starts the scan at each TIM15 update, ADSTART stays set")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | exten_rising | extsel(14));
  auto const start = tim15_trgo(f);
  static_cast<void>(start_scan(f));
  f.run_until(start + 10 * us);
  CHECK(f.cr("ADSTART") == 1);
  CHECK(f.isr("EOSMP") == 0);
  f.run_until(start + tim15_period - us);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.isr("EOSMP") == 0);
  for(std::uint64_t round = 1; round <= 5; ++round)
  {
    CAPTURE(round);
    read_scan(f, start + round * tim15_period);
  }
  f.run_until(start + 2'600 * ms);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.isr("EOS") == 0);
  CHECK(f.cr("ADSTART") == 1);
}

TEST_CASE("a TIM15 update during a scan is ignored, the next one after it starts a scan")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | exten_rising | extsel(14));
  auto const start = tim15_trgo(f, 0, 399);
  static_cast<void>(start_scan(f));
  auto const first = start + 100 * us + latency;
  f.run_until(first + scan_ps);
  CHECK(f.isr("EOS") == 1);
  f.write("ADC1", "ADC_ISR", (1u << 3) | (1u << 2) | ovr);
  CHECK(f.cpu_read("ADC_DR") == scan_code(9));
  auto const second = start + 1'200 * us + latency;
  f.run_until(second + scan_eoc(0) - us);
  CHECK(f.isr("EOC") == 0);
  f.run_until(second + scan_eoc(0));
  CHECK(f.isr("EOC") == 1);
  CHECK(f.cpu_read("ADC_DR") == scan_code(0));
  f.run_until(second + scan_ps - us);
  CHECK(f.isr("EOS") == 0);
  f.run_until(second + scan_ps);
  CHECK(f.isr("EOS") == 1);
}

TEST_CASE("ADSTP stops the triggers: TIM15 schedules nothing at its next update")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | exten_rising | extsel(14));
  auto const idle  = f.target->pending_events();
  auto const start = tim15_trgo(f);
  static_cast<void>(start_scan(f));
  read_scan(f, start + tim15_period);
  f.write("ADC1", "ADC_CR", advregen | aden | adstp);
  CHECK(f.cr("ADSTART") == 0);
  f.run_until(start + 2 * tim15_period + scan_ps);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.isr("EOSMP") == 0);
  CHECK(f.field("TIM15", "SR", "UIF") == 1);
  CHECK(f.target->pending_events() == idle);
}

TEST_CASE(
    "EXTSEL selecting a signal no model produces logs once per selection and converts nothing")
{
  struct selection
  {
    std::uint32_t    value;
    std::string_view signal;
  };
  for(auto const [value, signal] : {
          selection{6,  "trigger exti11"    },
          selection{0,  "trigger tim1_oc1"  },
          selection{10, "trigger tim1_trgo2"},
          selection{17, "reserved"          },
          selection{31, "reserved"          },
  })
  {
    CAPTURE(value);
    adc_fixture const             f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    REQUIRE(f.target->model("TIM1") != nullptr);
    downstream_scan(f, ovrmod | exten_rising | extsel(value));
    static_cast<void>(start_scan(f));
    f.write("ADC1", "ADC_CR", advregen | aden | adstp);
    static_cast<void>(start_scan(f));
    f.run_until(f.now() + 2 * scan_ps);
    CHECK(f.isr("EOC") == 0);
    CHECK(f.cr("ADSTART") == 1);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].peripheral == "ADC1");
    CHECK(logged[0].reg == "ADC_CFGR1");
    CHECK(logged[0].message.contains(signal));
  }
  adc_fixture const f{u575(), {}, true, true};
  downstream_scan(f, ovrmod | exten_rising | extsel(6));
  CHECK_THROWS_AS(
      static_cast<void>(
          f.target->mem().write(f.at("ADC1", "ADC_CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
}

TEST_CASE("after a DMNGT 01 end, a TIM15 trigger starts a sequence but issues no request")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | dmngt_oneshot | exten_rising | extsel(14));
  single_dma(f, 20);
  auto const start = tim15_trgo(f);
  static_cast<void>(start_scan(f));
  auto const update = start + tim15_period;
  f.run_until(update + scan_eoc(4) + 2 * us);
  CHECK(dma_flag(f, "TCF") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C1CR", "EN") == 0);
  CHECK(f.cr("ADSTART") == 1);
  clear_dma_flags(f);
  f.write("GPDMA1", "GPDMA_C1BR1", 20);
  f.write("GPDMA1", "GPDMA_C1CR", dma_enable);
  f.run_until(update + tim15_period + latency + scan_ps);
  CHECK(f.isr("EOS") == 1);
  CHECK(f.read("ADC_DR") == scan_code(9));
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 20);
  CHECK(
      buffer_words(f, 6)
      == std::vector<std::uint32_t>{
          scan_code(0), scan_code(1), scan_code(2), scan_code(3), scan_code(4), 0});
}

TEST_CASE("a snapshot between two triggered scans resumes with the same scan in a fresh machine")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | exten_rising | extsel(14));
  auto const start = tim15_trgo(f);
  static_cast<void>(start_scan(f));
  read_scan(f, start + tim15_period);
  f.run_until(start + 750 * ms);
  auto const saved = f.target->snapshot();
  read_scan(f, start + 2 * tim15_period);

  adc_fixture const other;
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  read_scan(other, start + 2 * tim15_period);
  other.run_until(start + 2 * tim15_period + 2 * scan_ps);
  CHECK(other.isr("EOC") == 0);
}

TEST_CASE(
    "EXTEN picks the TRGO edges: a pulse falls one timer clock after it rises, MMS 001 is CEN")
{
  struct setting
  {
    std::uint32_t exten;
    std::uint32_t mode;
  };
  for(auto const [exten, mode] : {
          setting{exten_rising,  2},
          setting{exten_falling, 2},
          setting{exten_both,    2},
          setting{exten_rising,  1},
          setting{exten_falling, 1},
          setting{exten_both,    1},
  })
  {
    CAPTURE(exten);
    CAPTURE(mode);
    adc_fixture const f;
    downstream_scan(f, ovrmod | exten | extsel(14));
    f.put("RCC", "RCC_APB2ENR", "TIM15EN", 1);
    f.put("TIM15", "CR2", "MMS", mode);
    f.write("TIM15", "PSC", 124);
    f.write("TIM15", "ARR", 15'999);
    f.write("TIM15", "EGR", 1);
    f.write("TIM15", "SR", 0);
    static_cast<void>(start_scan(f));
    auto const start = f.now();
    f.write("TIM15", "CR1", mode == 1 ? 1u << 3 | 1u : 1u);
    std::vector<std::uint64_t> edges;
    if(mode == 2)
    {
      edges.push_back(start + tim15_period + (exten == exten_falling ? tim15_clock : 0));
    }
    else
    {
      if(exten != exten_falling)
      {
        edges.push_back(start);
      }
      if(exten != exten_rising)
      {
        edges.push_back(start + tim15_period);
      }
    }
    for(auto const edge : edges)
    {
      read_scan(f, edge);
    }
    f.run_until(start + 2 * tim15_period - us);
    CHECK(f.isr("EOC") == 0);
  }
}

TEST_CASE("a trigger starts the sequence tLATR later: 3, 7, 13 ADC clocks for PRESC 0 to 2")
{
  struct setting
  {
    std::uint32_t presc;
    std::uint64_t divider;
    std::uint64_t cycles;
  };
  for(auto const [presc, divider, cycles] : {
          setting{0, 1, 3 },
          setting{1, 2, 7 },
          setting{2, 4, 13},
          setting{3, 6, 19},
  })
  {
    CAPTURE(presc);
    adc_fixture const f;
    f.put("RCC", "RCC_CFGR1", "SW", 1);
    f.run_until(f.now() + 200 * us);
    REQUIRE(f.field("RCC", "RCC_CFGR1", "SWS") == 1);
    downstream_scan(f, ovrmod | exten_rising | extsel(14));
    f.write("ADC12", "ADC12_CCR", vrefen | vsensesel | (presc << 18));
    auto const start = tim15_trgo(f, 499);
    static_cast<void>(start_scan(f));
    auto const eoc = start + tim15_period + (cycles + scan_eoc_cycles[0]) * divider * adc_cycle_ps;
    f.run_until(eoc - adc_cycle_ps);
    CHECK(f.isr("EOC") == 0);
    f.run_until(eoc);
    CHECK(f.isr("EOC") == 1);
    CHECK(f.cpu_read("ADC_DR") == scan_code(0));
  }
}

TEST_CASE("DISCEN with DISCNUM 2 converts three ranks per TIM15 update, ADSTART stays set")
{
  adc_fixture const f;
  downstream_scan(f, ovrmod | discen | (2u << 17) | exten_rising | extsel(14));
  auto const start = tim15_trgo(f);
  static_cast<void>(start_scan(f));
  for(std::size_t group = 0; group < 3; ++group)
  {
    CAPTURE(group);
    auto const first  = 3 * group;
    auto const begin  = start + (group + 1) * tim15_period + latency;
    auto const before = first == 0 ? 0 : scan_eoc(first - 1);
    for(auto rank = first; rank < first + 3; ++rank)
    {
      CAPTURE(rank);
      auto const end = begin + scan_eoc(rank) - before;
      f.run_until(end - us);
      CHECK(f.isr("EOC") == 0);
      f.run_until(end);
      CHECK(f.isr("EOC") == 1);
      CHECK(f.cpu_read("ADC_DR") == scan_code(rank));
    }
    f.run_until(begin + tim15_period / 2);
    CHECK(f.isr("EOC") == 0);
    CHECK(f.isr("EOS") == 0);
    CHECK(f.cr("ADSTART") == 1);
  }
}
