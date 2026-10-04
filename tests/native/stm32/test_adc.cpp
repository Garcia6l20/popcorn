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
#include <cstddef>
#include <cstdint>
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
    return read("ADC_DR");
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

TEST_CASE("ADSTART converts nothing while ADC1 is disabled")
{
  adc_fixture const f;
  f.write("ADC12", "ADC12_CCR", vrefen);
  f.write("ADC1", "ADC_CR", advregen | adstart);
  f.settle();
  CHECK(f.isr("EOC") == 0);
  CHECK(f.read("ADC_DR") == 0);
}

TEST_CASE("ADSTP clears ADSTART and itself")
{
  adc_fixture const f;
  f.write("ADC1", "ADC_CR", advregen | adstart);
  REQUIRE(f.cr("ADSTART") == 1);
  f.write("ADC1", "ADC_CR", advregen | adstp);
  CHECK(f.cr("ADSTP") == 0);
  CHECK(f.cr("ADSTART") == 0);
  f.enable();
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

TEST_CASE("continuous mode and JADSTART each log one warning, JADSTART clears")
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
  REQUIRE(logged.size() == 2);
  CHECK(logged[0].reg == "ADC_CFGR1");
  CHECK(logged[1].reg == "ADC_CR");
  CHECK(logged[1].message.contains("JADSTART"));
}

TEST_CASE("each unmodelled regular mode logs one warning and converts SQ1 once")
{
  struct setting
  {
    std::string_view reg;
    std::uint32_t    value;
  };
  for(auto const [reg, value] : {
          setting{"ADC_CFGR1", cont    },
          setting{"ADC_CFGR1", 1u << 16},
          setting{"ADC_CFGR1", 1u << 10},
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
    CHECK(logged.size() == 1);
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
