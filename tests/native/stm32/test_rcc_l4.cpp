#include "../support/inert_model.hpp"
#include "rcc_resets.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;

constexpr std::uint64_t                 mhz      = 1'000'000;
constexpr std::uint64_t                 hse_hz   = 8 * mhz;
constexpr auto                          firmware = std::string_view{"l476_l4_smoke"};
constexpr popcorn::address              stub     = 0x2000'8000;
constexpr std::uint32_t                 msi_on   = 1u << 0;
constexpr std::uint32_t                 msi_sel  = 1u << 3;
constexpr std::uint32_t                 msi_pll  = 1u << 2;
constexpr std::array<std::uint64_t, 12> msi_ranges{100'000,    200'000,    400'000,    800'000,
                                                   1'000'000,  2'000'000,  4'000'000,  8'000'000,
                                                   16'000'000, 24'000'000, 32'000'000, 48'000'000};

class listener final : public popcorn::testing::inert_model
{
public:
  explicit listener(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;

  [[nodiscard]] auto hz(std::string_view name) const -> std::uint64_t
  {
    return context.clock.hz(context.clock.domain(name));
  }
};

auto options() -> core::machine_options
{
  auto               out = popcorn::testing::stm32_options();
  stm32::board const board{.rcc = {.hse_hz = hse_hz}};
  out.models = stm32::factories(board, l476rc());
  out.models.insert_or_assign(
      "TSC",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<listener>(ctx); });
  return out;
}

auto rcc_desc() -> popcorn::peripheral_desc const&
{
  return *std::ranges::find(
      l476rc().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
}

auto offset_of(std::string_view reg) -> std::uint32_t
{
  auto const* const found = popcorn::find_register(rcc_desc(), reg);
  REQUIRE(found != nullptr);
  return found->offset;
}

auto field_of(std::string_view reg, std::string_view name) -> popcorn::field_desc const&
{
  auto const* const found = popcorn::find_register(rcc_desc(), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return *bits;
}

struct rig
{
  rig() : f{l476rc(), firmware, options()}
  {
    park();
  }

  void park() const
  {
    REQUIRE(f.target->mem().write(stub, 2, 0xE7FE).has_value());
    f.set("pc", stub | 1u);
  }

  [[nodiscard]] auto hz(std::string_view name) const -> std::uint64_t
  {
    auto* const found = f.model_of<listener>("TSC");
    REQUIRE(found != nullptr);
    return found->hz(name);
  }

  [[nodiscard]] auto core_hz() const -> std::uint64_t
  {
    return f.target->virtual_clock().core_hz();
  }

  [[nodiscard]] auto read(std::string_view reg) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(rcc_desc().base + offset_of(reg), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto peek(std::string_view reg) const -> std::uint32_t
  {
    return f.target->peripheral("RCC").peek(offset_of(reg));
  }

  [[nodiscard]] auto field(std::string_view reg, std::string_view name) const -> std::uint32_t
  {
    return f.field("RCC", reg, name);
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(rcc_desc().base + offset_of(reg), 4, value).has_value());
  }

  void set(std::string_view reg, std::string_view name, std::uint32_t value) const
  {
    auto const& bits = field_of(reg, name);
    auto const  mask = ((std::uint32_t{1} << bits.width) - 1u) << bits.lsb;
    write(reg, (peek(reg) & ~mask) | (value << bits.lsb));
  }

  void idle() const
  {
    f.run_for(150'000'000, icount{1'000'000});
  }

  void reset(reset_kind kind) const
  {
    f.target->reset(kind);
    park();
  }

  stm32_fixture f;
};
}

TEST_CASE(
    "RM0351 6.4.1: after reset the MSI at MSISRANGE 6 runs SYSCLK, HCLK and both APBs at 4 MHz")
{
  rig const r;
  CHECK(r.peek("CR") == 0x0000'0063);
  CHECK(r.peek("CSR") == 0x0C00'0600);
  CHECK(r.field("CFGR", "SWS") == 0);
  for(auto const name : {"msi", "sysclk", "hclk", "pclk1", "pclk2"})
  {
    CAPTURE(name);
    CHECK(r.hz(name) == 4 * mhz);
  }
  CHECK(r.hz("hsi16") == 0);
  CHECK(r.core_hz() == 4 * mhz);
}

TEST_CASE(
    "RM0351 6.4.1: with MSIRGSEL 1 MSIRANGE 0 to 11 runs the MSI and the core at 100 kHz to 48 MHz")
{
  rig const r;
  for(std::uint32_t code = 0; code < msi_ranges.size(); ++code)
  {
    CAPTURE(code);
    r.write("CR", msi_on | msi_sel | code << 4);
    CHECK(r.hz("msi") == msi_ranges[code]);
    CHECK(r.core_hz() == msi_ranges[code]);
  }
}

TEST_CASE("RM0351 6.4.1: with MSIRGSEL 0 the MSI runs at MSISRANGE of RCC_CSR, not at MSIRANGE")
{
  rig const r;
  r.write("CR", msi_on | 11u << 4);
  CHECK(r.field("CR", "MSIRANGE") == 11);
  CHECK(r.field("CR", "MSIRGSEL") == 0);
  CHECK(r.hz("msi") == 4 * mhz);
  CHECK(r.core_hz() == 4 * mhz);
}

TEST_CASE("RM0351 6.4.1: MSIRGSEL reads back on the bus and a write of 0 keeps it")
{
  rig const r;
  r.write("CR", msi_on | msi_sel | 8u << 4);
  CHECK((r.read("CR") & msi_sel) != 0);
  r.write("CR", msi_on | 11u << 4);
  CHECK((r.read("CR") & msi_sel) != 0);
  CHECK(r.hz("msi") == 48 * mhz);
}

TEST_CASE("RM0351 6.4.1: an MSIRANGE code from 12 to 15 is write-protected and keeps the range")
{
  rig const r;
  r.write("CR", msi_on | msi_sel | 8u << 4);
  for(std::uint32_t code = 12; code < 16; ++code)
  {
    CAPTURE(code);
    r.write("CR", msi_on | msi_sel | code << 4);
    CHECK(r.field("CR", "MSIRANGE") == 8);
    CHECK(r.hz("msi") == 16 * mhz);
  }
}

TEST_CASE(
    "RM0351 6.4.30: MSISRANGE takes a write only while MSIRGSEL is 1, leaves the running range, "
    "and a system reset returns the MSI to MSISRANGE 6")
{
  rig const r;
  r.set("CSR", "MSISRANGE", 7);
  CHECK(r.field("CSR", "MSISRANGE") == 6);
  CHECK(r.hz("msi") == 4 * mhz);
  r.write("CR", msi_on | msi_sel | 9u << 4);
  r.set("CSR", "MSISRANGE", 7);
  CHECK(r.field("CSR", "MSISRANGE") == 7);
  CHECK(r.hz("msi") == 24 * mhz);
  r.reset(reset_kind::software);
  CHECK(r.field("CR", "MSIRGSEL") == 0);
  CHECK(r.field("CSR", "MSISRANGE") == 6);
  CHECK(r.hz("msi") == 4 * mhz);
  CHECK(r.core_hz() == 4 * mhz);
}

TEST_CASE("RM0351 6.4.1: HSI16 is ready after HSION, SW 01 runs SYSCLK at 16 MHz and HSION holds")
{
  rig const r;
  r.set("CR", "HSION", 1);
  CHECK(r.field("CR", "HSIRDY") == 0);
  r.idle();
  REQUIRE(r.field("CR", "HSIRDY") == 1);
  CHECK(r.hz("hsi16") == 16 * mhz);
  r.set("CFGR", "SW", 1);
  CHECK(r.field("CFGR", "SWS") == 1);
  CHECK(r.hz("sysclk") == 16 * mhz);
  CHECK(r.core_hz() == 16 * mhz);
  r.set("CR", "HSION", 0);
  CHECK(r.field("CR", "HSION") == 1);
  r.set("CR", "MSION", 0);
  CHECK(r.field("CR", "MSION") == 0);
  CHECK(r.hz("msi") == 0);
}

TEST_CASE("RM0351 6.4.3: SW 10 waits for HSERDY, then SWS follows and SYSCLK runs at the board HSE")
{
  rig const r;
  r.set("CR", "HSEON", 1);
  r.set("CFGR", "SW", 2);
  CHECK(r.field("CFGR", "SWS") == 0);
  CHECK(r.core_hz() == 4 * mhz);
  r.idle();
  CHECK(r.field("CR", "HSERDY") == 1);
  CHECK(r.field("CFGR", "SWS") == 2);
  CHECK(r.hz("sysclk") == hse_hz);
  CHECK(r.core_hz() == hse_hz);
}

TEST_CASE(
    "RM0351 6.4.1: HSEBYP keeps its value while HSEON is 1 and through a system reset, a power "
    "reset clears it")
{
  rig const r;
  r.set("CR", "HSEBYP", 1);
  CHECK(r.field("CR", "HSEBYP") == 1);
  r.set("CR", "HSEON", 1);
  r.set("CR", "HSEBYP", 0);
  CHECK(r.field("CR", "HSEBYP") == 1);
  r.set("CR", "HSEON", 0);
  r.set("CR", "HSEBYP", 0);
  CHECK(r.field("CR", "HSEBYP") == 0);
  r.set("CR", "HSEBYP", 1);
  r.reset(reset_kind::software);
  CHECK(r.field("CR", "HSEBYP") == 1);
  CHECK(r.peek("CR") == (0x0000'0063 | 1u << 18));
  for(auto const kind : {reset_kind::power_on, reset_kind::brown_out})
  {
    CAPTURE(static_cast<int>(kind));
    r.set("CR", "HSEBYP", 1);
    r.reset(kind);
    CHECK(r.peek("CR") == 0x0000'0063);
  }
}

TEST_CASE("RM0351 6.4.1: CSSON is set only and reads back on the bus")
{
  rig const r;
  r.set("CR", "CSSON", 1);
  CHECK((r.read("CR") & 1u << 19) != 0);
  r.set("CR", "CSSON", 0);
  CHECK(r.field("CR", "CSSON") == 1);
}

TEST_CASE(
    "RM0351 6.4.30 and 6.4.29: LSION of RCC_CSR runs the LSI at 32 kHz, LSEON of RCC_BDCR the LSE "
    "at the board rate, and a system reset keeps RCC_BDCR")
{
  rig const r;
  r.set("CSR", "LSION", 1);
  r.set("BDCR", "LSEON", 1);
  CHECK(r.field("CSR", "LSIRDY") == 0);
  CHECK(r.field("BDCR", "LSERDY") == 0);
  r.idle();
  CHECK(r.field("CSR", "LSIRDY") == 1);
  CHECK(r.field("BDCR", "LSERDY") == 1);
  CHECK(r.hz("lsi") == 32'000);
  CHECK(r.hz("lse") == 32'768);
  r.set("CSR", "LSION", 0);
  CHECK(r.field("CSR", "LSIRDY") == 0);
  CHECK(r.hz("lsi") == 0);
  r.reset(reset_kind::software);
  CHECK(r.field("BDCR", "LSEON") == 1);
  CHECK(r.field("BDCR", "LSERDY") == 1);
  CHECK(r.hz("lse") == 32'768);
  r.reset(reset_kind::power_on);
  CHECK(r.peek("BDCR") == 0);
}

TEST_CASE("RM0351 6.4.3: HPRE divides SYSCLK into HCLK and the core clock, PPRE1 and PPRE2 HCLK")
{
  rig const r;
  r.set("CFGR", "HPRE", 8);
  r.set("CFGR", "PPRE1", 5);
  r.set("CFGR", "PPRE2", 7);
  CHECK(r.hz("sysclk") == 4 * mhz);
  CHECK(r.hz("hclk") == 2 * mhz);
  CHECK(r.core_hz() == 2 * mhz);
  CHECK(r.hz("pclk1") == mhz / 2);
  CHECK(r.hz("pclk2") == mhz / 8);
}

TEST_CASE("RM0351 6.2.15: a timer runs at its APB clock, twice it when the APB divides")
{
  rig const r;
  r.set("APB1ENR1", "TIM2EN", 1);
  r.set("APB2ENR", "TIM1EN", 1);
  CHECK(r.hz("tim2") == 4 * mhz);
  CHECK(r.hz("tim1") == 4 * mhz);
  CHECK(r.hz("tim3") == 0);
  r.set("CFGR", "PPRE1", 5);
  r.set("CFGR", "PPRE2", 7);
  CHECK(r.hz("tim2") == 2 * mhz);
  CHECK(r.hz("tim1") == mhz / 2);
}

TEST_CASE(
    "RM0351 6.4.8 and 6.4.9: a ready flag sets in RCC_CIFR under its RCC_CIER enable and its "
    "RCC_CICR bit clears it")
{
  rig const r;
  r.set("CIER", "HSIRDYIE", 1);
  r.set("CR", "HSION", 1);
  r.set("CSR", "LSION", 1);
  r.idle();
  CHECK(r.field("CIFR", "HSIRDYF") == 1);
  CHECK(r.field("CIFR", "LSIRDYF") == 0);
  r.write("CICR", 1u << 3);
  CHECK(r.field("CIFR", "HSIRDYF") == 0);
}

TEST_CASE("RM0351 6.4.1: MSIPLLEN takes a 1 only while the LSE is ready and clears when it stops")
{
  rig const r;
  r.write("CR", msi_on | msi_pll | 6u << 4);
  CHECK(r.field("CR", "MSIPLLEN") == 0);
  r.set("BDCR", "LSEON", 1);
  r.idle();
  REQUIRE(r.field("BDCR", "LSERDY") == 1);
  r.write("CR", msi_on | msi_pll | 6u << 4);
  CHECK(r.field("CR", "MSIPLLEN") == 1);
  CHECK(r.hz("msi") == 4 * mhz);
  r.set("BDCR", "LSEON", 0);
  CHECK(r.field("CR", "MSIPLLEN") == 0);
}

TEST_CASE(
    "RM0351 6.4: the LL sequence runs the MSI at 4 and 48 MHz, then HSI16 and HSE as SYSCLK, and "
    "SystemCoreClock equals HCLK at each step")
{
  stm32_fixture f{l476rc(), "l476_l4_rcc_ll", options()};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 19);
  CHECK(log[0] == 0x0C00'0000u);
  CHECK(log[1] == 0);
  CHECK(log[2] == 4 * mhz);
  CHECK(log[3] == 48 * mhz);
  CHECK(log[6] == 16 * mhz);
  CHECK(log[9] == hse_hz / 2);
  CHECK(
      std::vector<std::uint32_t>{log.begin() + 10, log.begin() + 14}
      == std::vector<std::uint32_t>{8 * mhz, 4 * mhz, 1 * mhz, 4 * mhz});
  CHECK(f.target->virtual_clock().core_hz() == hse_hz / 2);
  CHECK(log[14] == 0x0000'2002);
  CHECK(log[15] == 0x0000'8000);
  CHECK(log[16] == 0x0100'0400);
  CHECK(log[17] == 0x0000'5555);
  CHECK(log[18] == 0xFFFF'FEBF);
}

TEST_CASE(
    "RM0351 6.4.10 to 6.4.15: every RCC reset bit of the L476RC names an instance "
    "and holds it in reset")
{
  auto const& part = l476rc();
  for(auto const& reg : rcc_desc().registers)
  {
    if(not reg.name.contains("RSTR"))
    {
      continue;
    }
    for(auto const& bits : reg.fields)
    {
      CAPTURE(bits.name);
      CHECK(std::ranges::any_of(part.peripherals, [&](auto const& peripheral) {
        auto const found = stm32::reset_bit_of(rcc_desc(), peripheral.name);
        return found and found->field == bits.name;
      }));
    }
  }
  rig const r;
  r.set("AHB2RSTR", "ADCRST", 1);
  r.set("AHB3RSTR", "QSPIRST", 1);
  r.set("APB1RSTR1", "SPI3RST", 1);
  r.set("APB2RSTR", "SDMMC1RST", 1);
  r.set("AHB2RSTR", "OTGFSRST", 1);
  for(auto const name :
      {"ADC1", "ADC2", "ADC3", "ADC_Common", "QUADSPI", "SPI3", "SDMMC1", "OTG_FS"})
  {
    CAPTURE(name);
    CHECK(r.f.target->held_in_reset(name));
  }
  r.set("AHB2RSTR", "ADCRST", 0);
  CHECK_FALSE(r.f.target->held_in_reset("ADC1"));
}
