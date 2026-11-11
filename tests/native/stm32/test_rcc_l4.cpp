#include "../support/inert_model.hpp"
#include "rcc_resets.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/clock_output.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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
constexpr std::uint32_t                 pll_hse  = 3;
constexpr std::uint32_t                 pll_hsi  = 2;
constexpr std::uint32_t                 p_en     = 1u << 16;
constexpr std::uint32_t                 q_en     = 1u << 20;
constexpr std::uint32_t                 r_en     = 1u << 24;
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

auto options(std::optional<std::uint64_t> lsi = std::nullopt) -> core::machine_options
{
  auto               out = popcorn::testing::stm32_options();
  stm32::board const board{
      .rcc = {.hse_hz = hse_hz, .lsi_hz = lsi}
  };
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
  explicit rig(std::optional<std::uint64_t> lsi = std::nullopt)
      : f{l476rc(), firmware, options(lsi)}
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

  void start(std::string_view on) const
  {
    set("CR", on, 1);
    idle();
  }

  void pll(std::uint32_t src, std::uint32_t m, std::uint32_t n, std::uint32_t outputs) const
  {
    set("CR", "PLLON", 0);
    write("PLLCFGR", src | m << 4 | n << 8 | outputs);
    start("PLLON");
    REQUIRE(field("CR", "PLLRDY") == 1);
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

TEST_CASE("RM0351 6.2.17 LSCO: LSCOEN drives LSI or LSE onto PA2, DS10198 and DS10199 pin tables")
{
  rig const   r{31'550};
  auto* const out = dynamic_cast<stm32::clock_output_port*>(r.f.target->model("RCC"));
  REQUIRE(out != nullptr);
  r.set("BDCR", "LSCOEN", 1);
  CHECK(out->clock_hz("GPIOA", 2) == 0);
  r.set("CSR", "LSION", 1);
  r.set("BDCR", "LSEON", 1);
  r.idle();
  CHECK(out->clock_hz("GPIOA", 2) == 31'550);
  CHECK(out->clock_hz("GPIOB", 2) == 0);
  CHECK(out->clock_hz("GPIOA", 3) == 0);
  r.set("BDCR", "LSCOSEL", 1);
  CHECK(out->clock_hz("GPIOA", 2) == 32'768);
  r.set("BDCR", "LSCOEN", 0);
  CHECK(out->clock_hz("GPIOA", 2) == 0);
}

TEST_CASE("RccOptions lsi_hz sets the L4 LSI rate, DS10198 Table 61 bounds it")
{
  rig const r{31'550};
  r.set("CSR", "LSION", 1);
  r.idle();
  CHECK(r.hz("lsi") == 31'550);
  for(auto const accepted : {29'500u, 34'000u})
  {
    CHECK_NOTHROW(stm32_fixture(l476rc(), firmware, options(accepted)));
  }
  CHECK_THROWS_WITH_AS(
      stm32_fixture(l476rc(), firmware, options(34001u)),
      doctest::Contains("from 29500 to 34000 Hz, not 34001 Hz"), popcorn::config_error);
  for(auto const refused : {29'499u, 34'001u, 0u})
  {
    CHECK_THROWS_AS(stm32_fixture(l476rc(), firmware, options(refused)), popcorn::config_error);
  }
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

TEST_CASE("RM0351 6.4.19: the wwdg domain is PCLK1 while WWDGEN is set and 0 otherwise")
{
  rig const r;
  CHECK(r.hz("wwdg") == 0);
  r.set("APB1ENR1", "WWDGEN", 1);
  CHECK(r.hz("wwdg") == 4 * mhz);
  r.set("CFGR", "PPRE1", 5);
  CHECK(r.hz("wwdg") == mhz);
  r.set("APB1ENR1", "WWDGEN", 0);
  CHECK(r.hz("wwdg") == 0);
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
    "RM0351 6.4: the LL sequence runs the MSI at 4 and 48 MHz, HSI16 and HSE as SYSCLK, the PLL at "
    "80 MHz from HSE and from MSI, and SystemCoreClock equals HCLK at each step")
{
  stm32_fixture f{l476rc(), "l476_l4_rcc_ll", options()};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 29);
  CHECK(log[0] == 0x0C00'0000u);
  CHECK(log[1] == 0);
  CHECK(log[2] == 4 * mhz);
  CHECK(log[3] == 48 * mhz);
  CHECK(log[6] == 16 * mhz);
  CHECK(log[9] == hse_hz / 2);
  CHECK(
      std::vector<std::uint32_t>{log.begin() + 10, log.begin() + 14}
      == std::vector<std::uint32_t>{8 * mhz, 4 * mhz, 1 * mhz, 4 * mhz});
  CHECK(log[14] == 0x0000'2002);
  CHECK(log[15] == 0x0000'8000);
  CHECK(log[16] == 0x0100'0400);
  CHECK(log[17] == 0x0000'5555);
  CHECK(log[18] == 0xFFFF'FEBF);
  CHECK(log[21] == 80 * mhz);
  CHECK(log[26] == 80 * mhz);
  CHECK(log[28] == 0x0030'1800);
  CHECK(f.target->virtual_clock().core_hz() == 80 * mhz);
  CHECK(f.field("RCC", "PLLCFGR", "PLLM") == 5);
  CHECK(f.field("RCC", "PLLCFGR", "PLLSRC") == 1);
  CHECK(f.field("RCC", "CR", "PLLSAI1RDY") == 1);
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

TEST_CASE(
    "RM0351 6.4.4: the main PLL from HSE runs PLLCLK, PLL48M1CLK and PLLSAI3CLK at the VCO over R, "
    "Q and P, and SW 11 runs SYSCLK from PLLCLK")
{
  rig const r;
  r.start("HSEON");
  r.write("PLLCFGR", pll_hse | 20u << 8 | p_en | q_en | 1u << 21 | r_en);
  r.set("CR", "PLLON", 1);
  CHECK(r.field("CR", "PLLRDY") == 0);
  r.idle();
  REQUIRE(r.field("CR", "PLLRDY") == 1);
  CHECK(r.hz("pll1_r") == 80 * mhz);
  CHECK(r.hz("pll1_q") == 40 * mhz);
  CHECK(r.hz("pll1_p") == 160 * mhz / 7);
  r.set("CFGR", "SW", 3);
  CHECK(r.field("CFGR", "SWS") == 3);
  CHECK(r.hz("sysclk") == 80 * mhz);
  CHECK(r.core_hz() == 80 * mhz);
  r.set("PLLCFGR", "PLLREN", 0);
  CHECK(r.field("PLLCFGR", "PLLREN") == 1);
  r.set("PLLCFGR", "PLLQEN", 0);
  CHECK(r.hz("pll1_q") == 0);
}

TEST_CASE(
    "RM0351 6.4.4: PLLM divides by 1 to 8, PLLN multiplies by 8 to 86, PLLP divides by 7 or 17 and "
    "PLLQ and PLLR by 2, 4, 6 or 8")
{
  rig const r;
  r.start("HSION");
  for(std::uint32_t m = 0; m < 8; ++m)
  {
    CAPTURE(m);
    r.pll(pll_hsi, m, 16, r_en);
    CHECK(r.hz("pll1_r") == 16 * mhz * 16 / (m + 1) / 2);
  }
  for(std::uint32_t const n : {8u, 86u})
  {
    CAPTURE(n);
    r.pll(pll_hsi, 3, n, r_en);
    CHECK(r.hz("pll1_r") == 4 * mhz * n / 2);
  }
  for(std::uint32_t const n : {7u, 87u})
  {
    CAPTURE(n);
    r.pll(pll_hsi, 3, n, r_en);
    CHECK(r.hz("pll1_r") == 0);
  }
  for(std::uint32_t code = 0; code < 4; ++code)
  {
    CAPTURE(code);
    r.pll(pll_hsi, 3, 42, p_en | code % 2 << 17 | q_en | code << 21 | r_en | code << 25);
    CHECK(r.hz("pll1_p") == 168 * mhz / (code % 2 == 0 ? 7 : 17));
    CHECK(r.hz("pll1_q") == 168 * mhz / ((code + 1) * 2));
    CHECK(r.hz("pll1_r") == 168 * mhz / ((code + 1) * 2));
  }
}

TEST_CASE(
    "RM0351 6.4.5 and 6.4.6: PLLSAI1 and PLLSAI2 run on PLLSRC and PLLM with their own N, P, Q and "
    "R and output enables")
{
  rig const r;
  r.start("HSION");
  r.write("PLLCFGR", pll_hsi | 1u << 4);
  r.write("PLLSAI1CFGR", 24u << 8 | p_en | q_en | 1u << 21 | r_en | 1u << 25);
  r.write("PLLSAI2CFGR", 43u << 8 | p_en | 1u << 17 | r_en | 3u << 25);
  r.set("CR", "PLLSAI1ON", 1);
  CHECK(r.field("CR", "PLLSAI1RDY") == 0);
  r.idle();
  REQUIRE(r.field("CR", "PLLSAI1RDY") == 1);
  CHECK(r.field("CR", "PLLSAI2RDY") == 0);
  r.start("PLLSAI2ON");
  REQUIRE(r.field("CR", "PLLSAI2RDY") == 1);
  CHECK(r.field("CR", "PLLRDY") == 0);
  CHECK(r.hz("pll1_r") == 0);
  CHECK(r.hz("pllsai1_p") == 192 * mhz / 7);
  CHECK(r.hz("pllsai1_q") == 48 * mhz);
  CHECK(r.hz("pllsai1_r") == 48 * mhz);
  CHECK(r.hz("pllsai2_p") == 344 * mhz / 17);
  CHECK(r.hz("pllsai2_r") == 43 * mhz);
  r.set("PLLSAI1CFGR", "PLLSAI1QEN", 0);
  r.set("PLLSAI2CFGR", "PLLSAI2PEN", 0);
  CHECK(r.hz("pllsai1_q") == 0);
  CHECK(r.hz("pllsai1_p") == 192 * mhz / 7);
  CHECK(r.hz("pllsai2_p") == 0);
  CHECK(r.hz("pllsai2_r") == 43 * mhz);
}

TEST_CASE(
    "RM0351 6.4.4 to 6.4.6: PLLSRC and PLLM hold while any PLL is on or ready, N and the dividers "
    "of a PLL while that PLL is")
{
  rig const r;
  r.start("HSION");
  r.write("PLLCFGR", pll_hsi | 1u << 4 | 20u << 8 | r_en);
  r.write("PLLSAI1CFGR", 24u << 8 | r_en);
  r.start("PLLON");
  r.write("PLLCFGR", pll_hse | 30u << 8 | q_en | 1u << 25 | r_en);
  CHECK(r.field("PLLCFGR", "PLLSRC") == pll_hsi);
  CHECK(r.field("PLLCFGR", "PLLM") == 1);
  CHECK(r.field("PLLCFGR", "PLLN") == 20);
  CHECK(r.field("PLLCFGR", "PLLR") == 0);
  CHECK(r.field("PLLCFGR", "PLLQEN") == 1);
  r.set("PLLSAI1CFGR", "PLLSAI1N", 30);
  CHECK(r.field("PLLSAI1CFGR", "PLLSAI1N") == 30);
  r.set("CR", "PLLON", 0);
  r.start("PLLSAI1ON");
  r.write("PLLCFGR", pll_hse | 30u << 8 | r_en);
  CHECK(r.field("PLLCFGR", "PLLSRC") == pll_hsi);
  CHECK(r.field("PLLCFGR", "PLLM") == 1);
  CHECK(r.field("PLLCFGR", "PLLN") == 30);
  r.set("PLLSAI1CFGR", "PLLSAI1N", 40);
  CHECK(r.field("PLLSAI1CFGR", "PLLSAI1N") == 30);
  CHECK(r.hz("pllsai1_r") == 8 * mhz * 30 / 2);
  r.set("CR", "PLLSAI1ON", 0);
  r.start("PLLSAI2ON");
  REQUIRE(r.field("CR", "PLLSAI2RDY") == 1);
  r.write("PLLCFGR", pll_hse | 30u << 8 | r_en);
  CHECK(r.field("PLLCFGR", "PLLSRC") == pll_hsi);
  CHECK(r.field("PLLCFGR", "PLLM") == 1);
  r.set("CR", "PLLSAI2ON", 0);
  r.write("PLLCFGR", pll_hse | 30u << 8 | r_en);
  CHECK(r.field("PLLCFGR", "PLLSRC") == pll_hse);
  CHECK(r.field("PLLCFGR", "PLLM") == 0);
}

TEST_CASE(
    "RM0351 6.4.28: the USART, UART, LPUART, I2C and LPTIM kernel clocks follow their RCC_CCIPR "
    "selection and their enable bit")
{
  rig const r;
  r.start("HSION");
  r.set("CSR", "LSION", 1);
  r.set("BDCR", "LSEON", 1);
  r.idle();
  r.set("CFGR", "PPRE1", 4);
  r.set("APB2ENR", "USART1EN", 1);
  r.set("APB1ENR1", "USART2EN", 1);
  r.set("APB1ENR2", "LPUART1EN", 1);
  r.set("APB1ENR1", "I2C1EN", 1);
  r.set("APB1ENR1", "LPTIM1EN", 1);
  CHECK(r.hz("usart1") == 4 * mhz);
  CHECK(r.hz("usart2") == 2 * mhz);
  CHECK(r.hz("usart3") == 0);
  CHECK(r.hz("lpuart1") == 2 * mhz);
  CHECK(r.hz("i2c1") == 2 * mhz);
  CHECK(r.hz("lptim1") == 2 * mhz);
  r.write("CCIPR", 1u << 0 | 2u << 2 | 3u << 10 | 3u << 12 | 1u << 18);
  CHECK(r.hz("usart1") == 4 * mhz);
  CHECK(r.hz("usart2") == 16 * mhz);
  CHECK(r.hz("lpuart1") == 32'768);
  CHECK(r.hz("i2c1") == 0);
  CHECK(r.hz("lptim1") == 32'000);
  r.write("CCIPR", 3u << 0 | 1u << 2 | 2u << 12 | 3u << 18);
  CHECK(r.hz("usart1") == 32'768);
  CHECK(r.hz("usart2") == 4 * mhz);
  CHECK(r.hz("i2c1") == 16 * mhz);
  CHECK(r.hz("lptim1") == 32'768);
  r.set("APB2ENR", "USART1EN", 0);
  CHECK(r.hz("usart1") == 0);
}

TEST_CASE(
    "RM0351 6.4.28: SAI1, SAI2, the 48 MHz clock, the ADCs, SWPMI1 and DFSDM1 follow their "
    "RCC_CCIPR selection")
{
  rig const r;
  r.start("HSION");
  r.write("PLLCFGR", pll_hsi | 1u << 4 | 20u << 8 | p_en | q_en | 1u << 21 | r_en);
  r.write("PLLSAI1CFGR", 24u << 8 | p_en | q_en | 1u << 21 | r_en | 1u << 25);
  r.start("PLLON");
  r.start("PLLSAI1ON");
  REQUIRE(r.field("CR", "PLLSAI1RDY") == 1);
  r.set("CFGR", "SW", 1);
  r.set("CFGR", "PPRE1", 5);
  r.set("CFGR", "PPRE2", 4);
  r.set("APB2ENR", "SAI1EN", 1);
  r.set("APB2ENR", "SAI2EN", 1);
  r.set("AHB2ENR", "OTGFSEN", 1);
  r.set("AHB2ENR", "RNGEN", 1);
  r.set("APB2ENR", "SDMMC1EN", 1);
  r.set("AHB2ENR", "ADCEN", 1);
  r.set("APB1ENR2", "SWPMI1EN", 1);
  r.set("APB2ENR", "DFSDM1EN", 1);
  CHECK(r.hz("sai1") == 192 * mhz / 7);
  CHECK(r.hz("otg_fs") == 0);
  CHECK(r.hz("adc1") == 0);
  CHECK(r.hz("swpmi1") == 4 * mhz);
  CHECK(r.hz("dfsdm1") == 8 * mhz);
  r.write("CCIPR", 3u << 22 | 2u << 24 | 1u << 26 | 1u << 28 | 1u << 30 | 1u << 31);
  CHECK(r.hz("sai1") == 0);
  CHECK(r.hz("sai2") == 160 * mhz / 7);
  for(auto const name : {"otg_fs", "rng", "sdmmc1"})
  {
    CAPTURE(name);
    CHECK(r.hz(name) == 48 * mhz);
  }
  for(auto const name : {"adc1", "adc2", "adc3"})
  {
    CAPTURE(name);
    CHECK(r.hz(name) == 48 * mhz);
  }
  CHECK(r.hz("swpmi1") == 16 * mhz);
  CHECK(r.hz("dfsdm1") == 16 * mhz);
  r.write("CCIPR", 2u << 26 | 3u << 28);
  CHECK(r.hz("rng") == 40 * mhz);
  CHECK(r.hz("adc2") == 16 * mhz);
  r.write("CCIPR", 3u << 26);
  CHECK(r.hz("sdmmc1") == 4 * mhz);
  r.set("AHB2ENR", "ADCEN", 0);
  CHECK(r.hz("adc3") == 0);
}

TEST_CASE(
    "RM0351 6.4.29: RTCSEL keeps its first selection until BDRST, which resets RCC_BDCR but "
    "LSCOSEL and LSCOEN")
{
  rig const r;
  r.set("BDCR", "LSEON", 1);
  r.idle();
  r.write("BDCR", 1u << 0 | 1u << 8 | 1u << 15 | 1u << 24 | 1u << 25);
  CHECK(r.hz("rtc") == 32'768);
  r.set("BDCR", "RTCSEL", 2);
  CHECK(r.field("BDCR", "RTCSEL") == 1);
  r.reset(reset_kind::software);
  CHECK(r.peek("BDCR") == (1u << 0 | 1u << 1 | 1u << 8 | 1u << 15 | 1u << 24 | 1u << 25));
  r.set("BDCR", "BDRST", 1);
  CHECK(r.peek("BDCR") == (1u << 16 | 1u << 24 | 1u << 25));
  CHECK(r.hz("rtc") == 0);
  CHECK(r.hz("lse") == 0);
  r.set("BDCR", "BDRST", 0);
  r.set("BDCR", "RTCSEL", 2);
  CHECK(r.field("BDCR", "RTCSEL") == 2);
  r.reset(reset_kind::power_on);
  CHECK(r.peek("BDCR") == 0);
}

TEST_CASE("RM0351 6.4.30: each kind of reset sets its RCC_CSR flag and RMVF clears them all")
{
  struct expected
  {
    reset_kind    kind;
    std::uint32_t flags;
  };
  constexpr std::uint32_t pin = 1u << 26;
  for(auto const [kind, flags] : std::to_array<expected>({
          {reset_kind::power_on,        1u << 27 | pin},
          {reset_kind::brown_out,       1u << 27 | pin},
          {reset_kind::pin,             pin           },
          {reset_kind::software,        1u << 28      },
          {reset_kind::watchdog,        1u << 29      },
          {reset_kind::window_watchdog, 1u << 30      },
          {reset_kind::low_power,       1u << 31      },
          {reset_kind::option_load,     1u << 25      },
  }))
  {
    CAPTURE(static_cast<int>(kind));
    rig const r;
    r.set("CSR", "RMVF", 1);
    CHECK((r.peek("CSR") & 0xFF00'0000u) == 0);
    r.reset(kind);
    CHECK((r.peek("CSR") & 0xFF00'0000u) == flags);
  }
}

TEST_CASE(
    "RM0351 6.4.28: each RCC_CCIPR selection field moves its own kernel clock and leaves every "
    "other one at its reset selection")
{
  struct row
  {
    std::string_view domain;
    std::string_view enable_reg;
    std::string_view enable;
    std::uint32_t    shift;
    std::uint32_t    code;
    std::uint64_t    hz;
    std::uint64_t    reset_hz;
  };
  constexpr std::uint64_t pclk   = 2 * mhz;
  constexpr std::uint64_t sai1_p = 192 * mhz / 7;
  auto const              rows   = std::to_array<row>({
      {"usart1",  "APB2ENR",  "USART1EN",  0,  2, 16 * mhz,       pclk  },
      {"usart2",  "APB1ENR1", "USART2EN",  2,  2, 16 * mhz,       pclk  },
      {"usart3",  "APB1ENR1", "USART3EN",  4,  2, 16 * mhz,       pclk  },
      {"uart4",   "APB1ENR1", "UART4EN",   6,  2, 16 * mhz,       pclk  },
      {"uart5",   "APB1ENR1", "UART5EN",   8,  2, 16 * mhz,       pclk  },
      {"lpuart1", "APB1ENR2", "LPUART1EN", 10, 2, 16 * mhz,       pclk  },
      {"i2c1",    "APB1ENR1", "I2C1EN",    12, 2, 16 * mhz,       pclk  },
      {"i2c2",    "APB1ENR1", "I2C2EN",    14, 2, 16 * mhz,       pclk  },
      {"i2c3",    "APB1ENR1", "I2C3EN",    16, 2, 16 * mhz,       pclk  },
      {"lptim1",  "APB1ENR1", "LPTIM1EN",  18, 3, 32'768,         pclk  },
      {"lptim2",  "APB1ENR2", "LPTIM2EN",  20, 3, 32'768,         pclk  },
      {"sai1",    "APB2ENR",  "SAI1EN",    22, 1, 344 * mhz / 17, sai1_p},
      {"sai2",    "APB2ENR",  "SAI2EN",    24, 1, 344 * mhz / 17, sai1_p},
      {"otg_fs",  "AHB2ENR",  "OTGFSEN",   26, 1, 48 * mhz,       0     },
      {"adc1",    "AHB2ENR",  "ADCEN",     28, 2, 43 * mhz,       0     },
      {"swpmi1",  "APB1ENR2", "SWPMI1EN",  30, 1, 16 * mhz,       pclk  },
      {"dfsdm1",  "APB2ENR",  "DFSDM1EN",  31, 1, 4 * mhz,        pclk  },
  });
  rig const               r;
  r.start("HSION");
  r.set("BDCR", "LSEON", 1);
  r.set("CFGR", "PPRE1", 4);
  r.set("CFGR", "PPRE2", 4);
  r.write("PLLCFGR", pll_hsi | 1u << 4);
  r.write("PLLSAI1CFGR", 24u << 8 | p_en | q_en | 1u << 21);
  r.write("PLLSAI2CFGR", 43u << 8 | p_en | 1u << 17 | r_en | 3u << 25);
  r.set("CR", "PLLSAI1ON", 1);
  r.start("PLLSAI2ON");
  REQUIRE(r.field("CR", "PLLSAI1RDY") == 1);
  REQUIRE(r.field("CR", "PLLSAI2RDY") == 1);
  for(auto const& entry : rows)
  {
    r.set(entry.enable_reg, entry.enable, 1);
  }
  for(auto const& moved : rows)
  {
    CAPTURE(moved.domain);
    r.write("CCIPR", moved.code << moved.shift);
    for(auto const& entry : rows)
    {
      CAPTURE(entry.domain);
      CHECK(r.hz(entry.domain) == (&entry == &moved ? entry.hz : entry.reset_hz));
    }
  }
}

TEST_CASE("a PLL enabled at any VCO and range logs no limit warning on the STM32L4, ES0250 2.2.25")
{
  rig const                     r;
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  r.start("HSEON");
  r.write("PLLCFGR", pll_hse | 86u << 8 | p_en | q_en | r_en);
  r.set("CR", "PLLON", 1);
  r.idle();
  CHECK(r.field("CR", "PLLRDY") == 1);
  CHECK(std::ranges::none_of(logged, [](core::log_record const& record) {
    return record.message.contains("outside its limits");
  }));
}
