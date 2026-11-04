#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
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
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
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
constexpr std::string_view adc_ll   = "l476_l4_adc_ll";
constexpr std::uint64_t    cycle_ps = 250'000;
constexpr std::uint64_t    us       = 1'000'000;
constexpr std::uint64_t    ms       = 1'000 * us;

constexpr std::uint32_t aden     = 1u << 0;
constexpr std::uint32_t adstart  = 1u << 2;
constexpr std::uint32_t adstp    = 1u << 4;
constexpr std::uint32_t advregen = 1u << 28;
constexpr std::uint32_t deeppwd  = 1u << 29;
constexpr std::uint32_t adcal    = 1u << 31;
constexpr std::uint32_t adcaldif = 1u << 30;
constexpr std::uint32_t addis    = 1u << 1;
constexpr std::uint32_t eoc      = 1u << 2;
constexpr std::uint32_t eocie    = 1u << 2;
constexpr std::uint32_t exten_up = 1u << 10;
constexpr std::uint32_t cont     = 1u << 13;
constexpr std::uint32_t vrefen   = 1u << 22;
constexpr std::uint32_t ch17sel  = 1u << 23;
constexpr std::uint32_t ch18sel  = 1u << 24;
constexpr std::uint32_t jadstart = 1u << 3;
constexpr std::uint32_t jadstp   = 1u << 5;
constexpr std::uint32_t jeoc     = 1u << 5;
constexpr std::uint32_t jeos     = 1u << 6;
constexpr std::uint32_t align    = 1u << 5;
constexpr std::uint32_t jdiscen  = 1u << 20;
constexpr std::uint32_t discen   = 1u << 16;
constexpr std::uint32_t awd1en   = 1u << 23;
constexpr std::uint32_t jawd1en  = 1u << 24;
constexpr std::uint32_t jauto    = 1u << 25;
constexpr std::uint32_t jqdis    = 1u << 31;
constexpr std::uint32_t jqm      = 1u << 21;
constexpr std::uint32_t jqovf    = 1u << 10;
constexpr std::uint32_t jovse    = 1u << 1;
constexpr std::uint32_t enabled  = advregen | aden;

constexpr std::uint32_t vdda_mv     = 3000;
constexpr std::uint32_t ts_cal1     = 1037;
constexpr std::uint32_t vrefint_cal = 1654;
constexpr std::uint32_t adc12_irq   = 18;
constexpr std::uint32_t adc3_irq    = 47;

[[nodiscard]] constexpr auto extsel(std::uint32_t code) -> std::uint32_t
{
  return code << 6;
}

[[nodiscard]] constexpr auto ckmode(std::uint32_t code) -> std::uint32_t
{
  return code << 16;
}

[[nodiscard]] constexpr auto presc(std::uint32_t code) -> std::uint32_t
{
  return code << 18;
}

[[nodiscard]] constexpr auto jsqr(
    std::initializer_list<std::uint32_t> ranks,
    std::uint32_t                        jexten  = 0,
    std::uint32_t                        jextsel = 0) -> std::uint32_t
{
  constexpr std::array<unsigned, 4> lsbs{8, 14, 20, 26};
  auto word = static_cast<std::uint32_t>(ranks.size() - 1) | jextsel << 2 | jexten << 6;
  auto lsb  = lsbs.begin();
  for(auto const channel : ranks)
  {
    word |= channel << *lsb++;
  }
  return word;
}

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  explicit rig(bool strict = false, bool routed = true)
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
    if(routed)
    {
      for(auto const port : {"GPIOA", "GPIOB", "GPIOC", "GPIOF"})
      {
        set("RCC", "AHB2ENR", std::string{port} + "EN", 1);
        write(port, "ASCR", 0xFFFF);
      }
    }
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

  void cpu_write(std::string_view unit, std::string_view name, std::uint32_t value) const
  {
    auto&      regs   = f->target->peripheral(unit);
    auto const offset = reg(unit, name).offset;
    REQUIRE(f->target->model(unit)
                ->on_write(
                    regs, offset, 4, regs.peek(offset), value,
                    {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu})
                .has_value());
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
    run_until(now() + 15 * cycle_ps);
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

  void unpend(std::uint32_t irq) const
  {
    REQUIRE(f->target->mem().write(0xe000'e280u + 4 * (irq / 32), 4, 1u << (irq % 32)).has_value());
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

  [[nodiscard]] auto dues(std::string_view unit = "ADC1") const -> std::vector<std::uint64_t>
  {
    auto const& units = f->target->description().peripherals;
    auto const  owner = static_cast<std::uint32_t>(
        std::ranges::find(units, unit, &popcorn::peripheral_desc::name) - units.begin());
    std::vector<std::uint64_t> out;
    for(auto const& event : f->target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner)
      {
        out.push_back(event.due_ps);
      }
    }
    return out;
  }

  [[nodiscard]] auto last_due(std::string_view unit = "ADC1") const -> std::uint64_t
  {
    auto const& units = f->target->description().peripherals;
    auto const  owner = static_cast<std::uint32_t>(
        std::ranges::find(units, unit, &popcorn::peripheral_desc::name) - units.begin());
    auto latest = std::uint64_t{};
    for(auto const& event : f->target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner)
      {
        latest = std::max(latest, event.due_ps);
      }
    }
    return latest;
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

TEST_CASE("RM0351 Table 111: EXTSEL 6 starts a conversion at a rising edge of EXTI line 11")
{
  rig const r;
  r.set("RCC", "AHB2ENR", "GPIOAEN", 1);
  r.set("GPIOA", "MODER", "MODER11", 0);
  r.enable();
  r.write("ADC1", "ADC_CFGR", 0x8000'0000u | exten_up | extsel(6));
  r.start(1);
  r.run_until(r.now() + 10 * us);
  auto* const pins = r.f->model_of<stm32::gpio_port>("GPIOA");
  REQUIRE(pins != nullptr);
  auto const edge = r.now();
  pins->drive(11, true);
  r.ends_at(edge + edge_after(edge, cycle_ps) + (12 + 8 * 15) * cycle_ps / 8);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 Table 111: EXTSEL 8 tim8_trgo2 and 10 tim1_trgo2 log one warning each, no conversion")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_CFGR", 0x8000'0000u | exten_up | extsel(8));
  r.start(1);
  r.write("ADC1", "ADC_CR", advregen | aden | adstp);
  r.write("ADC1", "ADC_CFGR", 0x8000'0000u | exten_up | extsel(10));
  r.start(1);
  r.settle();
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 2);
  CHECK(logged[0].message.contains("EXTSEL 8: trigger tim8_trgo2"));
  CHECK(logged[1].message.contains("EXTSEL 10: trigger tim1_trgo2"));
  CHECK(r.isr("EOC") == 0);
}

TEST_CASE(
    "DS10198 Table 76: tLATR at CKMODE 01, 10 and 11 is 2, 2.25 and 2.125 cycles of HCLK / 1, 2 "
    "and 4, at CKMODE 00 the next ADC clock edge then 1.5 cycles, inferred")
{
  struct setting
  {
    std::uint32_t mode;
    std::uint32_t prescaler;
    std::uint64_t divider;
    std::uint64_t eighths;
    std::uint64_t offset_ps;
  };
  for(auto const [mode, prescaler, divider, eighths, offset] : {
          setting{0, 0, 1, 12, 0       },
          setting{0, 1, 2, 12, 0       },
          setting{0, 1, 2, 12, cycle_ps},
          setting{1, 0, 1, 16, 0       },
          setting{2, 0, 2, 18, 0       },
          setting{3, 0, 4, 17, 0       },
  })
  {
    CAPTURE(mode);
    CAPTURE(divider);
    CAPTURE(offset);
    rig const r;
    r.write("ADC_Common", "ADC_CCR", ckmode(mode) | presc(prescaler));
    r.enable();
    r.write("ADC1", "ADC_CFGR", 0x8000'0000u | exten_up | extsel(14));
    if(offset != 0)
    {
      r.run_until(r.now() + offset);
    }
    auto const trigger = r.timer("TIM15", "");
    r.start(1);
    auto const period = cycle_ps * divider;
    auto const edge   = mode == 0 ? edge_after(trigger, period) : 0;
    auto const end    = trigger + edge + (eighths + 8 * 15) * period / 8;
    r.run_until(trigger + us);
    CHECK(r.last_due() == end);
    r.ends_at(end);
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

TEST_CASE(
    "DS10198 Table 76 tSTAB, inferred as one 12-bit conversion: ADRDY sets 15 ADC clock cycles "
    "after the ADEN write, 30 at PRESC /2")
{
  for(auto const& [prescaler, divider] : {
          std::pair{0u, 1u},
          std::pair{1u, 2u}
  })
  {
    CAPTURE(divider);
    rig const r;
    r.write("ADC_Common", "ADC_CCR", prescaler << 18);
    r.write("ADC1", "ADC_CR", advregen);
    auto const at = r.now();
    r.write("ADC1", "ADC_CR", advregen | aden);
    r.run_until(at + cycle_ps * divider);
    CHECK(r.isr("ADRDY") == 0);
    r.run_until(at + (15 * divider - 1) * cycle_ps);
    CHECK(r.isr("ADRDY") == 0);
    r.run_until(at + 15 * divider * cycle_ps);
    CHECK(r.isr("ADRDY") == 1);
  }
}

TEST_CASE(
    "ADEN in deep power-down or with ADVREGEN 0 sets no ADRDY, and ADDIS before ADRDY drops the "
    "pending ADRDY")
{
  rig const r;
  r.write("ADC1", "ADC_CR", deeppwd | aden);
  r.settle();
  CHECK(r.isr("ADRDY") == 0);
  r.write("ADC1", "ADC_CR", aden | addis);
  r.write("ADC1", "ADC_CR", aden);
  r.settle();
  CHECK(r.isr("ADRDY") == 0);
  r.write("ADC1", "ADC_CR", aden | addis);
  r.write("ADC1", "ADC_CR", advregen);
  r.write("ADC1", "ADC_CR", advregen | aden);
  r.run_until(r.now() + 5 * cycle_ps);
  r.write("ADC1", "ADC_CR", advregen | aden | addis);
  r.run_until(r.now() + 5 * cycle_ps);
  auto const again = r.now();
  r.write("ADC1", "ADC_CR", advregen | aden);
  r.run_until(again + 14 * cycle_ps);
  CHECK(r.isr("ADRDY") == 0);
  r.run_until(again + 15 * cycle_ps);
  CHECK(r.isr("ADRDY") == 1);
}

TEST_CASE(
    "RM0351 18.4.8: ADCAL stays set for tCAL, 116 ADC clock cycles, DS10198 Table 76, and clears "
    "CALFACT_S, or CALFACT_D with ADCALDIF")
{
  rig const r;
  r.write("ADC1", "ADC_CR", advregen);
  r.write("ADC1", "ADC_CALFACT", 0x007F'007Fu);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", advregen | adcal);
  CHECK(r.get("ADC1", "ADC_CALFACT", "CALFACT_S") == 0);
  CHECK(r.get("ADC1", "ADC_CALFACT", "CALFACT_D") == 0x7F);
  r.write("ADC1", "ADC_CR", advregen);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 1);
  r.run_until(at + 115 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 1);
  r.run_until(at + 116 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 0);
  r.write("ADC1", "ADC_CR", advregen | adcaldif | adcal);
  CHECK(r.get("ADC1", "ADC_CALFACT", "CALFACT_D") == 0);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.9 as corrected by ES0250 2.8.4: ADEN written while ADCAL is set or within 4 ADC "
    "clock cycles after it clears is dropped and logged once, a model_error when strict")
{
  {
    rig const r;
    r.write("ADC1", "ADC_CR", advregen);
    auto const at = r.now();
    r.write("ADC1", "ADC_CR", advregen | adcal);
    r.write("ADC1", "ADC_CR", advregen | adcal | aden);
    CHECK(r.get("ADC1", "ADC_CR", "ADEN") == 0);
    r.run_until(at + 116 * cycle_ps);
    r.write("ADC1", "ADC_CR", advregen | aden);
    CHECK(r.get("ADC1", "ADC_CR", "ADEN") == 0);
    r.run_until(at + 119 * cycle_ps);
    r.write("ADC1", "ADC_CR", advregen | aden);
    CHECK(r.get("ADC1", "ADC_CR", "ADEN") == 0);
    r.settle();
    CHECK(r.isr("ADRDY") == 0);
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("ES0250 2.8.4"));
    r.run_until(at + 200 * cycle_ps);
    auto const late = r.now();
    r.write("ADC1", "ADC_CR", advregen | aden);
    CHECK(r.get("ADC1", "ADC_CR", "ADEN") == 1);
    r.run_until(late + 15 * cycle_ps);
    CHECK(r.isr("ADRDY") == 1);
  }
  rig const r{true};
  r.write("ADC1", "ADC_CR", advregen);
  r.write("ADC1", "ADC_CR", advregen | adcal);
  CHECK_THROWS_AS(
      static_cast<void>(r.f->target->mem().write(r.at("ADC1", "ADC_CR"), 4, advregen | aden)),
      popcorn::model_error);
}

TEST_CASE(
    "RM0351 18.4.8: ADCAL with ADEN 1, DEEPPWD 1 or ADVREGEN 0 is ignored and logged once, a "
    "model_error when strict")
{
  for(auto const setup : {advregen | aden, deeppwd, 0u})
  {
    CAPTURE(setup);
    rig const r;
    r.write("ADC1", "ADC_CR", setup == (advregen | aden) ? advregen : setup);
    r.write("ADC1", "ADC_CR", setup);
    r.write("ADC1", "ADC_CR", setup | adcal);
    CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 0);
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("ADCAL written with ADEN 1, DEEPPWD 1 or ADVREGEN 0"));
  }
  rig const r{true};
  r.write("ADC1", "ADC_CR", 0);
  CHECK_THROWS_AS(
      static_cast<void>(r.f->target->mem().write(r.at("ADC1", "ADC_CR"), 4, adcal)),
      popcorn::model_error);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 0);
}

TEST_CASE(
    "a calibration without an ADC kernel clock waits, then runs its 116 cycles from the ADCSEL "
    "write that selects a clock")
{
  rig const r;
  r.set("RCC", "CCIPR", "ADCSEL", 0);
  r.write("ADC1", "ADC_CR", advregen);
  r.write("ADC1", "ADC_CR", advregen | adcal);
  r.settle();
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 1);
  auto const at = r.now();
  r.set("RCC", "CCIPR", "ADCSEL", 3);
  r.run_until(at + 115 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 1);
  r.run_until(at + 116 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 0);
}

TEST_CASE(
    "checklist 31: ADCRST during a calibration drops its end, a calibration started after it "
    "ends 116 cycles after its own start")
{
  rig const r;
  r.write("ADC1", "ADC_CR", advregen);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", advregen | adcal);
  r.run_until(at + 40 * cycle_ps);
  popcorn::testing::rcc_reset_pulse(*r.f, "AHB2RSTR", "ADCRST");
  CHECK(r.read("ADC1", "ADC_CR") == deeppwd);
  r.write("ADC1", "ADC_CR", advregen);
  auto const again = r.now();
  r.write("ADC1", "ADC_CR", advregen | adcal);
  r.run_until(at + 120 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 1);
  r.run_until(again + 116 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 0);
}

TEST_CASE(
    "checklist 31: ADCRST during a calibration drops its end for a calibration that then waits "
    "for a kernel clock")
{
  rig const r;
  r.write("ADC1", "ADC_CR", advregen);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", advregen | adcal);
  r.run_until(at + 40 * cycle_ps);
  popcorn::testing::rcc_reset_pulse(*r.f, "AHB2RSTR", "ADCRST");
  r.set("RCC", "CCIPR", "ADCSEL", 0);
  r.write("ADC1", "ADC_CR", advregen);
  r.write("ADC1", "ADC_CR", advregen | adcal);
  r.run_until(at + 120 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 1);
  auto const clocked = r.now();
  r.set("RCC", "CCIPR", "ADCSEL", 3);
  r.run_until(clocked + 115 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 1);
  r.run_until(clocked + 116 * cycle_ps);
  CHECK(r.get("ADC1", "ADC_CR", "ADCAL") == 0);
}

TEST_CASE(
    "a snapshot taken during a calibration ends it at the same time once restored and keeps "
    "the ADEN lockout after it")
{
  rig const r;
  r.write("ADC1", "ADC_CR", advregen);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", advregen | adcal);
  r.run_until(at + 50 * cycle_ps);
  auto const saved = r.f->target->snapshot();
  r.run_until(at + 117 * cycle_ps);
  CHECK(
      popcorn::testing::saved_integer(*r.f, "ADC1", "enable_lock_ps")
      == static_cast<std::int64_t>(at + 120 * cycle_ps));
  auto const bytes = core::encode_snapshot(saved.data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  fresh.run_until(at + 115 * cycle_ps);
  CHECK(fresh.get("ADC1", "ADC_CR", "ADCAL") == 1);
  fresh.run_until(at + 117 * cycle_ps);
  CHECK(fresh.get("ADC1", "ADC_CR", "ADCAL") == 0);
  fresh.write("ADC1", "ADC_CR", advregen | aden);
  CHECK(fresh.get("ADC1", "ADC_CR", "ADEN") == 0);
}

TEST_CASE(
    "RM0351 18.4.11 caution: a channel converted with its GPIOx_ASCR switch open reads 0 and logs "
    "once, a model_error at ADSTART when strict, the pads of RM0351 Figures 68 to 70")
{
  struct leg
  {
    std::string_view unit;
    std::uint32_t    channel;
    std::string_view port;
    std::uint32_t    pin;
  };
  for(auto const& [unit, channel, port, pin] : {
          leg{"ADC1", 1,  "GPIOC", 0 },
          leg{"ADC1", 5,  "GPIOA", 0 },
          leg{"ADC2", 16, "GPIOB", 1 },
          leg{"ADC3", 6,  "GPIOF", 3 },
          leg{"ADC3", 13, "GPIOF", 10},
  })
  {
    CAPTURE(unit);
    CAPTURE(channel);
    rig const r;
    r.write(port, "ASCR", 0xFFFFu & ~(1u << pin));
    r.enable(unit);
    r.port(unit).drive(channel, 750'000);
    CHECK(r.convert(channel, unit) == 0);
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(
        logged[0].message.contains(
            std::format("channel {} converted with {}_ASCR.ASC{} clear", channel, port, pin)));
    r.write(port, "ASCR", 0xFFFF);
    CHECK(r.convert(channel, unit) == 1024);
    CHECK(r.warnings().size() == 1);
  }
  rig const r{true};
  r.write("GPIOC", "ASCR", 0);
  r.enable();
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  CHECK_THROWS_AS(
      static_cast<void>(
          r.f->target->mem().write(r.at("ADC1", "ADC_CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
}

TEST_CASE(
    "a snapshot taken during a regular sequence keeps the driven inputs and ends it at the same "
    "time once restored")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_SQR1", 1u | 1u << 6 | 2u << 12);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", advregen | aden | adstart);
  r.run_until(at + 20 * cycle_ps);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  auto const bytes = core::encode_snapshot(r.f->target->snapshot().data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(fresh.port().input(1) == 1'500'000);
  fresh.run_until(at + 29 * cycle_ps);
  CHECK(fresh.isr("EOS") == 0);
  fresh.run_until(at + 30 * cycle_ps);
  CHECK(fresh.isr("EOS") == 1);
  CHECK(fresh.cpu_read("ADC1", "ADC_DR") == 1024);
  CHECK(fresh.get("ADC1", "ADC_CR", "ADSTART") == 0);
}

TEST_CASE(
    "RM0351 18.4.19 and 18.7.16: JADSTART with JEXTEN 0 converts JSQ1 to JSQ4 into ADC_JDR1 to "
    "ADC_JDR4, JEOC at each, JEOS and the JADSTART clear at the last")
{
  rig const r;
  r.enable();
  r.port().drive(1, 375'000);
  r.port().drive(2, 750'000);
  r.port().drive(3, 1'125'000);
  r.port().drive(4, 1'500'000);
  r.write("ADC1", "ADC_JSQR", jsqr({4, 3, 2, 1}));
  CHECK(r.get("ADC1", "ADC_JSQR", "JL") == 3);
  CHECK(r.get("ADC1", "ADC_JSQR", "JSQ1") == 4);
  CHECK(r.get("ADC1", "ADC_JSQR", "JSQ4") == 1);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 14 * cycle_ps);
  CHECK(r.isr("JEOC") == 0);
  r.run_until(at + 15 * cycle_ps);
  CHECK(r.isr("JEOC") == 1);
  CHECK(r.isr("JEOS") == 0);
  CHECK(r.read("ADC1", "ADC_JDR1") == 2048);
  r.run_until(at + 59 * cycle_ps);
  CHECK(r.isr("JEOS") == 0);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 1);
  r.run_until(at + 60 * cycle_ps);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
  CHECK(r.read("ADC1", "ADC_JDR2") == 1536);
  CHECK(r.read("ADC1", "ADC_JDR3") == 1024);
  CHECK(r.read("ADC1", "ADC_JDR4") == 512);
  CHECK(r.isr("EOC") == 0);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.7.1: a CPU read of ADC_JDR1 clears JEOC, a debugger read does not, JEOCIE and "
    "JEOSIE raise ADC1_2")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_IER", jeoc);
  r.write("ADC1", "ADC_JSQR", jsqr({1}));
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.settle();
  CHECK(r.pending(adc12_irq));
  CHECK(r.read("ADC1", "ADC_JDR1") == 2048);
  CHECK(r.isr("JEOC") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 2048);
  CHECK(r.isr("JEOC") == 0);
  CHECK(r.isr("JEOS") == 1);
  r.unpend(adc12_irq);
  CHECK_FALSE(r.pending(adc12_irq));
  r.write("ADC1", "ADC_IER", jeos);
  CHECK(r.pending(adc12_irq));
  r.write("ADC1", "ADC_ISR", jeos);
  r.unpend(adc12_irq);
  CHECK_FALSE(r.pending(adc12_irq));
}

TEST_CASE(
    "RM0351 Table 112: each timer JEXTSEL starts the injected group at its event, waits for the "
    "ADC clock edge, then tLATR 1.5 cycles, inferred, JADSTART stays set")
{
  struct leg
  {
    std::uint32_t    selection;
    std::string_view unit;
    std::string_view ccr;
  };
  for(auto const& [selection, unit, ccr] : {
          leg{0,  "TIM1",  ""    },
          leg{1,  "TIM1",  "CCR4"},
          leg{2,  "TIM2",  ""    },
          leg{3,  "TIM2",  "CCR1"},
          leg{4,  "TIM3",  "CCR4"},
          leg{5,  "TIM4",  ""    },
          leg{7,  "TIM8",  "CCR4"},
          leg{9,  "TIM8",  ""    },
          leg{11, "TIM3",  "CCR3"},
          leg{12, "TIM3",  ""    },
          leg{13, "TIM3",  "CCR1"},
          leg{14, "TIM6",  ""    },
          leg{15, "TIM15", ""    },
  })
  {
    CAPTURE(selection);
    rig const r;
    r.enable();
    r.port().drive(1, 1'500'000);
    r.write("ADC1", "ADC_JSQR", jsqr({1}, 1, selection));
    auto const trigger = r.timer(unit, ccr);
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    auto const end = trigger + edge_after(trigger, cycle_ps) + (12 + 8 * 15) * cycle_ps / 8;
    r.run_until(end - cycle_ps);
    CHECK(r.isr("JEOC") == 0);
    r.run_until(end);
    CHECK(r.isr("JEOC") == 1);
    CHECK(r.read("ADC1", "ADC_JDR1") == 2048);
    CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 1);
    CHECK(r.isr("EOC") == 0);
    CHECK(r.warnings().empty());
  }
}

TEST_CASE(
    "RM0351 Table 112: JEXTSEL 6 starts the injected group at a rising edge of EXTI line 15, "
    "JEXTSEL 8 tim1_trgo2 and 10 tim8_trgo2 log one warning each")
{
  rig const r;
  r.set("GPIOA", "MODER", "MODER15", 0);
  r.enable();
  auto* const pins = r.f->model_of<stm32::gpio_port>("GPIOA");
  REQUIRE(pins != nullptr);
  pins->drive(15, false);
  r.write("ADC1", "ADC_JSQR", jsqr({1}, 1, 6));
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(r.now() + 10 * us);
  CHECK(r.isr("JEOC") == 0);
  auto const edge = r.now();
  pins->drive(15, true);
  r.run_until(edge + edge_after(edge, cycle_ps) + (12 + 8 * 15) * cycle_ps / 8);
  CHECK(r.isr("JEOC") == 1);
  CHECK(r.warnings().empty());
  for(auto const selection : {8u, 10u})
  {
    r.write("ADC1", "ADC_CR", enabled | jadstp);
    r.write("ADC1", "ADC_JSQR", jsqr({1}, 1, selection));
    r.write("ADC1", "ADC_CR", enabled | jadstart);
  }
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 2);
  CHECK(logged[0].message.contains("JEXTSEL 8: trigger tim1_trgo2"));
  CHECK(logged[1].message.contains("JEXTSEL 10: trigger tim8_trgo2"));
}

TEST_CASE(
    "RM0351 18.4.18: a TIM1 TRGO during an injected conversion is ignored, "
    "the next one after it converts")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 3);
  r.write("ADC1", "ADC_JSQR", jsqr({1}, 1, 0));
  r.clock("TIM1");
  r.write("TIM1", "PSC", 3);
  r.write("TIM1", "ARR", 99);
  r.write("TIM1", "CR2", 2u << 4);
  r.write("TIM1", "EGR", 1);
  r.write("TIM1", "SR", 0);
  auto const started = r.now();
  r.write("TIM1", "CR1", 1u);
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  auto const trigger = started + 100 * us;
  auto const end     = trigger + edge_after(trigger, cycle_ps) + (12 + 8 * 653) * cycle_ps / 8;
  r.run_until(end - cycle_ps);
  CHECK(r.isr("JEOC") == 0);
  r.run_until(end);
  CHECK(r.isr("JEOC") == 1);
  r.write("ADC1", "ADC_ISR", jeoc | jeos);
  r.run_until(end + 100 * us);
  CHECK(r.isr("JEOC") == 0);
  r.run_until(started + 300 * us + 170 * us);
  CHECK(r.isr("JEOC") == 1);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.20: with JDISCEN each TIM1 TRGO converts the next JSQ channel, JEOS at the last, "
    "the fourth converts JSQ1 again")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.port().drive(3, 375'000);
  r.write("ADC1", "ADC_CFGR", jqdis | jdiscen);
  r.write("ADC1", "ADC_JSQR", jsqr({1, 2, 3}, 1, 0));
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(trigger + 100 * us);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 2048);
  CHECK(r.read("ADC1", "ADC_JDR2") == 0);
  CHECK(r.isr("JEOS") == 0);
  r.run_until(trigger + ms + 100 * us);
  CHECK(r.read("ADC1", "ADC_JDR2") == 1024);
  CHECK(r.read("ADC1", "ADC_JDR3") == 0);
  CHECK(r.isr("JEOS") == 0);
  r.run_until(trigger + 2 * ms + 100 * us);
  CHECK(r.read("ADC1", "ADC_JDR3") == 512);
  CHECK(r.isr("JEOS") == 1);
  r.write("ADC1", "ADC_ISR", jeoc | jeos);
  r.port().drive(1, 375'000);
  r.run_until(trigger + 3 * ms + 100 * us);
  CHECK(r.read("ADC1", "ADC_JDR1") == 512);
  CHECK(r.isr("JEOC") == 1);
  CHECK(r.isr("JEOS") == 0);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 1);
}

TEST_CASE(
    "with JDISCEN and JEXTEN 0 each JADSTART converts the next JSQ channel and JADSTART clears "
    "after it, inferred")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_CFGR", jqdis | jdiscen);
  r.write("ADC1", "ADC_JSQR", jsqr({1, 2}));
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.settle();
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
  CHECK(r.read("ADC1", "ADC_JDR1") == 2048);
  CHECK(r.read("ADC1", "ADC_JDR2") == 0);
  CHECK(r.isr("JEOS") == 0);
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.settle();
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
  CHECK(r.read("ADC1", "ADC_JDR2") == 1024);
  CHECK(r.isr("JEOS") == 1);
}

TEST_CASE(
    "RM0351 18.7.3: JADSTP drops the injected conversion in progress and clears JADSTART and "
    "JADSTP, the next JADSTART restarts at JSQ1")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_JSQR", jsqr({1, 2}));
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 20 * cycle_ps);
  CHECK(r.read("ADC1", "ADC_JDR1") == 2048);
  r.write("ADC1", "ADC_CR", enabled | jadstp);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTP") == 0);
  r.run_until(at + 40 * cycle_ps);
  CHECK(r.read("ADC1", "ADC_JDR2") == 0);
  CHECK(r.isr("JEOS") == 0);
  r.port().drive(1, 375'000);
  auto const again = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(again + 15 * cycle_ps);
  CHECK(r.read("ADC1", "ADC_JDR1") == 512);
  CHECK(r.read("ADC1", "ADC_JDR2") == 0);
}

TEST_CASE(
    "RM0351 18.7.3: JADSTART with ADEN 0 is ignored and logged once, a model_error when strict")
{
  rig const r;
  r.write("ADC1", "ADC_CR", advregen);
  r.write("ADC1", "ADC_CR", advregen | jadstart);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("JADSTART written with ADEN 0 or ADDIS 1"));
  rig const strict{true};
  strict.write("ADC1", "ADC_CR", advregen);
  CHECK_THROWS_AS(
      static_cast<void>(
          strict.f->target->mem().write(strict.at("ADC1", "ADC_CR"), 4, advregen | jadstart)),
      popcorn::model_error);
  CHECK(strict.get("ADC1", "ADC_CR", "JADSTART") == 0);
}

TEST_CASE(
    "a software JADSTART with JQDIS 0, JOVSE and an injection into a ROVSE sequence with ROVSM 0 "
    "each log once, a model_error when strict")
{
  struct leg
  {
    std::string_view                    what;
    std::function_ref<void(rig const&)> setup;
    std::uint32_t                       cr;
  };
  auto const continued = [](rig const& r) {
    r.write("ADC1", "ADC_CFGR2", 1u);
    r.write("ADC1", "ADC_SMPR1", 7u << 3);
    r.write("ADC1", "ADC_JSQR", jsqr({2}));
    r.start(1);
  };
  auto const queue   = [](rig const& r) { r.write("ADC1", "ADC_CFGR", 0); };
  auto const sampled = [](rig const& r) { r.write("ADC1", "ADC_CFGR2", jovse); };
  for(auto const& [what, setup, cr] : {
          leg{"ROVSM 0",              continued, enabled | adstart | jadstart},
          leg{"JQDIS 0 and JEXTEN 0", queue,     enabled | jadstart          },
          leg{"JOVSE",                sampled,   enabled | jadstart          },
  })
  {
    CAPTURE(what);
    {
      rig const r;
      r.enable();
      setup(r);
      r.write("ADC1", "ADC_CR", cr);
      r.write("ADC1", "ADC_CR", cr);
      auto const logged = r.warnings();
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains(what));
    }
    rig const strict{true};
    strict.enable();
    setup(strict);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(strict.at("ADC1", "ADC_CR"), 4, cr)),
        popcorn::model_error);
  }
}

TEST_CASE(
    "RM0351 18.4.31: an injection into a ROVSE x2 conversion with ROVSM 1 logs nothing, the "
    "dropped rank converts two fresh samples after JEOS")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_CFGR2", 1u | 1u << 5 | 1u << 10);
  r.write("ADC1", "ADC_SMPR1", 7u << 3);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.start(1);
  r.run_until(r.now() + 100 * cycle_ps);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  auto const injected = at + edge_after(at, cycle_ps) + (20 + 8 * 15) * cycle_ps / 8;
  r.run_until(injected);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.read("ADC1", "ADC_JDR1") == 1024);
  r.run_until(injected + cycle_ps);
  CHECK(std::ranges::contains(r.dues(), injected + 2 * 653 * cycle_ps));
  r.run_until(injected + 2 * 653 * cycle_ps);
  CHECK(r.isr("EOC") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.19: JADSTART during the second regular rank drops it, the injected group starts "
    "after tLATRINJ, the dropped rank converts again after JEOS")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.port().drive(3, 375'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 3);
  r.write("ADC1", "ADC_SQR1", 1u | 2u << 6 | 1u << 12);
  r.write("ADC1", "ADC_JSQR", jsqr({3}));
  auto const start = r.now();
  r.write("ADC1", "ADC_CR", enabled | adstart);
  r.run_until(start + 15 * cycle_ps);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 1024);
  r.run_until(start + 100 * cycle_ps);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  auto const injected = at + edge_after(at, cycle_ps) + (20 + 8 * 15) * cycle_ps / 8;
  r.run_until(at + us);
  CHECK(std::ranges::contains(r.dues(), injected));
  r.run_until(injected);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.read("ADC1", "ADC_JDR1") == 512);
  CHECK(r.isr("EOC") == 0);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 1);
  r.run_until(injected + cycle_ps);
  CHECK(std::ranges::contains(r.dues(), injected + 653 * cycle_ps));
  r.run_until(injected + 652 * cycle_ps);
  CHECK(r.isr("EOC") == 0);
  r.run_until(injected + 653 * cycle_ps);
  CHECK(r.isr("EOC") == 1);
  CHECK(r.isr("EOS") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "DS10198 Table 76: tLATRINJ at CKMODE 01, 10 and 11 is 3, 3.25 and 3.125 cycles of HCLK / 1, "
    "2 and 4, at CKMODE 00 the next ADC clock edge then 2.5 cycles, inferred")
{
  struct setting
  {
    std::uint32_t mode;
    std::uint64_t divider;
    std::uint64_t eighths;
  };
  for(auto const [mode, divider, eighths] : {
          setting{0, 1, 20},
          setting{1, 1, 24},
          setting{2, 2, 26},
          setting{3, 4, 25},
  })
  {
    CAPTURE(mode);
    rig const r;
    r.write("ADC_Common", "ADC_CCR", ckmode(mode));
    r.enable();
    r.write("ADC1", "ADC_SMPR1", 7u << 3);
    r.write("ADC1", "ADC_CFGR", jqdis | cont);
    r.write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
    auto const trigger = r.timer("TIM1", "");
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.start(1);
    auto const period = cycle_ps * divider;
    auto const edge   = mode == 0 ? edge_after(trigger, period) : 0;
    auto const end    = trigger + edge + (eighths + 8 * 15) * period / 8;
    r.run_until(trigger + us);
    CHECK(std::ranges::contains(r.dues(), end));
    CHECK(r.isr("JEOC") == 0);
    r.run_until(end);
    CHECK(r.isr("JEOC") == 1);
    CHECK(r.warnings().empty());
  }
}

TEST_CASE(
    "RM0351 18.4.19: a regular start and a TIM1 TRGO regular trigger during the injected group "
    "wait for its end, the sequence starts at JEOS, inferred with no tLATR")
{
  for(auto const hardware : {false, true})
  {
    CAPTURE(hardware);
    rig const r;
    r.enable();
    r.port().drive(1, 1'500'000);
    r.write("ADC1", "ADC_SMPR1", 7u << 6);
    r.write("ADC1", "ADC_SQR1", 1u << 6);
    r.write("ADC1", "ADC_JSQR", jsqr({2}));
    auto at = r.now();
    if(hardware)
    {
      r.write("ADC1", "ADC_CFGR", jqdis | exten_up | extsel(9));
      auto const trigger = r.timer("TIM1", "");
      r.run_until(trigger - 50 * us);
      at = r.now();
      r.write("ADC1", "ADC_CR", enabled | jadstart);
      r.write("ADC1", "ADC_CR", enabled | adstart);
    }
    else
    {
      r.write("ADC1", "ADC_CR", enabled | jadstart);
      r.run_until(at + 10 * cycle_ps);
      r.write("ADC1", "ADC_CR", enabled | adstart);
    }
    auto const injected = at + 653 * cycle_ps;
    r.run_until(injected);
    CHECK(r.isr("JEOS") == 1);
    CHECK(r.isr("EOC") == 0);
    CHECK(std::ranges::contains(r.dues(), injected + 15 * cycle_ps));
    r.run_until(injected + 14 * cycle_ps);
    CHECK(r.isr("EOC") == 0);
    r.run_until(injected + 15 * cycle_ps);
    CHECK(r.isr("EOC") == 1);
    CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
    CHECK(r.warnings().empty());
  }
}

TEST_CASE(
    "RM0351 18.4.18: ADSTART with EXTEN set during the injected group arms the trigger, a TIM1 "
    "TRGO after JEOS starts the regular sequence after tLATR")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | exten_up | extsel(9));
  auto const trigger = r.timer("TIM1", "");
  r.run_until(trigger - 400 * us);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 10 * cycle_ps);
  r.write("ADC1", "ADC_CR", enabled | adstart);
  auto const injected = at + 653 * cycle_ps;
  r.run_until(injected);
  CHECK(r.isr("JEOS") == 1);
  r.run_until(injected + 15 * cycle_ps);
  CHECK(r.isr("EOC") == 0);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 1);
  auto const end = trigger + edge_after(trigger, cycle_ps) + (12 + 8 * 15) * cycle_ps / 8;
  r.run_until(trigger + us);
  CHECK(std::ranges::contains(r.dues(), end));
  r.ends_at(end);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.19: a TIM1 CC1 falling edge held ahead of the machine time that lands inside the "
    "injected group starts the regular sequence at JEOS")
{
  rig const r;
  r.set("RCC", "CFGR", "HPRE", 15);
  r.set("RCC", "CFGR", "PPRE2", 7);
  r.set("RCC", "CR", "MSIRGSEL", 1);
  r.set("RCC", "CR", "MSIRANGE", 11);
  r.clock("TIM1");
  r.write("TIM1", "ARR", 99);
  r.write("TIM1", "EGR", 1);
  r.write("TIM1", "SR", 0);
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2, 2}));
  r.write("ADC1", "ADC_CFGR", jqdis | 2u << 10 | extsel(0));
  r.start(1);
  r.write("TIM1", "EGR", 1u << 1);
  auto const held = r.dues();
  REQUIRE(held.size() == 1);
  r.run_until(held[0] - 10 * us);
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(held[0] + us);
  CHECK(r.isr("JEOS") == 0);
  CHECK(r.isr("EOC") == 0);
  r.run_until(held[0] + 60 * us);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.isr("EOC") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "ADSTP during an injected group that dropped a regular conversion cancels its resume, JADSTP "
    "resumes it at once, inferred")
{
  for(auto const injected_stop : {false, true})
  {
    CAPTURE(injected_stop);
    rig const r;
    r.enable();
    r.port().drive(1, 1'500'000);
    r.write("ADC1", "ADC_SMPR1", 7u << 6);
    r.write("ADC1", "ADC_SQR1", 1u << 6);
    r.write("ADC1", "ADC_JSQR", jsqr({2}));
    r.write("ADC1", "ADC_CR", enabled | adstart);
    r.run_until(r.now() + 5 * cycle_ps);
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.run_until(r.now() + 20 * cycle_ps);
    auto const at = r.now();
    r.write("ADC1", "ADC_CR", enabled | (injected_stop ? jadstp : adstp));
    if(injected_stop)
    {
      CHECK(std::ranges::contains(r.dues(), at + 15 * cycle_ps));
      r.run_until(at + 14 * cycle_ps);
      CHECK(r.isr("EOC") == 0);
      r.run_until(at + 15 * cycle_ps);
      CHECK(r.isr("EOC") == 1);
      CHECK(r.isr("JEOC") == 0);
      continue;
    }
    CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 0);
    r.run_until(at + 700 * cycle_ps);
    CHECK(r.isr("JEOS") == 1);
    CHECK(r.isr("EOC") == 0);
  }
}

TEST_CASE(
    "a snapshot taken during an injected group that dropped a regular conversion resumes the "
    "regular rank once restored")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CR", enabled | adstart);
  r.run_until(r.now() + 5 * cycle_ps);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  auto const injected = at + edge_after(at, cycle_ps) + (20 + 8 * 653) * cycle_ps / 8;
  r.run_until(at + 100 * cycle_ps);
  auto const bytes = core::encode_snapshot(r.f->target->snapshot().data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  fresh.run_until(injected);
  CHECK(fresh.isr("JEOS") == 1);
  fresh.run_until(injected + 15 * cycle_ps);
  CHECK(fresh.isr("EOC") == 1);
  CHECK(fresh.cpu_read("ADC1", "ADC_DR") == 2048);
}

TEST_CASE(
    "a snapshot taken during an injected group with a regular start latched starts the regular "
    "sequence at JEOS once restored")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 10 * cycle_ps);
  r.write("ADC1", "ADC_CR", enabled | adstart);
  auto const injected = at + 653 * cycle_ps;
  r.run_until(at + 100 * cycle_ps);
  auto const bytes = core::encode_snapshot(r.f->target->snapshot().data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  fresh.run_until(injected);
  CHECK(fresh.isr("JEOS") == 1);
  CHECK(std::ranges::contains(fresh.dues(), injected + 15 * cycle_ps));
  fresh.run_until(injected + 15 * cycle_ps);
  CHECK(fresh.isr("EOC") == 1);
  CHECK(fresh.cpu_read("ADC1", "ADC_DR") == 2048);
}

TEST_CASE(
    "RM0351 18.4.19: with JAUTO the injected group converts after the last regular rank with no "
    "delay, inferred, ADSTART clears at EOS and JADSTART reads 0 meanwhile, inferred")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | jauto);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | adstart);
  r.run_until(at + 15 * cycle_ps);
  CHECK(r.isr("EOS") == 1);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 0);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
  CHECK(std::ranges::contains(r.dues(), at + 668 * cycle_ps));
  r.run_until(at + 667 * cycle_ps);
  CHECK(r.isr("JEOS") == 0);
  r.run_until(at + 668 * cycle_ps);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.read("ADC1", "ADC_JDR1") == 1024);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(r.dues().empty());
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.19: with JAUTO and CONT the regular sequence starts again at JEOS, "
    "ADSTP stops both groups")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | jauto | cont);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | adstart);
  r.run_until(at + 668 * cycle_ps);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 1024);
  r.write("ADC1", "ADC_ISR", jeos);
  CHECK(std::ranges::contains(r.dues(), at + 683 * cycle_ps));
  r.run_until(at + 683 * cycle_ps);
  CHECK(r.isr("EOC") == 1);
  CHECK(std::ranges::contains(r.dues(), at + 1336 * cycle_ps));
  r.run_until(at + 700 * cycle_ps);
  r.write("ADC1", "ADC_CR", enabled | adstart | adstp);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 0);
  r.run_until(at + 1400 * cycle_ps);
  CHECK(r.isr("JEOS") == 0);
  CHECK(r.isr("JEOC") == 0);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.19: with JAUTO and a TIM1 TRGO regular trigger each trigger converts the regular "
    "rank then the injected group, ADSTART stays set")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | jauto | exten_up | extsel(9));
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | adstart);
  auto const regular = trigger + edge_after(trigger, cycle_ps) + (12 + 8 * 15) * cycle_ps / 8;
  r.run_until(trigger + us);
  CHECK(std::ranges::contains(r.dues(), regular));
  r.ends_at(regular);
  CHECK(r.isr("JEOS") == 0);
  CHECK(std::ranges::contains(r.dues(), regular + 653 * cycle_ps));
  r.run_until(regular + 653 * cycle_ps);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.read("ADC1", "ADC_JDR1") == 1024);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 1);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "a snapshot taken during the auto-injected group of a JAUTO CONT loop starts the regular "
    "sequence again at JEOS once restored")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | jauto | cont);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | adstart);
  r.run_until(at + 100 * cycle_ps);
  auto const bytes = core::encode_snapshot(r.f->target->snapshot().data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  fresh.run_until(at + 668 * cycle_ps);
  CHECK(fresh.isr("JEOS") == 1);
  CHECK(fresh.cpu_read("ADC1", "ADC_DR") == 2048);
  CHECK(std::ranges::contains(fresh.dues(), at + 683 * cycle_ps));
  fresh.run_until(at + 683 * cycle_ps);
  CHECK(fresh.isr("EOC") == 1);
}

TEST_CASE(
    "RM0351 18.4.19: with JAUTO the one-shot DMA transfer end stops the sequence, the injected "
    "group does not convert and ADSTART clears, inferred")
{
  constexpr std::uint32_t buffer = 0x2000'2000;
  rig const               r;
  r.set("RCC", "AHB1ENR", "DMA1EN", 1);
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | jauto | cont | 1u);
  r.write("DMA1", "CPAR1", static_cast<std::uint32_t>(r.at("ADC1", "ADC_DR")));
  r.write("DMA1", "CMAR1", buffer);
  r.write("DMA1", "CNDTR1", 1);
  r.write("DMA1", "CCR1", 1u | 1u << 7 | 2u << 8 | 2u << 10);
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | adstart);
  r.run_until(at + 15 * cycle_ps + us);
  auto const word = r.f->target->mem().read(buffer, 4);
  REQUIRE(word.has_value());
  CHECK(*word == 2048);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 0);
  r.run_until(at + 100 * cycle_ps);
  CHECK(r.isr("JEOS") == 0);
  CHECK(r.isr("JEOC") == 0);
  CHECK(r.dues().empty());
}

TEST_CASE(
    "JADSTART, JADSTP, JEXTEN set, DISCEN, JDISCEN and ROVSE without JOVSE with JAUTO each "
    "log once, a model_error when strict, JADSTART left 0")
{
  struct leg
  {
    std::string_view what;
    std::uint32_t    cfgr;
    std::uint32_t    cfgr2;
    std::uint32_t    jsqr;
    std::uint32_t    cr;
  };
  for(auto const& [what, cfgr, cfgr2, word, cr] : {
          leg{"JADSTART or JADSTP", jqdis | jauto, 0, jsqr({2}), enabled | jadstart},
          leg{"JADSTART or JADSTP", jqdis | jauto, 0, jsqr({2}), enabled | jadstp},
          leg{"JEXTEN set", jqdis | jauto, 0, jsqr({2}, 1, 0), enabled | adstart},
          leg{"DISCEN or JDISCEN", jqdis | jauto | discen, 0, jsqr({2}), enabled | adstart},
          leg{"DISCEN or JDISCEN", jqdis | jauto | jdiscen, 0, jsqr({2}), enabled | adstart},
          leg{"ROVSE 1 and JOVSE 0", jqdis | jauto, 1, jsqr({2}), enabled | adstart},
  })
  {
    CAPTURE(what);
    CAPTURE(cr);
    CAPTURE(cfgr);
    {
      rig const r;
      r.enable();
      r.write("ADC1", "ADC_JSQR", word);
      r.write("ADC1", "ADC_CFGR", cfgr);
      r.write("ADC1", "ADC_CFGR2", cfgr2);
      r.write("ADC1", "ADC_CR", cr);
      CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
      r.write("ADC1", "ADC_CR", enabled);
      r.write("ADC1", "ADC_CR", cr);
      auto const logged = r.warnings();
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains(what));
    }
    rig const strict{true};
    strict.enable();
    strict.write("ADC1", "ADC_JSQR", word);
    strict.write("ADC1", "ADC_CFGR", cfgr);
    strict.write("ADC1", "ADC_CFGR2", cfgr2);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(strict.at("ADC1", "ADC_CR"), 4, cr)),
        popcorn::model_error);
    CHECK(strict.get("ADC1", "ADC_CR", "JADSTART") == 0);
  }
}

TEST_CASE(
    "JAUTO converts as JAUTO alone: with DISCEN both regular ranks then the group, with JDISCEN "
    "both injected ranks, and with JAUTO set while JADSTART is 1, which RM0351 18.7.4 forbids, a "
    "TIM1 TRGO on JEXTSEL starts no injected group")
{
  {
    rig const r;
    r.enable();
    r.port().drive(1, 1'500'000);
    r.write("ADC1", "ADC_SQR1", 1u | 1u << 6 | 1u << 12);
    r.write("ADC1", "ADC_JSQR", jsqr({1, 1}));
    r.write("ADC1", "ADC_CFGR", jqdis | jauto | discen | jdiscen);
    auto const at = r.now();
    r.write("ADC1", "ADC_CR", enabled | adstart);
    r.run_until(at + 30 * cycle_ps);
    CHECK(r.isr("EOS") == 1);
    CHECK(std::ranges::contains(r.dues(), at + 45 * cycle_ps));
    r.run_until(at + 45 * cycle_ps);
    CHECK(r.isr("JEOC") == 1);
    CHECK(r.isr("JEOS") == 0);
    r.run_until(at + 60 * cycle_ps);
    CHECK(r.isr("JEOS") == 1);
    CHECK(r.read("ADC1", "ADC_JDR2") == 2048);
  }
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  r.write("ADC1", "ADC_CFGR", jqdis);
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.write("ADC1", "ADC_CFGR", jqdis | jauto);
  auto const trigger = r.timer("TIM1", "");
  r.run_until(trigger + 100 * us);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 1);
  CHECK(r.isr("JEOC") == 0);
  CHECK(r.dues().empty());
}

TEST_CASE(
    "RM0351 18.4.19: with JAUTO a regular EXTI11 edge during the auto-injected group starts the "
    "regular rank at JEOS with no tLATR, inferred, then the group again")
{
  rig const r;
  r.set("RCC", "AHB2ENR", "GPIOAEN", 1);
  r.set("GPIOA", "MODER", "MODER11", 0);
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_SQR1", 1u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | jauto | exten_up | extsel(6));
  r.write("ADC1", "ADC_CR", enabled | adstart);
  r.run_until(r.now() + 10 * us);
  auto* const pins = r.f->model_of<stm32::gpio_port>("GPIOA");
  REQUIRE(pins != nullptr);
  auto const edge    = r.now();
  auto const regular = edge + edge_after(edge, cycle_ps) + (12 + 8 * 15) * cycle_ps / 8;
  pins->drive(11, true);
  r.run_until(regular);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
  r.run_until(regular + 100 * cycle_ps);
  pins->drive(11, false);
  pins->drive(11, true);
  auto const injected = regular + 653 * cycle_ps;
  r.run_until(injected);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.isr("EOC") == 0);
  CHECK(std::ranges::contains(r.dues(), injected + 15 * cycle_ps));
  r.run_until(injected + 15 * cycle_ps);
  CHECK(r.isr("EOC") == 1);
  CHECK(std::ranges::contains(r.dues(), injected + 668 * cycle_ps));
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.11: JAUTO checks the injected channels at ADSTART, an open GPIOx_ASCR switch "
    "logs once and converts as 0")
{
  rig const r{false, false};
  r.set("RCC", "AHB2ENR", "GPIOAEN", 1);
  r.write("GPIOA", "ASCR", 1);
  r.enable();
  r.port().drive(5, 1'500'000);
  r.write("ADC1", "ADC_SQR1", 5u << 6);
  r.write("ADC1", "ADC_JSQR", jsqr({2}));
  r.write("ADC1", "ADC_CFGR", jqdis | jauto);
  r.write("ADC1", "ADC_CR", enabled | adstart);
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("channel 2"));
  r.settle();
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.read("ADC1", "ADC_JDR1") == 0);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == 2048);
}

TEST_CASE(
    "ES0250 2.8.1: with JQDIS 0 and JQM 0 a second ADC_JSQR write before JEOS sets JQOVF and is "
    "ignored, a write after JEOS applies at once, RM0351 18.4.21 Figures 82 and 84")
{
  for(auto const queued : {true, false})
  {
    CAPTURE(queued);
    rig const r;
    r.enable();
    r.port().drive(2, 750'000);
    r.port().drive(3, 375'000);
    r.write("ADC1", "ADC_CFGR", queued ? 0u : jqdis);
    auto const first  = jsqr({2}, 1, 0);
    auto const second = jsqr({3}, 1, 0);
    r.cpu_write("ADC1", "ADC_JSQR", first);
    auto const trigger = r.timer("TIM1", "");
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.cpu_write("ADC1", "ADC_JSQR", second);
    CHECK(r.isr("JQOVF") == (queued ? 1u : 0u));
    CHECK(r.read("ADC1", "ADC_JSQR") == (queued ? first : second));
    r.run_until(trigger + 100 * us);
    CHECK(r.isr("JEOS") == 1);
    CHECK(r.cpu_read("ADC1", "ADC_JDR1") == (queued ? 1024u : 512u));
    if(not queued)
    {
      continue;
    }
    r.write("ADC1", "ADC_ISR", jeos | jqovf);
    r.cpu_write("ADC1", "ADC_JSQR", second);
    CHECK(r.isr("JQOVF") == 0);
    CHECK(r.read("ADC1", "ADC_JSQR") == second);
    r.cpu_write("ADC1", "ADC_JSQR", first);
    CHECK(r.isr("JQOVF") == 1);
    r.run_until(trigger + ms + 100 * us);
    CHECK(r.isr("JEOS") == 1);
    CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 512);
    CHECK(r.warnings().empty());
  }
}

TEST_CASE(
    "ES0250 2.8.1: an ADC_JSQR write during the injected conversion sets JQOVF, raises the ADC "
    "interrupt with JQOVFIE and leaves the active context, RM0351 18.4.21 Figure 83")
{
  rig const r;
  r.enable();
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_CFGR", 0);
  r.write("ADC1", "ADC_IER", jqovf);
  auto const first = jsqr({2}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", first);
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(trigger + 10 * us);
  CHECK(r.isr("JEOC") == 0);
  CHECK_FALSE(r.pending(adc12_irq));
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({3}, 1, 0));
  CHECK(r.isr("JQOVF") == 1);
  CHECK(r.pending(adc12_irq));
  CHECK(r.read("ADC1", "ADC_JSQR") == first);
  r.run_until(trigger + 300 * us);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 1024);
}

TEST_CASE(
    "RM0351 18.4.21: JADSTP and ADDIS flush the queue of context, the next ADC_JSQR write "
    "applies at once, Figures 86, 88 and 90")
{
  for(auto const disable : {false, true})
  {
    CAPTURE(disable);
    rig const r;
    r.enable();
    r.write("ADC1", "ADC_CFGR", 0);
    auto const first = jsqr({2}, 1, 0);
    r.cpu_write("ADC1", "ADC_JSQR", first);
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.write("ADC1", "ADC_CR", enabled | (disable ? addis : jadstp));
    CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
    CHECK(r.read("ADC1", "ADC_JSQR") == first);
    auto const next = jsqr({3}, 1, 0);
    r.cpu_write("ADC1", "ADC_JSQR", next);
    CHECK(r.isr("JQOVF") == 0);
    CHECK(r.read("ADC1", "ADC_JSQR") == next);
  }
}

TEST_CASE(
    "checklist 31: ADCRST drops a context waiting in the queue, the first ADC_JSQR write after "
    "it applies at once")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_CFGR", 0);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  popcorn::testing::rcc_reset_pulse(*r.f, "AHB2RSTR", "ADCRST");
  r.enable();
  r.write("ADC1", "ADC_CFGR", 0);
  auto const next = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", next);
  CHECK(r.isr("JQOVF") == 0);
  CHECK(r.read("ADC1", "ADC_JSQR") == next);
}

TEST_CASE(
    "checklist 66: a context waiting in the queue survives a snapshot, the next ADC_JSQR write "
    "sets JQOVF once restored")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_CFGR", 0);
  auto const first = jsqr({2}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", first);
  auto const bytes = core::encode_snapshot(r.f->target->snapshot().data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  fresh.cpu_write("ADC1", "ADC_JSQR", jsqr({3}, 1, 0));
  CHECK(fresh.isr("JQOVF") == 1);
  CHECK(fresh.read("ADC1", "ADC_JSQR") == first);
}

TEST_CASE(
    "RM0351 18.7.4: a set or reset of JQDIS flushes the queue and clears ADC_JSQR, the next write "
    "applies at once")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_CFGR", 0);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  r.write("ADC1", "ADC_CFGR", jqdis);
  CHECK(r.read("ADC1", "ADC_JSQR") == 0);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  r.write("ADC1", "ADC_CFGR", 0);
  CHECK(r.read("ADC1", "ADC_JSQR") == 0);
  auto const next = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", next);
  CHECK(r.isr("JQOVF") == 0);
  CHECK(r.read("ADC1", "ADC_JSQR") == next);
}

TEST_CASE(
    "RM0351 18.4.21: with JQM 0 and JDISCEN the first trigger consumes the queue, a write then "
    "waits and becomes the context at JEOS")
{
  rig const r;
  r.enable();
  r.port().drive(2, 750'000);
  r.port().drive(3, 375'000);
  r.write("ADC1", "ADC_CFGR", jdiscen);
  auto const first  = jsqr({2, 3}, 1, 0);
  auto const second = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", first);
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(trigger + 100 * us);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 1024);
  CHECK(r.isr("JEOS") == 0);
  r.cpu_write("ADC1", "ADC_JSQR", second);
  CHECK(r.isr("JQOVF") == 0);
  CHECK(r.read("ADC1", "ADC_JSQR") == first);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  CHECK(r.isr("JQOVF") == 1);
  r.run_until(trigger + ms + 100 * us);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_JDR2") == 512);
  CHECK(r.read("ADC1", "ADC_JSQR") == second);
}

TEST_CASE(
    "RM0351 18.4.21: with JQM 0 and JDISCEN an ADC_JSQR write during the first conversion waits "
    "for JEOS, the sequence keeps its own ranks")
{
  rig const r;
  r.enable();
  r.port().drive(2, 750'000);
  r.port().drive(3, 375'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 6);
  r.write("ADC1", "ADC_CFGR", jdiscen);
  auto const first  = jsqr({2, 3}, 1, 0);
  auto const second = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", first);
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(trigger + 10 * cycle_ps);
  CHECK(r.isr("JEOC") == 0);
  r.cpu_write("ADC1", "ADC_JSQR", second);
  CHECK(r.isr("JQOVF") == 0);
  CHECK(r.read("ADC1", "ADC_JSQR") == first);
  r.run_until(trigger + 300 * us);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 1024);
  CHECK(r.isr("JEOS") == 0);
  r.run_until(trigger + ms + 300 * us);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_JDR2") == 512);
  CHECK(r.read("ADC1", "ADC_JSQR") == second);
}

TEST_CASE(
    "checklist 43: a debugger ADC_JSQR write with a context waiting stores the word, no JQOVF, on "
    "an empty JQM 1 queue it starts nothing, a strict software JADSTART leaves JADSTART 0")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_CFGR", 0);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  auto const probe = jsqr({3}, 1, 0);
  r.write("ADC1", "ADC_JSQR", probe);
  CHECK(r.isr("JQOVF") == 0);
  CHECK(r.read("ADC1", "ADC_JSQR") == probe);
  rig const empty;
  empty.enable();
  empty.port().drive(2, 750'000);
  empty.write("ADC1", "ADC_CFGR", jqm);
  auto const trigger = empty.timer("TIM1", "");
  empty.write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  empty.write("ADC1", "ADC_CR", enabled | jadstart);
  CHECK(empty.get("ADC1", "ADC_CR", "JADSTART") == 1);
  empty.run_until(trigger + 100 * us);
  CHECK(empty.isr("JEOS") == 0);
  rig const strict{true};
  strict.enable();
  strict.write("ADC1", "ADC_CFGR", 0);
  CHECK_THROWS_AS(
      static_cast<void>(
          strict.f->target->mem().write(strict.at("ADC1", "ADC_CR"), 4, enabled | jadstart)),
      popcorn::model_error);
  CHECK(strict.get("ADC1", "ADC_CR", "JADSTART") == 0);
}

TEST_CASE(
    "RM0351 18.4.21: with JQM 1 the queue empties at JEOS, ADC_JSQR reads 0, triggers are ignored "
    "until the next write, which applies at once, Figure 85")
{
  rig const r;
  r.enable();
  r.port().drive(2, 750'000);
  r.port().drive(3, 375'000);
  r.write("ADC1", "ADC_CFGR", jqm);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(trigger + 100 * us);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 1024);
  CHECK(r.read("ADC1", "ADC_JSQR") == 0);
  r.write("ADC1", "ADC_ISR", jeos);
  r.run_until(trigger + ms + 100 * us);
  CHECK(r.isr("JEOS") == 0);
  auto const third = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", third);
  CHECK(r.read("ADC1", "ADC_JSQR") == third);
  CHECK(r.isr("JQOVF") == 0);
  r.run_until(trigger + 2 * ms + 100 * us);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 512);
  CHECK(r.warnings().empty());
}

TEST_CASE(
    "RM0351 18.4.21: with JQM 1 the queue holds two contexts, a third write sets JQOVF, JEOS "
    "applies the next one, Figures 80 and 82")
{
  rig const r;
  r.enable();
  r.port().drive(2, 750'000);
  r.port().drive(3, 375'000);
  r.write("ADC1", "ADC_CFGR", jqm);
  auto const first  = jsqr({2}, 1, 0);
  auto const second = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", first);
  r.cpu_write("ADC1", "ADC_JSQR", second);
  CHECK(r.isr("JQOVF") == 0);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2, 3}, 1, 0));
  CHECK(r.isr("JQOVF") == 1);
  CHECK(r.read("ADC1", "ADC_JSQR") == first);
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(trigger + 100 * us);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 1024);
  CHECK(r.read("ADC1", "ADC_JSQR") == second);
  r.run_until(trigger + ms + 100 * us);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 512);
  CHECK(r.read("ADC1", "ADC_JSQR") == 0);
}

TEST_CASE(
    "RM0351 18.4.21: with JQM 1 and JDISCEN the first trigger consumes the queue, the last one "
    "changes the context")
{
  rig const r;
  r.enable();
  r.port().drive(2, 750'000);
  r.port().drive(3, 375'000);
  r.write("ADC1", "ADC_CFGR", jqm | jdiscen);
  auto const first  = jsqr({2, 3}, 1, 0);
  auto const second = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", first);
  r.cpu_write("ADC1", "ADC_JSQR", second);
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(trigger + 100 * us);
  CHECK(r.cpu_read("ADC1", "ADC_JDR1") == 1024);
  CHECK(r.isr("JEOS") == 0);
  CHECK(r.read("ADC1", "ADC_JSQR") == first);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  CHECK(r.isr("JQOVF") == 0);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  CHECK(r.isr("JQOVF") == 1);
  r.run_until(trigger + ms + 100 * us);
  CHECK(r.isr("JEOS") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_JDR2") == 512);
  CHECK(r.read("ADC1", "ADC_JSQR") == second);
}

TEST_CASE(
    "RM0351 18.4.21: with JQM 1 JADSTP and ADDIS empty the queue, ADC_JSQR reads 0, the next "
    "write applies at once, Figures 89 and 91")
{
  for(auto const disable : {false, true})
  {
    CAPTURE(disable);
    rig const r;
    r.enable();
    r.write("ADC1", "ADC_CFGR", jqm);
    r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
    r.cpu_write("ADC1", "ADC_JSQR", jsqr({3}, 1, 0));
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.write("ADC1", "ADC_CR", enabled | (disable ? addis : jadstp));
    CHECK(r.read("ADC1", "ADC_JSQR") == 0);
    auto const next = jsqr({3, 2}, 1, 0);
    r.cpu_write("ADC1", "ADC_JSQR", next);
    CHECK(r.isr("JQOVF") == 0);
    CHECK(r.read("ADC1", "ADC_JSQR") == next);
  }
}

TEST_CASE(
    "checklist 66: two contexts in a JQM 1 queue survive a snapshot, JEOS applies the second "
    "once restored")
{
  rig const r;
  r.enable();
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_CFGR", jqm);
  auto const second = jsqr({3}, 1, 0);
  r.cpu_write("ADC1", "ADC_JSQR", jsqr({2}, 1, 0));
  r.cpu_write("ADC1", "ADC_JSQR", second);
  auto const trigger = r.timer("TIM1", "");
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  auto const bytes = core::encode_snapshot(r.f->target->snapshot().data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  fresh.run_until(trigger + 100 * us);
  CHECK(fresh.cpu_read("ADC1", "ADC_JDR1") == 1024);
  CHECK(fresh.read("ADC1", "ADC_JSQR") == second);
}

TEST_CASE("RM0351 18.4.26: ALIGN 1 left-aligns ADC_JDR1, 2048 reads 0x8000")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_CFGR", jqdis | align);
  r.write("ADC1", "ADC_JSQR", jsqr({1}));
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.settle();
  CHECK(r.read("ADC1", "ADC_JDR1") == 0x8000);
}

TEST_CASE(
    "RM0351 18.4.31: ROVSE oversamples the regular channels only, an injected conversion takes "
    "one conversion time and ADC_JDR1 reads the plain code")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_CFGR2", 1u | 3u << 2);
  r.write("ADC1", "ADC_JSQR", jsqr({1}));
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 14 * cycle_ps);
  CHECK(r.isr("JEOC") == 0);
  r.run_until(at + 15 * cycle_ps);
  CHECK(r.isr("JEOC") == 1);
  CHECK(r.read("ADC1", "ADC_JDR1") == 2048);
}

TEST_CASE(
    "ES0250 2.8.2: an ADC_JSQR write with JADSTART 1 and JQDIS 1 logs once, "
    "a model_error when strict")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig const r{strict};
    r.enable();
    r.write("ADC1", "ADC_SMPR1", 7u << 3);
    r.write("ADC1", "ADC_JSQR", jsqr({1}));
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    if(strict)
    {
      CHECK_THROWS_AS(
          static_cast<void>(r.f->target->mem().write(r.at("ADC1", "ADC_JSQR"), 4, jsqr({2}))),
          popcorn::model_error);
      continue;
    }
    r.write("ADC1", "ADC_JSQR", jsqr({2}));
    r.write("ADC1", "ADC_JSQR", jsqr({3}));
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("ES0250 2.8.2"));
    CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 1);
  }
}

TEST_CASE("RM0351 18.4.29: JAWD1EN guards the injected results with AWD1, AWD1EN alone does not")
{
  for(auto const& [config, flagged] : {
          std::pair{jqdis | jawd1en, 1u},
          std::pair{jqdis | awd1en,  0u},
  })
  {
    CAPTURE(config);
    rig const r;
    r.enable();
    r.port().drive(1, 1'500'000);
    r.write("ADC1", "ADC_TR1", 1000u << 16);
    r.write("ADC1", "ADC_CFGR", config);
    r.write("ADC1", "ADC_JSQR", jsqr({1}));
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.settle();
    CHECK(r.isr("JEOC") == 1);
    CHECK(r.isr("AWD1") == flagged);
  }
}

TEST_CASE(
    "checklist 31: ADCRST during an injected conversion drops its end, no flag and no "
    "result past it")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_JSQR", jsqr({1}));
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 5 * cycle_ps);
  popcorn::testing::rcc_reset_pulse(*r.f, "AHB2RSTR", "ADCRST");
  r.run_until(at + 40 * cycle_ps);
  CHECK(r.read("ADC1", "ADC_ISR") == 0);
  CHECK(r.read("ADC1", "ADC_JDR1") == 0);
  CHECK(r.read("ADC1", "ADC_DR") == 0);
}

TEST_CASE(
    "checklist 31: an injected falling edge pending ahead of the machine time converts, "
    "ADCRST drops it")
{
  constexpr std::uint64_t tick = 2'000'000;
  for(auto const reset : {false, true})
  {
    CAPTURE(reset);
    rig const r;
    r.port().drive(1, 1'500'000);
    r.write("ADC1", "ADC_CR", advregen);
    r.write("ADC1", "ADC_CR", enabled);
    r.write("ADC1", "ADC_JSQR", jsqr({1}, 2, 1));
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.set("RCC", "CFGR", "PPRE2", 7);
    r.clock("TIM1");
    r.write("TIM1", "ARR", 6);
    r.write("TIM1", "CCR4", 1);
    r.write("TIM1", "EGR", 1);
    r.write("TIM1", "SR", 0);
    auto const from = r.now();
    r.set("TIM1", "CR1", "CEN", 1);
    r.run_until(from + tick + tick / 2);
    if(reset)
    {
      popcorn::testing::rcc_reset_pulse(*r.f, "AHB2RSTR", "ADCRST");
      r.write("ADC1", "ADC_CR", advregen);
      r.write("ADC1", "ADC_CR", enabled);
      r.write("ADC1", "ADC_JSQR", jsqr({1}, 2, 1));
      r.write("ADC1", "ADC_CR", enabled | jadstart);
    }
    r.run_until(from + 6 * tick);
    CHECK(r.isr("JEOC") == (reset ? 0u : 1u));
  }
}

TEST_CASE(
    "checklist 66: an injected falling edge pending ahead of the machine time survives a "
    "snapshot, it and the next edge convert at the times of the run without one")
{
  constexpr std::uint64_t tick = 2'000'000;
  auto const              pend = [](rig const& r) {
    r.port().drive(1, 1'500'000);
    r.write("ADC1", "ADC_CR", advregen);
    r.write("ADC1", "ADC_CR", enabled);
    r.write("ADC1", "ADC_JSQR", jsqr({1}, 2, 1));
    r.write("ADC1", "ADC_CR", enabled | jadstart);
    r.set("RCC", "CFGR", "PPRE2", 7);
    r.clock("TIM1");
    r.write("TIM1", "ARR", 6);
    r.write("TIM1", "CCR4", 1);
    r.write("TIM1", "EGR", 1);
    r.write("TIM1", "SR", 0);
    auto const from = r.now();
    r.set("TIM1", "CR1", "CEN", 1);
    return from;
  };
  auto const rises = [](rig const& r, std::uint64_t from, std::uint64_t to) {
    for(auto t = from; t <= to; t += cycle_ps / 8)
    {
      r.run_until(t);
      if(r.isr("JEOC") != 0)
      {
        r.write("ADC1", "ADC_ISR", jeoc | jeos);
        return t;
      }
    }
    return std::uint64_t{};
  };
  rig const  reference;
  auto const from  = pend(reference);
  auto const first = rises(reference, from + tick + tick / 2, from + 6 * tick);
  auto const next  = rises(reference, first + cycle_ps, first + 9 * tick);
  REQUIRE(first != 0);
  REQUIRE(next != 0);
  rig const snapped;
  CHECK(pend(snapped) == from);
  snapped.run_until(from + tick + tick / 2);
  auto const bytes = core::encode_snapshot(snapped.f->target->snapshot().data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(rises(fresh, from + tick + tick / 2, from + 6 * tick) == first);
  CHECK(rises(fresh, first + cycle_ps, first + 9 * tick) == next);
}

TEST_CASE(
    "two TIM1 compare falling edges ahead of the machine time each start a conversion, on the "
    "regular and on the injected group")
{
  for(auto const injected : {false, true})
  {
    CAPTURE(injected);
    rig const r;
    r.set("RCC", "CR", "MSIRGSEL", 1);
    r.set("RCC", "CR", "MSIRANGE", 11);
    r.set("RCC", "CFGR", "HPRE", 15);
    r.set("RCC", "CFGR", "PPRE2", 7);
    r.clock("TIM1");
    r.write("TIM1", "ARR", 99);
    r.write("TIM1", "EGR", 1);
    r.write("TIM1", "SR", 0);
    r.enable();
    r.port().drive(1, 1'500'000);
    if(injected)
    {
      r.write("ADC1", "ADC_JSQR", jsqr({1}, 2, 1));
      r.write("ADC1", "ADC_CR", enabled | jadstart);
    }
    else
    {
      r.write("ADC1", "ADC_CFGR", jqdis | 2u << 10 | extsel(0));
      r.start(1);
    }
    auto const              event = injected ? 1u << 4 : 1u << 1;
    auto const              flag  = injected ? "JEOC" : "EOC";
    auto const              data  = injected ? "ADC_JDR1" : "ADC_DR";
    constexpr std::uint64_t tick  = 85'333'334;
    auto const              first = r.now();
    r.write("TIM1", "EGR", event);
    r.run_until(first + 40 * us);
    auto const second = r.now();
    r.write("TIM1", "EGR", event);
    r.run_until(first + tick + 10 * us);
    CHECK(r.isr(flag) == 1);
    CHECK(r.cpu_read("ADC1", data) == 2048);
    CHECK(r.isr(flag) == 0);
    r.run_until(second + tick + 10 * us);
    CHECK(r.isr(flag) == 1);
  }
}

TEST_CASE("ADDIS during an injected conversion drops it and clears JADSTART, no flag past its end")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.write("ADC1", "ADC_JSQR", jsqr({1}));
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 5 * cycle_ps);
  r.write("ADC1", "ADC_CR", enabled | addis);
  CHECK(r.get("ADC1", "ADC_CR", "JADSTART") == 0);
  r.run_until(at + 40 * cycle_ps);
  CHECK(r.isr("JEOC") == 0);
  CHECK(r.isr("EOC") == 0);
}

TEST_CASE("a snapshot taken during an injected sequence ends it at the same time once restored")
{
  rig const r;
  r.enable();
  r.port().drive(1, 1'500'000);
  r.port().drive(2, 750'000);
  r.write("ADC1", "ADC_JSQR", jsqr({1, 2}));
  auto const at = r.now();
  r.write("ADC1", "ADC_CR", enabled | jadstart);
  r.run_until(at + 20 * cycle_ps);
  auto const saved = r.f->target->snapshot();
  auto const bytes = core::encode_snapshot(saved.data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  fresh.run_until(at + 29 * cycle_ps);
  CHECK(fresh.isr("JEOS") == 0);
  fresh.run_until(at + 30 * cycle_ps);
  CHECK(fresh.isr("JEOS") == 1);
  CHECK(fresh.read("ADC1", "ADC_JDR1") == 2048);
  CHECK(fresh.read("ADC1", "ADC_JDR2") == 1024);
  CHECK(fresh.get("ADC1", "ADC_CR", "JADSTART") == 0);
}

namespace
{
struct firmware_result
{
  std::vector<std::uint32_t>    log;
  std::vector<core::log_record> warnings;
};

auto run_adc_ll(std::string_view entry) -> firmware_result
{
  stm32::board board{};
  board.supply.vdda_mv = vdda_mv;
  auto options         = popcorn::testing::stm32_options();
  options.models       = stm32::factories(board, l476rc());
  auto const      f    = std::make_unique<stm32_fixture>(l476rc(), adc_ll, std::move(options));
  firmware_result out;
  auto const      sink = f->target->add_log_sink([&out](core::log_record const& record) {
    if(record.kind == core::log_kind::model_warning)
    {
      out.warnings.push_back(record);
    }
  });
  auto* const     port = f->model_of<stm32::analog_port>("ADC1");
  REQUIRE(port != nullptr);
  port->drive(5, 1'500'000);
  if(entry != "main")
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(adc_ll, "main")}).reason
        == stop_reason::address);
    f->set("pc", sym(adc_ll, entry));
  }
  auto const stopped = f->run_to_stop({.max_instructions = icount{1'000'000}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  out.log = f->log();
  return out;
}
}

TEST_CASE(
    "LL firmware: calibration, ADEN after LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES, PA0 with ASCR at "
    "1500 mV reads 2048, VREFINT reads VREFINT_CAL, TS reads TS_CAL1, VREF+ computes to 3000 mV")
{
  auto const run = run_adc_ll("main");
  CHECK(run.log == std::vector<std::uint32_t>{0, 1, 2048, vrefint_cal, ts_cal1, 3000});
  CHECK(run.warnings.empty());
}

TEST_CASE(
    "LL firmware, ES0250 2.8.4: LL_ADC_Enable right after the calibration leaves the ADC disabled "
    "with no ADRDY, logged once")
{
  auto const run = run_adc_ll("aden_at_once");
  CHECK(run.log == std::vector<std::uint32_t>{0, 0});
  REQUIRE(run.warnings.size() == 1);
  CHECK(run.warnings[0].message.contains("ES0250 2.8.4"));
}

TEST_CASE(
    "LL firmware: LL_ADC_INJ_SetTrigAuto converts PA0 then VREFINT into ADC_JDR1 after one "
    "LL_ADC_REG_StartConversion, both groups idle at JEOS")
{
  auto const run = run_adc_ll("auto_injected");
  CHECK(run.log == std::vector<std::uint32_t>{2048, 1654, 0, 0});
  CHECK(run.warnings.empty());
}

TEST_CASE(
    "LL firmware: LL_ADC_INJ_StartConversion with a software trigger converts PA0 into "
    "ADC_JDR1, its read clears JEOC and JADSTART is clear")
{
  auto const run = run_adc_ll("injected");
  CHECK(run.log == std::vector<std::uint32_t>{2048, 0, 0});
  CHECK(run.warnings.empty());
}
