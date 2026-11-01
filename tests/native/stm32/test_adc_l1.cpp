#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
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
constexpr std::uint32_t htr      = 0x28;
constexpr std::uint32_t ltr      = 0x2C;
constexpr std::uint32_t awd      = 1u << 0;
constexpr std::uint32_t awdie    = 1u << 6;
constexpr std::uint32_t awdsgl   = 1u << 9;
constexpr std::uint32_t jawden   = 1u << 22;
constexpr std::uint32_t awden    = 1u << 23;
constexpr std::uint32_t dma      = 1u << 8;
constexpr std::uint32_t dds      = 1u << 9;
constexpr std::uint32_t buffer   = 0x2000'2000;
constexpr std::uint32_t dma_en   = 1u << 0;
constexpr std::uint32_t dma_circ = 1u << 5;
constexpr std::uint32_t dma_half = (1u << 7) | (1u << 8) | (1u << 10);
constexpr std::uint32_t jsqr     = 0x44;
constexpr std::uint32_t jdr1     = 0x48;
constexpr std::uint32_t jswstart = 1u << 22;
constexpr std::uint32_t jauto    = 1u << 10;
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

auto without_unit(part_description desc, std::string_view unit) -> part_description
{
  REQUIRE(
      std::erase_if(
          desc.peripherals, [&](popcorn::peripheral_desc const& p) { return p.name == unit; })
      == 1);
  return desc;
}

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

  void dma_channel1(std::uint32_t items, std::uint32_t control) const
  {
    poke("RCC", 0x1C, 1u << 24);
    poke("DMA1", 0x08, 0);
    poke("DMA1", 0x10, base + dr);
    poke("DMA1", 0x14, buffer);
    poke("DMA1", 0x0C, items);
    poke("DMA1", 0x08, control);
  }

  [[nodiscard]] auto memory(std::uint32_t index) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(buffer + 2 * index, 2);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto dma1(std::uint32_t offset) const -> std::uint32_t
  {
    auto const at    = f.target->peripheral("DMA1").description().base + offset;
    auto const value = f.target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void timer_compares(
      std::string_view unit,
      std::uint32_t    enable_offset,
      std::uint32_t    enable,
      std::uint32_t    channel,
      std::uint32_t    ccr) const
  {
    poke("RCC", enable_offset, enable);
    poke(unit, 0x2C, 209);
    poke(unit, 0x34 + 4 * (channel - 1), ccr);
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
  CHECK(re.port().external_channels(stm32::analog_bank::a) == 0xFBFC'FFFF);
  CHECK(re.port().bonded_channels(stm32::analog_bank::a) == 0x003C'FFFF);
  CHECK(re.read(sr) == 0);
  CHECK(re.read(smpr0) == 0);

  adc_rig const rb{l152rb()};
  REQUIRE(rb.f.target->model("ADC") != nullptr);
  CHECK(rb.port().external_channels(stm32::analog_bank::a) == 0x03FC'FFFF);
  CHECK(rb.port().bonded_channels(stm32::analog_bank::a) == 0x003C'FFFF);
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

TEST_CASE(
    "RM0038 12.3.3 and Table 41: bank B reads its own pad's drive, a shared pad its bank A "
    "drive, VCOMP logs as unmodelled")
{
  adc_rig const r;
  r.port().drive(13, 1'500'000);
  r.port().drive(0, 750'000);
  r.port().drive({0, stm32::analog_bank::b}, 1'500'000);
  r.power();
  CHECK(r.convert(13, bank_b) == 2048);
  CHECK(r.convert(0) == 1024);
  CHECK(r.convert(0, bank_b) == 2048);
  CHECK(r.logged.empty());
  CHECK(r.convert(26, bank_b) == 0);
  CHECK(r.convert(26) == 0);
  CHECK(r.warnings() == 1);

  adc_rig const strict{l152re(), true};
  strict.power();
  strict.write(sqr5, 26);
  CHECK_THROWS_AS(strict.attempt(cr2, adon | bank_b | swstart), popcorn::model_error);
  CHECK((strict.read(sr) & strt) == 0);
}

TEST_CASE(
    "RM0038 Table 41: each of the 11 own pads of bank B reads its bank B drive, bank A its own")
{
  adc_rig const r;
  r.power();
  for(std::uint8_t channel = 0; channel < 13; ++channel)
  {
    if((0x1FCFu >> channel & 1u) == 0)
    {
      continue;
    }
    CAPTURE(channel);
    auto const own = 100'000u * (channel + 1u);
    r.port().drive({channel, stm32::analog_bank::b}, own);
    r.port().drive(channel, 2'900'000);
    CHECK(r.port().input({channel, stm32::analog_bank::b}) == own);
    CHECK(r.convert(channel, bank_b) == std::uint64_t{own} * 4096u / 3'000'000u);
    CHECK(r.convert(channel) == 3959);
  }
  CHECK(r.logged.empty());
}

TEST_CASE(
    "a bank B drive while bank A is selected is stored and read by the next "
    "ADC_CFG 1 conversion")
{
  adc_rig const r;
  r.power();
  CHECK(r.convert(1) == 0);
  r.port().drive({1, stm32::analog_bank::b}, 2'250'000);
  CHECK(r.convert(1) == 0);
  CHECK(r.convert(1, bank_b) == 3072);
}

TEST_CASE("checklist 26: a bank B drive after the sampling of 1b leaves the held code")
{
  adc_rig const r;
  r.port().drive({1, stm32::analog_bank::b}, 1'500'000);
  r.power();
  r.write(smpr3, 7u << 3);
  r.write(sqr5, 1);
  auto const start = r.now();
  r.write(cr2, adon | bank_b | swstart);
  r.run_until(start + 385 * cycle);
  r.port().drive({1, stm32::analog_bank::b}, 750'000);
  r.run_until(start + 396 * cycle + us);
  REQUIRE((r.read(sr) & eoc) != 0);
  CHECK(r.read(dr) == 2048);
  r.write(cr2, adon | bank_b | swstart);
  r.run_until(r.now() + 396 * cycle + us);
  CHECK(r.read(dr) == 1024);
}

TEST_CASE("the bank B drives are board state: kept by a reset, restored by a snapshot")
{
  adc_rig const r;
  r.port().drive({3, stm32::analog_bank::b}, 1'000'000);
  r.port().drive(3, 2'000'000);
  r.f.target->reset();
  CHECK(r.port().input({3, stm32::analog_bank::b}) == 1'000'000);
  CHECK(r.port().input(3) == 2'000'000);
  auto const saved = r.f.target->snapshot();
  r.port().drive({3, stm32::analog_bank::b}, 0);
  r.f.target->restore(saved);
  CHECK(r.port().input({3, stm32::analog_bank::b}) == 1'000'000);
  adc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.port().input({3, stm32::analog_bank::b}) == 1'000'000);
  CHECK(other.port().input(3) == 2'000'000);
}

TEST_CASE(
    "a bank B channel sharing its pad, absent or on Cat.1 and Cat.2 is a config_error, "
    "both banks left unchanged")
{
  adc_rig const r;
  r.port().drive(13, 1'000'000);
  r.port().drive({0, stm32::analog_bank::b}, 2'000'000);
  for(std::uint8_t channel : {4, 5, 13, 15, 18, 25, 27, 31})
  {
    CAPTURE(channel);
    CHECK_THROWS_AS(
        r.port().drive({channel, stm32::analog_bank::b}, 500'000), popcorn::config_error);
    CHECK_THROWS_AS(
        static_cast<void>(r.port().input({channel, stm32::analog_bank::b})), popcorn::config_error);
  }
  for(std::uint8_t channel : {16, 17, 26, 32})
  {
    CAPTURE(channel);
    CHECK_THROWS_AS(
        r.port().drive({channel, stm32::analog_bank::b}, 500'000), popcorn::config_error);
  }
  CHECK(r.port().input(13) == 1'000'000);
  CHECK(r.port().input({0, stm32::analog_bank::b}) == 2'000'000);
  CHECK(r.port().external_channels(stm32::analog_bank::b) == 0x0000'1FCF);
  CHECK(r.port().bonded_channels(stm32::analog_bank::b) == 0x0000'0001);

  adc_rig const rb{l152rb()};
  CHECK(rb.port().external_channels(stm32::analog_bank::b) == 0);
  CHECK(rb.port().bonded_channels(stm32::analog_bank::b) == 0);
  CHECK_THROWS_AS(rb.port().drive({0, stm32::analog_bank::b}, 500'000), popcorn::config_error);
}

TEST_CASE(
    "bonded_channels_b is the description's mask, every own pad without one, a wider mask throws")
{
  auto       description = l152re();
  auto const adc         = std::ranges::find(
      description.peripherals, std::string_view{"ADC"}, &popcorn::peripheral_desc::name);
  REQUIRE(adc != description.peripherals.end());
  REQUIRE(adc->bonded_channels_b == std::optional<std::uint32_t>{0x0000'0001});
  adc->bonded_channels_b.reset();
  {
    adc_rig const r{description};
    CHECK(r.port().bonded_channels(stm32::analog_bank::b) == 0x0000'1FCF);
  }
  adc->bonded_channels_b = 0x0000'0010;
  CHECK_THROWS_AS(adc_rig{description}, popcorn::config_error);
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
          row{cr2,  adon | (1u << 28) | (11u << 24), false},
          row{cr1,  1u << 17,                        true },
          row{cr1,  1u << 16,                        true },
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
  adc_rig const aborted;
  aborted.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  REQUIRE(aborted.due() == start + 396 * cycle);
  aborted.write(sqr5, 5);
  CHECK(aborted.due() == ~std::uint64_t{});
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

TEST_CASE(
    "an EXTSEL whose source has no model, tim2_cc3 on a part without TIM2, logs once and starts "
    "nothing, a model_error when strict")
{
  adc_rig const r{without_unit(l152re(), "TIM2")};
  r.power();
  r.write(cr2, adon | (1u << 28) | (2u << 24));
  r.run_until(r.now() + 300 * us);
  CHECK((r.read(sr) & strt) == 0);
  REQUIRE(r.warnings() == 1);
  CHECK(r.logged.front().message.contains("tim2_cc3"));

  adc_rig const strict{without_unit(l152re(), "TIM2"), true};
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
    "an EXTSEL whose source has no model written with ADON 0 logs once at the ADONS event, "
    "a model_error when strict")
{
  adc_rig const r{without_unit(l152re(), "TIM2")};
  r.write(cr2, (1u << 28) | (2u << 24));
  r.write(cr2, adon | (1u << 28) | (2u << 24));
  CHECK(r.warnings() == 0);
  r.run_until(r.now() + 4 * us);
  REQUIRE((r.read(sr) & adons) != 0);
  CHECK(r.warnings() == 1);

  adc_rig const strict{without_unit(l152re(), "TIM2"), true};
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
  r.write(cr2, adon | swstart | jswstart);
  CHECK((r.read(cr2) & (swstart | jswstart)) == swstart);
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

TEST_CASE(
    "RM0038 12.3.9: JAUTO converts the injected group after the regular group, CONT both over")
{
  for(auto const control : {0u, cont})
  {
    CAPTURE(control);
    adc_rig const r;
    r.port().drive(1, 750'000);
    r.port().drive(5, 1'500'000);
    r.write(cr1, jauto);
    r.power();
    r.write(sqr5, 1);
    r.write(jsqr, 5u << 15);
    auto const start = r.now();
    r.write(cr2, adon | control | swstart);
    r.run_until(start + 16 * cycle + ns);
    CHECK((r.read(sr) & (eoc | jstrt)) == (eoc | jstrt));
    CHECK(r.read(dr) == 1024);
    CHECK(r.due() == start + 32 * cycle);
    r.run_until(start + 32 * cycle + ns);
    CHECK((r.read(sr) & jeoc) != 0);
    CHECK(r.read(jdr1) == 2048);
    CHECK(r.due() == (control != 0 ? start + 48 * cycle : ~std::uint64_t{}));
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "JSWSTART or JEXTEN with JAUTO, JAUTO or DISCEN with JDISCEN and DELS with JAUTO log once, a "
    "model_error when strict")
{
  for(auto const& [control, command] : {
          std::pair{jauto,            jswstart                         },
          std::pair{jauto,            (1u << 20) | (1u << 16) | swstart},
          std::pair{jauto | jdiscen,  swstart                          },
          std::pair{discen | jdiscen, jswstart                         },
          std::pair{jauto,            (1u << 4) | swstart              },
  })
  {
    CAPTURE(control);
    CAPTURE(command);
    adc_rig const lax;
    lax.write(cr1, control);
    lax.power();
    lax.write(cr2, adon | command);
    lax.run_until(lax.now() + 40 * us);
    lax.write(cr2, adon | command);
    CHECK(lax.warnings() == 1);

    adc_rig const strict{l152re(), true};
    strict.write(cr1, control);
    strict.power();
    CHECK_THROWS_AS(strict.attempt(cr2, adon | command), popcorn::model_error);
    CHECK((strict.read(sr) & (strt | jstrt)) == 0);
  }
}

TEST_CASE(
    "a JEXTSEL whose source has no model or a reserved one logs once and starts nothing, "
    "a model_error when strict")
{
  for(auto const& [selection, named, dropped] : {
          std::tuple{0u,  std::string_view{"tim9_cc1"}, std::string_view{"TIM9"}},
          std::tuple{15u, std::string_view{"exti15"},   std::string_view{"EXTI"}},
          std::tuple{11u, std::string_view{"reserved"}, std::string_view{}      },
  })
  {
    CAPTURE(selection);
    auto const    desc = dropped.empty() ? l152re() : without_unit(l152re(), dropped);
    adc_rig const r{desc};
    r.power();
    r.write(cr2, adon | (1u << 20) | (selection << 16));
    r.write(cr2, adon | (1u << 20) | (selection << 16));
    r.run_until(r.now() + 40 * us);
    CHECK((r.read(sr) & jstrt) == 0);
    REQUIRE(r.warnings() == 1);
    CHECK(r.logged.front().message.contains(named));

    adc_rig const strict{desc, true};
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

TEST_CASE("an injected bank B channel on a pad of its own reads its bank B drive, no log")
{
  adc_rig const r{l152re(), true};
  r.port().drive(0, 750'000);
  r.port().drive({0, stm32::analog_bank::b}, 1'500'000);
  r.power();
  r.write(cr2, adon | bank_b);
  r.write(cr2, adon | bank_b | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK((r.read(sr) & jeoc) != 0);
  CHECK(r.read(jdr1) == 2048);
  CHECK(r.logged.empty());
}

TEST_CASE("LL ADC: JSWSTART injects ADC_IN5 over a regular conversion, JOFR1 0x400, DR after it")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc, true, false, {}, "l1_adc_ll"};
    r.port().drive(1, 750'000);
    r.port().drive(5, 2'250'000);
    CHECK(r.run("injected") == std::vector<std::uint32_t>{2048, 0, 1024});
    CHECK(r.logged.empty());
  }
}

TEST_CASE("a JSQR write between JDISCEN subgroups starts the next JSWSTART at JDR1, inferred")
{
  adc_rig const r;
  for(std::uint8_t channel : {2, 3, 4, 7, 8, 9})
  {
    r.port().drive(channel, 300'000u * channel);
  }
  r.write(cr1, scan | jdiscen);
  r.power();
  r.write(jsqr, (2u << 20) | (2u << 5) | (3u << 10) | (4u << 15));
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  REQUIRE(r.read(jdr(0)) == code_of(600'000));
  r.write(jsqr, (2u << 20) | (7u << 5) | (8u << 10) | (9u << 15));
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(jdr(0)) == code_of(2'100'000));
  CHECK(r.read(jdr(1)) == 0);
  CHECK((r.read(sr) & jeoc) == 0);
}

TEST_CASE("an SQR write between DISCEN subgroups starts the next SWSTART at rank 1, inferred")
{
  adc_rig const r;
  for(std::uint8_t channel : {1, 2, 3, 7})
  {
    r.port().drive(channel, 300'000u * channel);
  }
  r.write(cr1, scan | discen);
  r.power();
  r.write(sqr1, 2u << 20);
  r.write(sqr5, 1u | (2u << 5) | (3u << 10));
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 40 * us);
  REQUIRE(r.read(dr) == code_of(300'000));
  r.write(sqr5, 7u | (2u << 5) | (3u << 10));
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(dr) == code_of(2'100'000));
}

TEST_CASE("with JAUTO an injected trigger edge starts nothing, the group follows the regular one")
{
  adc_rig const r;
  r.port().drive(1, 750'000);
  r.port().drive(5, 1'500'000);
  r.write(cr1, jauto);
  r.power();
  r.write(smpr3, 7u << 3);
  r.write(sqr5, 1);
  r.write(jsqr, 5u << 15);
  r.write(cr2, adon | (1u << 20) | (2u << 16));
  r.tim2_updates(41);
  r.run_until(r.now() + 30 * us);
  auto const start = r.now();
  r.write(cr2, adon | (1u << 20) | (2u << 16) | swstart);
  r.run_until(start + 24 * us);
  CHECK((r.read(sr) & jstrt) == 0);
  r.run_until(start + 396 * cycle + ns);
  CHECK((r.read(sr) & (eoc | jstrt)) == (eoc | jstrt));
  CHECK(r.read(dr) == 1024);
  r.run_until(start + 412 * cycle + ns);
  CHECK((r.read(sr) & jeoc) != 0);
  CHECK(r.read(jdr1) == 2048);
  CHECK(r.warnings() == 1);
}

TEST_CASE(
    "with JAUTO and JDISCEN the whole injected group follows the regular one from JDR1, inferred")
{
  adc_rig const r;
  r.port().drive(1, 750'000);
  r.port().drive(2, 600'000);
  r.port().drive(5, 1'500'000);
  r.write(cr1, scan | jdiscen);
  r.power();
  r.write(sqr5, 1);
  r.write(jsqr, (1u << 20) | (5u << 10) | (2u << 15));
  r.write(cr2, adon | jswstart);
  r.run_until(r.now() + 40 * us);
  REQUIRE(r.read(jdr(0)) == 2048);
  REQUIRE(r.read(jdr(1)) == 0);
  r.write(sr, 0);
  r.port().drive(5, 2'250'000);
  r.write(cr1, scan | jdiscen | jauto);
  r.write(cr2, adon | swstart);
  r.run_until(r.now() + 40 * us);
  CHECK(r.read(dr) == 1024);
  CHECK(r.read(jdr(0)) == 3072);
  CHECK(r.read(jdr(1)) == code_of(600'000));
  CHECK((r.read(sr) & jeoc) != 0);
}

TEST_CASE(
    "a JSQR write during an injection inside a regular DELS delay resumes the regular group when "
    "that delay ends")
{
  adc_rig const r;
  r.power();
  r.write(smpr3, 7u << 27);
  r.write(jsqr, 9u << 15);
  auto const start = r.now();
  r.write(cr2, adon | cont | (5u << 4) | swstart);
  r.run_until(start + 16 * cycle + ns);
  REQUIRE((r.read(sr) & eoc) != 0);
  auto const resume = start + 16 * cycle + 30'517'579 + 3 * cycle;
  r.run_until(r.now() + us);
  auto const injected = r.now();
  r.write(cr2, adon | cont | (5u << 4) | jswstart);
  r.run_until(injected + 2 * us);
  r.write(sr, 0);
  r.write(jsqr, 9u << 15);
  CHECK(r.due() == resume);
  r.run_until(resume - us);
  CHECK((r.read(sr) & (eoc | jeoc)) == 0);
  r.run_until(resume + 16 * cycle + ns);
  CHECK((r.read(sr) & (eoc | jeoc)) == eoc);
}

TEST_CASE("one edge for both groups runs both groups' checks first: a refused group starts nothing")
{
  adc_rig const strict{l152re(), true};
  strict.power();
  strict.write(sqr5, 26);
  strict.write(cr2, adon | (1u << 28) | (6u << 24) | (1u << 20) | (2u << 16));
  strict.tim2_updates(209);
  auto const update = strict.due("TIM2");
  CHECK_THROWS_AS(
      strict.f.run_to_stop(
          {.max_instructions = icount{1'000'000'000}, .until_time = vtime{update + 600 * ns}}),
      popcorn::model_error);
  CHECK((strict.read(sr) & (jstrt | strt)) == 0);
  CHECK(strict.due() == ~std::uint64_t{});
}

TEST_CASE("a strict refusal at SWSTART or JSWSTART clears the bit, the fixed setup starts next")
{
  adc_rig const strict{l152re(), true};
  strict.power();
  strict.write(sqr5, 26);
  CHECK_THROWS_AS(strict.attempt(cr2, adon | swstart), popcorn::model_error);
  CHECK((strict.read(cr2) & swstart) == 0);
  strict.write(sqr5, 5);
  strict.write(cr2, adon | swstart);
  CHECK((strict.read(sr) & strt) != 0);
  strict.run_until(strict.now() + 2 * us);
  strict.write(jsqr, 26u << 15);
  CHECK_THROWS_AS(strict.attempt(cr2, adon | jswstart), popcorn::model_error);
  CHECK((strict.read(cr2) & jswstart) == 0);
  strict.write(jsqr, 5u << 15);
  strict.write(cr2, adon | jswstart);
  CHECK((strict.read(sr) & jstrt) != 0);
}

TEST_CASE(
    "RM0038 12.11.1: with DMA 1 a scan of 3 ranks fills memory through DMA1 channel 1, each "
    "transfer clearing EOC, no OVR")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc};
    r.port().drive(1, 750'000);
    r.port().drive(2, 1'500'000);
    r.port().drive(3, 2'250'000);
    r.write(cr1, scan);
    r.power();
    r.write(smpr3, (7u << 3) | (7u << 6) | (7u << 9));
    r.write(sqr1, 2u << 20);
    r.write(sqr5, 1u | (2u << 5) | (3u << 10));
    r.dma_channel1(3, dma_half | dma_en);
    auto const start = r.now();
    r.write(cr2, adon | eocs | dma | swstart);
    r.run_until(start + 3 * 396 * cycle + 10 * us);
    CHECK(r.memory(0) == 1024);
    CHECK(r.memory(1) == 2048);
    CHECK(r.memory(2) == 3072);
    CHECK(r.dma1(0x0C) == 0);
    CHECK((r.read(sr) & (eoc | ovr)) == 0);
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "RM0038 12.11.1: with DMA 1 an unread result makes the next an overrun, the group stops and "
    "SWSTART waits for OVR to clear, inferred, the valid result still transfers")
{
  adc_rig const r;
  r.port().drive(0, 1'500'000);
  r.power();
  auto const start = r.now();
  r.write(cr2, adon | cont | dma | swstart);
  r.run_until(start + 32 * cycle + ns);
  CHECK((r.read(sr) & ovr) != 0);
  r.port().drive(0, 750'000);
  r.run_until(start + 80 * cycle);
  CHECK(r.due() == ~std::uint64_t{});
  CHECK(r.read(dr) == 2048);
  r.write(sr, ~strt);
  r.write(cr2, adon | cont | dma | swstart);
  CHECK((r.read(sr) & strt) == 0);
  CHECK((r.read(cr2) & swstart) == 0);
  r.dma_channel1(1, dma_half | dma_en);
  r.run_until(r.now() + 10 * us);
  CHECK(r.memory(0) == 2048);
  r.write(sr, ~ovr);
  auto const again = r.now();
  r.write(cr2, adon | dma | swstart);
  CHECK(r.due() == again + 16 * cycle);
}

TEST_CASE(
    "RM0038 12.11.1: DDS 0 stops the requests after the last transfer until DMA is written 0 "
    "then 1, DDS 1 keeps requesting, inferred")
{
  for(auto const selection : {0u, dds})
  {
    CAPTURE(selection);
    adc_rig const r;
    r.port().drive(1, 750'000);
    r.power();
    r.write(smpr3, 7u << 3);
    r.write(sqr5, 1);
    r.dma_channel1(1, dma_half | dma_en);
    auto const start = r.now();
    r.write(cr2, adon | cont | dma | selection | swstart);
    r.run_until(start + 396 * cycle + 10 * us);
    REQUIRE(r.memory(0) == 1024);
    r.port().drive(1, 1'500'000);
    r.run_until(start + 3 * 396 * cycle + 10 * us);
    CHECK(((r.read(sr) & ovr) != 0) == (selection != 0));
    if(selection != 0)
    {
      continue;
    }
    CHECK(r.read(dr) == 2048);
    r.port().drive(1, 2'250'000);
    r.dma_channel1(1, dma_half | dma_en);
    r.write(cr2, adon | cont | swstart);
    r.write(cr2, adon | cont | dma);
    r.run_until(r.now() + 2 * 396 * cycle);
    CHECK(r.memory(0) == 3072);
  }
}

TEST_CASE("a DMA bit set while a result waits unread raises the request at once")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(sqr5, 5);
  r.write(cr2, adon | eocs | swstart);
  r.run_until(r.now() + 2 * us);
  REQUIRE((r.read(sr) & eoc) != 0);
  r.dma_channel1(1, dma_half | dma_en);
  r.run_until(r.now() + 10 * us);
  CHECK(r.memory(0) == 0);
  r.write(cr2, adon | eocs | dma);
  r.run_until(r.now() + 10 * us);
  CHECK(r.memory(0) == 2048);
  CHECK((r.read(sr) & eoc) == 0);
}

TEST_CASE("ADC1RST ends the DDS 0 stop: DMA requests start again after the reset")
{
  adc_rig const r;
  r.power();
  r.dma_channel1(1, dma_half | dma_en);
  r.write(cr2, adon | dma | swstart);
  r.run_until(r.now() + 10 * us);
  REQUIRE(r.dma1(0x0C) == 0);
  rcc_reset_pulse(r.f, "APB2RSTR", "ADC1RST");
  r.port().drive(0, 1'500'000);
  r.power();
  r.dma_channel1(1, dma_half | dma_en);
  r.write(cr2, adon | dma | swstart);
  r.run_until(r.now() + 10 * us);
  CHECK(r.memory(0) == 2048);
}

TEST_CASE(
    "RM0038 12.3.7: AWDEN sets AWD for a code above HT or below LT, before alignment, AWDIE "
    "raises the line")
{
  for(auto const& [microvolts, control, flagged] : {
          std::tuple{1'500'000u, 0u,    false},
          std::tuple{2'500'000u, 0u,    true },
          std::tuple{300'000u,   0u,    true },
          std::tuple{1'500'000u, align, false},
  })
  {
    CAPTURE(microvolts);
    CAPTURE(control);
    adc_rig const r;
    r.port().drive(5, microvolts);
    r.write(cr1, awden | awdie);
    r.power();
    r.write(htr, 3000);
    r.write(ltr, 1000);
    CHECK(r.read(htr) == 3000);
    static_cast<void>(r.convert(5, control));
    CHECK(((r.read(sr) & awd) != 0) == flagged);
    CHECK(r.pending() == flagged);
  }
}

TEST_CASE("RM0038 Table 59: AWDSGL guards the AWDCH channel only")
{
  for(auto const& [channel, flagged] : {
          std::pair{2u, false},
          std::pair{1u, true }
  })
  {
    CAPTURE(channel);
    adc_rig const r;
    r.port().drive(1, 2'900'000);
    r.port().drive(2, 1'500'000);
    r.write(cr1, scan | awden | awdsgl | channel);
    r.power();
    r.write(htr, 3000);
    r.write(sqr1, 1u << 20);
    r.write(sqr5, 1u | (2u << 5));
    r.write(cr2, adon | swstart);
    r.run_until(r.now() + 40 * us);
    CHECK((r.read(sr) & eoc) != 0);
    CHECK(((r.read(sr) & awd) != 0) == flagged);
  }
}

TEST_CASE(
    "RM0038 Table 59: JAWDEN guards injected conversions before their offset, "
    "AWDEN the regular ones only, inferred")
{
  for(auto const& [control, injected, flagged] : {
          std::tuple{awden,  true,  false},
          std::tuple{jawden, true,  true },
          std::tuple{jawden, false, false},
          std::tuple{awden,  false, true },
  })
  {
    CAPTURE(control);
    CAPTURE(injected);
    adc_rig const r;
    r.port().drive(5, 2'500'000);
    r.write(cr1, control);
    r.power();
    r.write(htr, 3000);
    r.write(jofr1, 2000);
    r.write(sqr5, 5);
    r.write(jsqr, 5u << 15);
    r.write(cr2, adon | (injected ? jswstart : swstart));
    r.run_until(r.now() + 40 * us);
    CHECK(((r.read(sr) & awd) != 0) == flagged);
  }
}

TEST_CASE("below 12 bits the watchdog compares the code shifted to 12 bits, inferred")
{
  for(auto const& [high, flagged] : {
          std::pair{3000u, true },
          std::pair{3300u, false}
  })
  {
    CAPTURE(high);
    adc_rig const r;
    r.port().drive(5, 2'400'000);
    r.write(cr1, awden | (2u << 24));
    r.power();
    r.write(htr, high);
    CHECK(r.convert(5) == 204);
    CHECK(((r.read(sr) & awd) != 0) == flagged);
  }
}

TEST_CASE("LL ADC: DMA1 channel 1 takes a scan of ADC_IN1, ADC_IN18 and ADC_IN5, no OVR")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc, true, false, {}, "l1_adc_ll"};
    r.port().drive(1, 750'000);
    r.port().drive(18, 1'500'000);
    r.port().drive(5, 2'250'000);
    CHECK(r.run("dma3") == std::vector<std::uint32_t>{1024, 2048, 3072, 0});
    CHECK(r.logged.empty());
  }
}

TEST_CASE("LL ADC: the watchdog on ADC_IN5 flags a 3072 code above HT 3000")
{
  for(auto const& desc : {l152re(), l152rb()})
  {
    CAPTURE(desc.part);
    adc_rig const r{desc, true, false, {}, "l1_adc_ll"};
    r.port().drive(5, 2'250'000);
    CHECK(r.run("watchdog") == std::vector<std::uint32_t>{3072, 1});
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "RM0038 12.11.4: an SR write of 1 to EOC keeps a DMA scan result unread, the late channel "
    "takes all three, no OVR")
{
  adc_rig const r;
  r.port().drive(1, 750'000);
  r.port().drive(2, 1'500'000);
  r.port().drive(3, 2'250'000);
  r.write(cr1, scan | awden);
  r.power();
  r.write(htr, 500);
  r.write(smpr3, (7u << 3) | (7u << 6) | (7u << 9));
  r.write(sqr1, 2u << 20);
  r.write(sqr5, 1u | (2u << 5) | (3u << 10));
  r.write(cr2, adon | dma | swstart);
  r.run_until(r.due() + ns);
  REQUIRE((r.read(sr) & awd) != 0);
  REQUIRE((r.read(sr) & eoc) == 0);
  r.write(sr, ~awd);
  r.dma_channel1(3, dma_half | dma_en);
  r.run_until(r.now() + 3 * 396 * cycle + 10 * us);
  CHECK(r.memory(0) == 1024);
  CHECK(r.memory(1) == 2048);
  CHECK(r.memory(2) == 3072);
  CHECK((r.read(sr) & ovr) == 0);
}

TEST_CASE(
    "RM0038 12.11.1: a circular channel with DDS 0 takes one pass and the requests stop, DDS 1 "
    "keeps refilling, inferred")
{
  for(auto const selection : {0u, dds})
  {
    CAPTURE(selection);
    adc_rig const r;
    r.port().drive(1, 750'000);
    r.power();
    r.write(smpr3, 7u << 3);
    r.write(sqr5, 1);
    r.dma_channel1(3, dma_circ | dma_half | dma_en);
    auto const start = r.now();
    r.write(cr2, adon | cont | dma | selection | swstart);
    r.run_until(start + 3 * 396 * cycle + 10 * us);
    REQUIRE(r.memory(2) == 1024);
    r.port().drive(1, 1'500'000);
    r.run_until(start + 10 * 396 * cycle + 10 * us);
    auto const refilled = selection != 0 ? 2048u : 1024u;
    CHECK(r.memory(0) == refilled);
    CHECK(r.memory(1) == refilled);
    CHECK(r.memory(2) == refilled);
    if(selection == 0)
    {
      CHECK(r.dma1(0x0C) == 3);
    }
    CHECK((r.read(sr) & ovr) == 0);
  }
}

TEST_CASE(
    "RM0038 12.15.2: with SCAN and AWDIE a crossing aborts the regular scan, the next start "
    "begins at rank 1, rank and flags inferred")
{
  for(auto const interrupt : {awdie, 0u})
  {
    CAPTURE(interrupt);
    adc_rig const r;
    r.port().drive(1, 2'900'000);
    r.port().drive(2, 1'500'000);
    r.port().drive(3, 750'000);
    r.write(cr1, scan | awden | interrupt);
    r.power();
    r.write(htr, 3000);
    r.write(smpr3, (7u << 3) | (7u << 6) | (7u << 9));
    r.write(sqr1, 2u << 20);
    r.write(sqr5, 1u | (2u << 5) | (3u << 10));
    r.dma_channel1(4, dma_half | dma_en);
    r.write(cr2, adon | cont | dma | dds | swstart);
    r.run_until(r.now() + 3 * 396 * cycle + 10 * us);
    CHECK((r.read(sr) & awd) != 0);
    CHECK(r.memory(0) == 3959);
    if(interrupt == 0)
    {
      CHECK(r.memory(1) == 2048);
      CHECK(r.memory(2) == 1024);
      continue;
    }
    CHECK(r.memory(1) == 0);
    CHECK(r.due() == ~std::uint64_t{});
    CHECK(r.dma1(0x0C) == 3);
    r.port().drive(1, 750'000);
    r.write(sr, ~(awd | strt));
    r.write(cr2, adon | dma | dds | swstart);
    r.run_until(r.now() + 396 * cycle + 10 * us);
    CHECK(r.memory(1) == 1024);
  }
}

TEST_CASE(
    "RM0038 12.15.2: with DISCEN an AWDIE crossing at rank 1 ends the subgroup, the next "
    "trigger converts from rank 1, inferred")
{
  adc_rig const r;
  r.port().drive(1, 2'900'000);
  r.port().drive(2, 1'500'000);
  r.port().drive(3, 750'000);
  r.write(cr1, scan | discen | (2u << 13) | awden | awdie);
  r.power();
  r.write(htr, 3000);
  r.write(smpr3, (7u << 3) | (7u << 6) | (7u << 9));
  r.write(sqr1, 2u << 20);
  r.write(sqr5, 1u | (2u << 5) | (3u << 10));
  r.dma_channel1(4, dma_half | dma_en);
  r.write(cr2, adon | dma | dds | swstart);
  r.run_until(r.now() + 3 * 396 * cycle + 10 * us);
  REQUIRE(r.memory(0) == 3959);
  REQUIRE(r.memory(1) == 0);
  r.port().drive(1, 750'000);
  r.write(sr, ~(awd | strt));
  r.write(cr2, adon | dma | dds | swstart);
  r.run_until(r.now() + 396 * cycle + 10 * us);
  CHECK(r.memory(1) == 1024);
}

TEST_CASE(
    "RM0038 12.15.2: with SCAN and AWDIE a crossing ends the injected group "
    "with JEOC, inferred")
{
  for(auto const interrupt : {awdie, 0u})
  {
    CAPTURE(interrupt);
    adc_rig const r;
    r.port().drive(1, 2'900'000);
    r.port().drive(2, 1'500'000);
    r.write(cr1, scan | jawden | interrupt);
    r.power();
    r.write(htr, 3000);
    r.write(jsqr, (1u << 20) | (1u << 10) | (2u << 15));
    r.write(cr2, adon | jswstart);
    r.run_until(r.now() + 40 * us);
    CHECK((r.read(sr) & (jeoc | awd)) == (jeoc | awd));
    CHECK(r.read(jdr1) == 3959);
    CHECK(r.read(jdr1 + 4) == (interrupt != 0 ? 0u : 2048u));
  }
}

TEST_CASE("with DDS 0 after the last transfer an overrun leaves SWSTART working, as with DMA 0")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(sqr5, 5);
  r.dma_channel1(1, dma_half | dma_en);
  for(int start = 0; start < 3; ++start)
  {
    r.write(sr, ~strt);
    r.write(cr2, adon | eocs | dma | swstart);
    r.run_until(r.now() + 10 * us);
  }
  REQUIRE(r.memory(0) == 2048);
  REQUIRE((r.read(sr) & ovr) != 0);
  r.write(sr, ~strt);
  r.write(cr2, adon | eocs | dma | swstart);
  CHECK((r.read(sr) & strt) != 0);
}

TEST_CASE(
    "the end of a TIM2_CC3 count on DMA1 channel 1 while the ADC DMA bit is 0 leaves the "
    "ADC's later DMA scan requesting")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(sqr5, 5);
  r.poke("RCC", 0x24, 1);
  r.poke("TIM2", 0x0C, 1u << 11);
  r.dma_channel1(1, dma_half | dma_en);
  r.poke("TIM2", 0x14, 1u << 3);
  r.run_until(r.now() + 10 * us);
  REQUIRE(r.dma1(0x0C) == 0);
  r.dma_channel1(1, dma_half | dma_en);
  r.write(cr2, adon | dma | swstart);
  r.run_until(r.now() + 10 * us);
  CHECK(r.memory(0) == 2048);
}

TEST_CASE(
    "the end of a TIM2 update count on DMA1 channel 2 leaves the ADC DDS 0 requests on "
    "channel 1 going")
{
  adc_rig const r;
  r.port().drive(5, 1'500'000);
  r.power();
  r.write(sqr5, 5);
  r.write(cr2, adon | dma);
  r.poke("RCC", 0x24, 1);
  r.poke("TIM2", 0x0C, 1u << 8);
  r.poke("DMA1", 0x1C, 0);
  r.poke("DMA1", 0x24, base + dr);
  r.poke("DMA1", 0x28, buffer + 0x100);
  r.poke("DMA1", 0x20, 1);
  r.poke("DMA1", 0x1C, dma_half | dma_en);
  r.poke("TIM2", 0x14, 1);
  r.run_until(r.now() + 10 * us);
  REQUIRE(r.dma1(0x20) == 0);
  r.dma_channel1(1, dma_half | dma_en);
  r.write(cr2, adon | dma | swstart);
  r.run_until(r.now() + 10 * us);
  CHECK(r.memory(0) == 2048);
}

TEST_CASE("RM0038 12.11.1: with DMA 1 and AWDEN a result an overrun loses still sets AWD, inferred")
{
  adc_rig const r;
  r.port().drive(0, 1'500'000);
  r.write(cr1, awden);
  r.power();
  r.write(htr, 3000);
  r.write(smpr3, 7);
  auto const start = r.now();
  r.write(cr2, adon | cont | dma | swstart);
  r.run_until(start + 396 * cycle + ns);
  REQUIRE((r.read(sr) & awd) == 0);
  r.port().drive(0, 2'900'000);
  r.run_until(start + 792 * cycle + ns);
  CHECK((r.read(sr) & ovr) != 0);
  CHECK(r.read(dr) == 2048);
  CHECK((r.read(sr) & awd) != 0);
}

TEST_CASE(
    "RM0038 Table 61: EXTSEL 0000, 0010, 0011, 0101, 0111 and 1000 start on the TIM9 CC2, "
    "TIM2 CC3, TIM2 CC2, TIM4 CC4, TIM3 CC1 and TIM3 CC3 events")
{
  for(auto const& [selection, unit, enable_offset, enable, channel] : {
          std::tuple{0u, std::string_view{"TIM9"}, 0x20u, 1u << 2, 2u},
          std::tuple{2u, std::string_view{"TIM2"}, 0x24u, 1u << 0, 3u},
          std::tuple{3u, std::string_view{"TIM2"}, 0x24u, 1u << 0, 2u},
          std::tuple{5u, std::string_view{"TIM4"}, 0x24u, 1u << 2, 4u},
          std::tuple{7u, std::string_view{"TIM3"}, 0x24u, 1u << 1, 1u},
          std::tuple{8u, std::string_view{"TIM3"}, 0x24u, 1u << 1, 3u},
  })
  {
    CAPTURE(selection);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | (1u << 28) | (selection << 24));
    r.timer_compares(unit, enable_offset, enable, channel, 100);
    auto const match = r.due(unit);
    r.run_until(match + 600 * ns);
    CHECK((r.read(sr) & strt) != 0);
    CHECK(r.due() == match + 218'750 + 16 * cycle);
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "RM0038 Table 62: JEXTSEL 0000, 0011, 0100, 0110 to 1000 and 1001 start on the TIM9 CC1, "
    "TIM2 CC1, TIM3 CC4, TIM4 CC1 to CC3 and TIM10 CC1 events")
{
  for(auto const& [selection, unit, enable_offset, enable, channel] : {
          std::tuple{0u, std::string_view{"TIM9"},  0x20u, 1u << 2, 1u},
          std::tuple{3u, std::string_view{"TIM2"},  0x24u, 1u << 0, 1u},
          std::tuple{4u, std::string_view{"TIM3"},  0x24u, 1u << 1, 4u},
          std::tuple{6u, std::string_view{"TIM4"},  0x24u, 1u << 2, 1u},
          std::tuple{7u, std::string_view{"TIM4"},  0x24u, 1u << 2, 2u},
          std::tuple{8u, std::string_view{"TIM4"},  0x24u, 1u << 2, 3u},
          std::tuple{9u, std::string_view{"TIM10"}, 0x20u, 1u << 3, 1u},
  })
  {
    CAPTURE(selection);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | (1u << 20) | (selection << 16));
    r.timer_compares(unit, enable_offset, enable, channel, 100);
    auto const match = r.due(unit);
    r.run_until(match + 600 * ns);
    CHECK((r.read(sr) & jstrt) != 0);
    CHECK(r.due() == match + 218'750 + 16 * cycle);
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "RM0038 Tables 61 and 62 inferred: EXTI line 11 on PC11 starts the regular group and line 15 "
    "on PB15 the injected group, 3.5 ADC cycles after the pin edge's own time, whatever EXTI_IMR")
{
  struct leg
  {
    std::uint32_t    control;
    std::uint32_t    exticr;
    std::uint32_t    code;
    std::string_view port;
    std::uint8_t     pin;
    std::uint32_t    started;
  };
  for(auto const& [control, exticr, code, port, pin, started] : {
          leg{(1u << 28) | (15u << 24), 0x10, 2u << 12, "GPIOC", 11, strt },
          leg{(1u << 20) | (15u << 16), 0x14, 1u << 12, "GPIOB", 15, jstrt},
  })
  {
    CAPTURE(pin);
    adc_rig const r;
    r.power();
    r.poke("SYSCFG", exticr, code);
    r.write(cr2, adon | control);
    auto* const gpio = r.f.model_of<stm32::gpio_port>(port);
    REQUIRE(gpio != nullptr);
    auto const edge = r.now() + 10 * us + 123;
    gpio->drive_at(pin, true, vtime{edge});
    r.run_until(edge + 600 * ns);
    CHECK((r.read(sr) & started) != 0);
    CHECK(r.due() == edge + 218'750 + 16 * cycle);
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "a TIM2 CC2 trigger 1.43 us after the last, its conversion over at 1.22 us, starts each time "
    "though both fall in one core cycle")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (1u << 28) | (3u << 24));
  r.write(sr, 0);
  r.poke("RCC", 0x24, 1);
  r.poke("TIM2", 0x2C, 2);
  r.poke("TIM2", 0x38, 1);
  auto const from = r.now();
  r.poke("TIM2", 0x00, 1);
  constexpr std::uint64_t    tick = 476'837;
  std::vector<std::uint32_t> started;
  for(std::uint64_t k = 0; k < 20; ++k)
  {
    r.run_until(from + (3 * k + 1) * tick + 2 * tick - 1);
    started.push_back((r.read(sr) & strt) != 0 ? 1u : 0u);
    r.write(sr, 0);
    static_cast<void>(r.cpu_read(dr));
  }
  CHECK(started == std::vector<std::uint32_t>(20, 1u));
}

namespace
{
constexpr std::uint64_t half_msi_tick = 953'674;

void falling_cc2_triggers(adc_rig const& r)
{
  r.power();
  r.write(smpr3, 1);
  r.write(cr2, adon | (2u << 28) | (3u << 24));
  r.write(sr, 0);
  r.poke("RCC", 0x08, 5u << 8);
  r.poke("RCC", 0x24, 1);
  r.poke("TIM2", 0x2C, 1);
  r.poke("TIM2", 0x38, 1);
}
}

TEST_CASE(
    "a TIM2 CC2 falling edge 1.91 us after the last, its conversion over at 1.53 us, starts each "
    "time though the edge comes one timer clock ahead of the machine time")
{
  adc_rig const r;
  falling_cc2_triggers(r);
  auto const from = r.now();
  r.poke("TIM2", 0x00, 1);
  std::vector<std::uint32_t> started;
  for(std::uint64_t k = 0; k < 20; ++k)
  {
    r.run_until(from + (8 * k + 11) * half_msi_tick / 4);
    started.push_back((r.read(sr) & strt) != 0 ? 1u : 0u);
    r.write(sr, 0);
    static_cast<void>(r.cpu_read(dr));
  }
  CHECK(started == std::vector<std::uint32_t>(20, 1u));
}

TEST_CASE("a falling edge pending ahead of the machine time survives a snapshot, ADC1RST drops it")
{
  adc_rig const r;
  falling_cc2_triggers(r);
  auto const from = r.now();
  r.poke("TIM2", 0x00, 1);
  r.run_until(from + 3 * half_msi_tick / 2);
  auto const edge = r.due();
  REQUIRE(edge > r.now());
  REQUIRE(edge < from + 2 * half_msi_tick + ns);
  CHECK((r.read(sr) & strt) == 0);
  auto const saved = r.f.target->snapshot();
  auto const check = [&](adc_rig const& g) {
    CHECK(g.due() == edge);
    g.run_until(edge + 300 * ns);
    CHECK((g.read(sr) & strt) != 0);
  };
  check(r);
  adc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(other);

  r.f.target->restore(saved);
  rcc_reset_pulse(r.f, "APB2RSTR", "ADC1RST");
  CHECK(r.due() == ~std::uint64_t{});
}

TEST_CASE("a pending edge ahead of the machine time starts nothing once EXTEN is cleared before it")
{
  adc_rig const r;
  falling_cc2_triggers(r);
  auto const from = r.now();
  r.poke("TIM2", 0x00, 1);
  r.run_until(from + 3 * half_msi_tick / 2);
  auto const edge = r.due();
  REQUIRE(edge > r.now());
  r.write(cr2, adon | (3u << 24));
  r.run_until(edge + 300 * ns);
  CHECK((r.read(sr) & strt) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE(
    "TIM2 CC1 and CC2 falling edges at one match, ahead of the machine time, start the injected "
    "group on tim2_cc1 and the regular group on tim2_cc2 each time")
{
  adc_rig const r;
  r.power();
  r.write(smpr3, 1);
  r.write(jsqr, 1u << 15);
  r.write(cr2, adon | (2u << 28) | (3u << 24) | (2u << 20) | (3u << 16));
  r.write(sr, 0);
  r.poke("RCC", 0x08, 5u << 8);
  r.poke("RCC", 0x24, 1);
  r.poke("TIM2", 0x2C, 2);
  r.poke("TIM2", 0x34, 1);
  r.poke("TIM2", 0x38, 1);
  auto const from = r.now();
  r.poke("TIM2", 0x00, 1);
  std::vector<std::uint32_t> regular;
  std::vector<std::uint32_t> injected;
  for(std::uint64_t k = 0; k < 20; ++k)
  {
    r.run_until(from + (30 * k + 41) * half_msi_tick / 10);
    regular.push_back((r.read(sr) & strt) != 0 ? 1u : 0u);
    injected.push_back((r.read(sr) & jstrt) != 0 ? 1u : 0u);
    r.write(sr, 0);
    static_cast<void>(r.cpu_read(dr));
  }
  CHECK(injected == std::vector<std::uint32_t>(20, 1u));
  CHECK(regular == std::vector<std::uint32_t>(20, 1u));
}

TEST_CASE("a pending falling edge starts nothing once EXTEN or JEXTEN detects rising edges only")
{
  for(bool const injected_group : {false, true})
  {
    CAPTURE(injected_group);
    adc_rig const r;
    falling_cc2_triggers(r);
    auto const cc = injected_group ? (2u << 20) | (3u << 16) : (2u << 28) | (3u << 24);
    r.write(cr2, adon | cc);
    r.poke("TIM2", 0x34, 1);
    auto const from = r.now();
    r.poke("TIM2", 0x00, 1);
    r.run_until(from + 3 * half_msi_tick / 2);
    auto const edge = r.due();
    REQUIRE(edge > r.now());
    REQUIRE(edge < from + 2 * half_msi_tick + ns);
    r.write(cr2, adon | (injected_group ? (1u << 20) | (3u << 16) : (1u << 28) | (3u << 24)));
    r.run_until(edge + 300 * ns);
    CHECK((r.read(sr) & (strt | jstrt)) == 0);
  }
}

TEST_CASE(
    "a JEXTEN 10 edge on tim2_cc1 ahead of the machine time survives a snapshot and starts the "
    "injected group at its time, none once JAUTO is set before it")
{
  adc_rig const r;
  falling_cc2_triggers(r);
  r.write(cr2, adon | (2u << 20) | (3u << 16));
  r.poke("TIM2", 0x34, 1);
  auto const from = r.now();
  r.poke("TIM2", 0x00, 1);
  r.run_until(from + 3 * half_msi_tick / 2);
  auto const edge = r.due();
  REQUIRE(edge > r.now());
  REQUIRE(edge < from + 2 * half_msi_tick + ns);
  CHECK((r.read(sr) & jstrt) == 0);
  auto const    saved = r.f.target->snapshot();
  adc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  for(auto const* g : {&r, &other})
  {
    g->run_until(edge + 300 * ns);
    CHECK((g->read(sr) & jstrt) != 0);
  }
  r.f.target->restore(saved);
  r.write(cr1, jauto);
  r.run_until(edge + 300 * ns);
  CHECK((r.read(sr) & jstrt) == 0);
}

namespace
{
constexpr std::uint64_t msi_hz = 2'097'152;

auto msi_tick_ps(std::uint64_t from, std::uint64_t ticks) -> std::uint64_t
{
  auto const exact = static_cast<unsigned __int128>(from) * msi_hz
                   + static_cast<unsigned __int128>(ticks) * 1'000'000'000'000u;
  return static_cast<std::uint64_t>((exact + msi_hz - 1) / msi_hz);
}

auto tim2_flagged(adc_rig const& r) -> std::uint64_t
{
  r.poke("RCC", 0x24, 1);
  r.poke("TIM2", 0x2C, 99);
  r.poke("TIM2", 0x38, 30);
  auto const from = r.now();
  r.poke("TIM2", 0x00, 1);
  r.run_until(msi_tick_ps(from, 105));
  r.poke("TIM2", 0x10, ~1u);
  return from;
}

auto tim2_due(adc_rig const& r) -> std::optional<std::int64_t>
{
  if(popcorn::testing::saved_integer(*r.f.target, "TIM2", "scheduled") == 0)
  {
    return std::nullopt;
  }
  return popcorn::testing::saved_integer(*r.f.target, "TIM2", "due_ps");
}
}

TEST_CASE(
    "TIM2 schedules its flagged CC2 match from the CR2 write that makes the ADC listen to "
    "tim2_cc2 and only its wrap after the write that stops it")
{
  adc_rig const r;
  r.power();
  auto const from = tim2_flagged(r);
  CHECK(tim2_due(r) == static_cast<std::int64_t>(msi_tick_ps(from, 200)));
  r.write(cr2, adon | (1u << 28) | (3u << 24));
  CHECK(tim2_due(r) == static_cast<std::int64_t>(msi_tick_ps(from, 130)));
  r.write(cr2, adon);
  CHECK(tim2_due(r) == static_cast<std::int64_t>(msi_tick_ps(from, 200)));
}

TEST_CASE(
    "a snapshot of an ADC listening to tim2_cc2, restored into a machine whose TIM2 ran with no "
    "listener, starts the ADC at the next CC2 match")
{
  adc_rig const a;
  a.power();
  a.write(cr2, adon | (1u << 28) | (3u << 24));
  auto const from = tim2_flagged(a);
  a.write(sr, 0);
  static_cast<void>(a.cpu_read(dr));
  REQUIRE((a.read(sr) & strt) == 0);
  auto const    saved = a.f.target->snapshot();
  adc_rig const b;
  b.power();
  static_cast<void>(tim2_flagged(b));
  CHECK(tim2_due(b) != tim2_due(a));
  b.f.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  b.run_until(msi_tick_ps(from, 130) + 600 * ns);
  CHECK((b.read(sr) & strt) != 0);
}

TEST_CASE(
    "after a machine reset of an ADC listening to tim2_cc2, TIM2 run again with CC2IF set "
    "schedules only its wrap")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (1u << 28) | (3u << 24));
  static_cast<void>(tim2_flagged(r));
  REQUIRE(tim2_due(r).has_value());
  r.f.target->reset(popcorn::reset_kind::software);
  r.f.set("pc", sym(r.firmware, "idle"));
  auto const from = tim2_flagged(r);
  CHECK(tim2_due(r) == static_cast<std::int64_t>(msi_tick_ps(from, 200)));
}

TEST_CASE(
    "a TIM2 CC2 match that the next update's ARR or CCR2 preload makes reachable starts the ADC "
    "with UIF left set and no TIM2 access after the write")
{
  for(bool const arr_leg : {true, false})
  {
    CAPTURE(arr_leg);
    adc_rig const r;
    r.power();
    r.write(cr2, adon | (1u << 28) | (3u << 24));
    r.poke("RCC", 0x24, 1);
    r.poke("TIM2", 0x00, arr_leg ? 0x80u : 0u);
    r.poke("TIM2", 0x18, arr_leg ? 0u : 1u << 11);
    r.poke("TIM2", 0x2C, arr_leg ? 20u : 99u);
    r.poke("TIM2", 0x38, arr_leg ? 30u : 150u);
    r.poke("TIM2", 0x14, 1);
    r.poke("TIM2", 0x10, 0);
    auto const from = r.now();
    r.poke("TIM2", 0x00, arr_leg ? 0x81u : 1u);
    r.run_until(msi_tick_ps(from, arr_leg ? 25 : 110));
    r.write(sr, 0);
    r.poke("TIM2", arr_leg ? 0x2Cu : 0x38u, arr_leg ? 99u : 30u);
    auto const update = arr_leg ? 42u : 200u;
    CHECK(tim2_due(r) == static_cast<std::int64_t>(msi_tick_ps(from, update)));
    r.run_until(msi_tick_ps(from, update + 30) + 600 * ns);
    CHECK((r.read(sr) & strt) != 0);
  }
}

TEST_CASE(
    "one TRGO falling edge both groups select, given ahead of the machine time, converts the "
    "injected group, then the regular group")
{
  adc_rig const r;
  r.power();
  r.write(cr2, adon | (2u << 28) | (6u << 24) | (2u << 20) | (2u << 16));
  r.tim2_updates(209);
  auto const fall  = r.due("TIM2") + 476'838;
  auto const begin = fall + 218'750;
  r.run_until(fall + 600 * ns);
  CHECK((r.read(sr) & (jstrt | strt)) == jstrt);
  CHECK(r.due() == begin + 16 * cycle);
  r.run_until(begin + 16 * cycle + ns);
  CHECK((r.read(sr) & (jeoc | eoc | strt)) == (jeoc | strt));
  CHECK(r.due() == begin + 32 * cycle);
}
