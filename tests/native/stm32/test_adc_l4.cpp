#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
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
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware = "l476_l4_rcc_ll";
constexpr std::uint64_t    cycle_ps = 250'000;
constexpr std::uint64_t    us       = 1'000'000;
constexpr std::uint64_t    ms       = 1'000 * us;

constexpr std::uint32_t aden     = 1u << 0;
constexpr std::uint32_t adstart  = 1u << 2;
constexpr std::uint32_t adstp    = 1u << 4;
constexpr std::uint32_t advregen = 1u << 28;
constexpr std::uint32_t deeppwd  = 1u << 29;
constexpr std::uint32_t eoc      = 1u << 2;
constexpr std::uint32_t eocie    = 1u << 2;
constexpr std::uint32_t exten_up = 1u << 10;
constexpr std::uint32_t cont     = 1u << 13;
constexpr std::uint32_t vrefen   = 1u << 22;
constexpr std::uint32_t ch17sel  = 1u << 23;
constexpr std::uint32_t ch18sel  = 1u << 24;

constexpr std::uint32_t vdda_mv     = 3000;
constexpr std::uint32_t ts_cal1     = 1037;
constexpr std::uint32_t vrefint_cal = 1654;
constexpr std::uint32_t adc12_irq   = 18;
constexpr std::uint32_t adc3_irq    = 47;

[[nodiscard]] constexpr auto extsel(std::uint32_t code) -> std::uint32_t
{
  return code << 6;
}

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  explicit rig(bool strict = false)
  {
    stm32::board board{};
    board.supply.vdda_mv       = vdda_mv;
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(board, l476rc());
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(l476rc(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    set("RCC", "AHB2ENR", "ADCEN", 1);
    set("RCC", "CCIPR", "ADCSEL", 3);
  }

  [[nodiscard]] auto reg(std::string_view unit, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const* found =
        popcorn::find_register(f->target->peripheral(unit).description(), std::string{name});
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto at(std::string_view unit, std::string_view name) const -> popcorn::address
  {
    return f->target->peripheral(unit).description().base + reg(unit, name).offset;
  }

  [[nodiscard]] auto read(std::string_view unit, std::string_view name) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at(unit, name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view unit, std::string_view name, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at(unit, name), 4, value).has_value());
  }

  [[nodiscard]] auto get(std::string_view unit, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    auto const& fields = reg(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    return (read(unit, name) >> found->lsb) & ((std::uint32_t{1} << found->width) - 1u);
  }

  void
      set(std::string_view unit,
          std::string_view name,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const& fields = reg(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    auto const mask = ((std::uint32_t{1} << found->width) - 1u) << found->lsb;
    write(unit, name, (read(unit, name) & ~mask) | ((value << found->lsb) & mask));
  }

  [[nodiscard]] auto isr(std::string_view field, std::string_view unit = "ADC1") const
      -> std::uint32_t
  {
    return get(unit, "ADC_ISR", field);
  }

  [[nodiscard]] auto cpu_read(std::string_view unit, std::string_view name) const -> std::uint32_t
  {
    auto&      regs  = f->target->peripheral(unit);
    auto const value = f->target->model(unit)->on_read(
        regs, reg(unit, name).offset, 4,
        {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto port(std::string_view unit = "ADC1") const -> stm32::analog_port&
  {
    auto* const found = f->model_of<stm32::analog_port>(unit);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    auto const result = f->run(
        {.max_instructions = icount{1'000'000},
         .wall_budget      = std::chrono::seconds{10},
         .until_time       = vtime{ps}});
    REQUIRE(result.stopped);
    REQUIRE(result.stopped->reason == stop_reason::time);
  }

  void settle() const
  {
    run_until(now() + 100 * us);
  }

  void enable(std::string_view unit = "ADC1") const
  {
    write(unit, "ADC_CR", advregen);
    write(unit, "ADC_CR", advregen | aden);
  }

  void start(std::uint32_t channel, std::string_view unit = "ADC1") const
  {
    write(unit, "ADC_SQR1", channel << 6);
    write(unit, "ADC_CR", advregen | aden | adstart);
  }

  [[nodiscard]] auto convert(std::uint32_t channel, std::string_view unit = "ADC1") const
      -> std::uint32_t
  {
    start(channel, unit);
    settle();
    return cpu_read(unit, "ADC_DR");
  }

  void ends_at(std::uint64_t end, std::string_view unit = "ADC1") const
  {
    run_until((end + cycle_ps - 1) / cycle_ps * cycle_ps - cycle_ps);
    CHECK(isr("EOC", unit) == 0);
    run_until(end);
    CHECK(isr("EOC", unit) == 1);
  }

  [[nodiscard]] auto warnings() const -> std::vector<core::log_record>
  {
    std::vector<core::log_record> out;
    std::ranges::copy_if(logged, std::back_inserter(out), [](core::log_record const& record) {
      return record.kind == core::log_kind::model_warning;
    });
    return out;
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    auto const word = f->target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }

  void clock(std::string_view unit) const
  {
    auto const apb2 = unit == "TIM1" or unit == "TIM8" or unit == "TIM15";
    set("RCC", apb2 ? "APB2ENR" : "APB1ENR1", std::string{unit} + "EN", 1);
  }

  [[nodiscard]] auto timer(std::string_view unit, std::string_view ccr) const -> std::uint64_t
  {
    clock(unit);
    write(unit, "PSC", 3);
    write(unit, "ARR", 999);
    if(ccr.empty())
    {
      write(unit, "CR2", 2u << 4);
    }
    else
    {
      write(unit, ccr, 400);
    }
    write(unit, "EGR", 1);
    write(unit, "SR", 0);
    auto const started = now();
    write(unit, "CR1", 1u);
    return started + (ccr.empty() ? ms : 400 * us);
  }
};

[[nodiscard]] constexpr auto edge_after(std::uint64_t at, std::uint64_t period) -> std::uint64_t
{
  return (at + period - 1) / period * period - at;
}
}

TEST_CASE("RM0351 18.7 and 18.8: ADC1, ADC2, ADC3 and ADC_Common of the L476 attach at reset")
{
  rig const r;
  for(auto const unit : {"ADC1", "ADC2", "ADC3"})
  {
    CAPTURE(unit);
    REQUIRE(r.f->target->model(unit) != nullptr);
    CHECK(r.read(unit, "ADC_CR") == deeppwd);
    CHECK(r.read(unit, "ADC_CFGR") == 0x8000'0000u);
    CHECK(r.read(unit, "ADC_TR1") == 0x0FFF'0000u);
    CHECK(r.read(unit, "ADC_TR2") == 0x00FF'0000u);
    CHECK(r.read(unit, "ADC_TR3") == 0x00FF'0000u);
  }
  REQUIRE(r.f->target->model("ADC_Common") != nullptr);
  CHECK(r.read("ADC_Common", "ADC_CCR") == 0);
  CHECK(r.read("ADC_Common", "ADC_CSR") == 0);
}

TEST_CASE(
    "RM0351 Table 106: DFSDMCFG and SMPPLUS are reserved on the L476, SMP 000 takes 2.5 cycles "
    "with bit 31 of ADC_SMPR1 set, a conversion 15")
{
  rig const r;
  r.write("ADC1", "ADC_CFGR", 0x8000'0000u | 1u << 2);
  CHECK(r.read("ADC1", "ADC_CFGR") == 0x8000'0000u);
  r.write("ADC1", "ADC_SMPR1", 1u << 31);
  r.enable();
  auto const at = r.now();
  r.start(1);
  r.ends_at(at + 15 * cycle_ps);
  CHECK(r.warnings().empty());
}

TEST_CASE("RM0351 18.7.21: the ADC_DIFSEL bit of a converted channel logs once as unmodelled")
{
  rig const r;
  r.write("ADC1", "ADC_DIFSEL", 1u << 2);
  r.enable();
  static_cast<void>(r.convert(1));
  CHECK(r.warnings().empty());
  static_cast<void>(r.convert(2));
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("DIFSEL: differential conversion"));
}

TEST_CASE(
    "channel 1 at 1500 mV with VDDA 3000 mV reads 2048 on ADC1, ADC2 and ADC3, EOCIE raises "
    "ADC1_2 for ADC1 and ADC2 and ADC3 for ADC3")
{
  for(auto const& [unit, irq, other] : {
          std::tuple{"ADC1", adc12_irq, adc3_irq },
          std::tuple{"ADC2", adc12_irq, adc3_irq },
          std::tuple{"ADC3", adc3_irq,  adc12_irq},
  })
  {
    CAPTURE(unit);
    rig const r;
    r.enable(unit);
    r.write(unit, "ADC_IER", eocie);
    r.port(unit).drive(1, 1'500'000);
    r.start(1, unit);
    r.settle();
    CHECK((r.read(unit, "ADC_ISR") & eoc) != 0);
    CHECK(r.pending(irq));
    CHECK_FALSE(r.pending(other));
    CHECK(r.cpu_read(unit, "ADC_DR") == 2048);
  }
}

TEST_CASE(
    "RM0351 18.4.26 OVRMOD 0: with CONT a result finding EOC set sets OVR and is discarded, ADC_DR "
    "keeps the first, a result after the read is discarded while OVR stays set")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_CFGR", 0x8000'0000u | cont);
  r.start(1);
  r.run_until(r.now() + 40 * cycle_ps);
  CHECK(r.isr("OVR") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(r.isr("EOC") == 0);
  r.run_until(r.now() + 40 * cycle_ps);
  CHECK(r.isr("EOC") == 0);
  r.write("ADC1", "ADC_CR", advregen | aden | adstp);
  r.write("ADC1", "ADC_CFGR", 0x8000'0000u);
  r.write("ADC1", "ADC_ISR", 1u << 4);
  CHECK(r.convert(2) == 1024);
}

TEST_CASE(
    "RM0351 Figure 70: ADC3 converts its external channels 1 to 4 and 6 to 13, a drive of 5, 14, "
    "15 or 16 is a config_error")
{
  rig const r;
  r.enable("ADC3");
  CHECK(r.port("ADC3").external_channels(stm32::analog_bank::a) == 0x3FDEu);
  CHECK(r.port("ADC1").external_channels(stm32::analog_bank::a) == 0x1FFFEu);
  r.port("ADC3").drive(13, 750'000);
  CHECK(r.convert(13, "ADC3") == 1024);
  for(auto const channel : {5u, 14u, 15u, 16u})
  {
    CAPTURE(channel);
    CHECK_THROWS_AS(r.port("ADC3").drive(channel, 750'000), popcorn::config_error);
  }
  r.enable("ADC2");
  r.port("ADC2").drive(16, 750'000);
  CHECK(r.convert(16, "ADC2") == 1024);
}

TEST_CASE(
    "RM0351 18.4.11 and 18.8.2: ADC1 reads VREFINT_CAL on channel 0 with VREFEN and TS_CAL1 on 17 "
    "with CH17SEL, ADC3 TS_CAL1 on 17, ADC2 channel 17 and VBAT on 18 read 0")
{
  rig const r;
  r.enable("ADC1");
  r.enable("ADC2");
  r.enable("ADC3");
  CHECK(r.convert(0) == 0);
  CHECK(r.convert(17) == 0);
  r.write("ADC_Common", "ADC_CCR", vrefen);
  CHECK(r.convert(0) == vrefint_cal);
  CHECK(r.convert(17) == 0);
  r.write("ADC_Common", "ADC_CCR", ch17sel | ch18sel);
  CHECK(r.convert(0) == 0);
  CHECK(r.convert(17) == ts_cal1);
  CHECK(r.convert(18) == 0);
  CHECK(r.convert(17, "ADC3") == ts_cal1);
  CHECK(r.convert(17, "ADC2") == 0);
  CHECK(r.convert(0, "ADC2") == 0);
}

TEST_CASE(
    "RM0351 18.8.1: ADC_CSR reads ADC1 ADC_ISR bits 10:0 at 10:0 and ADC2's at 26:16, ADC3 has "
    "no bits, ADRDY clears by writing 1")
{
  rig const r;
  r.enable("ADC3");
  CHECK(r.read("ADC_Common", "ADC_CSR") == 0);
  r.enable("ADC1");
  CHECK(r.get("ADC_Common", "ADC_CSR", "ADRDY_MST") == 1);
  r.enable("ADC2");
  CHECK(r.read("ADC_Common", "ADC_CSR") == (1u | 1u << 16));
  r.start(1, "ADC2");
  r.settle();
  auto const flags = r.read("ADC2", "ADC_ISR");
  CHECK((flags & eoc) != 0);
  CHECK(r.read("ADC_Common", "ADC_CSR") == (1u | (flags & 0x7FFu) << 16));
  r.write("ADC2", "ADC_ISR", 0x7FF);
  CHECK(r.read("ADC2", "ADC_ISR") == 0);
  CHECK(r.read("ADC_Common", "ADC_CSR") == 1u);
}

TEST_CASE(
    "RM0351 Table 111: each timer EXTSEL starts a conversion at its event, waits for the ADC clock "
    "edge, then tLATR 1.5 cycles, inferred")
{
  struct leg
  {
    std::uint32_t    selection;
    std::string_view unit;
    std::string_view ccr;
  };
  for(auto const& [selection, unit, ccr] : {
          leg{0,  "TIM1",  "CCR1"},
          leg{1,  "TIM1",  "CCR2"},
          leg{2,  "TIM1",  "CCR3"},
          leg{3,  "TIM2",  "CCR2"},
          leg{4,  "TIM3",  ""    },
          leg{5,  "TIM4",  "CCR4"},
          leg{7,  "TIM8",  ""    },
          leg{9,  "TIM1",  ""    },
          leg{11, "TIM2",  ""    },
          leg{12, "TIM4",  ""    },
          leg{13, "TIM6",  ""    },
          leg{14, "TIM15", ""    },
          leg{15, "TIM3",  "CCR4"},
  })
  {
    CAPTURE(selection);
    rig const r;
    r.enable();
    r.write("ADC1", "ADC_CFGR", 0x8000'0000u | exten_up | extsel(selection));
    auto const trigger = r.timer(unit, ccr);
    r.start(1);
    r.ends_at(trigger + edge_after(trigger, cycle_ps) + (12 + 8 * 15) * cycle_ps / 8);
    CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 1);
    CHECK(r.warnings().empty());
  }
}

TEST_CASE("a DUAL write other than 0 logs once on ADC_Common, a model_error when strict")
{
  {
    rig const r;
    r.write("ADC_Common", "ADC_CCR", 6u);
    r.write("ADC_Common", "ADC_CCR", 1u);
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].peripheral == "ADC_Common");
    CHECK(logged[0].message.contains("DUAL"));
  }
  rig const r{true};
  CHECK_NOTHROW(r.write("ADC_Common", "ADC_CCR", ch17sel));
  CHECK_THROWS_AS(
      static_cast<void>(r.f->target->mem().write(r.at("ADC_Common", "ADC_CCR"), 4, 6u)),
      popcorn::model_error);
}
