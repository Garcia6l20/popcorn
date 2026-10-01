#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::testing::stm32_fixture;
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

struct adc_fixture : stm32_fixture
{
  adc_fixture() : stm32_fixture{u575(), "boot_ll"}
  {}

  explicit adc_fixture(popcorn::mcu_description description)
      : stm32_fixture{std::move(description), "boot_ll"}
  {}

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

  void enable() const
  {
    write("ADC1", "ADC_CR", advregen);
    write("ADC1", "ADC_CR", advregen | aden);
  }

  [[nodiscard]] auto convert(std::uint32_t channel) const -> std::uint32_t
  {
    write("ADC1", "ADC_SQR1", channel << 6);
    write("ADC1", "ADC_CR", advregen | adstart);
    return read("ADC_DR");
  }
};
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

TEST_CASE("a conversion sets EOC and EOS and clears ADSTART, reading ADC_DR clears EOC")
{
  adc_fixture const f;
  f.write("ADC12", "ADC12_CCR", vrefen);
  f.enable();
  f.write("ADC1", "ADC_CR", advregen | adstart);
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
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
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
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    f.write("ADC12", "ADC12_CCR", vrefen);
    f.enable();
    f.write("ADC1", reg, value);
    f.write("ADC1", "ADC_CR", advregen | adstart);
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
  adc_fixture const f{std::move(description)};
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
