#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::rcc_reset_pulse;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::uint32_t base    = 0x4001'2400;
constexpr std::uint32_t sr      = 0x00;
constexpr std::uint32_t cr1     = 0x04;
constexpr std::uint32_t cr2     = 0x08;
constexpr std::uint32_t smpr1   = 0x0C;
constexpr std::uint32_t smpr2   = 0x10;
constexpr std::uint32_t smpr3   = 0x14;
constexpr std::uint32_t sqr1    = 0x30;
constexpr std::uint32_t sqr2    = 0x34;
constexpr std::uint32_t sqr3    = 0x38;
constexpr std::uint32_t sqr4    = 0x3C;
constexpr std::uint32_t sqr5    = 0x40;
constexpr std::uint32_t dr      = 0x58;
constexpr std::uint32_t smpr0   = 0x5C;
constexpr std::uint32_t csr     = 0x300;
constexpr std::uint32_t ccr     = 0x304;
constexpr std::uint32_t adon    = 1u << 0;
constexpr std::uint32_t cont    = 1u << 1;
constexpr std::uint32_t bank_b  = 1u << 2;
constexpr std::uint32_t eocs    = 1u << 10;
constexpr std::uint32_t align   = 1u << 11;
constexpr std::uint32_t swstart = 1u << 30;
constexpr std::uint32_t scan    = 1u << 8;
constexpr std::uint32_t eocie   = 1u << 5;
constexpr std::uint32_t ovrie   = 1u << 26;
constexpr std::uint32_t eoc     = 1u << 1;
constexpr std::uint32_t strt    = 1u << 4;
constexpr std::uint32_t ovr     = 1u << 5;
constexpr std::uint32_t adons   = 1u << 6;
constexpr std::uint32_t tsvrefe = 1u << 23;
constexpr std::uint64_t ns      = 1'000;
constexpr std::uint64_t us      = 1'000'000;
constexpr std::uint64_t cycle   = 62'500;
constexpr std::uint32_t vrefint = 1671;
constexpr std::uint32_t ts_cal1 = 680;

auto options(part_description const& desc, bool strict, stm32::supply_options supply)
    -> core::machine_options
{
  stm32::board board{};
  board.supply           = supply;
  auto out               = popcorn::testing::stm32_options();
  out.models             = stm32::factories(board, desc);
  out.strict_peripherals = strict;
  return out;
}

struct adc_rig
{
  explicit adc_rig(
      part_description      desc    = l152re(),
      bool                  strict  = false,
      bool                  hsi     = true,
      stm32::supply_options supply  = {},
      std::string_view      program = "l1_tim_ll")
      : firmware{std::format(
            "{}_{}",
            desc.part.starts_with("STM32L152RB") ? "l152xb" : "l152xe",
            program)},
        f{desc, firmware, options(desc, strict, supply)}
  {
    f.set("pc", sym(firmware, "idle"));
    if(hsi)
    {
      hsi_on();
    }
  }

  void hsi_on() const
  {
    auto const rcc = f.target->peripheral("RCC").description().base;
    REQUIRE(f.target->mem().write(rcc, 4, 0x0000'0301u).has_value());
    run_until(now() + 200 * us);
    REQUIRE(f.field("RCC", "CR", "HSIRDY") == 1);
  }

  [[nodiscard]] auto read(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(base + offset, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto cpu_read(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = f.target->model("ADC")->on_read(
        f.target->peripheral("ADC"), offset, 4,
        {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  void attempt(std::uint32_t offset, std::uint32_t value) const
  {
    static_cast<void>(f.target->mem().write(base + offset, 4, value));
  }

  [[nodiscard]] auto due() const -> std::uint64_t
  {
    auto const& units = f.target->description().peripherals;
    auto const  owner = static_cast<std::uint32_t>(
        std::ranges::find(units, std::string_view{"ADC"}, &popcorn::peripheral_desc::name)
        - units.begin());
    auto earliest = ~std::uint64_t{};
    for(auto const& event : f.target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner and event.due_ps >= now())
      {
        earliest = std::min(earliest, event.due_ps);
      }
    }
    return earliest;
  }

  void write(std::uint32_t offset, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(base + offset, 4, value).has_value());
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  void power() const
  {
    write(cr2, adon);
    run_until(now() + 4 * us);
    REQUIRE((read(sr) & adons) != 0);
  }

  [[nodiscard]] auto port() const -> stm32::analog_port&
  {
    auto* const found = f.model_of<stm32::analog_port>("ADC");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto convert(std::uint32_t channel, std::uint32_t control = 0) const
      -> std::uint32_t
  {
    write(sqr5, channel);
    write(cr2, adon | control | swstart);
    run_until(now() + 40 * us);
    return cpu_read(dr);
  }

  [[nodiscard]] auto run(std::string_view entry) const -> std::vector<std::uint32_t>
  {
    f.set("pc", sym(firmware, entry));
    auto const stopped = f.run_to_stop({.max_instructions = icount{2'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f.log();
  }

  [[nodiscard]] auto pending() const -> bool
  {
    auto const word = f.target->mem().read(0xE000'E200u, 4);
    REQUIRE(word.has_value());
    return (*word >> 18 & 1u) != 0;
  }

  void unpend() const
  {
    REQUIRE(f.target->mem().write(0xE000'E280u, 4, 1u << 18).has_value());
    run_until(now() + 100 * ns);
  }

  [[nodiscard]] auto warnings() const -> std::ptrdiff_t
  {
    return std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind);
  }

  std::string                   firmware;
  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink =
      f.target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
};

auto without(part_description desc, std::string_view reg, std::string_view field = {})
    -> part_description
{
  auto adc =
      std::ranges::find(desc.peripherals, std::string_view{"ADC"}, &popcorn::peripheral_desc::name);
  REQUIRE(adc != desc.peripherals.end());
  auto found = std::ranges::find(adc->registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != adc->registers.end());
  if(field.empty())
  {
    adc->registers.erase(found);
  }
  else
  {
    auto bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    found->fields.erase(bits);
  }
  return desc;
}

auto with_cat3_fields(part_description desc) -> part_description
{
  auto const& donor = l152re();
  auto const  from  = std::ranges::find(
      donor.peripherals, std::string_view{"ADC"}, &popcorn::peripheral_desc::name);
  auto to =
      std::ranges::find(desc.peripherals, std::string_view{"ADC"}, &popcorn::peripheral_desc::name);
  REQUIRE(from != donor.peripherals.end());
  REQUIRE(to != desc.peripherals.end());
  auto const cr2_of = [](auto& p) {
    return std::ranges::find(p.registers, std::string_view{"CR2"}, &popcorn::register_desc::name);
  };
  cr2_of(*to)->fields = cr2_of(*from)->fields;
  return desc;
}
}

TEST_CASE("RM0038 12.15: the L1 ADC model serves SR to SMPR0, CSR and CCR on L152RE and RB")
{
  adc_rig const re;
  REQUIRE(re.f.target->model("ADC") != nullptr);
  CHECK(re.port().external_channels() == 0xFBFC'FFFF);
  CHECK(re.port().bonded_channels() == 0x003C'FFFF);
  CHECK(re.read(sr) == 0);
  CHECK(re.read(smpr0) == 0);

  adc_rig const rb{l152rb()};
  REQUIRE(rb.f.target->model("ADC") != nullptr);
  CHECK(rb.port().external_channels() == 0x03FC'FFFF);
  CHECK(rb.port().bonded_channels() == 0x003C'FFFF);
  auto const& desc = rb.f.target->peripheral("ADC").description();
  CHECK(popcorn::find_register(desc, "SMPR0") == nullptr);
  CHECK(re.logged.empty());
}

TEST_CASE("checklist 39: Cat.3 and up need SMPR0 and ADC_CFG, Cat.1 and Cat.2 refuse ADC_CFG")
{
  CHECK_THROWS_AS(adc_rig{without(l152re(), "SMPR0")}, popcorn::config_error);
  CHECK_THROWS_AS(adc_rig{without(l152re(), "CR2", "ADC_CFG")}, popcorn::config_error);
  CHECK_THROWS_AS(adc_rig{without(l152re(), "SQR1", "SQ28")}, popcorn::config_error);
  CHECK_THROWS_AS(adc_rig{with_cat3_fields(l152rb())}, popcorn::config_error);
}

TEST_CASE("RM0038 12.3.1: ADONS sets 3.5 us after the ADON edge, ADON 0 clears it at once")
{
  adc_rig const r;
  auto const    start = r.now();
  r.write(cr2, adon);
  CHECK(r.due() == start + 3'500 * ns);
  CHECK((r.read(sr) & adons) == 0);
  r.run_until(start + 3'500 * ns + us);
  CHECK((r.read(sr) & adons) != 0);
  CHECK((r.read(csr) & adons) != 0);
  r.write(cr2, 0);
  CHECK((r.read(sr) & adons) == 0);
}

TEST_CASE("without HSI16 the power-up waits for it and logs once")
{
  adc_rig const r{l152re(), false, false};
  r.write(cr2, adon);
  r.run_until(r.now() + 20 * us);
  CHECK((r.read(sr) & adons) == 0);
  CHECK(r.warnings() == 1);
  r.hsi_on();
  CHECK((r.read(sr) & adons) != 0);
  CHECK(r.warnings() == 1);
}

TEST_CASE("RM0038 12.3.4: SWSTART converts SQ1 in SMP + 12 cycles, STRT, EOC, SWSTART cleared")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(smpr3, 2u << 15);
  r.write(sqr5, 5);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  CHECK((r.read(cr2) & swstart) == 0);
  CHECK((r.read(sr) & strt) != 0);
  CHECK(r.due() == start + 28 * cycle);
  CHECK((r.read(sr) & eoc) == 0);
  r.run_until(start + 28 * cycle + us);
  CHECK((r.read(sr) & eoc) != 0);
  CHECK(r.read(dr) == 2048);
  CHECK((r.read(sr) & eoc) != 0);
  CHECK(r.cpu_read(dr) == 2048);
  CHECK((r.read(sr) & eoc) == 0);
}

TEST_CASE("RM0038 12.8: RES shortens TCONV to 11, 9 and 7 cycles and drops two bits per step")
{
  for(auto const& [res, cycles, code] : {
          std::tuple{0u, 16u, 2048u},
          std::tuple{1u, 15u, 512u },
          std::tuple{2u, 13u, 128u },
          std::tuple{3u, 11u, 32u  }
  })
  {
    CAPTURE(res);
    adc_rig const r;
    r.port().drive(5, 1'500'000);
    r.power();
    r.write(cr1, res << 24);
    r.write(sqr5, 5);
    auto const start = r.now();
    r.write(cr2, adon | swstart);
    CHECK(r.due() == start + cycles * cycle);
    CHECK((r.read(sr) & eoc) == 0);
    r.run_until(start + cycles * cycle + us);
    CHECK((r.read(sr) & eoc) != 0);
    CHECK(r.read(dr) == code);
  }
}

TEST_CASE("RM0038 12.5: each channel's SMP field of SMPR3 to SMPR0 sets its sampling time")
{
  for(auto const& [channel, reg, code, cycles] : {
          std::tuple{9u,  smpr3, 7u, 396u},
          std::tuple{10u, smpr2, 1u, 21u },
          std::tuple{25u, smpr1, 4u, 60u },
          std::tuple{31u, smpr0, 5u, 108u}
  })
  {
    CAPTURE(channel);
    adc_rig const r;
    r.power();
    r.write(reg, code << (3 * (channel % 10)));
    r.write(sqr5, channel);
    auto const start = r.now();
    r.write(cr2, adon | swstart);
    CHECK(r.due() == start + cycles * cycle);
    CHECK((r.read(sr) & eoc) == 0);
    r.run_until(start + cycles * cycle + us);
    CHECK((r.read(sr) & eoc) != 0);
  }
}

TEST_CASE("RM0038 12.4: ALIGN 1 puts a 12-bit code on bit 15, a 6-bit code on bit 7, inferred")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  CHECK(r.convert(5, align) == 2048u << 4);
  r.write(cr2, 0);
  r.write(cr1, 3u << 24);
  r.power();
  CHECK(r.convert(5, align) == 32u << 2);
}

TEST_CASE("RM0038 12.15.10 to 12.15.14: rank n reads SQn across SQR5 to SQR1, 28 ranks on Cat.3")
{
  adc_rig const r;
  for(std::uint8_t channel = 0; channel < 16; ++channel)
  {
    r.port().drive(channel, 100'000u * channel);
  }
  r.power();
  r.write(sqr5, (1u << 25) | (2u << 0));
  r.write(sqr4, 3u << 0);
  r.write(sqr3, (4u << 25) | (5u << 0));
  r.write(sqr2, (6u << 25) | (7u << 0));
  r.write(sqr1, (27u << 20) | (8u << 15) | (9u << 10) | (10u << 0));
  r.write(cr1, scan);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  std::vector<std::uint32_t> seen;
  for(std::uint64_t rank = 1; rank <= 28; ++rank)
  {
    r.run_until(start + rank * 16 * cycle + ns);
    seen.push_back(r.read(dr));
  }
  auto const code = [](std::uint32_t channel) { return 100'000u * channel * 4096u / 3'000'000u; };
  CHECK(seen[0] == code(2));
  CHECK(seen[5] == code(1));
  CHECK(seen[6] == code(3));
  CHECK(seen[12] == code(5));
  CHECK(seen[17] == code(4));
  CHECK(seen[18] == code(7));
  CHECK(seen[23] == code(6));
  CHECK(seen[24] == code(10));
  CHECK(seen[26] == code(9));
  CHECK(seen[27] == code(8));
  CHECK((r.read(sr) & eoc) != 0);
  CHECK(r.read(sr) == (adons | strt | eoc));
}

TEST_CASE("RM0038 12.3.8: EOCS 0 sets EOC at the end of the scan, EOCS 1 after each rank")
{
  for(auto const control : {0u, eocs})
  {
    CAPTURE(control);
    adc_rig const r;
    r.power();
    r.write(cr1, scan);
    r.write(sqr1, 2u << 20);
    auto const start = r.now();
    r.write(cr2, adon | control | swstart);
    r.run_until(start + 16 * cycle + ns);
    CHECK(((r.read(sr) & eoc) != 0) == (control != 0));
    static_cast<void>(r.cpu_read(dr));
    r.run_until(start + 32 * cycle + ns);
    CHECK(((r.read(sr) & eoc) != 0) == (control != 0));
    static_cast<void>(r.cpu_read(dr));
    r.run_until(start + 48 * cycle + ns);
    CHECK((r.read(sr) & eoc) != 0);
    r.run_until(start + 80 * cycle);
    CHECK((r.read(sr) & ovr) == 0);
  }
}

TEST_CASE(
    "RM0038 12.3.5: CONT starts the group over until ADON clears, a SWSTART meanwhile is ignored")
{
  adc_rig const r;
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | cont | swstart);
  r.run_until(start + 16 * cycle + ns);
  REQUIRE((r.read(sr) & eoc) != 0);
  r.write(sr, 0);
  r.write(cr2, adon | cont | swstart);
  r.run_until(start + 32 * cycle + ns);
  CHECK((r.read(sr) & eoc) != 0);
  r.write(sr, 0);
  r.write(cr2, 0);
  r.run_until(start + 64 * cycle);
  CHECK((r.read(sr) & (eoc | strt)) == 0);
}

TEST_CASE(
    "RM0038 12.11.4: with EOCS 1 an unread result makes the next one an overrun, OVRIE raises, "
    "the group goes on, inferred")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(cr1, ovrie);
  r.write(sqr5, 5);
  auto const start = r.now();
  r.write(cr2, adon | eocs | cont | swstart);
  r.run_until(start + 16 * cycle + ns);
  CHECK((r.read(sr) & ovr) == 0);
  CHECK_FALSE(r.pending());
  r.port().drive(5, 750'000);
  r.run_until(start + 32 * cycle + ns);
  CHECK((r.read(sr) & ovr) != 0);
  CHECK(r.read(dr) == 2048);
  CHECK(r.pending());
  r.write(sr, ~ovr);
  r.unpend();
  CHECK_FALSE(r.pending());
  CHECK(r.cpu_read(dr) == 2048);
  r.run_until(start + 48 * cycle + ns);
  CHECK(r.read(dr) == 1024);
  CHECK((r.read(sr) & ovr) == 0);
}

TEST_CASE("RM0038 12.11.3: with EOCS 0 and no DMA a result overwrites ADC_DR, no overrun")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(sqr5, 5);
  auto const start = r.now();
  r.write(cr2, adon | cont | swstart);
  r.run_until(start + 16 * cycle + ns);
  r.port().drive(5, 750'000);
  r.run_until(start + 48 * cycle + ns);
  CHECK(r.read(dr) == 1024);
  CHECK((r.read(sr) & ovr) == 0);
}

TEST_CASE("RM0038 12.15.1: SR flags clear on a write of 0, keep on 1, ADONS ignores the write")
{
  adc_rig const r;
  r.power();
  r.write(cr1, eocie);
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 2 * us);
  REQUIRE(r.read(sr) == (adons | strt | eoc));
  CHECK(r.pending());
  r.write(sr, eoc);
  CHECK(r.read(sr) == (adons | eoc));
  r.write(sr, 0);
  CHECK(r.read(sr) == adons);
  CHECK(r.read(csr) == adons);
  r.unpend();
  CHECK_FALSE(r.pending());
  r.write(sr, 0x3F);
  CHECK(r.read(sr) == adons);
}

TEST_CASE("RM0038 12.12: TSVREFE connects TS_CAL1 on 16 and VREFINT_CAL on 17, scaled by VDDA")
{
  adc_rig const r;
  r.power();
  CHECK(r.convert(16) == 0);
  CHECK(r.convert(17) == 0);
  r.write(ccr, tsvrefe);
  CHECK(r.convert(16) == ts_cal1);
  CHECK(r.convert(17) == vrefint);

  adc_rig const high{l152re(), false, true, {.vdda_mv = 3300}};
  high.power();
  high.write(ccr, tsvrefe);
  CHECK(high.convert(17) == vrefint * 3000 / 3300);
  CHECK(high.convert(16) == ts_cal1 * 3000 / 3300);
}

TEST_CASE("RM0038 12.3.2: ADCPRE divides HSI16 by 2 and 4, a conversion waits for HSI16")
{
  for(auto const& [code, divider] : {
          std::pair{1u, 2u},
          std::pair{2u, 4u}
  })
  {
    CAPTURE(code);
    adc_rig const r;
    r.write(ccr, code << 16);
    r.power();
    auto const start = r.now();
    r.write(cr2, adon | swstart);
    CHECK(r.due() == start + divider * 16 * cycle);
    CHECK((r.read(sr) & eoc) == 0);
    r.run_until(start + divider * 16 * cycle + us);
    CHECK((r.read(sr) & eoc) != 0);
  }
  adc_rig const r;
  r.power();
  auto const rcc = r.f.target->peripheral("RCC").description().base;
  REQUIRE(r.f.target->mem().write(rcc, 4, 0x0000'0300u).has_value());
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 20 * us);
  CHECK((r.read(sr) & eoc) == 0);
  CHECK((r.read(cr2) & swstart) != 0);
  r.hsi_on();
  CHECK((r.read(sr) & eoc) != 0);
  CHECK((r.read(cr2) & swstart) == 0);
}

TEST_CASE("a conversion holds its input at the end of the sampling phase")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.write(ccr, 2u << 16);
  r.power();
  r.write(smpr3, 7u << 15);
  r.write(sqr5, 5);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  REQUIRE(r.due() == start + 396 * 4 * cycle);
  r.run_until(start + 90 * us);
  r.port().drive(5, 750'000);
  r.run_until(start + 97'500 * ns);
  r.port().drive(5, 0);
  r.run_until(start + 100 * us);
  CHECK(r.cpu_read(dr) == 1024);
}

TEST_CASE("RM0038 12.7.2: an SQR write during a conversion drops it until the next SWSTART")
{
  adc_rig const r;
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | cont | swstart);
  r.run_until(start + 8 * cycle);
  r.write(sqr5, 3);
  r.run_until(start + 64 * cycle);
  CHECK((r.read(sr) & eoc) == 0);
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 17 * cycle);
  CHECK((r.read(sr) & eoc) != 0);
}

TEST_CASE("RM0038 12.3.3: bank B channels on a shared pad read it, others log as unmodelled")
{
  adc_rig const r;
  r.port().drive(13, 1'500'000);
  r.port().drive(0, 1'500'000);
  r.power();
  CHECK(r.convert(13, bank_b) == 2048);
  CHECK(r.convert(0) == 2048);
  CHECK(r.logged.empty());
  CHECK(r.convert(0, bank_b) == 0);
  CHECK(r.convert(0, bank_b) == 0);
  CHECK(r.warnings() == 1);

  adc_rig const strict{l152re(), true};
  strict.power();
  strict.write(sqr5, 0);
  CHECK_THROWS_AS(strict.attempt(cr2, adon | bank_b | swstart), popcorn::model_error);
  CHECK((strict.read(sr) & strt) == 0);
}

TEST_CASE("checklist 53: each function the L1 ADC does not model raises model_error when strict")
{
  struct row
  {
    std::uint32_t reg;
    std::uint32_t value;
    bool          at_start;
  };
  for(auto const& [reg, value, at_start] : {
          row{cr2,  adon | (1u << 22), false},
          row{cr2,  adon | (1u << 28), false},
          row{cr2,  adon | (1u << 20), false},
          row{cr2,  adon | (1u << 8),  true },
          row{cr2,  adon | (1u << 4),  true },
          row{cr1,  1u << 17,          true },
          row{cr1,  1u << 16,          true },
          row{cr1,  1u << 11,          true },
          row{cr1,  1u << 10,          true },
          row{cr1,  1u << 23,          true },
          row{sqr5, 26,                true },
          row{ccr,  3u << 16,          true },
  })
  {
    CAPTURE(reg);
    CAPTURE(value);
    adc_rig const lax;
    lax.power();
    if(at_start)
    {
      auto const control = reg == cr2 ? value : adon;
      lax.write(reg, value);
      lax.write(cr2, control | swstart);
      lax.run_until(lax.now() + 40 * us);
      CHECK((lax.read(sr) & eoc) != 0);
      lax.write(cr2, control | swstart);
      lax.run_until(lax.now() + 40 * us);
    }
    else
    {
      lax.write(reg, value);
      lax.write(cr2, adon);
      lax.write(reg, value);
    }
    CHECK(lax.warnings() == 1);

    adc_rig const strict{l152re(), true};
    strict.power();
    if(at_start)
    {
      strict.write(reg, value);
      CHECK_THROWS_AS(
          strict.attempt(cr2, (reg == cr2 ? value : adon) | swstart), popcorn::model_error);
      CHECK((strict.read(sr) & strt) == 0);
    }
    else
    {
      CHECK_THROWS_AS(strict.attempt(reg, value), popcorn::model_error);
    }
  }
}

TEST_CASE("SWSTART with ADONS clear is ignored and logs once, a model_error when strict")
{
  adc_rig const r;
  r.write(cr2, adon | swstart);
  CHECK((r.read(cr2) & swstart) == 0);
  r.run_until(r.now() + 40 * us);
  CHECK((r.read(sr) & (eoc | strt)) == 0);
  CHECK(r.warnings() == 1);

  adc_rig const strict{l152re(), true};
  CHECK_THROWS_AS(strict.attempt(cr2, adon | swstart), popcorn::model_error);
}

TEST_CASE("Cat.1 and Cat.2: L 27 converts 27 ranks and channel 27 reads 0, each logs once")
{
  adc_rig const r{l152rb()};
  r.port().drive(25, 1'500'000);
  r.power();
  CHECK(r.convert(25) == 2048);
  CHECK(r.convert(27) == 0);
  CHECK(r.warnings() == 1);
  r.write(cr1, scan);
  r.write(sqr1, 27u << 20);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  r.run_until(start + 26 * 16 * cycle + 500 * ns);
  CHECK(r.due() == start + 27 * 16 * cycle);
  CHECK((r.read(sr) & eoc) == 0);
  r.run_until(start + 27 * 16 * cycle + us);
  CHECK((r.read(sr) & eoc) != 0);
  CHECK(r.warnings() == 2);

  adc_rig const strict{l152rb(), true};
  strict.power();
  strict.write(cr1, scan);
  strict.write(sqr1, 27u << 20);
  CHECK_THROWS_AS(strict.attempt(cr2, adon | swstart), popcorn::model_error);
}

TEST_CASE("only the external channels take a drive")
{
  adc_rig const r;
  CHECK_THROWS_AS(r.port().drive(16, 1), popcorn::config_error);
  CHECK_THROWS_AS(r.port().drive(26, 1), popcorn::config_error);
  CHECK_NOTHROW(r.port().drive(31, 1));
  adc_rig const rb{l152rb()};
  CHECK_THROWS_AS(rb.port().drive(27, 1), popcorn::config_error);
}

TEST_CASE("a snapshot mid-conversion ends it at the same time with the same code")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(smpr3, 7u << 15);
  r.write(sqr5, 5);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  r.run_until(start + 100 * cycle);
  auto const saved = r.f.target->snapshot();
  auto const check = [&](adc_rig const& g) {
    CHECK(g.port().input(5) == 1'500'000);
    CHECK(g.due() == start + 396 * cycle);
    CHECK((g.read(sr) & eoc) == 0);
    g.run_until(start + 396 * cycle + us);
    CHECK((g.read(sr) & eoc) != 0);
    CHECK(g.read(dr) == 2048);
  };
  r.port().drive(5, 0);
  r.f.target->restore(saved);
  check(r);
  adc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(other);
}

TEST_CASE(
    "checklist 31: ADC1RST drops a pending conversion and power-up, a new start keeps its time")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(smpr3, 7u << 15);
  r.write(sqr5, 5);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  r.run_until(start + 100 * cycle);
  rcc_reset_pulse(r.f, "APB2RSTR", "ADC1RST");
  CHECK(r.read(cr2) == 0);
  CHECK(r.read(sr) == 0);
  r.write(cr2, adon);
  auto const again = r.now();
  r.run_until(start + 396 * cycle + ns);
  CHECK(r.read(sr) == adons);
  r.run_until(again + 3'500 * ns + ns);
  r.write(sqr5, 5);
  auto const second = r.now();
  r.write(cr2, adon | swstart);
  CHECK(r.due() == second + 16 * cycle);
  CHECK((r.read(sr) & eoc) == 0);
  r.run_until(second + 16 * cycle + us);
  CHECK((r.read(sr) & eoc) != 0);
  CHECK(r.read(dr) == 2048);

  adc_rig const pending;
  pending.write(cr2, adon);
  pending.run_until(pending.now() + us);
  rcc_reset_pulse(pending.f, "APB2RSTR", "ADC1RST");
  pending.run_until(pending.now() + 10 * us);
  CHECK(pending.read(sr) == 0);
}
