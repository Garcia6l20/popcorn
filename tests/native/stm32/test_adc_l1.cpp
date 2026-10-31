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

constexpr std::uint32_t base     = 0x4001'2400;
constexpr std::uint32_t sr       = 0x00;
constexpr std::uint32_t cr1      = 0x04;
constexpr std::uint32_t cr2      = 0x08;
constexpr std::uint32_t smpr1    = 0x0C;
constexpr std::uint32_t smpr2    = 0x10;
constexpr std::uint32_t smpr3    = 0x14;
constexpr std::uint32_t sqr1     = 0x30;
constexpr std::uint32_t sqr2     = 0x34;
constexpr std::uint32_t sqr3     = 0x38;
constexpr std::uint32_t sqr4     = 0x3C;
constexpr std::uint32_t sqr5     = 0x40;
constexpr std::uint32_t dr       = 0x58;
constexpr std::uint32_t smpr0    = 0x5C;
constexpr std::uint32_t jofr1    = 0x18;
constexpr std::uint32_t jsqr     = 0x44;
constexpr std::uint32_t jdr1     = 0x48;
constexpr std::uint32_t jswstart = 1u << 22;
constexpr std::uint32_t jdiscen  = 1u << 12;
constexpr std::uint32_t discen   = 1u << 11;
constexpr std::uint32_t jeocie   = 1u << 7;
constexpr std::uint32_t jeoc     = 1u << 2;
constexpr std::uint32_t jstrt    = 1u << 3;
constexpr std::uint32_t csr      = 0x300;
constexpr std::uint32_t ccr      = 0x304;
constexpr std::uint32_t adon     = 1u << 0;
constexpr std::uint32_t cont     = 1u << 1;
constexpr std::uint32_t bank_b   = 1u << 2;
constexpr std::uint32_t eocs     = 1u << 10;
constexpr std::uint32_t align    = 1u << 11;
constexpr std::uint32_t swstart  = 1u << 30;
constexpr std::uint32_t scan     = 1u << 8;
constexpr std::uint32_t eocie    = 1u << 5;
constexpr std::uint32_t ovrie    = 1u << 26;
constexpr std::uint32_t eoc      = 1u << 1;
constexpr std::uint32_t strt     = 1u << 4;
constexpr std::uint32_t ovr      = 1u << 5;
constexpr std::uint32_t adons    = 1u << 6;
constexpr std::uint32_t tsvrefe  = 1u << 23;
constexpr std::uint64_t ns       = 1'000;
constexpr std::uint64_t us       = 1'000'000;
constexpr std::uint64_t cycle    = 62'500;
constexpr std::uint32_t vrefint  = 1671;
constexpr std::uint32_t ts_cal1  = 680;

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

  [[nodiscard]] auto due(std::string_view unit = "ADC") const -> std::uint64_t
  {
    auto const& units = f.target->description().peripherals;
    auto const  owner = static_cast<std::uint32_t>(
        std::ranges::find(units, unit, &popcorn::peripheral_desc::name) - units.begin());
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

  void poke(std::string_view unit, std::uint32_t offset, std::uint32_t value) const
  {
    auto const at = f.target->peripheral(unit).description().base + offset;
    REQUIRE(f.target->mem().write(at, 4, value).has_value());
  }

  void timer_updates(
      std::string_view unit,
      std::uint32_t    enable_offset,
      std::uint32_t    enable,
      std::uint32_t    arr) const
  {
    poke("RCC", enable_offset, enable);
    poke(unit, 0x2C, arr);
    poke(unit, 0x04, 2u << 4);
    poke(unit, 0x00, 1);
  }

  void tim2_updates(std::uint32_t arr) const
  {
    timer_updates("TIM2", 0x24, 1, arr);
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
    r.write(cr1, res << 24);
    r.power();
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

TEST_CASE("RM0038 12.4: ALIGN 1 puts a 12-bit code on bit 15, a 6-bit code on bit 7, Figure 46")
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
  r.write(cr1, scan);
  r.power();
  r.write(sqr5, (1u << 25) | (2u << 0));
  r.write(sqr4, 3u << 0);
  r.write(sqr3, (4u << 25) | (5u << 0));
  r.write(sqr2, (6u << 25) | (7u << 0));
  r.write(sqr1, (27u << 20) | (8u << 15) | (9u << 10) | (10u << 0));
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
    r.write(cr1, scan);
    r.power();
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
    "RM0038 12.3.5: CONT starts the group over until ADON clears, a SWSTART meanwhile is "
    "ignored, inferred")
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

TEST_CASE(
    "RM0038 12.12: TSVREFE connects TS_CAL1 on 16 and VREFINT_CAL on 17, scaled by VDDA, 0 "
    "with TSVREFE clear, inferred")
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
          row{cr2,  adon | (1u << 28),               false},
          row{cr2,  adon | (1u << 8),                false},
          row{cr2,  adon | (1u << 28) | (11u << 24), false},
          row{cr1,  1u << 17,                        true },
          row{cr1,  1u << 16,                        true },
          row{cr1,  1u << 10,                        true },
          row{cr1,  1u << 23,                        false},
          row{cr1,  1u << 22,                        false},
          row{sqr5, 26,                              true },
          row{ccr,  3u << 16,                        true },
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
  r.write(cr1, scan);
  r.power();
  CHECK(r.convert(25) == 2048);
  CHECK(r.convert(27) == 0);
  CHECK(r.warnings() == 1);
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

TEST_CASE("LL ADC: a single conversion of ADC_IN5 at 1.5 V gives 2048, STRT, EOC read clear")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc, true, false, {}, "l1_adc_ll"};
    r.port().drive(5, 1'500'000);
    CHECK(r.run("single") == std::vector<std::uint32_t>{2048, adons | strt});
    CHECK(r.logged.empty());
  }
}

TEST_CASE("LL ADC: a scan of ADC_IN1, ADC_IN18 and ADC_IN5 with EOCS 1 reads each rank, no OVR")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc, true, false, {}, "l1_adc_ll"};
    r.port().drive(1, 750'000);
    r.port().drive(18, 1'500'000);
    r.port().drive(5, 2'250'000);
    CHECK(r.run("scan3") == std::vector<std::uint32_t>{1024, 2048, 3072, 0});
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "LL ADC: VREFINT gives 3000 mV and the sensor 30 C through the LL macros on both categories")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc, true, false, {}, "l1_adc_ll"};
    CHECK(r.run("factory") == std::vector<std::uint32_t>{vrefint, 3000, ts_cal1, 30});
    CHECK(r.logged.empty());
  }
}

TEST_CASE("DMA and AWDEN set during a continuous run raise model_error at the write when strict")
{
  adc_rig const strict{l152re(), true};
  strict.power();
  strict.write(cr2, adon | cont | swstart);
  strict.run_until(strict.now() + 3 * us);
  CHECK_THROWS_AS(strict.attempt(cr2, adon | cont | (1u << 8)), popcorn::model_error);
  CHECK_THROWS_AS(strict.attempt(cr1, 1u << 23), popcorn::model_error);
}

TEST_CASE(
    "a RES write during a conversion applies from the next one, an SMPR write keeps it running")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.write(smpr3, 2u << 15);
  r.write(sqr5, 5);
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | cont | swstart);
  r.run_until(start + 300 * ns);
  r.write(cr1, 3u << 24);
  CHECK(r.due() == start + 28 * cycle);
  r.run_until(start + 28 * cycle + 100 * ns);
  CHECK(r.read(dr) == 2048);
  CHECK(r.due() == start + 51 * cycle);
  r.write(smpr3, 7u << 15);
  CHECK(r.due() == start + 51 * cycle);
  r.run_until(start + 51 * cycle + 100 * ns);
  CHECK(r.read(dr) == 32);
}

TEST_CASE("ADON 0 while SWSTART waits for HSI16 clears SWSTART, no model_error when strict")
{
  adc_rig const r{l152re(), true};
  r.power();
  auto const rcc = r.f.target->peripheral("RCC").description().base;
  REQUIRE(r.f.target->mem().write(rcc, 4, 0x0000'0300u).has_value());
  r.write(cr2, adon | swstart);
  REQUIRE((r.read(cr2) & swstart) != 0);
  CHECK_NOTHROW(r.write(cr2, r.read(cr2) & ~adon));
  CHECK((r.read(cr2) & swstart) == 0);
}

TEST_CASE("from Cat.3 an L of 28 to 31 converts 28 ranks and logs once, a model_error when strict")
{
  adc_rig const r;
  r.write(cr1, scan);
  r.write(sqr1, 30u << 20);
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  CHECK(r.warnings() == 1);
  r.run_until(start + 27 * 16 * cycle + 500 * ns);
  CHECK(r.due() == start + 28 * 16 * cycle);

  adc_rig const strict{l152re(), true};
  strict.write(cr1, scan);
  strict.write(sqr1, 30u << 20);
  strict.power();
  CHECK_THROWS_AS(strict.attempt(cr2, adon | swstart), popcorn::model_error);
}

TEST_CASE("RM0038 12.6: EXTEN 01 with EXTSEL 0110 starts the group 3.5 ADC cycles after TIM2 TRGO")
{
  adc_rig const r;
  r.write(cr2, adon | (1u << 28) | (6u << 24));
  r.run_until(r.now() + 4 * us);
  REQUIRE((r.read(sr) & adons) != 0);
  r.tim2_updates(209);
  auto const update = r.due("TIM2");
  r.run_until(update + 600 * ns);
  CHECK((r.read(sr) & strt) != 0);
  CHECK(r.due() == update + 218'750 + 16 * cycle);
  r.run_until(update + 218'750 + 16 * cycle + ns);
  CHECK((r.read(sr) & eoc) != 0);
  for(int more = 0; more < 3; ++more)
  {
    static_cast<void>(r.cpu_read(dr));
    auto const next = r.due("TIM2");
    r.run_until(next + 2 * us);
    CHECK((r.read(sr) & eoc) != 0);
  }
  CHECK(r.logged.empty());
}

TEST_CASE("RM0038 12.6: EXTEN 10 starts at the TRGO falling edge, one timer clock after the rise")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (2u << 28) | (6u << 24));
  r.tim2_updates(209);
  auto const update = r.due("TIM2");
  r.run_until(update + 300 * ns);
  CHECK((r.read(sr) & strt) == 0);
  r.run_until(update + 476'837 + 600 * ns);
  auto const end = r.due();
  CHECK(end >= update + 476'836 + 218'750 + 16 * cycle);
  CHECK(end <= update + 476'838 + 218'750 + 16 * cycle);
}

TEST_CASE("RM0038 12.6: a TRGO edge during a conversion is ignored, the next one after it starts")
{
  adc_rig const r;
  r.power();
  r.write(smpr3, 7u);
  r.write(cr2, adon | (1u << 28) | (6u << 24));
  r.write(sr, 0);
  auto const start = r.now();
  r.tim2_updates(20);
  std::uint32_t conversions = 0;
  for(std::uint64_t step = 1; step <= 100; ++step)
  {
    r.run_until(start + step * us);
    if((r.read(sr) & eoc) != 0)
    {
      ++conversions;
      static_cast<void>(r.cpu_read(dr));
    }
  }
  CHECK(conversions == 3);
}

TEST_CASE("an EXTSEL no model produces logs once and starts nothing, a model_error when strict")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (1u << 28) | (2u << 24));
  r.tim2_updates(209);
  r.run_until(r.now() + 300 * us);
  CHECK((r.read(sr) & strt) == 0);
  REQUIRE(r.warnings() == 1);
  CHECK(r.logged.front().message.contains("tim2_cc3"));

  adc_rig const strict{l152re(), true};
  strict.power();
  CHECK_THROWS_AS(strict.attempt(cr2, adon | (1u << 28) | (2u << 24)), popcorn::model_error);
  CHECK((strict.read(cr2) >> 28) == 0);
}

TEST_CASE("ADC1RST stops the listening: a TRGO after the reset starts nothing")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (1u << 28) | (6u << 24));
  r.tim2_updates(209);
  rcc_reset_pulse(r.f, "APB2RSTR", "ADC1RST");
  r.run_until(r.now() + 300 * us);
  CHECK(r.read(sr) == 0);
}

TEST_CASE("RM0038 12.3.10: DISCEN with DISCNUM 1 converts 2, 2 and 1 ranks per SWSTART, then over")
{
  adc_rig const r;
  for(std::uint8_t channel = 0; channel < 6; ++channel)
  {
    r.port().drive(channel, 300'000u * (channel + 1));
  }
  r.power();
  r.write(cr1, scan | (1u << 11) | (1u << 13));
  r.write(sqr1, 4u << 20);
  r.write(sqr5, (1u << 0) | (2u << 5) | (3u << 10) | (4u << 15) | (5u << 20));
  auto const code = [](std::uint64_t channel) {
    return static_cast<std::uint32_t>(300'000u * (channel + 1) * 4096u / 3'000'000u);
  };
  std::vector<std::uint32_t> seen;
  std::vector<bool>          ends;
  for(int group = 0; group < 4; ++group)
  {
    r.write(cr2, adon | swstart);
    r.run_until(r.now() + 40 * us);
    seen.push_back(r.read(dr));
    ends.push_back((r.read(sr) & eoc) != 0);
    r.write(sr, 0);
  }
  CHECK(seen == std::vector<std::uint32_t>{code(2), code(4), code(5), code(2)});
  CHECK(ends == std::vector<bool>{false, false, true, false});
}

TEST_CASE(
    "DISCEN with CONT converts as DISCEN alone and logs once, a model_error when strict, inferred")
{
  adc_rig const r;
  r.power();
  r.write(cr1, scan | (1u << 11));
  r.write(sqr1, 2u << 20);
  r.write(cr2, adon | cont | swstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.warnings() == 1);
  CHECK((r.read(sr) & eoc) == 0);

  adc_rig const strict{l152re(), true};
  strict.power();
  strict.write(cr1, scan | (1u << 11));
  CHECK_THROWS_AS(strict.attempt(cr2, adon | cont | swstart), popcorn::model_error);
}

TEST_CASE("RM0038 12.9: DELS 001 holds the next conversion until ADC_DR is read or EOC cleared")
{
  for(bool by_read : {true, false})
  {
    CAPTURE(by_read);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | eocs | cont | (1u << 4) | swstart);
    r.run_until(r.now() + 2 * us);
    REQUIRE((r.read(sr) & eoc) != 0);
    r.write(sr, eoc);
    r.run_until(r.now() + 40 * us);
    CHECK((r.read(sr) & (strt | ovr)) == 0);
    auto const release = r.now();
    if(by_read)
    {
      static_cast<void>(r.cpu_read(dr));
    }
    else
    {
      r.write(sr, 0);
    }
    CHECK((r.read(sr) & strt) != 0);
    CHECK(r.due() == release + 16 * cycle);
  }
}

TEST_CASE(
    "RM0038 12.9: DELS 010 starts the next conversion 7 + 1 PCLK2 and 3 ADC cycles later, inferred")
{
  adc_rig const r;
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | cont | (2u << 4) | swstart);
  r.run_until(start + 16 * cycle + ns);
  REQUIRE((r.read(sr) & eoc) != 0);
  CHECK(r.due() == start + 16 * cycle + 3'814'698 + 3 * cycle);
}

TEST_CASE("RM0038 12.9: a SWSTART during the DELS delay is ignored, one after it starts")
{
  adc_rig const r;
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | (5u << 4) | swstart);
  r.run_until(start + 16 * cycle + ns);
  REQUIRE((r.read(sr) & eoc) != 0);
  r.write(sr, 0);
  r.write(cr2, adon | (5u << 4) | swstart);
  r.run_until(r.now() + 2 * us);
  CHECK((r.read(sr) & strt) == 0);
  r.run_until(start + 40 * us);
  r.write(cr2, adon | (5u << 4) | swstart);
  r.run_until(r.now() + 2 * us);
  CHECK((r.read(sr) & eoc) != 0);
}

TEST_CASE("LL ADC: TIM2 TRGO starts the three-rank scan at each update, twice, no OVR")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc, true, false, {}, "l1_adc_ll"};
    r.port().drive(1, 750'000);
    r.port().drive(18, 1'500'000);
    r.port().drive(5, 2'250'000);
    CHECK(r.run("triggered") == std::vector<std::uint32_t>{1024, 2048, 3072, 1024, 2048, 3072, 0});
    CHECK(r.logged.empty());
  }
}

TEST_CASE("EXTEN set after TIM2 wrapped lazily: no past TRGO starts the group, the next one does")
{
  adc_rig const r;
  r.power();
  r.tim2_updates(209);
  r.run_until(r.now() + 350 * us);
  r.write(cr2, adon | (1u << 28) | (6u << 24));
  r.run_until(r.now() + us);
  CHECK((r.read(sr) & (strt | eoc)) == 0);
  CHECK(r.due() == ~std::uint64_t{});
  r.run_until(r.now() + 101 * us);
  CHECK((r.read(sr) & eoc) != 0);
}

TEST_CASE("the ADONS event starts the listening: TRGO wraps from before it start nothing")
{
  adc_rig const r;
  r.write(cr2, (1u << 28) | (6u << 24));
  r.tim2_updates(41);
  r.run_until(r.now() + 100 * us);
  r.write(cr2, adon | (1u << 28) | (6u << 24));
  r.run_until(r.now() + 4'500 * ns);
  REQUIRE((r.read(sr) & adons) != 0);
  CHECK((r.read(sr) & (strt | eoc)) == 0);
  CHECK(r.due() == ~std::uint64_t{});
  r.run_until(r.now() + 21 * us);
  CHECK((r.read(sr) & eoc) != 0);
}

TEST_CASE(
    "an EXTSEL no model produces written with ADON 0 logs once at the ADONS event, "
    "a model_error when strict")
{
  adc_rig const r;
  r.write(cr2, (1u << 28) | (2u << 24));
  r.write(cr2, adon | (1u << 28) | (2u << 24));
  CHECK(r.warnings() == 0);
  r.run_until(r.now() + 4 * us);
  REQUIRE((r.read(sr) & adons) != 0);
  CHECK(r.warnings() == 1);

  adc_rig const strict{l152re(), true};
  strict.write(cr2, (1u << 28) | (2u << 24));
  strict.write(cr2, adon | (1u << 28) | (2u << 24));
  CHECK_THROWS_AS(
      strict.f.run_to_stop(
          {.max_instructions = icount{1'000'000}, .until_time = vtime{strict.now() + 4 * us}}),
      popcorn::model_error);
}

TEST_CASE("RM0038 Table 61: EXTSEL 0001, 0100, 1001 and 1010 start on TIM9, TIM3, TIM4, TIM6 TRGO")
{
  for(auto const& [selection, unit, enable_offset, enable] : {
          std::tuple{1u,  std::string_view{"TIM9"}, 0x20u, 1u << 2},
          std::tuple{4u,  std::string_view{"TIM3"}, 0x24u, 1u << 1},
          std::tuple{9u,  std::string_view{"TIM4"}, 0x24u, 1u << 2},
          std::tuple{10u, std::string_view{"TIM6"}, 0x24u, 1u << 4},
  })
  {
    CAPTURE(unit);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | (1u << 28) | (selection << 24));
    r.timer_updates(unit, enable_offset, enable, 209);
    auto const update = r.due(unit);
    r.run_until(update + 600 * ns);
    CHECK((r.read(sr) & strt) != 0);
    CHECK(r.due() == update + 218'750 + 16 * cycle);
    CHECK(r.logged.empty());
  }
}

TEST_CASE("RM0038 Table 60: EXTEN 11 starts at both TRGO edges of TIM2 MMS 001, EXTEN 01 at one")
{
  for(auto const& [polarity, starts] : {
          std::pair{3u, 2},
          std::pair{1u, 1}
  })
  {
    CAPTURE(polarity);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | (polarity << 28) | (6u << 24));
    r.poke("RCC", 0x24, 1);
    r.poke("TIM2", 0x04, 1u << 4);
    int seen = 0;
    for(auto const control : {1u, 0u})
    {
      r.poke("TIM2", 0x00, control);
      r.run_until(r.now() + 2 * us);
      if((r.read(sr) & eoc) != 0)
      {
        ++seen;
        static_cast<void>(r.cpu_read(dr));
      }
    }
    CHECK(seen == starts);
  }
}

TEST_CASE("RM0038 12.9: DELS 001 with EOCS 1 holds each rank of a scan until its data is read")
{
  adc_rig const r;
  r.power();
  r.write(cr1, scan);
  r.write(sqr1, 1u << 20);
  auto const start = r.now();
  r.write(cr2, adon | eocs | (1u << 4) | swstart);
  r.run_until(start + 16 * cycle + ns);
  REQUIRE((r.read(sr) & eoc) != 0);
  r.run_until(start + 40 * us);
  CHECK(r.due() == ~std::uint64_t{});
  auto const release = r.now();
  static_cast<void>(r.cpu_read(dr));
  CHECK(r.due() == release + 16 * cycle);
  r.run_until(release + 16 * cycle + ns);
  CHECK((r.read(sr) & eoc) != 0);
  static_cast<void>(r.cpu_read(dr));
  CHECK((r.read(sr) & ovr) == 0);
}

TEST_CASE("DELS 001 with EOCS 0 converts a scan back to back and holds after its EOC, inferred")
{
  adc_rig const r;
  r.power();
  r.write(cr1, scan);
  r.write(sqr1, 1u << 20);
  auto const start = r.now();
  r.write(cr2, adon | (1u << 4) | swstart);
  r.run_until(start + 16 * cycle + ns);
  CHECK((r.read(sr) & eoc) == 0);
  CHECK(r.due() == start + 32 * cycle);
  r.run_until(start + 32 * cycle + ns);
  CHECK((r.read(sr) & eoc) != 0);
  r.write(sr, ~strt);
  r.write(cr2, adon | (1u << 4) | swstart);
  CHECK(r.due() == ~std::uint64_t{});
  r.run_until(r.now() + 40 * us);
  CHECK((r.read(sr) & strt) == 0);
  CHECK((r.read(cr2) & swstart) == 0);
  static_cast<void>(r.cpu_read(dr));
  auto const again = r.now();
  r.write(cr2, adon | (1u << 4) | swstart);
  CHECK(r.due() == again + 16 * cycle);
}

namespace
{
auto code_of(std::uint64_t microvolts) -> std::uint32_t
{
  return static_cast<std::uint32_t>(microvolts * 4096u / 3'000'000u);
}

auto jdr(std::uint32_t rank) -> std::uint32_t
{
  return jdr1 + 4u * rank;
}
}

TEST_CASE("RM0038 12.3.9: JSWSTART converts JSQ4 with JL 0 into JDR1, JSTRT, then JEOC and JEOCIE")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(cr1, jeocie);
  r.write(jsqr, 5u << 15);
  auto const start = r.now();
  r.write(cr2, adon | jswstart);
  CHECK((r.read(cr2) & jswstart) == 0);
  CHECK(r.read(sr) == (adons | jstrt));
  CHECK(r.due() == start + 16 * cycle);
  CHECK_FALSE(r.pending());
  r.run_until(start + 16 * cycle + ns);
  CHECK(r.read(sr) == (adons | jstrt | jeoc));
  CHECK(r.read(jdr1) == 2048);
  CHECK(r.cpu_read(jdr1) == 2048);
  CHECK(r.read(dr) == 0);
  CHECK(r.pending());
  r.write(sr, ~jeoc);
  r.unpend();
  CHECK_FALSE(r.pending());
  CHECK(r.logged.empty());
}

TEST_CASE(
    "RM0038 12.15.15: JL 3 converts JSQ1 to JSQ4 and JL 1 JSQ3 and JSQ4, into JDR1 onward, "
    "inferred, JEOC after the group")
{
  adc_rig const r;
  for(std::uint8_t channel = 1; channel <= 4; ++channel)
  {
    r.port().drive(channel, 300'000u * channel);
  }
  r.write(cr1, scan);
  r.power();
  r.write(jsqr, (3u << 20) | 1u | (2u << 5) | (3u << 10) | (4u << 15));
  auto const start = r.now();
  r.write(cr2, adon | jswstart);
  r.run_until(start + 48 * cycle + ns);
  CHECK((r.read(sr) & jeoc) == 0);
  CHECK(r.due() == start + 64 * cycle);
  r.run_until(start + 64 * cycle + ns);
  CHECK((r.read(sr) & jeoc) != 0);
  for(std::uint32_t rank = 0; rank < 4; ++rank)
  {
    CAPTURE(rank);
    CHECK(r.read(jdr(rank)) == code_of(300'000u * (rank + 1)));
  }
  r.write(sr, 0);
  r.write(jsqr, (1u << 20) | 9u | (9u << 5) | (3u << 10) | (4u << 15));
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(jdr(0)) == code_of(900'000));
  CHECK(r.read(jdr(1)) == code_of(1'200'000));
  CHECK(r.read(jdr(2)) == code_of(900'000));
}

TEST_CASE("with SCAN 0 the injected group is one conversion of JSQ(4 - JL) into JDR1, inferred")
{
  for(auto const& [length, channel] : {
          std::pair{3u, 1u},
          std::pair{1u, 3u},
          std::pair{0u, 4u}
  })
  {
    CAPTURE(length);
    adc_rig const r;
    for(std::uint8_t pad = 1; pad <= 4; ++pad)
    {
      r.port().drive(pad, 300'000u * pad);
    }
    r.power();
    r.write(jsqr, (length << 20) | 1u | (2u << 5) | (3u << 10) | (4u << 15));
    auto const start = r.now();
    r.write(cr2, adon | jswstart);
    CHECK(r.due() == start + 16 * cycle);
    r.run_until(start + 16 * cycle + ns);
    CHECK((r.read(sr) & jeoc) != 0);
    CHECK(r.read(jdr1) == code_of(300'000u * channel));
    CHECK(r.read(jdr(1)) == 0);
  }
}

TEST_CASE(
    "RM0038 12.4: JOFRk subtracts from the k-th injected code, sign-extended right-aligned, "
    "SEXT on bit 15 left-aligned, inferred rank order")
{
  for(auto const& [offset, control, expected] : {
          std::tuple{1024u, 0u,    1024u  },
          std::tuple{3072u, 0u,    0xFC00u},
          std::tuple{1024u, align, 8192u  },
          std::tuple{3072u, align, 0xE000u},
  })
  {
    CAPTURE(offset);
    CAPTURE(control);
    adc_rig const r;
    r.port().drive(5, 1'500'000);
    r.power();
    r.write(jsqr, 5u << 15);
    r.write(jofr1, offset);
    r.write(cr2, adon | control | jswstart);
    r.run_until(r.now() + 40 * us);
    CHECK(r.read(jdr1) == expected);
  }
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.write(cr1, scan);
  r.power();
  r.write(jsqr, (1u << 20) | (5u << 10) | (5u << 15));
  r.write(jofr1, 1000);
  r.write(jofr1 + 12, 2000);
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(jdr(0)) == 1048);
  CHECK(r.read(jdr(1)) == 2048);
}

TEST_CASE(
    "below 12 bits JOFFSET counts from bit 11 and the injected result keeps the regular "
    "alignment less one bit, inferred")
{
  for(auto const& [res, offset, control, expected] : {
          std::tuple{3u, 0xC3Fu, 0u,    0xFFF0u},
          std::tuple{3u, 0xC3Fu, align, 0xFFE0u},
          std::tuple{2u, 0x40Fu, 0u,    64u    },
          std::tuple{2u, 0x40Fu, align, 8192u  },
  })
  {
    CAPTURE(res);
    CAPTURE(control);
    adc_rig const r;
    r.port().drive(5, 1'500'000);
    r.write(cr1, res << 24);
    r.power();
    r.write(jsqr, 5u << 15);
    r.write(jofr1, offset);
    r.write(cr2, adon | control | jswstart);
    r.run_until(r.now() + 40 * us);
    CHECK(r.read(jdr1) == expected);
  }
}

TEST_CASE("an injected conversion holds its input at the end of its sampling phase")
{
  adc_rig const r;
  r.port().drive(9, 1'500'000);
  r.power();
  r.write(smpr3, 7u << 27);
  r.write(jsqr, 9u << 15);
  auto const start = r.now();
  r.write(cr2, adon | jswstart);
  REQUIRE(r.due() == start + 396 * cycle);
  r.run_until(start + 23 * us);
  r.port().drive(9, 750'000);
  r.run_until(start + 24'050 * ns);
  REQUIRE(r.due() == start + 396 * cycle);
  r.port().drive(9, 0);
  r.run_until(start + 396 * cycle + ns);
  CHECK(r.read(jdr1) == 1024);
}

TEST_CASE(
    "RM0038 12.3.9: a JSWSTART during a regular conversion drops it, that rank converts again "
    "after the injected group")
{
  adc_rig const r;
  r.port().drive(1, 750'000);
  r.port().drive(2, 1'500'000);
  r.port().drive(5, 2'250'000);
  r.write(cr1, scan);
  r.power();
  r.write(smpr3, 7u << 3);
  r.write(sqr1, 1u << 20);
  r.write(sqr5, 2u | (1u << 5));
  r.write(jsqr, 5u << 15);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  r.run_until(start + 16 * cycle + ns);
  CHECK(r.read(dr) == 2048);
  r.run_until(start + 100 * cycle);
  auto const injected = r.now();
  r.write(cr2, adon | jswstart);
  CHECK(r.due() == injected + 16 * cycle);
  r.run_until(injected + 16 * cycle + ns);
  CHECK((r.read(sr) & (jeoc | eoc)) == jeoc);
  CHECK(r.read(jdr1) == 3072);
  r.run_until(start + 420 * cycle);
  CHECK((r.read(sr) & eoc) == 0);
  CHECK(r.due() == injected + 16 * cycle + 396 * cycle);
  r.run_until(injected + 412 * cycle + ns);
  CHECK(r.read(dr) == 1024);
  CHECK((r.read(sr) & eoc) != 0);
  CHECK(r.due() == ~std::uint64_t{});
}

TEST_CASE(
    "RM0038 12.3.9: a SWSTART during an injected conversion starts the regular group after it")
{
  adc_rig const r;
  r.power();
  r.write(smpr3, 7u << 27);
  r.write(jsqr, 9u << 15);
  auto const start = r.now();
  r.write(cr2, adon | jswstart);
  r.run_until(start + 10 * cycle);
  r.write(cr2, adon | swstart);
  CHECK((r.read(cr2) & swstart) != 0);
  CHECK((r.read(sr) & strt) == 0);
  CHECK(r.due() == start + 396 * cycle);
  r.run_until(start + 396 * cycle + ns);
  CHECK((r.read(sr) & (jeoc | strt)) == (jeoc | strt));
  CHECK((r.read(cr2) & swstart) == 0);
  CHECK(r.due() == start + 412 * cycle);
  r.run_until(start + 412 * cycle + ns);
  CHECK((r.read(sr) & eoc) != 0);
}

TEST_CASE("a JSWSTART during an injected conversion is cleared and ignored, inferred")
{
  adc_rig const r;
  r.power();
  r.write(smpr3, 7u << 27);
  r.write(jsqr, 9u << 15);
  auto const start = r.now();
  r.write(cr2, adon | jswstart);
  r.run_until(start + 10 * cycle);
  r.write(cr2, adon | jswstart);
  CHECK((r.read(cr2) & jswstart) == 0);
  CHECK(r.due() == start + 396 * cycle);
  r.run_until(start + 396 * cycle + ns);
  CHECK((r.read(sr) & jeoc) != 0);
  CHECK(r.due() == ~std::uint64_t{});
}

TEST_CASE(
    "RM0038 12.7.1: a JSQR write during the injected group drops it and resumes the interrupted "
    "regular conversion")
{
  adc_rig const r;
  r.port().drive(1, 750'000);
  r.port().drive(9, 1'500'000);
  r.power();
  r.write(smpr3, (7u << 3) | (7u << 27));
  r.write(sqr5, 1);
  r.write(jsqr, 9u << 15);
  auto const start = r.now();
  r.write(cr2, adon | swstart);
  r.run_until(start + 100 * cycle);
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 10 * cycle);
  auto const dropped = r.now();
  r.write(jsqr, 9u << 15);
  r.run_until(start + 500 * cycle);
  CHECK((r.read(sr) & (eoc | jeoc)) == 0);
  CHECK(r.due() == dropped + 396 * cycle);
  r.run_until(dropped + 396 * cycle + ns);
  CHECK((r.read(sr) & (eoc | jeoc)) == eoc);
  CHECK(r.read(dr) == 1024);
  CHECK(r.read(jdr1) == 0);
}

TEST_CASE(
    "RM0038 12.6: JEXTEN 01 with JEXTSEL 0010 starts the injected group tlatr after TIM2 "
    "TRGO, inferred")
{
  adc_rig const r;
  r.power();
  r.write(jsqr, 5u << 15);
  r.port().drive(5, 1'500'000);
  r.write(cr2, adon | (1u << 20) | (2u << 16));
  r.tim2_updates(209);
  auto const update = r.due("TIM2");
  r.run_until(update + 600 * ns);
  CHECK((r.read(sr) & (jstrt | strt)) == jstrt);
  CHECK(r.due() == update + 218'750 + 16 * cycle);
  r.run_until(update + 218'750 + 16 * cycle + ns);
  CHECK((r.read(sr) & jeoc) != 0);
  CHECK(r.read(jdr1) == 2048);
  CHECK(r.logged.empty());
}

TEST_CASE("RM0038 12.6: JEXTEN 10 starts the injected group at the TRGO falling edge")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (2u << 20) | (2u << 16));
  r.tim2_updates(209);
  auto const update = r.due("TIM2");
  r.run_until(update + 300 * ns);
  CHECK((r.read(sr) & jstrt) == 0);
  r.run_until(update + 476'837 + 600 * ns);
  auto const end = r.due();
  CHECK(end >= update + 476'836 + 218'750 + 16 * cycle);
  CHECK(end <= update + 476'838 + 218'750 + 16 * cycle);
}

TEST_CASE("RM0038 Table 62: JEXTSEL 0001, 0101 and 1010 start on TIM9, TIM4 and TIM7 TRGO")
{
  for(auto const& [selection, unit, enable_offset, enable] : {
          std::tuple{1u,  std::string_view{"TIM9"}, 0x20u, 1u << 2},
          std::tuple{5u,  std::string_view{"TIM4"}, 0x24u, 1u << 2},
          std::tuple{10u, std::string_view{"TIM7"}, 0x24u, 1u << 5},
  })
  {
    CAPTURE(unit);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | (1u << 20) | (selection << 16));
    r.timer_updates(unit, enable_offset, enable, 209);
    auto const update = r.due(unit);
    r.run_until(update + 600 * ns);
    CHECK((r.read(sr) & jstrt) != 0);
    CHECK(r.due() == update + 218'750 + 16 * cycle);
    CHECK(r.logged.empty());
  }
}

TEST_CASE("one TRGO edge both groups select converts the injected group, then the regular group")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (1u << 28) | (6u << 24) | (1u << 20) | (2u << 16));
  r.tim2_updates(209);
  auto const update = r.due("TIM2");
  auto const begin  = update + 218'750;
  r.run_until(update + 600 * ns);
  CHECK((r.read(sr) & (jstrt | strt)) == jstrt);
  CHECK(r.due() == begin + 16 * cycle);
  r.run_until(begin + 16 * cycle + ns);
  CHECK((r.read(sr) & (jeoc | eoc | strt)) == (jeoc | strt));
  CHECK(r.due() == begin + 32 * cycle);
  r.run_until(begin + 32 * cycle + ns);
  CHECK((r.read(sr) & eoc) != 0);
}

TEST_CASE(
    "RM0038 12.9: a JSWSTART during a regular DELS delay starts at once, the regular group "
    "resumes when that delay ends")
{
  adc_rig const r;
  r.power();
  r.write(jsqr, 5u << 15);
  auto const start = r.now();
  r.write(cr2, adon | cont | (5u << 4) | swstart);
  r.run_until(start + 16 * cycle + ns);
  REQUIRE((r.read(sr) & eoc) != 0);
  auto const resume = start + 16 * cycle + 30'517'579 + 3 * cycle;
  REQUIRE(r.due() == resume);
  r.run_until(r.now() + us);
  auto const injected = r.now();
  r.write(cr2, adon | cont | (5u << 4) | jswstart);
  CHECK(r.due() == injected + 16 * cycle);
  r.run_until(injected + 16 * cycle + ns);
  CHECK((r.read(sr) & jeoc) != 0);
  CHECK(r.due() == resume);
}

TEST_CASE(
    "RM0038 12.9: DELS 001 holds the next injected group until JEOC clears, "
    "not the regular group")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (1u << 4) | jswstart);
  r.run_until(r.now() + 2 * us);
  REQUIRE((r.read(sr) & jeoc) != 0);
  r.write(cr2, adon | (1u << 4) | jswstart);
  CHECK((r.read(cr2) & jswstart) == 0);
  CHECK(r.due() == ~std::uint64_t{});
  auto const regular = r.now();
  r.write(cr2, adon | (1u << 4) | swstart);
  CHECK(r.due() == regular + 16 * cycle);
  r.run_until(r.now() + 2 * us);
  r.write(sr, ~jeoc);
  auto const release = r.now();
  r.write(cr2, adon | (1u << 4) | jswstart);
  CHECK(r.due() == release + 16 * cycle);
}

TEST_CASE(
    "RM0038 12.9: DELS 010 delays the next injected group 7 + 1 PCLK2 and 3 ADC cycles, inferred")
{
  adc_rig const r;
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | (2u << 4) | jswstart);
  r.run_until(start + 16 * cycle + us);
  REQUIRE((r.read(sr) & jeoc) != 0);
  r.write(cr2, adon | (2u << 4) | jswstart);
  CHECK(r.due() == ~std::uint64_t{});
  r.run_until(start + 16 * cycle + 3'814'698 + 3 * cycle + ns);
  auto const again = r.now();
  r.write(cr2, adon | (2u << 4) | jswstart);
  CHECK(r.due() == again + 16 * cycle);
}

TEST_CASE(
    "RM0038 12.3.10: JDISCEN with DISCNUM 1 converts 2, then 1 injected conversions per JSWSTART, "
    "JEOC alone after the last, inferred, then over")
{
  adc_rig const r;
  for(std::uint8_t channel = 2; channel <= 4; ++channel)
  {
    r.port().drive(channel, 300'000u * channel);
  }
  r.write(cr1, scan | jdiscen | (1u << 13));
  r.power();
  r.write(jsqr, (2u << 20) | (2u << 5) | (3u << 10) | (4u << 15));
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(jdr(0)) == code_of(600'000));
  CHECK(r.read(jdr(1)) == code_of(900'000));
  CHECK(r.read(jdr(2)) == 0);
  CHECK((r.read(sr) & jeoc) == 0);
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(jdr(2)) == code_of(1'200'000));
  CHECK((r.read(sr) & jeoc) != 0);
  r.write(sr, 0);
  r.port().drive(2, 0);
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(jdr(0)) == 0);
  CHECK((r.read(sr) & jeoc) == 0);
}

TEST_CASE("DISCEN with JDISCEN logs once at the injected start, a model_error when strict")
{
  adc_rig const lax;
  lax.write(cr1, discen | jdiscen);
  lax.power();
  lax.write(cr2, adon | jswstart);
  lax.run_until(lax.now() + 40 * us);
  CHECK((lax.read(sr) & jeoc) != 0);
  lax.write(cr2, adon | jswstart);
  CHECK(lax.warnings() == 1);

  adc_rig const strict{l152re(), true};
  strict.write(cr1, discen | jdiscen);
  strict.power();
  CHECK_THROWS_AS(strict.attempt(cr2, adon | jswstart), popcorn::model_error);
  CHECK((strict.read(sr) & jstrt) == 0);
}

TEST_CASE(
    "a JEXTSEL no model produces or reserved logs once and starts nothing, "
    "a model_error when strict")
{
  for(auto const& [selection, named] : {
          std::pair{0u,  std::string_view{"tim9_cc1"}},
          std::pair{15u, std::string_view{"exti15"}  },
          std::pair{11u, std::string_view{"reserved"}},
  })
  {
    CAPTURE(selection);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | (1u << 20) | (selection << 16));
    r.write(cr2, adon | (1u << 20) | (selection << 16));
    r.run_until(r.now() + 40 * us);
    CHECK((r.read(sr) & jstrt) == 0);
    REQUIRE(r.warnings() == 1);
    CHECK(r.logged.front().message.contains(named));

    adc_rig const strict{l152re(), true};
    strict.power();
    CHECK_THROWS_AS(
        strict.attempt(cr2, adon | (1u << 20) | (selection << 16)), popcorn::model_error);
    CHECK(((strict.read(cr2) >> 20) & 3u) == 0);
  }
}

TEST_CASE(
    "JEXTEN set after TIM2 wrapped lazily: no past TRGO starts the injected group, the next does")
{
  adc_rig const r;
  r.power();
  r.tim2_updates(209);
  r.run_until(r.now() + 350 * us);
  r.write(cr2, adon | (1u << 20) | (2u << 16));
  r.run_until(r.now() + us);
  CHECK((r.read(sr) & (jstrt | jeoc)) == 0);
  CHECK(r.due() == ~std::uint64_t{});
  r.run_until(r.now() + 101 * us);
  CHECK((r.read(sr) & jeoc) != 0);
}

TEST_CASE("JSWSTART with ADONS clear is ignored and logs once, a model_error when strict")
{
  adc_rig const r;
  r.write(cr2, adon | jswstart);
  CHECK((r.read(cr2) & jswstart) == 0);
  r.run_until(r.now() + 40 * us);
  CHECK((r.read(sr) & (jeoc | jstrt)) == 0);
  CHECK(r.warnings() == 1);

  adc_rig const strict{l152re(), true};
  CHECK_THROWS_AS(strict.attempt(cr2, adon | jswstart), popcorn::model_error);
}

TEST_CASE("JSWSTART waits for HSI16 set, ADON 0 clears it, no model_error when strict")
{
  adc_rig const r{l152re(), true};
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(jsqr, 5u << 15);
  auto const rcc = r.f.target->peripheral("RCC").description().base;
  REQUIRE(r.f.target->mem().write(rcc, 4, 0x0000'0300u).has_value());
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 20 * us);
  CHECK((r.read(cr2) & jswstart) != 0);
  CHECK((r.read(sr) & jstrt) == 0);
  r.hsi_on();
  CHECK((r.read(sr) & jeoc) != 0);
  CHECK(r.read(jdr1) == 2048);
  CHECK((r.read(cr2) & jswstart) == 0);
  REQUIRE(r.f.target->mem().write(rcc, 4, 0x0000'0300u).has_value());
  r.write(cr2, adon | jswstart);
  REQUIRE((r.read(cr2) & jswstart) != 0);
  CHECK_NOTHROW(r.write(cr2, r.read(cr2) & ~adon));
  CHECK((r.read(cr2) & jswstart) == 0);
}

TEST_CASE(
    "checklist 31: ADC1RST drops an injection over a regular conversion, a SWSTART after it "
    "starts at once")
{
  adc_rig const r;
  r.power();
  r.write(smpr3, (7u << 3) | (7u << 27));
  r.write(sqr5, 1);
  r.write(jsqr, 9u << 15);
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 100 * cycle);
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 10 * cycle);
  rcc_reset_pulse(r.f, "APB2RSTR", "ADC1RST");
  CHECK(r.read(jsqr) == 0);
  r.power();
  auto const regular = r.now();
  r.write(cr2, adon | swstart);
  CHECK((r.read(sr) & strt) != 0);
  CHECK(r.due() == regular + 16 * cycle);
  r.run_until(regular + 16 * cycle + ns);
  CHECK((r.read(sr) & eoc) != 0);
  auto const injected = r.now();
  r.write(cr2, adon | jswstart);
  CHECK(r.due() == injected + 16 * cycle);
  r.run_until(injected + 16 * cycle + ns);
  CHECK((r.read(sr) & jeoc) != 0);
}

TEST_CASE("a snapshot during an injection over a regular conversion ends both at the same times")
{
  adc_rig const r;
  r.port().drive(1, 750'000);
  r.port().drive(9, 1'500'000);
  r.power();
  r.write(smpr3, (7u << 3) | (7u << 27));
  r.write(sqr5, 1);
  r.write(jsqr, 9u << 15);
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 100 * cycle);
  auto const injected = r.now();
  r.write(cr2, adon | jswstart);
  r.run_until(injected + 100 * cycle);
  auto const saved = r.f.target->snapshot();
  auto const check = [&](adc_rig const& g) {
    g.run_until(injected + 300 * cycle);
    CHECK(g.due() == injected + 396 * cycle);
    g.run_until(injected + 396 * cycle + ns);
    CHECK((g.read(sr) & (jeoc | eoc)) == jeoc);
    CHECK(g.read(jdr1) == 2048);
    CHECK(g.due() == injected + 792 * cycle);
    g.run_until(injected + 792 * cycle + ns);
    CHECK((g.read(sr) & eoc) != 0);
    CHECK(g.read(dr) == 1024);
  };
  r.run_until(injected + 500 * cycle);
  r.f.target->restore(saved);
  check(r);
  adc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(other);
}

TEST_CASE(
    "an injected bank B channel on a pad of its own reads 0 and logs once, "
    "a model_error when strict")
{
  adc_rig const r;
  r.port().drive(0, 1'500'000);
  r.power();
  r.write(cr2, adon | bank_b);
  r.write(cr2, adon | bank_b | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK((r.read(sr) & jeoc) != 0);
  CHECK(r.read(jdr1) == 0);
  CHECK(r.warnings() == 1);

  adc_rig const strict{l152re(), true};
  strict.power();
  strict.write(cr2, adon | bank_b);
  CHECK_THROWS_AS(strict.attempt(cr2, adon | bank_b | jswstart), popcorn::model_error);
  CHECK((strict.read(sr) & jstrt) == 0);
}
