#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <ranges>
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
using popcorn::testing::no_reset;
using popcorn::testing::no_reset_t;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;
using popcorn::testing::u585;

constexpr std::uint32_t aden      = 1u << 0;
constexpr std::uint32_t addis     = 1u << 1;
constexpr std::uint32_t adstart   = 1u << 2;
constexpr std::uint32_t adstp     = 1u << 4;
constexpr std::uint32_t advregen  = 1u << 28;
constexpr std::uint32_t adcal     = 1u << 31;
constexpr std::uint32_t adrdy     = 1u << 0;
constexpr std::uint32_t eocal     = 1u << 11;
constexpr std::uint32_t ldordy    = 1u << 12;
constexpr std::uint32_t autoff    = 1u << 0;
constexpr std::uint32_t irq       = 113;
constexpr std::uint64_t us        = 1'000'000;
constexpr std::uint64_t cycle     = 62'500;
constexpr std::uint64_t ldo       = 25 * us;
constexpr std::uint64_t tcal      = 123 * cycle;
constexpr std::uint64_t tstab     = 17 * cycle;
constexpr std::uint64_t tdis      = 6 * cycle;
constexpr std::uint64_t early     = 300'000;
constexpr std::uint64_t latency   = 4 * cycle;
constexpr std::uint32_t cont      = 1u << 13;
constexpr std::uint32_t discen    = 1u << 16;
constexpr std::uint32_t chselrmod = 1u << 21;
constexpr std::uint32_t scandir   = 1u << 4;
constexpr std::uint32_t align     = 1u << 5;
constexpr std::uint32_t ovrmod    = 1u << 12;
constexpr std::uint32_t eoc       = 1u << 2;
constexpr std::uint32_t eos       = 1u << 3;
constexpr std::uint32_t eosmp     = 1u << 1;
constexpr std::uint32_t ovr       = 1u << 4;
constexpr std::uint32_t pa4       = 9;
constexpr std::uint32_t pa5       = 10;
constexpr std::uint32_t pa6       = 11;

[[nodiscard]] constexpr auto halves(std::uint64_t count) -> std::uint64_t
{
  return count * cycle / 2;
}

[[nodiscard]] constexpr auto code_of(std::uint64_t microvolts) -> std::uint32_t
{
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(microvolts * 4096 / 3'000'000, 4095));
}

auto options_with(popcorn::part_description const& description, bool strict)
    -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.models             = stm32::factories(stm32::board{}, description);
  options.strict_peripherals = strict;
  return options;
}

struct adc4_fixture : stm32_fixture
{
  explicit adc4_fixture(
      popcorn::part_description const& description = u575(),
      bool                             strict      = false,
      bool                             hsi         = true)
      : stm32_fixture{description, "rcc_ll", options_with(description, strict)}
  {
    set("pc", sym("rcc_ll", "idle"));
    put("RCC", "RCC_AHB3ENR", "ADC4EN", 1);
    if(hsi)
    {
      put("RCC", "RCC_CR", "HSION", 1);
      run_until(now() + 200 * us);
      REQUIRE(field("RCC", "RCC_CR", "HSIRDY") == 1);
    }
    put("RCC", "RCC_CCIPR3", "ADCDACSEL", 4);
  }

  explicit adc4_fixture(no_reset_t)
      : stm32_fixture{no_reset, u575(), "rcc_ll", options_with(u575(), false)}
  {}

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> popcorn::address
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(target->mem().write(at(peripheral, reg), 4, value).has_value());
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    write("ADC4", reg, value);
  }

  void
      put(std::string_view peripheral,
          std::string_view reg,
          std::string_view name,
          std::uint32_t    value) const
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = ((std::uint64_t{1} << bits->width) - 1u) << bits->lsb;
    auto const old  = target->mem().read(at(peripheral, reg), 4);
    REQUIRE(old.has_value());
    write(
        peripheral, reg,
        static_cast<std::uint32_t>((*old & ~mask) | ((std::uint64_t{value} << bits->lsb) & mask)));
  }

  [[nodiscard]] auto read(std::string_view reg) const -> std::uint32_t
  {
    auto const value = target->mem().read(at("ADC4", reg), 4);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto isr(std::string_view name) const -> std::uint32_t
  {
    return field("ADC4", "ISR", name);
  }

  [[nodiscard]] auto cr(std::string_view name) const -> std::uint32_t
  {
    return field("ADC4", "CR", name);
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

  [[nodiscard]] auto dues() const -> std::vector<std::uint64_t>
  {
    auto const& units = target->description().peripherals;
    auto const  owner = static_cast<std::uint32_t>(
        std::ranges::find(units, std::string_view{"ADC4"}, &popcorn::peripheral_desc::name)
        - units.begin());
    std::vector<std::uint64_t> out;
    for(auto const& event : target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner)
      {
        out.push_back(event.due_ps);
      }
    }
    std::ranges::sort(out);
    return out;
  }

  [[nodiscard]] auto regulate() const -> std::uint64_t
  {
    write("CR", advregen);
    auto const ready = now() + ldo;
    run_until(ready);
    REQUIRE(isr("LDORDY") == 1);
    return ready;
  }

  [[nodiscard]] auto calibrate() const -> std::uint64_t
  {
    static_cast<void>(regulate());
    write("CR", advregen | adcal);
    auto const done = now() + tcal;
    run_until(done);
    REQUIRE(cr("ADCAL") == 0);
    return done;
  }

  [[nodiscard]] auto
      cpu_read(std::string_view reg, popcorn::initiator by = popcorn::initiator::cpu) const
      -> std::uint32_t
  {
    auto&       regs  = target->peripheral("ADC4");
    auto const* found = popcorn::find_register(regs.description(), reg);
    REQUIRE(found != nullptr);
    auto const value = target->model("ADC4")->on_read(
        regs, found->offset, 4, {.pc = 0, .now = icount{0}, .by = by});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto all_dues() const -> std::vector<std::uint64_t>
  {
    std::vector<std::uint64_t> out;
    for(auto const& event : target->snapshot().data().events.pending)
    {
      out.push_back(event.due_ps);
    }
    return out;
  }

  [[nodiscard]] auto port() const -> stm32::analog_port&
  {
    auto* const found = model_of<stm32::analog_port>("ADC4");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto start() const -> std::uint64_t
  {
    auto const at = now();
    write("CR", advregen | aden | adstart);
    return at + latency;
  }

  [[nodiscard]] auto enable() const -> std::uint64_t
  {
    static_cast<void>(regulate());
    write("CR", advregen | aden);
    auto const ready = now() + tstab;
    run_until(ready);
    REQUIRE(isr("ADRDY") == 1);
    return ready;
  }
};

auto recorder(adc4_fixture const& f, std::vector<core::log_record>& logged)
{
  return f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
}

void nothing(adc4_fixture const&)
{}

void check_refused(
    std::uint32_t    before,
    std::uint32_t    value,
    std::string_view reg,
    std::string_view phrase,
    std::uint32_t    cr_before         = advregen,
    void (*setup)(adc4_fixture const&) = nothing)
{
  {
    adc4_fixture const f;
    setup(f);
    f.write("CR", cr_before);
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write(reg, before);
    auto const kept = f.read(reg);
    f.write(reg, value);
    CHECK(f.read(reg) == kept);
    f.write(reg, value);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].peripheral == "ADC4");
    CHECK(logged[0].message.contains(phrase));
  }
  adc4_fixture const f{u575(), true};
  setup(f);
  f.write("CR", cr_before);
  f.write(reg, before);
  auto const kept = f.read(reg);
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(f.at("ADC4", reg), 4, value)), popcorn::model_error);
  CHECK(f.read(reg) == kept);
}
}

TEST_CASE("RM0456 34: ADC4 has its own model on the STM32U575 and STM32U585, ADC1 keeps its row")
{
  for(auto const* description : {&u575(), &u585()})
  {
    adc4_fixture const f{*description};
    CHECK(f.target->model("ADC4") != nullptr);
    CHECK(f.target->model("ADC1") != nullptr);
    CHECK(f.target->model("ADC4") != f.target->model("ADC1"));
    CHECK(f.read("CR") == 0);
    CHECK(f.read("ISR") == 0);
  }
}

TEST_CASE("RM0456 34.7.3: ADEN, ADDIS, ADSTART, ADSTP and ADCAL are rs, a write of 0 keeps them")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.write("CR", 0);
  CHECK(f.cr("ADEN") == 1);
  CHECK(f.cr("ADVREGEN") == 1);
  f.write("CR", advregen | addis);
  f.write("CR", advregen);
  CHECK(f.cr("ADDIS") == 1);
}

TEST_CASE(
    "RM0456 34.4.3 and DS13737 Table 106: LDORDY "
    "sets 25 us after ADVREGEN, with LDORDYIE the IRQ")
{
  adc4_fixture const f;
  f.write("IER", ldordy);
  auto const start = f.now();
  f.write("CR", advregen);
  CHECK(f.dues() == std::vector<std::uint64_t>{start + ldo});
  f.run_until(start + ldo - early);
  CHECK(f.isr("LDORDY") == 0);
  CHECK_FALSE(f.pending(irq));
  f.run_until(start + ldo);
  CHECK(f.isr("LDORDY") == 1);
  CHECK(f.pending(irq));
  f.write("ISR", ldordy);
  CHECK(f.isr("LDORDY") == 0);
}

TEST_CASE("RM0456 34.4.3: LDORDY stays set when ADVREGEN clears, a cleared ADVREGEN drops it")
{
  adc4_fixture const f;
  static_cast<void>(f.regulate());
  f.write("CR", 0);
  CHECK(f.isr("LDORDY") == 1);
  f.write("ISR", ldordy);
  f.write("CR", advregen);
  f.run_until(f.now() + 10 * us);
  f.write("CR", 0);
  f.run_until(f.now() + ldo);
  CHECK(f.isr("LDORDY") == 0);
}

TEST_CASE(
    "RM0456 34.4.4 and DS13737 Table 106: ADCAL stays set 123 ADC clock cycles, then "
    "EOCAL, the factor in ADC_CALFACT and ADC_DR, inferred 0, and with EOCALIE the IRQ")
{
  adc4_fixture const f;
  static_cast<void>(f.regulate());
  f.write("IER", eocal);
  auto const start = f.now();
  f.write("CR", advregen | adcal);
  CHECK(f.dues() == std::vector<std::uint64_t>{start + tcal});
  CHECK(f.read("DR") == 0);
  f.run_until(start + tcal - early);
  CHECK(f.cr("ADCAL") == 1);
  CHECK(f.isr("EOCAL") == 0);
  f.run_until(start + tcal);
  CHECK(f.cr("ADCAL") == 0);
  CHECK(f.isr("EOCAL") == 1);
  CHECK(f.read("CALFACT") == 0);
  CHECK(f.read("DR") == 0);
  CHECK(f.pending(irq));
}

TEST_CASE("RM0456 34.4.6: PRESC divides the calibration clock, /6 takes 6 x 123 cycles")
{
  adc4_fixture const f;
  f.write("CCR", 3u << 18);
  static_cast<void>(f.regulate());
  auto const start = f.now();
  f.write("CR", advregen | adcal);
  CHECK(
      f.dues()
      == std::vector<std::uint64_t>{start + (123 * 1'000'000'000'000 + 2'666'665) / 2'666'666});
}

TEST_CASE("RM0456 34.4.4: ADCAL with ADEN 1 is ignored and logs once, a model_error when strict")
{
  check_refused(advregen, advregen | adcal, "CR", "ADCAL", advregen, [](adc4_fixture const& f) {
    static_cast<void>(f.enable());
  });
}

TEST_CASE("RM0456 34.4.4: ADCAL with AUTOFF 1 is ignored and logs once, a model_error when strict")
{
  check_refused(advregen, advregen | adcal, "CR", "ADCAL", advregen, [](adc4_fixture const& f) {
    f.write("PWR", autoff);
    static_cast<void>(f.regulate());
  });
}

TEST_CASE(
    "RM0456 34.4.4: ADCAL with ADVREGEN 0 or the regulator not "
    "ready is ignored and logs once, a model_error when strict")
{
  check_refused(0, adcal, "CR", "ADCAL", 0);
  check_refused(advregen, advregen | adcal, "CR", "ADCAL", advregen);
  adc4_fixture const f;
  static_cast<void>(f.regulate());
  f.write("ISR", ldordy);
  f.write("CR", advregen | adcal);
  CHECK(f.cr("ADCAL") == 1);
}

TEST_CASE(
    "DS13737 Table 106: ADRDY sets tSTAB, 17 ADC clock "
    "cycles, after ADEN, inferred, with ADRDYIE the IRQ")
{
  adc4_fixture const f;
  static_cast<void>(f.regulate());
  f.write("IER", adrdy);
  auto const start = f.now();
  f.write("CR", advregen | aden);
  CHECK(f.dues() == std::vector<std::uint64_t>{start + tstab});
  f.run_until(start + tstab - early);
  CHECK(f.isr("ADRDY") == 0);
  f.run_until(start + tstab);
  CHECK(f.isr("ADRDY") == 1);
  CHECK(f.pending(irq));
}

TEST_CASE("ADEN before LDORDY sets ADRDY tSTAB after LDORDY, inferred")
{
  adc4_fixture const f;
  auto const         start = f.now();
  f.write("CR", advregen);
  f.run_until(start + 5 * us);
  f.write("CR", advregen | aden);
  CHECK(f.dues() == std::vector<std::uint64_t>{start + ldo});
  f.run_until(start + ldo);
  CHECK(f.dues() == std::vector<std::uint64_t>{start + ldo + tstab});
  f.run_until(start + ldo + tstab - early);
  CHECK(f.isr("ADRDY") == 0);
  f.run_until(start + ldo + tstab);
  CHECK(f.isr("ADRDY") == 1);
}

TEST_CASE("ES0499 2.9.2: ADEN with ADVREGEN 0 sets ADVREGEN, LDORDY and ADRDY follow, inferred")
{
  adc4_fixture const f;
  auto const         start = f.now();
  f.write("CR", aden);
  CHECK(f.cr("ADVREGEN") == 1);
  CHECK(f.cr("ADEN") == 1);
  f.run_until(start + ldo);
  CHECK(f.isr("LDORDY") == 1);
  f.run_until(start + ldo + tstab);
  CHECK(f.isr("ADRDY") == 1);
}

TEST_CASE("RM0456 34.4.5 Note: with AUTOFF 1 ADEN sets no ADRDY")
{
  adc4_fixture const f;
  f.write("PWR", autoff);
  static_cast<void>(f.regulate());
  f.write("CR", advregen | aden);
  f.run_until(f.now() + 2 * tstab);
  CHECK(f.cr("ADEN") == 1);
  CHECK(f.isr("ADRDY") == 0);
}

TEST_CASE(
    "RM0456 34.4.5 Caution: ADEN during ADCAL or within 4 ADC clock "
    "cycles after it is lost, logs once, a model_error when strict")
{
  {
    adc4_fixture const            f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("CCR", 4u << 18);
    static_cast<void>(f.regulate());
    f.write("CR", advregen | adcal);
    f.write("CR", advregen | aden);
    CHECK(f.cr("ADEN") == 0);
    auto const done = f.now() + 8 * tcal;
    f.run_until(done);
    REQUIRE(f.cr("ADCAL") == 0);
    f.write("CR", advregen | aden);
    CHECK(f.cr("ADEN") == 0);
    f.run_until(done + 32 * cycle - early);
    f.write("CR", advregen | aden);
    CHECK(f.cr("ADEN") == 0);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("4 ADC clock cycles"));
    f.run_until(done + 32 * cycle);
    f.write("CR", advregen | aden);
    CHECK(f.cr("ADEN") == 1);
  }
  adc4_fixture const f{u575(), true};
  static_cast<void>(f.regulate());
  f.write("CR", advregen | adcal);
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden)),
      popcorn::model_error);
  CHECK(f.read("CR") == (advregen | adcal));
}

TEST_CASE(
    "ES0499 2.9.2: ADDIS clears ADEN and ADDIS 6 ADC clock cycles "
    "later, inferred after ADRDY, ADRDY stays set, RM0456 34.4.5")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  auto const start = f.now();
  f.write("CR", advregen | aden | addis);
  CHECK(f.dues() == std::vector<std::uint64_t>{start + tdis});
  f.run_until(start + tdis - early);
  CHECK(f.cr("ADEN") == 1);
  CHECK(f.cr("ADDIS") == 1);
  f.run_until(start + tdis);
  CHECK(f.cr("ADEN") == 0);
  CHECK(f.cr("ADDIS") == 0);
  CHECK(f.isr("ADRDY") == 1);
}

TEST_CASE("ADDIS before ADRDY drops the pending ADRDY")
{
  adc4_fixture const f;
  static_cast<void>(f.regulate());
  auto const start = f.now();
  f.write("CR", advregen | aden);
  f.write("CR", advregen | aden | addis);
  CHECK(f.dues() == std::vector<std::uint64_t>{start + tdis, start + tstab});
  f.run_until(start + 2 * tstab);
  CHECK(f.isr("ADRDY") == 0);
  CHECK(f.cr("ADEN") == 0);
}

TEST_CASE("RM0456 34.7.3: ADDIS with ADEN 0 is ignored and logs once, a model_error when strict")
{
  check_refused(advregen, advregen | addis, "CR", "ADDIS");
}

TEST_CASE(
    "RM0456 34.7.3: ADVREGEN cleared with ADEN 1 "
    "stays 1 and logs once, a model_error when strict")
{
  check_refused(advregen | aden, aden, "CR", "ADVREGEN", advregen, [](adc4_fixture const& f) {
    static_cast<void>(f.enable());
  });
}

TEST_CASE(
    "RM0456 34.7.3: ADVREGEN cleared during ADCAL "
    "stays 1 and logs once, a model_error when strict")
{
  check_refused(advregen | adcal, adcal, "CR", "ADVREGEN", advregen, [](adc4_fixture const& f) {
    static_cast<void>(f.regulate());
  });
}

TEST_CASE(
    "an ADSTART with no channel in the sequence is "
    "ignored and logs once, inferred, a model_error when strict")
{
  {
    adc4_fixture const            f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    static_cast<void>(f.enable());
    f.write("CR", advregen | aden | adstart);
    CHECK(f.cr("ADSTART") == 0);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("no channel"));
  }
  adc4_fixture const f{u575(), true};
  static_cast<void>(f.enable());
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
  CHECK(f.read("CR") == (advregen | aden));
}

TEST_CASE(
    "RM0456 34.7.3 and 34.4.8: ADSTART with ADEN 0 and ADSTP with "
    "ADEN 0 are ignored, each logs once, a model_error when strict")
{
  check_refused(advregen, advregen | adstart, "CR", "ADSTART");
  check_refused(advregen, advregen | adstp, "CR", "ADSTP");
}

TEST_CASE(
    "RM0456 34.4.13 Note and 34.4.5: ADSTP with ADEN 1 and "
    "ADSTART 0 clears itself and logs nothing, strict included")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    adc4_fixture const            f{u575(), strict};
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    static_cast<void>(f.enable());
    f.write("CR", advregen | aden | adstp);
    CHECK(f.read("CR") == (advregen | aden));
    CHECK(logged.empty());
  }
}

TEST_CASE(
    "RM0456 34.4.4: ADC_CALFACT takes a write with ADEN 1, the regulator off resets the factor")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.write("CALFACT", 0x55);
  CHECK(f.read("CALFACT") == 0x55);
  f.write("CR", advregen | aden | addis);
  f.run_until(f.now() + tdis);
  CHECK(f.read("CALFACT") == 0x55);
  f.write("CR", 0);
  CHECK(f.read("CALFACT") == 0);
}

TEST_CASE("RM0456 34.7.16: ADC_CALFACT written with ADEN 0 is ignored, a model_error when strict")
{
  check_refused(0, 0x2a, "CALFACT", "CALFACT", 0);
}

TEST_CASE("RM0456 34.7.13: ADC_PWRR written with ADEN 1 is ignored, a model_error when strict")
{
  check_refused(0, autoff, "PWR", "ADC_PWRR", advregen, [](adc4_fixture const& f) {
    static_cast<void>(f.enable());
  });
}

TEST_CASE(
    "RM0456 34.7.18: PRESC written with ADEN 1 keeps the old PRESC, a model_error when strict")
{
  check_refused(0, 2u << 18, "CCR", "PRESC", advregen, [](adc4_fixture const& f) {
    static_cast<void>(f.enable());
  });
}

TEST_CASE("a reserved PRESC divides by 256 and logs once, a model_error when strict that keeps CCR")
{
  {
    adc4_fixture const            f;
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("CCR", 12u << 18);
    CHECK(f.read("CCR") == 12u << 18);
    f.write("CCR", 13u << 18);
    static_cast<void>(f.regulate());
    auto const start = f.now();
    f.write("CR", advregen | adcal);
    CHECK(f.dues() == std::vector<std::uint64_t>{start + 256 * tcal});
    REQUIRE(logged.size() == 2);
    CHECK(logged[0].message.contains("reserved"));
    CHECK(logged[1].message.contains("outside"));
  }
  adc4_fixture const f{u575(), true};
  f.write("CCR", 1u << 22);
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(f.at("ADC4", "CCR"), 4, 15u << 18)),
      popcorn::model_error);
  CHECK(f.read("CCR") == 1u << 22);
}

TEST_CASE("without an ADC4 kernel clock the calibration waits for one and logs once")
{
  adc4_fixture const            f{u575(), false, false};
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  static_cast<void>(f.regulate());
  f.write("CR", advregen | adcal);
  CHECK(f.dues().empty());
  f.run_until(f.now() + 10 * tcal);
  CHECK(f.cr("ADCAL") == 1);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("no ADC4 kernel clock"));
  f.put("RCC", "RCC_CR", "HSION", 1);
  f.run_until(f.now() + 200 * us);
  CHECK(f.cr("ADCAL") == 0);
  CHECK(f.isr("EOCAL") == 1);
}

TEST_CASE("DS13737 Table 106: an ADEN edge with the ADC clock above 55 MHz logs a warning")
{
  adc4_fixture const f;
  f.write("RCC", "RCC_PLL2DIVR", (8u << 0) | (1u << 9) | (1u << 16) | (1u << 24));
  f.write("RCC", "RCC_PLL2CFGR", (2u << 0) | (3u << 2) | (1u << 18));
  f.put("RCC", "RCC_CR", "PLL2ON", 1);
  f.run_until(f.now() + 200 * us);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL2RDY") == 1);
  f.put("RCC", "RCC_CCIPR3", "ADCDACSEL", 2);
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  static_cast<void>(f.regulate());
  f.write("CR", advregen | aden);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("outside"));
  f.write("CR", advregen | aden | addis);
  f.run_until(f.now() + us);
  f.write("CCR", 1u << 18);
  f.write("CR", advregen | aden);
  CHECK(logged.size() == 1);
}

TEST_CASE("RM0456 34.6: each ADC_ISR flag with its ADC_IER bit raises ADC4_IRQn")
{
  adc4_fixture const f;
  f.run_until(f.calibrate() + 4 * cycle);
  f.write("CR", advregen | aden);
  f.run_until(f.now() + tstab);
  REQUIRE((f.read("ISR") & (adrdy | eocal | ldordy)) == (adrdy | eocal | ldordy));
  for(auto const flag : {adrdy, eocal, ldordy})
  {
    CAPTURE(flag);
    f.write("IER", flag);
    CHECK(f.pending(irq));
    f.write("IER", 0);
    REQUIRE(f.target->mem().write(0xe000'e280u + 4 * (irq / 32), 4, 1u << (irq % 32)).has_value());
    CHECK_FALSE(f.pending(irq));
  }
}

TEST_CASE(
    "checklist 31: an RCC reset of ADC4 drops a pending "
    "ADRDY, a later ADEN sets it at its own LDORDY plus tSTAB")
{
  adc4_fixture const f;
  static_cast<void>(f.regulate());
  auto const stale = f.now() + tstab;
  f.write("CR", advregen | aden);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  CHECK(f.read("CR") == 0);
  auto const begin = f.now();
  f.write("CR", aden);
  REQUIRE(begin + ldo > stale);
  f.run_until(stale);
  CHECK(f.isr("ADRDY") == 0);
  f.run_until(begin + ldo + tstab - early);
  CHECK(f.isr("ADRDY") == 0);
  f.run_until(begin + ldo + tstab);
  CHECK(f.isr("ADRDY") == 1);
}

TEST_CASE(
    "checklist 31: an RCC reset of ADC4 drops a pending LDORDY, "
    "calibration end and ADDIS end, each ignored at its old due time")
{
  {
    adc4_fixture const f;
    auto const         stale = f.now() + ldo;
    f.write("CR", advregen);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
    f.run_until(stale + us);
    CHECK(f.isr("LDORDY") == 0);
  }
  {
    adc4_fixture const f;
    f.write("CCR", 11u << 18);
    static_cast<void>(f.regulate());
    auto const stale = f.now() + 256 * tcal;
    f.write("CR", advregen | adcal);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
    static_cast<void>(f.regulate());
    f.run_until(stale + us);
    CHECK(f.isr("EOCAL") == 0);
    CHECK(f.read("CR") == advregen);
  }
  {
    adc4_fixture const f;
    f.write("CCR", 11u << 18);
    static_cast<void>(f.regulate());
    f.write("CR", advregen | aden);
    auto const stale = f.now() + 256 * tdis;
    f.write("CR", advregen | aden | addis);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
    f.write("CR", aden);
    f.run_until(stale + us);
    CHECK(f.cr("ADEN") == 1);
  }
}

TEST_CASE(
    "checklist 31: a calibration started after an RCC reset "
    "of ADC4 ends at its own time, not at the dropped one's")
{
  adc4_fixture const f;
  f.write("CCR", 11u << 18);
  static_cast<void>(f.regulate());
  auto const stale = f.now() + 256 * tcal;
  f.write("CR", advregen | adcal);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  CHECK(f.read("CR") == 0);
  f.write("CCR", 11u << 18);
  static_cast<void>(f.regulate());
  auto const own = f.now() + 256 * tcal;
  f.write("CR", advregen | adcal);
  f.run_until(stale + us);
  CHECK(f.cr("ADCAL") == 1);
  f.run_until(own);
  CHECK(f.cr("ADCAL") == 0);
  CHECK(f.isr("EOCAL") == 1);
}

TEST_CASE(
    "checklist 66 and 72: snapshots mid-calibration, in the ADEN lockout and "
    "mid-ADRDY end each at the same time, restored into a machine never reset")
{
  adc4_fixture const f;
  f.write("CCR", 4u << 18);
  static_cast<void>(f.regulate());
  auto const start = f.now();
  auto const done  = start + 8 * tcal;
  f.write("CR", advregen | adcal);
  f.run_until(start + 4 * tcal);
  auto const calibrating = f.target->snapshot();
  f.run_until(done);
  auto const locked = f.target->snapshot();
  f.run_until(done + 32 * cycle);
  f.write("CR", advregen | aden);
  auto const enabled = f.now();
  f.run_until(enabled + 4 * tstab);
  auto const readying = f.target->snapshot();
  auto const into     = [](adc4_fixture const& g, core::snapshot const& saved) {
    g.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  };
  auto const from_calibrating = [&](adc4_fixture const& g) {
    g.run_until(done - early);
    CHECK(g.cr("ADCAL") == 1);
    g.run_until(done);
    CHECK(g.cr("ADCAL") == 0);
    CHECK(g.isr("EOCAL") == 1);
    g.write("CR", advregen | aden);
    CHECK(g.cr("ADEN") == 0);
  };
  auto const from_locked = [&](adc4_fixture const& g) {
    REQUIRE(g.cr("ADCAL") == 0);
    g.run_until(done + 32 * cycle - early);
    g.write("CR", advregen | aden);
    CHECK(g.cr("ADEN") == 0);
    g.run_until(done + 32 * cycle);
    g.write("CR", advregen | aden);
    CHECK(g.cr("ADEN") == 1);
  };
  auto const from_readying = [&](adc4_fixture const& g) {
    g.run_until(enabled + 8 * tstab - early);
    CHECK(g.isr("ADRDY") == 0);
    g.run_until(enabled + 8 * tstab);
    CHECK(g.isr("ADRDY") == 1);
  };
  for(auto const& [saved, check] :
      std::to_array<std::pair<core::snapshot const*, std::function_ref<void(adc4_fixture const&)>>>(
          {
              {&calibrating, from_calibrating},
              {&locked,      from_locked     },
              {&readying,    from_readying   }
  }))
  {
    f.target->restore(*saved);
    check(f);
    adc4_fixture const fresh{no_reset};
    into(fresh, *saved);
    check(fresh);
  }
}

TEST_CASE(
    "checklist 66: a snapshot between ADEN and LDORDY restored "
    "into a machine never reset sets ADRDY at LDORDY plus tSTAB")
{
  adc4_fixture const f;
  auto const         start = f.now();
  f.write("CR", aden);
  f.run_until(start + 10 * us);
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  fresh.run_until(start + ldo + tstab - early);
  CHECK(fresh.isr("LDORDY") == 1);
  CHECK(fresh.isr("ADRDY") == 0);
  fresh.run_until(start + ldo + tstab);
  CHECK(fresh.isr("ADRDY") == 1);
}

TEST_CASE(
    "RM0456 34.4.5 Caution: the 4-cycle ADEN lockout counts from the "
    "calibration end, not from its dispatch at the next instruction")
{
  adc4_fixture const f;
  f.write("RCC", "RCC_PLL2DIVR", (8u << 0) | (1u << 9) | (1u << 16) | (1u << 24));
  f.write("RCC", "RCC_PLL2CFGR", (2u << 0) | (3u << 2) | (1u << 18));
  f.put("RCC", "RCC_CR", "PLL2ON", 1);
  f.run_until(f.now() + 200 * us);
  REQUIRE(f.field("RCC", "RCC_CR", "PLL2RDY") == 1);
  f.put("RCC", "RCC_CCIPR3", "ADCDACSEL", 2);
  f.write("CCR", 7u << 18);
  static_cast<void>(f.regulate());
  auto const start = f.now();
  f.write("CR", advregen | adcal);
  auto const done = start + (123 * 1'000'000'000'000 + 4'499'999) / 4'500'000;
  auto const free = done + (4 * 1'000'000'000'000 + 4'499'999) / 4'500'000;
  REQUIRE(done % 250'000 != 0);
  f.run_until(free);
  REQUIRE(f.now() < done - done % 250'000 + 250'000 + (free - done));
  f.write("CR", advregen | aden);
  CHECK(f.cr("ADEN") == 1);
}

TEST_CASE(
    "checklist 79: ADRDY after LDORDY counts tSTAB "
    "from the LDORDY due, off the grid of a 3.072 MHz core")
{
  adc4_fixture const f;
  f.put("RCC", "RCC_ICSCR1", "MSIRGSEL", 1);
  f.put("RCC", "RCC_ICSCR1", "MSISRANGE", 8);
  f.run_until(f.now() + 10 * us);
  auto const start = f.now();
  f.write("CR", advregen | aden);
  REQUIRE((ldo * 3'072'000) % 1'000'000'000'000 != 0);
  f.run_until(start + ldo);
  CHECK(f.dues().back() == start + ldo + tstab);
}

TEST_CASE(
    "checklist 31: an ADVREGEN rewritten after an RCC "
    "reset sets LDORDY at its own due, not at the dropped one's")
{
  adc4_fixture const f;
  auto const         first = f.now();
  f.write("CR", advregen);
  f.run_until(first + 5 * us);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  f.run_until(first + 10 * us);
  auto const second = f.now();
  f.write("CR", advregen);
  f.run_until(first + ldo + us);
  CHECK(f.isr("LDORDY") == 0);
  f.run_until(second + ldo);
  CHECK(f.isr("LDORDY") == 1);
}

TEST_CASE(
    "checklist 31: an ADDIS written after an RCC reset "
    "clears ADEN at its own due, not at the dropped one's")
{
  adc4_fixture const f;
  f.write("CCR", 11u << 18);
  f.write("CR", aden);
  auto const first = f.now();
  f.write("CR", advregen | aden | addis);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  f.write("CCR", 11u << 18);
  f.run_until(first + 10 * us);
  f.write("CR", aden);
  auto const second = f.now();
  f.write("CR", advregen | aden | addis);
  f.run_until(first + 256 * tdis + us);
  CHECK(f.cr("ADEN") == 1);
  f.run_until(second + 256 * tdis);
  CHECK(f.cr("ADEN") == 0);
}

TEST_CASE(
    "ES0499 2.9.2 workaround: ADEN, ADDIS 2 cycles later, "
    "ADVREGEN cleared once ADEN reads 0, leaves no LDORDY and no ADRDY")
{
  adc4_fixture const f;
  auto const         start = f.now();
  f.write("CR", aden);
  f.run_until(start + 2 * cycle);
  f.write("CR", advregen | aden | addis);
  f.run_until(f.now() + tdis);
  REQUIRE(f.cr("ADEN") == 0);
  f.write("CR", 0);
  f.run_until(start + 2 * ldo);
  CHECK(f.read("CR") == 0);
  CHECK(f.isr("LDORDY") == 0);
  CHECK(f.isr("ADRDY") == 0);
}

TEST_CASE("checklist 66: a snapshot mid-ADDIS restored into a machine never reset ends it on time")
{
  adc4_fixture const f;
  f.write("CCR", 4u << 18);
  static_cast<void>(f.regulate());
  f.write("CR", advregen | aden);
  f.run_until(f.now() + 8 * tstab);
  auto const start = f.now();
  f.write("CR", advregen | aden | addis);
  f.run_until(start + 4 * tdis);
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  fresh.run_until(start + 8 * tdis - early);
  CHECK(fresh.cr("ADEN") == 1);
  fresh.run_until(start + 8 * tdis);
  CHECK(fresh.cr("ADEN") == 0);
}

TEST_CASE(
    "checklist 66: a snapshot after LDORDY restored into a machine never reset takes an ADCAL")
{
  adc4_fixture const f;
  static_cast<void>(f.regulate());
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  fresh.write("CR", advregen | adcal);
  CHECK(fresh.cr("ADCAL") == 1);
}

TEST_CASE(
    "without an ADC4 kernel clock ADRDY and ADDIS wait for "
    "one, also across a snapshot into a machine never reset")
{
  for(auto const restored : {false, true})
  {
    CAPTURE(restored);
    adc4_fixture const f{u575(), false, false};
    static_cast<void>(f.regulate());
    f.write("CR", advregen | aden);
    f.run_until(f.now() + 10 * tstab);
    CHECK(f.isr("ADRDY") == 0);
    f.write("CR", advregen | aden | addis);
    f.run_until(f.now() + 10 * tdis);
    CHECK(f.cr("ADEN") == 1);
    auto const         saved = f.target->snapshot();
    adc4_fixture const fresh{no_reset};
    auto const&        g = restored ? fresh : f;
    if(restored)
    {
      fresh.target->restore(
          core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    }
    g.put("RCC", "RCC_CR", "HSION", 1);
    g.run_until(g.now() + 200 * us);
    CHECK(g.cr("ADEN") == 0);
    CHECK(g.cr("ADDIS") == 0);
  }
}

TEST_CASE("without an ADC4 kernel clock ADRDY waits for one, then sets tSTAB after it")
{
  for(auto const restored : {false, true})
  {
    CAPTURE(restored);
    adc4_fixture const f{u575(), false, false};
    static_cast<void>(f.regulate());
    f.write("CR", advregen | aden);
    f.run_until(f.now() + 10 * tstab);
    auto const         saved = f.target->snapshot();
    adc4_fixture const fresh{no_reset};
    auto const&        g = restored ? fresh : f;
    if(restored)
    {
      fresh.target->restore(
          core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    }
    CHECK(g.isr("ADRDY") == 0);
    g.put("RCC", "RCC_CR", "HSION", 1);
    g.run_until(g.now() + 200 * us);
    CHECK(g.isr("ADRDY") == 1);
  }
}

TEST_CASE(
    "a calibration waiting for an ADC4 kernel clock "
    "survives a snapshot into a machine never reset")
{
  adc4_fixture const f{u575(), false, false};
  static_cast<void>(f.regulate());
  f.write("CR", advregen | adcal);
  f.run_until(f.now() + 10 * tcal);
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  fresh.put("RCC", "RCC_CR", "HSION", 1);
  fresh.run_until(fresh.now() + 200 * us);
  CHECK(fresh.cr("ADCAL") == 0);
  CHECK(fresh.isr("EOCAL") == 1);
}

TEST_CASE(
    "RM0456 34.4.11 and 34.4.14: a single conversion samples 1.5 "
    "cycles from tLATR, 4 cycles after ADSTART, inferred, sets "
    "EOSMP, then EOC, EOS and the code 12.5 cycles later, ADSTART cleared")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 1'500'000);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  CHECK(f.dues().back() == begin);
  f.run_until(begin);
  CHECK(f.dues() == std::vector<std::uint64_t>{begin + halves(3), begin + halves(28)});
  f.run_until(begin + halves(3));
  CHECK((f.read("ISR") & (eosmp | eoc)) == eosmp);
  f.run_until(begin + halves(28) - early);
  CHECK(f.isr("EOC") == 0);
  f.run_until(begin + halves(28));
  CHECK((f.read("ISR") & (eoc | eos)) == (eoc | eos));
  CHECK(f.read("DR") == code_of(1'500'000));
  CHECK(f.cr("ADSTART") == 0);
}

TEST_CASE("RM0456 34.7.6: SMPSELx picks SMP1 or SMP2, each code sets its sampling time")
{
  constexpr std::array<std::uint64_t, 8> sampling{3, 7, 15, 25, 39, 79, 159, 1629};
  for(std::uint32_t smp = 0; smp < 8; ++smp)
  {
    for(auto const second : {false, true})
    {
      CAPTURE(smp);
      CAPTURE(second);
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.write("SMPR", second ? (smp << 4) | (1u << (8 + pa4)) : smp);
      f.write("CHSELRMOD0", 1u << pa4);
      auto const begin = f.start();
      f.run_until(begin);
      CHECK(
          f.dues()
          == std::vector<std::uint64_t>{
              begin + halves(sampling[smp]), begin + halves(sampling[smp] + 25)});
    }
  }
}

TEST_CASE(
    "RM0456 Table 332 and Figure 291: RES sets tSAR and drops two bits "
    "per step, ALIGN 1 puts the result on bit 15, a 6-bit one on bit 7")
{
  constexpr std::array<std::uint64_t, 4> sar{25, 21, 17, 13};
  constexpr std::array<std::uint32_t, 4> left{4, 6, 8, 2};
  for(std::uint32_t res = 0; res < 4; ++res)
  {
    for(auto const left_aligned : {false, true})
    {
      CAPTURE(res);
      CAPTURE(left_aligned);
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.port().drive(pa4, 2'345'678);
      f.write("CFGR1", (res << 2) | (left_aligned ? align : 0u));
      f.write("CHSELRMOD0", 1u << pa4);
      auto const begin = f.start();
      f.run_until(begin);
      CHECK(f.dues().back() == begin + halves(3 + sar[res]));
      f.run_until(begin + halves(3 + sar[res]));
      auto const result = code_of(2'345'678) >> (2 * res);
      CHECK(f.read("DR") == (left_aligned ? result << left[res] : result));
    }
  }
}

TEST_CASE(
    "RM0456 34.4.9: CHSELRMOD 0 scans the CHSEL channels up with SCANDIR "
    "0 and down with SCANDIR 1, CHSELRMOD 1 follows SQ1 to SQ8 up to 1111")
{
  struct order
  {
    std::uint32_t                config;
    std::uint32_t                selection;
    std::array<std::uint32_t, 3> channels;
  };
  for(auto const& [config, selection, channels] : {
          order{0,         (1u << pa4) | (1u << pa5) | (1u << pa6),      {pa4, pa5, pa6}},
          order{scandir,   (1u << pa4) | (1u << pa5) | (1u << pa6),      {pa6, pa5, pa4}},
          order{chselrmod, 0xFFFF'F000u | (pa5 << 8) | (pa4 << 4) | pa6, {pa6, pa4, pa5}}
  })
  {
    CAPTURE(config);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, 500'000);
    f.port().drive(pa5, 1'000'000);
    f.port().drive(pa6, 2'000'000);
    f.write("CFGR1", config | ovrmod);
    f.write("CHSELRMOD0", selection);
    auto const begin = f.start();
    for(std::uint64_t index = 0; index < 3; ++index)
    {
      CAPTURE(index);
      f.run_until(begin + (index + 1) * halves(28));
      CHECK(f.read("DR") == code_of(f.port().input(channels[index])));
      CHECK(f.isr("EOS") == (index == 2 ? 1u : 0u));
    }
    CHECK(f.cr("ADSTART") == 0);
  }
}

TEST_CASE("RM0456 34.4.12 and 34.4.15: CONT 1 starts the sequence over after EOS until ADSTP")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 600'000);
  f.port().drive(pa5, 1'200'000);
  f.write("CFGR1", cont | ovrmod);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  auto const begin = f.start();
  f.run_until(begin + 3 * halves(28));
  CHECK(f.read("DR") == code_of(600'000));
  CHECK(f.cr("ADSTART") == 1);
  f.write("CR", advregen | aden | adstart | adstp);
  CHECK(f.read("CR") == (advregen | aden));
  auto const stopped = f.read("DR");
  f.run_until(f.now() + 4 * halves(28));
  CHECK(f.read("DR") == stopped);
  f.write("ISR", eos);
  auto const again = f.start();
  f.run_until(again + halves(28));
  CHECK(f.read("DR") == code_of(600'000));
}

TEST_CASE(
    "RM0456 34.4.17: DISCEN converts one channel per "
    "ADSTART, ADSTART cleared at each EOC, EOS after the last")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 700'000);
  f.port().drive(pa5, 1'400'000);
  f.write("CFGR1", discen | ovrmod);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  for(auto const& [channel, last] : {
          std::pair{pa4, false},
          std::pair{pa5, true },
          std::pair{pa4, false}
  })
  {
    CAPTURE(channel);
    auto const begin = f.start();
    f.run_until(begin + 2 * halves(28));
    CHECK(f.read("DR") == code_of(f.port().input(channel)));
    CHECK(f.cr("ADSTART") == 0);
    CHECK(f.isr("EOS") == (last ? 1u : 0u));
    f.write("ISR", eos);
  }
}

TEST_CASE("a pad drive after the end of sampling leaves the held level, one before it converts")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 1'000'000);
  f.write("SMPR", 7);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + halves(1629) - us);
  f.port().drive(pa4, 2'000'000);
  f.run_until(begin + halves(1629) + early);
  f.port().drive(pa4, 500'000);
  f.run_until(begin + halves(1654));
  CHECK(f.read("DR") == code_of(2'000'000));
}

TEST_CASE(
    "checklist 53: each function ADC4 does not model yet logs "
    "once at ADSTART, a model_error when strict that keeps ADC_CR")
{
  struct function
  {
    std::string_view reg;
    std::uint32_t    value;
    std::uint32_t    selection;
    std::string_view phrase;
  };
  for(auto const& [reg, value, selection, phrase] : {
          function{"CFGR1",  1u << 10,      1u << pa4, "EXTEN"           },
          function{"CFGR1",  1u << 0,       1u << pa4, "DMAEN"           },
          function{"CFGR1",  1u << 14,      1u << pa4, "WAIT"            },
          function{"CFGR2",  1u << 0,       1u << pa4, "OVSE"            },
          function{"CFGR2",  1u << 29,      1u << pa4, "LFTRIG"          },
          function{"CFGR1",  1u << 23,      1u << pa4, "AWD1EN"          },
          function{"AWD2CR", 1u << pa4,     1u << pa4, "AWD1EN"          },
          function{"AWD3CR", 1u << pa4,     1u << pa4, "AWD1EN"          },
          function{"CFGR1",  cont | discen, 1u << pa4, "DISCEN with CONT"},
          function{"CFGR1",  0,             1u << 13,  "internal channel"}
  })
  {
    CAPTURE(reg);
    CAPTURE(phrase);
    {
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.write(reg, value);
      f.write("CHSELRMOD0", selection);
      std::vector<core::log_record> logged;
      auto const                    sink = recorder(f, logged);
      static_cast<void>(f.start());
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains(phrase));
    }
    adc4_fixture const f{u575(), true};
    static_cast<void>(f.enable());
    f.write(reg, value);
    f.write("CHSELRMOD0", selection);
    CHECK_THROWS_AS(
        static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden | adstart)),
        popcorn::model_error);
    CHECK(f.read("CR") == (advregen | aden));
  }
}

TEST_CASE("with EXTEN not 00 ADSTART stays set and converts nothing")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.write("CFGR1", 1u << 10);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + 4 * halves(28));
  CHECK(f.cr("ADSTART") == 1);
  CHECK(f.isr("EOC") == 0);
}

TEST_CASE("ADSTP during a conversion drops its result, the sequence starts over at rank 1")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 900'000);
  f.port().drive(pa5, 1'800'000);
  f.write("CFGR1", discen);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  auto const begin = f.start();
  f.run_until(begin + halves(28));
  static_cast<void>(f.cpu_read("DR"));
  auto const second = f.start();
  f.run_until(second + halves(10));
  f.write("CR", advregen | aden | adstart | adstp);
  f.run_until(second + 2 * halves(28));
  CHECK(f.isr("EOC") == 0);
  auto const third = f.start();
  f.run_until(third + halves(28));
  CHECK(f.read("DR") == code_of(900'000));
}

TEST_CASE("without an ADC4 clock the sequence waits for one, then converts")
{
  adc4_fixture const f{u575(), false, false};
  f.put("RCC", "RCC_CR", "HSION", 1);
  f.run_until(f.now() + 200 * us);
  static_cast<void>(f.enable());
  f.write("CHSELRMOD0", 1u << pa4);
  f.put("RCC", "RCC_CR", "HSION", 0);
  f.write("CR", advregen | aden | adstart);
  f.run_until(f.now() + 10 * us);
  CHECK(f.isr("EOC") == 0);
  f.put("RCC", "RCC_CR", "HSION", 1);
  f.run_until(f.now() + 200 * us);
  CHECK(f.isr("EOC") == 1);
}

TEST_CASE(
    "checklist 31: an RCC reset of ADC4 drops a "
    "conversion in progress, a later one ends at its own time")
{
  adc4_fixture const f;
  auto const         slow_enable = [&] {
    f.write("CCR", 11u << 18);
    static_cast<void>(f.regulate());
    f.write("CR", advregen | aden);
    f.run_until(f.now() + 256 * tstab);
    REQUIRE(f.isr("ADRDY") == 1);
  };
  slow_enable();
  f.write("CHSELRMOD0", 1u << pa4);
  auto const at    = f.now();
  auto const stale = at + 256 * (latency + halves(28));
  f.write("CR", advregen | aden | adstart);
  f.run_until(at + 256 * latency + us);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  slow_enable();
  f.write("CHSELRMOD0", 1u << pa4);
  auto const own = f.now() + 256 * (latency + halves(28));
  f.write("CR", advregen | aden | adstart);
  REQUIRE(own > stale);
  f.run_until(stale + us);
  CHECK(f.isr("EOC") == 0);
  f.run_until(own);
  CHECK(f.isr("EOC") == 1);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot mid-sequence restored into a machine never "
    "reset converts the held level and the rest of the sequence at the same times")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 1'100'000);
  f.port().drive(pa5, 2'200'000);
  f.write("CFGR1", ovrmod);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  auto const begin = f.start();
  f.run_until(begin + halves(3) + early);
  auto const saved = f.target->snapshot();
  auto const check = [&](adc4_fixture const& g) {
    g.port().drive(pa4, 100'000);
    g.run_until(begin + halves(28));
    CHECK(g.read("DR") == code_of(1'100'000));
    g.run_until(begin + 2 * halves(28) - early);
    CHECK(g.isr("EOS") == 0);
    g.run_until(begin + 2 * halves(28));
    CHECK(g.isr("EOS") == 1);
    CHECK(g.read("DR") == code_of(2'200'000));
  };
  f.target->restore(saved);
  check(f);
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(fresh);
}

TEST_CASE(
    "checklist 72: a pad drive before the end of "
    "sampling reaches a machine restored from a snapshot")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 1'000'000);
  f.write("SMPR", 7);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + us);
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  fresh.run_until(begin + halves(1629) + early);
  fresh.port().drive(pa4, 2'500'000);
  fresh.run_until(begin + halves(1654));
  CHECK(fresh.read("DR") == code_of(1'000'000));
}

TEST_CASE(
    "RM0456 34.7.3 and 34.4.8: ADSTART with ADDIS 1, ADDIS with ADSTART 1 and "
    "ADSTP with ADDIS 1 are ignored, each logs once, a model_error when strict")
{
  check_refused(
      advregen | aden | addis, advregen | aden | addis | adstart, "CR", "ADSTART", advregen,
      [](adc4_fixture const& f) {
        static_cast<void>(f.enable());
        f.write("CHSELRMOD0", 1u << pa4);
      });
  check_refused(
      advregen | aden | adstart, advregen | aden | adstart | addis, "CR", "ADDIS", advregen,
      [](adc4_fixture const& f) {
        static_cast<void>(f.enable());
        f.write("CFGR1", cont);
        f.write("CHSELRMOD0", 1u << pa4);
      });
  check_refused(
      advregen | aden | addis, advregen | aden | addis | adstp, "CR", "ADSTP", advregen,
      [](adc4_fixture const& f) { static_cast<void>(f.enable()); });
}

TEST_CASE(
    "checklist 66: a snapshot inside the tLATR of an ADSTART "
    "restored into a machine never reset samples at the same time")
{
  adc4_fixture const f;
  f.write("CCR", 7u << 18);
  static_cast<void>(f.regulate());
  f.write("CR", advregen | aden);
  f.run_until(f.now() + 16 * tstab);
  f.port().drive(pa4, 1'700'000);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const at = f.now();
  f.write("CR", advregen | aden | adstart);
  f.run_until(at + 8 * latency);
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  auto const done = at + 15 * 4 * cycle + 16 * halves(28);
  fresh.run_until(done - early);
  CHECK(fresh.isr("EOC") == 0);
  fresh.run_until(done);
  CHECK(fresh.isr("EOC") == 1);
  CHECK(fresh.read("DR") == code_of(1'700'000));
}

TEST_CASE(
    "a sequence waiting for an ADC4 clock survives a snapshot "
    "into a machine never reset, then converts when the clock arrives")
{
  adc4_fixture const f{u575(), false, false};
  f.put("RCC", "RCC_CR", "HSION", 1);
  f.run_until(f.now() + 200 * us);
  static_cast<void>(f.enable());
  f.write("CHSELRMOD0", 1u << pa4);
  f.put("RCC", "RCC_CR", "HSION", 0);
  f.write("CR", advregen | aden | adstart);
  f.run_until(f.now() + 10 * us);
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(fresh.isr("EOC") == 0);
  fresh.put("RCC", "RCC_CR", "HSION", 1);
  fresh.run_until(fresh.now() + 200 * us);
  CHECK(fresh.isr("EOC") == 1);
}

TEST_CASE(
    "checklist 66: a snapshot at rank 2 of a sequence "
    "restored into a machine never reset converts rank 2")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 300'000);
  f.port().drive(pa5, 2'600'000);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  auto const begin = f.start();
  f.run_until(begin + halves(28) + halves(3) + early);
  static_cast<void>(f.cpu_read("DR"));
  auto const         saved = f.target->snapshot();
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  fresh.run_until(begin + 2 * halves(28));
  CHECK(fresh.isr("EOS") == 1);
  CHECK(fresh.read("DR") == code_of(2'600'000));
}

TEST_CASE(
    "RM0456 34.4.23: a CPU read of ADC_DR clears EOC, a result finding EOC set sets OVR and is "
    "lost with OVRMOD 0, replaces ADC_DR with OVRMOD 1")
{
  for(auto const overwrite : {false, true})
  {
    CAPTURE(overwrite);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, 800'000);
    f.port().drive(pa5, 1'600'000);
    f.write("CFGR1", overwrite ? ovrmod : 0u);
    f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
    auto const begin = f.start();
    f.run_until(begin + 2 * halves(28));
    CHECK(f.isr("OVR") == 1);
    CHECK(f.read("DR") == code_of(overwrite ? 1'600'000 : 800'000));
    CHECK(f.isr("EOC") == 1);
    static_cast<void>(f.cpu_read("DR"));
    CHECK(f.isr("EOC") == 0);
  }
}

TEST_CASE(
    "RM0456 34.7.4 to 34.7.10: ADC_CFGR1, ADC_SMPR or ADC_CHSELR written with ADSTART 1 keeps the "
    "old register, logs once, a model_error when strict")
{
  for(auto const* reg : {"CFGR1", "SMPR", "CHSELRMOD0"})
  {
    CAPTURE(std::string_view{reg});
    {
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.write("CFGR1", cont);
      f.write("CHSELRMOD0", 1u << pa4);
      static_cast<void>(f.start());
      std::vector<core::log_record> logged;
      auto const                    sink = recorder(f, logged);
      auto const                    kept = f.read(reg);
      f.write(reg, kept ^ (1u << 2));
      CHECK(f.read(reg) == kept);
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains("ADSTART 1"));
    }
    adc4_fixture const f{u575(), true};
    static_cast<void>(f.enable());
    f.write("CFGR1", cont);
    f.write("CHSELRMOD0", 1u << pa4);
    static_cast<void>(f.start());
    auto const kept = f.read(reg);
    CHECK_THROWS_AS(
        static_cast<void>(f.target->mem().write(f.at("ADC4", reg), 4, kept ^ (1u << 2))),
        popcorn::model_error);
    CHECK(f.read(reg) == kept);
  }
}

TEST_CASE(
    "RM0456 34.4.23: a result read in time sets no OVR, a debugger read keeps EOC, a "
    "DMA read clears it")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 800'000);
  f.port().drive(pa5, 1'600'000);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  auto const begin = f.start();
  f.run_until(begin + halves(28));
  CHECK(f.read("DR") == code_of(800'000));
  CHECK(f.isr("EOC") == 1);
  static_cast<void>(f.cpu_read("DR", popcorn::initiator::dma));
  CHECK(f.isr("EOC") == 0);
  f.run_until(begin + 2 * halves(28));
  CHECK(f.isr("OVR") == 0);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("DR") == code_of(1'600'000));
  CHECK(f.isr("EOC") == 1);
}

TEST_CASE(
    "RM0456 34.4.23: with OVRMOD 0 every result is discarded while OVR stays set, a result "
    "lands again once software clears OVR")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 400'000);
  f.port().drive(pa5, 900'000);
  f.port().drive(pa6, 1'500'000);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5) | (1u << pa6));
  auto const begin = f.start();
  f.run_until(begin + 2 * halves(28));
  CHECK(f.isr("OVR") == 1);
  CHECK(f.read("DR") == code_of(400'000));
  static_cast<void>(f.cpu_read("DR"));
  CHECK(f.isr("EOC") == 0);
  f.run_until(begin + 3 * halves(28));
  CHECK(f.isr("EOS") == 1);
  CHECK(f.isr("OVR") == 1);
  CHECK(f.isr("EOC") == 0);
  CHECK(f.read("DR") == code_of(400'000));
  f.write("ISR", ovr | eos);
  auto const again = f.start();
  f.run_until(again + halves(28));
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("DR") == code_of(400'000));
}

TEST_CASE("ADSTART with DISCEN after CHSELR shrank below the rank restarts at the first channel")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, 700'000);
  f.port().drive(pa5, 1'400'000);
  f.port().drive(pa6, 2'100'000);
  f.write("CFGR1", discen);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  auto const first = f.start();
  f.run_until(first + halves(28));
  static_cast<void>(f.cpu_read("DR"));
  REQUIRE(f.cr("ADSTART") == 0);
  f.write("CHSELRMOD0", 1u << pa6);
  auto const second = f.start();
  f.run_until(second + halves(28));
  CHECK(f.read("DR") == code_of(2'100'000));
  CHECK(f.isr("EOS") == 1);
  CHECK(f.cr("ADSTART") == 0);
}

TEST_CASE(
    "ADSTART with every function ADC4 does not model yet logs ten times, one model_error when "
    "strict that keeps ADC_CR")
{
  auto const setup = [](adc4_fixture const& f) {
    static_cast<void>(f.regulate());
    f.put("ADC4", "PWR", "AUTOFF", 1);
    f.put("ADC4", "PWR", "VREFPROT", 1);
    f.write("CR", advregen | aden);
    f.write("CFGR1", (1u << 10) | (1u << 0) | (1u << 14) | (1u << 23) | cont | discen);
    f.write("CFGR2", (1u << 0) | (1u << 29));
    f.write("CHSELRMOD0", (1u << pa4) | (1u << 13));
  };
  {
    adc4_fixture const f;
    setup(f);
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("CR", advregen | aden | adstart);
    CHECK(logged.size() == 10);
  }
  adc4_fixture const f{u575(), true};
  setup(f);
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
  CHECK(f.read("CR") == (advregen | aden));
}

TEST_CASE("VREFPROT and VREFSECSMP are not modelled, logged at ADSTART")
{
  for(auto const* name : {"VREFPROT", "VREFSECSMP"})
  {
    CAPTURE(std::string_view{name});
    adc4_fixture const f;
    f.put("ADC4", "PWR", name, 1);
    static_cast<void>(f.enable());
    f.write("CHSELRMOD0", 1u << pa4);
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    static_cast<void>(f.start());
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("VREFPROT"));
  }
}

TEST_CASE(
    "DS13737 Table 106: tLATR is 4 ADC clock cycles at fADC = HCLK and HCLK / 2, 3.75 at "
    "HCLK / 4, inferred for every other clock")
{
  for(auto const& [presc, divider, quarters] : {
          std::tuple{0u, 1u,  16u},
          std::tuple{2u, 4u,  16u},
          std::tuple{4u, 8u,  16u},
          std::tuple{6u, 12u, 16u},
          std::tuple{7u, 16u, 15u}
  })
  {
    CAPTURE(presc);
    adc4_fixture const f;
    f.write("CCR", presc << 18);
    static_cast<void>(f.regulate());
    f.write("CR", advregen | aden);
    f.run_until(f.now() + 64 * tstab);
    REQUIRE(f.isr("ADRDY") == 1);
    f.write("CHSELRMOD0", 1u << pa4);
    auto const at = f.now();
    f.write("CR", advregen | aden | adstart);
    REQUIRE(f.dues().size() == 1);
    auto const hz = 16'000'000ull / divider;
    CHECK(f.dues()[0] == at + (quarters * 1'000'000'000'000ull + 4 * hz - 1) / (4 * hz));
  }
}

TEST_CASE("a sequence that waited for the ADC4 clock samples when the clock arrives, no tLATR")
{
  adc4_fixture const f{u575(), false, false};
  f.put("RCC", "RCC_CR", "HSION", 1);
  f.run_until(f.now() + 200 * us);
  static_cast<void>(f.enable());
  f.write("SMPR", 7);
  f.write("CHSELRMOD0", 1u << pa4);
  f.put("RCC", "RCC_CR", "HSION", 0);
  f.write("CR", advregen | aden | adstart);
  f.run_until(f.now() + 10 * us);
  REQUIRE(f.dues().empty());
  f.put("RCC", "RCC_CR", "HSION", 1);
  auto const rcc = f.all_dues();
  REQUIRE_FALSE(rcc.empty());
  while(f.dues().empty())
  {
    f.run_until(f.now() + us);
  }
  auto const dues = f.dues();
  REQUIRE(dues.size() == 2);
  CHECK(dues[1] - dues[0] == halves(25));
  auto const arrival = dues[0] - halves(1629);
  CHECK(std::ranges::any_of(rcc, [&](std::uint64_t due) {
    return arrival >= due and arrival - due < cycle;
  }));
}
