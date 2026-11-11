#include "factory_data.hpp"
#include "stm32_fixture.hpp"
#include "trigger_signal.hpp"

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
#include <format>
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
constexpr std::uint32_t awd1en    = 1u << 23;
constexpr std::uint32_t awd1sgl   = 1u << 22;

[[nodiscard]] constexpr auto halves(std::uint64_t count) -> std::uint64_t
{
  return count * cycle / 2;
}

[[nodiscard]] constexpr auto code_of(std::uint64_t microvolts) -> std::uint32_t
{
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(microvolts * 4096 / 3'000'000, 4095));
}

[[nodiscard]] constexpr auto level_of(std::uint32_t code) -> std::uint32_t
{
  return static_cast<std::uint32_t>((std::uint64_t{code} * 3'000'000 + 4095) / 4096);
}

[[nodiscard]] constexpr auto window(std::uint32_t high, std::uint32_t low) -> std::uint32_t
{
  return high << 16 | low;
}

[[nodiscard]] constexpr auto awd1ch(std::uint32_t channel) -> std::uint32_t
{
  return channel << 26;
}

auto options_with(
    popcorn::part_description const& description,
    bool                             strict,
    double                           cpi     = 1.0,
    std::uint32_t                    vdda_mv = 3000) -> core::machine_options
{
  auto board                 = stm32::board{};
  board.supply.vdda_mv       = vdda_mv;
  auto options               = popcorn::testing::stm32_options();
  options.cpi                = cpi;
  options.models             = stm32::factories(board, description);
  options.strict_peripherals = strict;
  return options;
}

struct adc4_fixture : stm32_fixture
{
  explicit adc4_fixture(
      popcorn::part_description const& description = u575(),
      bool                             strict      = false,
      bool                             hsi         = true,
      double                           cpi         = 1.0,
      std::uint32_t                    vdda_mv     = 3000)
      : stm32_fixture{description, "rcc_ll", options_with(description, strict, cpi, vdda_mv)}
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

void guard_pa4(adc4_fixture const& f, std::uint32_t watchdog)
{
  if(watchdog == 1)
  {
    f.write("CFGR1", awd1en | awd1sgl | awd1ch(pa4));
  }
  else
  {
    f.write(watchdog == 2 ? "AWD2CR" : "AWD3CR", 1u << pa4);
  }
  f.write(std::format("AWD{}TR", watchdog), window(0x800, 0x400));
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
          function{"CFGR1", (1u << 10) | (5u << 6),                1u << pa4, "EXTSEL 5"                },
          function{"CFGR1", 1u << 14,                              1u << pa4, "WAIT"                    },
          function{"CFGR2", (1u << 0) | (9u << 5),                 1u << pa4, "OVSS 9 to 15"            },
          function{"CFGR2", 1u << 29,                              1u << pa4, "LFTRIG"                  },
          function{
                   "CFGR1", (1u << 23) | (1u << 22) | (24u << 26), 1u << pa4,
                   "AWD1CH 18 to 21 and 24 to 31"                                                       },
          function{"CFGR1", cont | discen,                         1u << pa4, "DISCEN or TOVS with CONT"},
          function{"CFGR1", 0,                                     1u << 12,  "VCORE"                   }
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
  auto const done = at + 16 * (latency + halves(28));
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
    "RM0456 34.7.4 to 34.7.17: ADC_CFGR1, ADC_SMPR, ADC_CHSELR, ADC_CFGR2, "
    "ADC_AWD2CR, ADC_AWD3CR or ADC_OR written with ADSTART 1 keeps the old register, logs once, a "
    "model_error when strict")
{
  for(auto const* reg : {"CFGR1", "SMPR", "CHSELRMOD0", "CFGR2", "AWD2CR", "AWD3CR", "OR"})
  {
    CAPTURE(std::string_view{reg});
    auto const flip = std::string_view{reg} == "OR" ? 1u : 1u << 2;
    {
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.write("CFGR1", cont);
      f.write("CHSELRMOD0", 1u << pa4);
      static_cast<void>(f.start());
      std::vector<core::log_record> logged;
      auto const                    sink = recorder(f, logged);
      auto const                    kept = f.read(reg);
      f.write(reg, kept ^ flip);
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
        static_cast<void>(f.target->mem().write(f.at("ADC4", reg), 4, kept ^ flip)),
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
  f.port().drive(pa4, 2'400'000);
  auto const again = f.start();
  f.run_until(again + halves(28));
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("DR") == code_of(2'400'000));
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
    "ADSTART with DISCEN with CONT, an unproduced EXTSEL, WAIT, AUTOFF, OVSS 9, LFTRIG, AWD1CH 24, "
    "VREFPROT and VCORE logs nine times, one model_error when "
    "strict that keeps ADC_CR")
{
  auto const setup = [](adc4_fixture const& f) {
    static_cast<void>(f.regulate());
    f.put("ADC4", "PWR", "AUTOFF", 1);
    f.put("ADC4", "PWR", "VREFPROT", 1);
    f.write("CR", advregen | aden);
    f.write(
        "CFGR1", (1u << 10)
                     | (5u << 6)
                     | (1u << 14)
                     | (1u << 23)
                     | (1u << 22)
                     | (24u << 26)
                     | cont
                     | discen);
    f.write("CFGR2", (1u << 0) | (9u << 5) | (1u << 29));
    f.write("CHSELRMOD0", (1u << pa4) | (1u << 12));
  };
  {
    adc4_fixture const f;
    setup(f);
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("CR", advregen | aden | adstart);
    CHECK(logged.size() == 9);
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
    "DS13737 Table 106: tLATR is 4 ADC clock cycles at fADC = HCLK and HCLK / 2, 3.75 at HCLK / 4 "
    "with the bus clock as kernel clock, 4 with any other kernel clock, inferred")
{
  struct leg
  {
    std::uint32_t select;
    std::uint32_t presc;
    std::uint32_t divider;
    std::uint64_t kernel_hz;
    std::uint32_t quarters;
  };
  for(auto const& [select, presc, divider, kernel_hz, quarters] : {
          leg{4, 0, 1,  16'000'000, 16},
          leg{4, 2, 4,  16'000'000, 16},
          leg{4, 7, 16, 16'000'000, 16},
          leg{0, 0, 1,  4'000'000,  16},
          leg{0, 1, 2,  4'000'000,  16},
          leg{0, 2, 4,  4'000'000,  15},
          leg{0, 3, 6,  4'000'000,  16}
  })
  {
    CAPTURE(select);
    CAPTURE(presc);
    adc4_fixture const f;
    f.put("RCC", "RCC_CCIPR3", "ADCDACSEL", select);
    f.write("CCR", presc << 18);
    static_cast<void>(f.regulate());
    f.write("CR", advregen | aden);
    f.run_until(f.now() + 64 * 17 * 4 * cycle * divider);
    REQUIRE(f.isr("ADRDY") == 1);
    f.write("CHSELRMOD0", 1u << pa4);
    auto const at = f.now();
    f.write("CR", advregen | aden | adstart);
    REQUIRE(f.dues().size() == 1);
    auto const hz = kernel_hz / divider;
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

namespace
{
constexpr std::uint64_t    ms            = 1'000 * us;
constexpr std::uint64_t    slow_cycle    = 32 * cycle;
constexpr std::uint64_t    slow_latency  = 4 * slow_cycle;
constexpr std::uint64_t    timer_period  = ms;
constexpr std::uint64_t    timer_clock   = 250'000;
constexpr std::uint32_t    exten_rising  = 1u << 10;
constexpr std::uint32_t    exten_falling = 2u << 10;
constexpr std::uint32_t    exten_both    = 3u << 10;
constexpr std::uint32_t    dmaen         = 1u << 0;
constexpr std::uint32_t    dmacfg        = 1u << 1;
constexpr std::uint32_t    dma_enable    = 1u << 0;
constexpr std::uint32_t    dma_word_copy = 0x400A'0002;
constexpr popcorn::address buffer        = 0x2001'1000;

[[nodiscard]] constexpr auto slow_halves(std::uint64_t count) -> std::uint64_t
{
  return count * slow_cycle / 2;
}

[[nodiscard]] constexpr auto extsel(std::uint32_t selection) -> std::uint32_t
{
  return selection << 6;
}

struct timer_source
{
  std::string_view unit;
  std::string_view enable_register;
  std::string_view enable_bit;
  std::string_view master_field;
};

constexpr timer_source tim15{"TIM15", "RCC_APB2ENR", "TIM15EN", "MMS"};
constexpr timer_source tim6{"TIM6", "RCC_APB1ENR1", "TIM6EN", "MMS"};
constexpr timer_source tim2{"TIM2", "RCC_APB1ENR1", "TIM2EN", "MMS"};

auto start_timer(adc4_fixture const& f, timer_source const& timer, std::uint32_t mode = 2)
    -> std::uint64_t
{
  f.put("RCC", timer.enable_register, timer.enable_bit, 1);
  f.write(timer.unit, "PSC", 0);
  f.write(timer.unit, "ARR", 3'999);
  f.write(timer.unit, "EGR", 1);
  f.write(timer.unit, "SR", 0);
  f.put(timer.unit, "CR2", timer.master_field, mode);
  auto const at = f.now();
  f.put(timer.unit, "CR1", "CEN", 1);
  return at;
}

void slow_enable(adc4_fixture const& f)
{
  f.write("CCR", 8u << 18);
  static_cast<void>(f.regulate());
  f.write("CR", advregen | aden);
  f.run_until(f.now() + 17 * slow_cycle);
  REQUIRE(f.isr("ADRDY") == 1);
}

void arm(
    adc4_fixture const& f,
    std::uint32_t       trigger,
    std::uint32_t       selection,
    std::uint32_t       extra = 0,
    bool                start = true)
{
  slow_enable(f);
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR1", ovrmod | trigger | extsel(selection) | extra);
  if(start)
  {
    f.write("CR", advregen | aden | adstart);
  }
}
}

TEST_CASE(
    "RM0456 Table 329: EXTSEL 2, 3 and 4 take tim2_trgo, tim15_trgo and tim6_trgo, "
    "a rising TRGO starts the sequence tLATR later and ADSTART stays set")
{
  for(auto const& [selection, timer, mode] : {
          std::tuple{2u, tim2,  2u},
          std::tuple{3u, tim15, 2u},
          std::tuple{4u, tim6,  2u}
  })
  {
    CAPTURE(selection);
    adc4_fixture const f;
    auto const         origin = start_timer(f, timer, mode);
    arm(f, exten_rising, selection, 0, false);
    f.port().drive(pa4, 900'000);
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("CR", advregen | aden | adstart);
    CHECK(logged.empty());
    CHECK(f.cr("ADSTART") == 1);
    CHECK(f.dues().empty());
    auto const edge = origin + timer_period;
    f.run_until(edge + us);
    REQUIRE(f.dues().size() == 1);
    CHECK(f.dues()[0] == edge + slow_latency);
    f.run_until(edge + slow_latency + slow_halves(28));
    CHECK(f.isr("EOS") == 1);
    CHECK(f.cr("ADSTART") == 1);
    CHECK(f.read("DR") == code_of(900'000));
    static_cast<void>(f.cpu_read("DR"));
    f.write("ISR", eoc | eos | eosmp);
    f.run_until(edge + timer_period + slow_latency + slow_halves(28));
    CHECK(f.isr("EOS") == 1);
    CHECK(f.isr("OVR") == 0);
  }
}

TEST_CASE("RM0456 Table 329: EXTSEL 1 takes the capture/compare event tim1_cc4, inferred")
{
  adc4_fixture const f;
  f.put("RCC", "RCC_APB2ENR", "TIM1EN", 1);
  f.write("TIM1", "PSC", 0);
  f.write("TIM1", "ARR", 3'999);
  f.write("TIM1", "CCR4", 2'000);
  f.write("TIM1", "EGR", 1);
  f.write("TIM1", "SR", 0);
  auto const origin = f.now();
  f.put("TIM1", "CR1", "CEN", 1);
  arm(f, exten_rising, 1);
  f.port().drive(pa4, 500'000);
  f.run_until(origin + 2'000 * timer_clock + us);
  REQUIRE(f.dues().size() == 1);
  CHECK(f.dues()[0] == origin + 2'000 * timer_clock + slow_latency);
}

TEST_CASE(
    "RM0456 34.4.16: EXTEN picks the TRGO edges: 01 the rising, 10 the falling one timer clock "
    "later, 11 both, the second ignored, on and off the instruction grid")
{
  for(auto const cpi : {1.0, 1.5})
  {
    for(auto const& [trigger, offset] : {
            std::pair{exten_rising,  std::uint64_t{0}},
            std::pair{exten_falling, timer_clock     },
            std::pair{exten_both,    std::uint64_t{0}}
    })
    {
      CAPTURE(trigger);
      CAPTURE(cpi);
      adc4_fixture const f{u575(), false, true, cpi};
      auto const         origin = start_timer(f, tim15);
      arm(f, trigger, 3);
      auto const edge = origin + timer_period;
      f.run_until(edge + 2 * us);
      REQUIRE(f.dues().size() == 1);
      CHECK(f.dues()[0] == edge + offset + slow_latency);
    }
  }
}

TEST_CASE("RM0456 34.4.16: EXTEN 00 and ADSTART 0 start nothing on a TRGO edge")
{
  {
    adc4_fixture const f;
    arm(f, 0, 3);
    f.write("CR", advregen | aden | adstart);
    f.run_until(f.now() + slow_halves(28) + 20 * us);
    REQUIRE(f.isr("EOS") == 1);
    f.write("ISR", eoc | eos | eosmp);
    auto const origin = start_timer(f, tim15);
    f.run_until(origin + timer_period + 20 * us);
    CHECK(f.dues().empty());
    CHECK(f.isr("EOC") == 0);
  }
  adc4_fixture const f;
  auto const         origin = start_timer(f, tim15);
  arm(f, exten_rising, 3, 0, false);
  f.write("CR", advregen | aden);
  f.run_until(origin + timer_period + 20 * us);
  CHECK(f.dues().empty());
  CHECK(f.isr("EOC") == 0);
}

TEST_CASE("RM0456 34.4.16: a trigger during a conversion is ignored, the next one starts a scan")
{
  adc4_fixture const f;
  auto const         origin = start_timer(f, tim15);
  arm(f, exten_rising, 3, 0, false);
  f.write("SMPR", 7);
  f.write("CR", advregen | aden | adstart);
  auto const first = origin + timer_period;
  f.run_until(first + timer_period + 20 * us);
  auto const dues = f.dues();
  REQUIRE(dues.size() == 2);
  CHECK(dues[0] == first + slow_latency + slow_halves(1629));
  CHECK(dues[1] == first + slow_latency + slow_halves(1654));
  f.run_until(first + 2 * timer_period + 2 * us);
  CHECK(f.isr("EOS") == 1);
  REQUIRE(f.dues().size() == 1);
  CHECK(f.dues()[0] == first + 2 * timer_period + slow_latency);
}

TEST_CASE("ADSTP stops the triggers: the timer schedules nothing at its next update")
{
  adc4_fixture const f;
  auto const         idle   = f.target->pending_events();
  auto const         origin = start_timer(f, tim15);
  arm(f, exten_rising, 3, 0, false);
  f.write("CR", advregen | aden | adstart);
  f.run_until(origin + timer_period + slow_latency + slow_halves(28) + us);
  REQUIRE(f.isr("EOS") == 1);
  f.write("CR", advregen | aden | adstp);
  CHECK(f.cr("ADSTART") == 0);
  f.write("ISR", eoc | eos | eosmp);
  f.run_until(origin + 2 * timer_period + 20 * us);
  CHECK(f.dues().empty());
  CHECK(f.isr("EOC") == 0);
  CHECK(f.field("TIM15", "SR", "UIF") == 1);
  CHECK(f.target->pending_events() == idle);
}

TEST_CASE(
    "EXTSEL selecting a signal no model produces logs once per selection and converts nothing, a "
    "model_error when strict that keeps ADC_CR")
{
  for(auto const& [selection, signal] : {
          std::pair{0u, std::string_view{"tim1_trgo2"}},
          std::pair{5u, std::string_view{"lptim1_ch1"}},
          std::pair{6u, std::string_view{"lptim3_ch2"}},
          std::pair{7u, std::string_view{"exti15"}    }
  })
  {
    CAPTURE(signal);
    {
      adc4_fixture const f;
      arm(f, exten_rising, selection, 0, false);
      std::vector<core::log_record> logged;
      auto const                    sink = recorder(f, logged);
      f.write("CR", advregen | aden | adstart);
      f.write("CR", advregen | aden | adstp);
      f.write("CR", advregen | aden | adstart);
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains(signal));
      CHECK(f.cr("ADSTART") == 1);
      f.run_until(f.now() + 4 * timer_period);
      CHECK(f.isr("EOC") == 0);
    }
    adc4_fixture const f{u575(), true};
    arm(f, exten_rising, selection, 0, false);
    CHECK_THROWS_AS(
        static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden | adstart)),
        popcorn::model_error);
    CHECK(f.read("CR") == (advregen | aden));
  }
}

TEST_CASE("a snapshot between two triggered scans resumes with the same scan in a fresh machine")
{
  adc4_fixture const f;
  auto const         origin = start_timer(f, tim15);
  arm(f, exten_rising, 3);
  f.port().drive(pa4, 1'300'000);
  f.run_until(origin + timer_period + slow_latency + slow_halves(28) + us);
  static_cast<void>(f.cpu_read("DR"));
  f.write("ISR", eoc | eos | eosmp);
  f.run_until(origin + timer_period + timer_period / 2);
  auto const saved = f.target->snapshot();
  f.port().drive(pa4, 300'000);
  f.run_until(origin + 2 * timer_period + slow_latency + slow_halves(28) + us);
  CHECK(f.read("DR") == code_of(300'000));

  adc4_fixture const other{no_reset};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.run_until(origin + 2 * timer_period + slow_latency + slow_halves(28) + us);
  CHECK(other.isr("EOS") == 1);
  CHECK(other.read("DR") == code_of(1'300'000));
}

TEST_CASE("an RCC reset of ADC4 stops the triggers and the timer schedules nothing for it")
{
  adc4_fixture const f;
  auto const         idle   = f.target->pending_events();
  auto const         origin = start_timer(f, tim15);
  arm(f, exten_rising, 3, 0, false);
  f.write("CR", advregen | aden | adstart);
  f.run_until(origin + timer_period / 2);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  f.run_until(origin + 2 * timer_period + 20 * us);
  CHECK(f.dues().empty());
  CHECK(f.isr("EOC") == 0);
  CHECK(f.target->pending_events() == idle);
}

namespace
{
void dma_channel(adc4_fixture const& f, std::uint32_t bytes)
{
  f.put("RCC", "RCC_AHB1ENR", "GPDMA1EN", 1);
  f.write("GPDMA1", "GPDMA_C1TR1", dma_word_copy);
  f.write("GPDMA1", "GPDMA_C1TR2", 1);
  f.write("GPDMA1", "GPDMA_C1BR1", bytes);
  f.write("GPDMA1", "GPDMA_C1SAR", static_cast<std::uint32_t>(f.at("ADC4", "DR")));
  f.write("GPDMA1", "GPDMA_C1DAR", static_cast<std::uint32_t>(buffer));
  f.write("GPDMA1", "GPDMA_C1LLR", 0);
  f.write("GPDMA1", "GPDMA_C1CR", dma_enable);
}

auto buffer_words(adc4_fixture const& f, std::size_t count) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::size_t index = 0; index < count; ++index)
  {
    auto const word = f.target->mem().read(buffer + 4 * index, 4);
    REQUIRE(word.has_value());
    out.push_back(static_cast<std::uint32_t>(*word));
  }
  return out;
}
}

TEST_CASE(
    "RM0456 34.4.23: DMAEN raises adc4_dma per result and the GPDMA read clears EOC, DMACFG 0 "
    "stops the requests and freezes ADC_DR once the channel completes")
{
  adc4_fixture const f;
  slow_enable(f);
  for(auto const& [pad, microvolts] : {
          std::pair{pa4, 600'000u  },
          std::pair{pa5, 1'200'000u}
  })
  {
    f.port().drive(pad, microvolts);
  }
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  f.write("CFGR1", cont | dmaen);
  dma_channel(f, 12);
  auto const begin = f.now();
  f.write("CR", advregen | aden | adstart);
  f.run_until(begin + slow_latency + 3 * slow_halves(28) + 20 * us);
  CHECK(
      buffer_words(f, 4)
      == std::vector<std::uint32_t>{code_of(600'000), code_of(1'200'000), code_of(600'000), 0});
  CHECK(f.field("GPDMA1", "GPDMA_C1SR", "TCF") == 1);
  CHECK(f.isr("OVR") == 0);
  CHECK(f.cr("ADSTART") == 1);
  auto const frozen = f.read("DR");
  f.port().drive(pa4, 2'000'000);
  f.run_until(f.now() + 6 * slow_halves(28));
  CHECK(f.read("DR") == frozen);
  CHECK(f.isr("OVR") == 0);
  CHECK(f.dues().empty());
}

TEST_CASE(
    "RM0456 34.4.23: DMACFG 1 keeps requesting after the channel completes, ADC_DR follows "
    "the conversions")
{
  adc4_fixture const f;
  slow_enable(f);
  f.port().drive(pa4, 600'000);
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR1", cont | dmaen | dmacfg | ovrmod);
  dma_channel(f, 8);
  auto const begin = f.now();
  f.write("CR", advregen | aden | adstart);
  f.run_until(begin + slow_latency + 2 * slow_halves(28) + 20 * us);
  CHECK(f.field("GPDMA1", "GPDMA_C1SR", "TCF") == 1);
  f.port().drive(pa4, 2'000'000);
  f.run_until(f.now() + 3 * slow_halves(28));
  CHECK(f.read("DR") == code_of(2'000'000));
  CHECK(f.cr("ADSTART") == 1);
}

TEST_CASE(
    "RM0456 34.4.23: an overrun blocks the DMA requests until software clears OVR, "
    "DMAEN 0 raises no request")
{
  {
    adc4_fixture const f;
    slow_enable(f);
    f.port().drive(pa4, 700'000);
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR1", cont);
    dma_channel(f, 8);
    f.write("CR", advregen | aden | adstart);
    f.run_until(f.now() + 4 * slow_halves(28));
    CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 8);
  }
  adc4_fixture const f;
  slow_enable(f);
  f.port().drive(pa4, 700'000);
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR1", cont | dmaen);
  f.write("CR", advregen | aden | adstart);
  f.run_until(f.now() + slow_latency + 3 * slow_halves(28));
  REQUIRE(f.isr("OVR") == 1);
  dma_channel(f, 8);
  f.run_until(f.now() + 20 * us);
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 8);
  CHECK(f.isr("EOC") == 1);
  f.write("ISR", ovr);
  f.run_until(f.now() + 5 * us);
  CHECK(f.field("GPDMA1", "GPDMA_C1BR1", "BNDT") == 4);
  CHECK(buffer_words(f, 1) == std::vector<std::uint32_t>{code_of(700'000)});
}

TEST_CASE(
    "a snapshot after a DMACFG 0 completion restored into a machine never reset keeps ADC_DR "
    "frozen at the next trigger")
{
  adc4_fixture const f;
  auto const         origin = start_timer(f, tim15);
  arm(f, exten_rising, 3, dmaen, false);
  f.port().drive(pa4, 600'000);
  dma_channel(f, 4);
  f.write("CR", advregen | aden | adstart);
  auto const first = origin + timer_period;
  f.run_until(first + slow_latency + slow_halves(28) + 20 * us);
  REQUIRE(f.field("GPDMA1", "GPDMA_C1SR", "TCF") == 1);
  auto const         saved = f.target->snapshot();
  adc4_fixture const other{no_reset};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.port().drive(pa4, 2'000'000);
  other.run_until(first + timer_period + slow_latency + slow_halves(28) + 20 * us);
  CHECK(other.read("DR") == code_of(600'000));
  CHECK(other.isr("EOC") == 0);
  CHECK(other.isr("OVR") == 0);
}

TEST_CASE(
    "after a DMACFG 0 completion a hardware trigger starts a sequence whose result leaves "
    "ADC_DR and EOC frozen, until ADSTART rises again")
{
  adc4_fixture const f;
  auto const         origin = start_timer(f, tim15);
  arm(f, exten_rising, 3, dmaen, false);
  f.port().drive(pa4, 800'000);
  dma_channel(f, 4);
  f.write("CR", advregen | aden | adstart);
  auto const first = origin + timer_period;
  f.run_until(first + slow_latency + slow_halves(28) + 20 * us);
  REQUIRE(f.field("GPDMA1", "GPDMA_C1SR", "TCF") == 1);
  CHECK(buffer_words(f, 1) == std::vector<std::uint32_t>{code_of(800'000)});
  f.write("ISR", eos);
  f.port().drive(pa4, 2'000'000);
  f.run_until(first + timer_period + 2 * us);
  CHECK(f.dues().size() == 1);
  f.run_until(first + timer_period + slow_latency + slow_halves(28) + 20 * us);
  CHECK(f.read("DR") == code_of(800'000));
  CHECK(f.isr("EOC") == 0);
  CHECK(f.isr("OVR") == 0);
  CHECK(f.isr("EOS") == 1);
}

namespace
{
constexpr std::string_view trgo = "tim15_trgo";

auto sink_of(adc4_fixture const& f) -> stm32::trigger_sink&
{
  auto* const found = f.model_of<stm32::trigger_sink>("ADC4");
  REQUIRE(found != nullptr);
  return *found;
}
}

TEST_CASE(
    "checklist 66 and 72: edges held for their own time survive a snapshot restored into a "
    "machine never reset, and a later edge keeps its own slot")
{
  adc4_fixture const f;
  arm(f, exten_falling, 3);
  auto const origin = f.now();
  sink_of(f).triggered(trgo, stm32::trigger_edge::falling, origin + 100 * us);
  auto const saved = f.target->snapshot();
  auto const check = [&](adc4_fixture const& g) {
    sink_of(g).triggered(trgo, stm32::trigger_edge::falling, origin + 20 * us);
    g.run_until(origin + 30 * us);
    CHECK(g.dues().size() == 3);
    g.run_until(origin + 56 * us + early);
    sink_of(g).triggered(trgo, stm32::trigger_edge::falling, origin + 60 * us);
    g.run_until(origin + 61 * us);
    CHECK(
        g.dues() == std::vector<std::uint64_t>{origin + 60 * us + slow_latency, origin + 100 * us});
    g.run_until(origin + 101 * us);
    CHECK(g.dues() == std::vector<std::uint64_t>{origin + 100 * us + slow_latency});
  };
  f.target->restore(saved);
  check(f);
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(fresh);
}

TEST_CASE(
    "checklist 31: an RCC reset of ADC4 drops an edge held for its own time, re-armed before its "
    "due the stale event starts nothing and a later edge keeps its own slot")
{
  for(std::uint32_t consumed = 0; consumed < 4; ++consumed)
  {
    CAPTURE(consumed);
    adc4_fixture const f;
    arm(f, exten_falling, 3);
    auto const origin = f.now();
    auto const stale  = origin + 500 * us;
    auto const later  = origin + 900 * us;
    sink_of(f).triggered(trgo, stm32::trigger_edge::falling, stale);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
    f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
    arm(f, exten_falling, 3);
    for(std::uint32_t edge = 0; edge < consumed; ++edge)
    {
      auto const at = f.now() + us;
      sink_of(f).triggered(trgo, stm32::trigger_edge::falling, at);
      f.run_until(at + 41 * us);
    }
    REQUIRE(f.now() < stale - 10 * us);
    sink_of(f).triggered(trgo, stm32::trigger_edge::falling, later);
    f.run_until(stale + us);
    CHECK(f.dues() == std::vector<std::uint64_t>{later});
    f.run_until(later + us);
    CHECK(f.dues() == std::vector<std::uint64_t>{later + slow_latency});
  }
}

TEST_CASE(
    "a trigger edge held for the exact end of the conversion is decided after the ADC's own "
    "events due then, and starts the next sequence, inferred")
{
  adc4_fixture const f;
  arm(f, exten_rising, 3);
  auto const origin = f.now();
  sink_of(f).triggered(trgo, stm32::trigger_edge::rising, origin);
  auto const end = origin + slow_latency + slow_halves(28);
  sink_of(f).triggered(trgo, stm32::trigger_edge::rising, end);
  f.run_until(end + us);
  CHECK(f.isr("EOS") == 1);
  CHECK(f.dues() == std::vector<std::uint64_t>{end + slow_latency});
}

TEST_CASE(
    "DS13737 Table 106: tLATR of an edge is 4 ADC clock cycles at fADC = HCLK and HCLK / 2, 3.75 "
    "at HCLK / 4 on the bus clock, 4 on any other kernel clock, inferred, counted from the edge")
{
  struct leg
  {
    std::uint32_t select;
    std::uint32_t presc;
    std::uint32_t divider;
    std::uint64_t kernel_hz;
    std::uint32_t quarters;
  };
  for(auto const& [select, presc, divider, kernel_hz, quarters] : {
          leg{4, 0, 1, 16'000'000, 16},
          leg{0, 0, 1, 4'000'000,  16},
          leg{0, 2, 4, 4'000'000,  15}
  })
  {
    CAPTURE(select);
    CAPTURE(presc);
    adc4_fixture const f;
    f.put("RCC", "RCC_CCIPR3", "ADCDACSEL", select);
    f.write("CCR", presc << 18);
    static_cast<void>(f.regulate());
    f.write("CR", advregen | aden);
    f.run_until(f.now() + 64 * 17 * 4 * cycle * divider);
    REQUIRE(f.isr("ADRDY") == 1);
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR1", ovrmod | exten_rising | extsel(3));
    f.write("CR", advregen | aden | adstart);
    auto const at = f.now();
    sink_of(f).triggered(trgo, stm32::trigger_edge::rising, at);
    auto const hz = kernel_hz / divider;
    REQUIRE(f.dues().size() == 1);
    CHECK(f.dues()[0] == at + (quarters * 1'000'000'000'000ull + 4 * hz - 1) / (4 * hz));
  }
}

TEST_CASE("RM0456 34.4.23: DMAEN written with EOC set raises the request at once, inferred")
{
  adc4_fixture const f;
  slow_enable(f);
  f.port().drive(pa4, 700'000);
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CR", advregen | aden | adstart);
  f.run_until(f.now() + slow_latency + slow_halves(28) + us);
  REQUIRE(f.isr("EOC") == 1);
  f.write("CR", advregen | aden | adstp);
  dma_channel(f, 4);
  f.run_until(f.now() + 20 * us);
  CHECK(buffer_words(f, 1) == std::vector<std::uint32_t>{0});
  f.write("CFGR1", dmaen);
  f.run_until(f.now() + 5 * us);
  CHECK(buffer_words(f, 1) == std::vector<std::uint32_t>{code_of(700'000)});
  CHECK(f.isr("EOC") == 0);
}

TEST_CASE(
    "RM0456 TIM15 MMS 000: the UG write of EGR is a TRGO pulse, an ADC4 armed before the timer "
    "starts its sequence tLATR after that write")
{
  adc4_fixture const f;
  arm(f, exten_rising, 3);
  auto const at = f.now();
  f.put("RCC", "RCC_APB2ENR", "TIM15EN", 1);
  f.write("TIM15", "PSC", 0);
  f.write("TIM15", "ARR", 3'999);
  CHECK(f.dues().empty());
  f.write("TIM15", "EGR", 1);
  CHECK(f.dues() == std::vector<std::uint64_t>{at + slow_latency});
}

TEST_CASE(
    "RM0456 34.4.25: a result below LT1 or above HT1 sets AWD1 with EOC, one at either "
    "threshold does not, the set time inferred")
{
  struct row
  {
    std::uint32_t code;
    bool          flagged;
  };
  for(auto const& [code, flagged] : {
          row{0x3FF, true },
          row{0x400, false},
          row{0x800, false},
          row{0x801, true }
  })
  {
    CAPTURE(code);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(code));
    f.write("AWD1TR", window(0x800, 0x400));
    f.write("CFGR1", awd1en);
    f.write("CHSELRMOD0", 1u << pa4);
    auto const begin = f.start();
    f.run_until(begin + halves(28) - early);
    CHECK(f.isr("AWD1") == 0);
    f.run_until(begin + halves(28));
    CHECK(f.isr("EOC") == 1);
    CHECK(f.isr("AWD1") == (flagged ? 1u : 0u));
    CHECK(f.isr("AWD2") == 0);
    CHECK(f.isr("AWD3") == 0);
  }
}

TEST_CASE("RM0456 34.4.25: an LT above HT flags every result")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x600));
  f.write("AWD1TR", window(0x400, 0x800));
  f.write("CFGR1", awd1en);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + halves(28));
  CHECK(f.isr("AWD1") == 1);
}

TEST_CASE(
    "RM0456 34.4.25, Table 334: AWD1SGL 0 guards every channel of the sequence, AWD1SGL 1 "
    "only the channel AWD1CH names")
{
  struct row
  {
    std::uint32_t config;
    std::uint32_t code4;
    std::uint32_t code5;
    bool          flagged;
  };
  for(auto const& [config, code4, code5, flagged] : {
          row{awd1en,                         0x600, 0x900, true },
          row{awd1en,                         0x900, 0x600, true },
          row{awd1en | awd1sgl | awd1ch(pa5), 0x600, 0x900, true },
          row{awd1en | awd1sgl | awd1ch(pa5), 0x900, 0x600, false},
          row{awd1en | awd1sgl | awd1ch(pa4), 0x900, 0x600, true },
          row{awd1en | awd1sgl | awd1ch(pa4), 0x600, 0x900, false},
          row{awd1en | awd1sgl | awd1ch(pa6), 0x900, 0x900, false},
          row{awd1sgl | awd1ch(pa5),          0x900, 0x900, false},
          row{awd1en,                         0x600, 0x600, false}
  })
  {
    CAPTURE(config);
    CAPTURE(code4);
    CAPTURE(code5);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(code4));
    f.port().drive(pa5, level_of(code5));
    f.write("AWD1TR", window(0x800, 0x400));
    f.write("CFGR1", config);
    f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
    auto const begin = f.start();
    f.run_until(begin + 2 * halves(28));
    CHECK(f.isr("EOS") == 1);
    CHECK(f.isr("AWD1") == (flagged ? 1u : 0u));
  }
}

TEST_CASE(
    "RM0456 34.4.25: AWD2 and AWD3 guard the channels of ADC_AWD2CR and ADC_AWD3CR, a "
    "neighbour channel out of the window sets nothing")
{
  for(std::uint32_t watchdog : {2u, 3u})
  {
    CAPTURE(watchdog);
    auto const         mask = watchdog == 2 ? "AWD2CR" : "AWD3CR";
    auto const         flag = watchdog == 2 ? "AWD2" : "AWD3";
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0x900));
    f.port().drive(pa5, level_of(0x600));
    f.port().drive(pa6, level_of(0x900));
    f.write(std::format("AWD{}TR", watchdog), window(0x800, 0x400));
    f.write(mask, 1u << pa5);
    f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5) | (1u << pa6));
    auto const first = f.start();
    f.run_until(first + 3 * halves(28));
    CHECK(f.isr("EOS") == 1);
    CHECK(f.isr(flag) == 0);
    f.write("ISR", eos | eoc | ovr);
    f.port().drive(pa5, level_of(0x401 - 2));
    auto const second = f.start();
    f.run_until(second + 3 * halves(28));
    CHECK(f.isr(flag) == 1);
  }
}

TEST_CASE("RM0456 34.7.7 to 34.7.15: each watchdog compares with its own thresholds and mask")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x500));
  f.write("AWD2TR", window(0x800, 0x400));
  f.write("AWD3TR", window(0xC00, 0x600));
  f.write("AWD2CR", 1u << pa4);
  f.write("AWD3CR", 1u << pa4);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + halves(28));
  CHECK(f.isr("AWD1") == 0);
  CHECK(f.isr("AWD2") == 0);
  CHECK(f.isr("AWD3") == 1);
}

TEST_CASE("RM0456 34.7.1 and 34.6: AWD1 to AWD3 with their AWDxIE raise ADC4_IRQn until cleared")
{
  for(std::uint32_t watchdog : {1u, 2u, 3u})
  {
    CAPTURE(watchdog);
    auto const         bit  = 1u << (6 + watchdog);
    auto const         flag = std::format("AWD{}", watchdog);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0x900));
    guard_pa4(f, watchdog);
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("IER", bit);
    auto const begin = f.start();
    f.run_until(begin + halves(28) - early);
    CHECK_FALSE(f.pending(irq));
    f.run_until(begin + halves(28));
    CHECK(f.isr(flag) == 1);
    CHECK(f.pending(irq));
    f.write("ISR", bit);
    CHECK(f.isr(flag) == 0);
    REQUIRE(f.target->mem().write(0xe000'e280u + 4 * (irq / 32), 4, 1u << (irq % 32)).has_value());
    CHECK_FALSE(f.pending(irq));
  }
}

TEST_CASE(
    "RM0456 34.4.25, Table 333: the comparison uses the raw result left aligned, a 10, 8 or 6 "
    "bit result is shifted left 2, 4 or 6 bits")
{
  struct row
  {
    std::uint32_t resolution;
    std::uint32_t code;
    bool          flagged;
  };
  for(auto const& [resolution, code, flagged] : {
          row{1, 0x804, true },
          row{1, 0x801, false},
          row{2, 0x810, true },
          row{2, 0x80F, false},
          row{3, 0x840, true },
          row{3, 0x83F, false},
          row{0, 0x801, true }
  })
  {
    CAPTURE(resolution);
    CAPTURE(code);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(code));
    f.write("AWD1TR", window(0x800, 0x400));
    f.write("CFGR1", awd1en | (resolution << 2));
    f.write("CHSELRMOD0", 1u << pa4);
    auto const begin = f.start();
    f.run_until(begin + halves(28));
    CHECK(f.isr("EOC") == 1);
    CHECK(f.isr("AWD1") == (flagged ? 1u : 0u));
  }
}

TEST_CASE("RM0456 34.4.25: a result an overrun discards is compared, AWD1 sets for it")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x600));
  f.port().drive(pa5, level_of(0x900));
  f.write("AWD1TR", window(0x800, 0x400));
  f.write("CFGR1", awd1en);
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  auto const begin = f.start();
  f.run_until(begin + halves(28) + halves(3) + early);
  CHECK(f.isr("AWD1") == 0);
  f.run_until(begin + 2 * halves(28));
  CHECK(f.isr("OVR") == 1);
  CHECK(f.read("DR") == code_of(level_of(0x600)));
  CHECK(f.isr("AWD1") == 1);
}

TEST_CASE(
    "RM0456 34.4.25: a threshold write during the conversion masks that watchdog for it, the "
    "next conversion compares with the new thresholds, the start of conversion taken as the "
    "start of sampling, inferred")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x800));
  f.write("AWD1TR", window(0xA00, 0x100));
  f.write("AWD2TR", window(0xA00, 0x100));
  f.write("CFGR1", awd1en | cont);
  f.write("AWD2CR", 1u << pa4);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + halves(1));
  f.write("AWD1TR", window(0x200, 0x100));
  f.run_until(begin + halves(28));
  CHECK(f.isr("EOC") == 1);
  CHECK(f.isr("AWD1") == 0);
  f.write("AWD2TR", window(0x200, 0x100));
  f.run_until(begin + 2 * halves(28));
  CHECK(f.isr("AWD1") == 1);
  CHECK(f.isr("AWD2") == 0);
  f.run_until(begin + 3 * halves(28));
  CHECK(f.isr("AWD2") == 1);
}

TEST_CASE("RM0456 34.4.25: a threshold write while idle or in the tLATR applies to the next result")
{
  adc4_fixture const f;
  slow_enable(f);
  f.port().drive(pa4, level_of(0x800));
  f.write("CFGR1", awd1en);
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("AWD1TR", window(0x200, 0x100));
  auto const first = f.now();
  f.write("CR", advregen | aden | adstart);
  f.run_until(first + slow_latency + slow_halves(28));
  CHECK(f.isr("AWD1") == 1);
  f.write("ISR", eos | eoc | (1u << 7));
  f.write("AWD1TR", window(0xA00, 0x100));
  auto const second = f.now();
  f.write("CR", advregen | aden | adstart);
  f.run_until(second + slow_latency / 2);
  f.write("AWD1TR", window(0x200, 0x100));
  f.run_until(second + slow_latency + slow_halves(28));
  CHECK(f.isr("AWD1") == 1);
}

TEST_CASE(
    "RM0456 34.7.4: an AWD1CH of 18 to 21 or 24 to 31 guards no channel and logs once at "
    "ADSTART, a valid code or AWD1SGL 0 logs nothing")
{
  struct row
  {
    std::uint32_t config;
    bool          reserved;
  };
  for(auto const& [config, reserved] : {
          row{awd1en | awd1sgl | awd1ch(18), true },
          row{awd1en | awd1sgl | awd1ch(19), true },
          row{awd1en | awd1sgl | awd1ch(20), true },
          row{awd1en | awd1sgl | awd1ch(21), true },
          row{awd1en | awd1sgl | awd1ch(24), true },
          row{awd1en | awd1sgl | awd1ch(31), true },
          row{awd1en | awd1sgl | awd1ch(17), false},
          row{awd1en | awd1sgl | awd1ch(22), false},
          row{awd1en | awd1sgl | awd1ch(23), false},
          row{awd1en | awd1ch(24),           false},
          row{awd1en | awd1ch(19),           false}
  })
  {
    CAPTURE(config);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0x900));
    f.write("AWD1TR", window(0x800, 0x400));
    f.write("CFGR1", config);
    f.write("CHSELRMOD0", 1u << pa4);
    std::vector<core::log_record> logged;
    auto const                    sink  = recorder(f, logged);
    auto const                    begin = f.start();
    f.run_until(begin + halves(28));
    CHECK(f.isr("AWD1") == ((config & awd1sgl) == 0 ? 1u : 0u));
    REQUIRE(logged.size() == (reserved ? 1u : 0u));
    if(reserved)
    {
      CHECK(logged[0].message.contains("AWD1CH 18 to 21 and 24 to 31"));
    }
  }
}

TEST_CASE(
    "RM0456 34.4.25: a threshold write of AWD3 during the conversion masks AWD3 only, "
    "AWD2 keeps comparing")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x800));
  f.write("AWD2TR", window(0x700, 0x100));
  f.write("AWD3TR", window(0xA00, 0x100));
  f.write("CFGR1", cont);
  f.write("AWD2CR", 1u << pa4);
  f.write("AWD3CR", 1u << pa4);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + halves(1));
  f.write("AWD3TR", window(0x200, 0x100));
  f.run_until(begin + halves(28));
  CHECK(f.isr("EOC") == 1);
  CHECK(f.isr("AWD3") == 0);
  CHECK(f.isr("AWD2") == 1);
  f.run_until(begin + 2 * halves(28));
  CHECK(f.isr("AWD3") == 1);
}

TEST_CASE(
    "RM0456 34.7.7: an RCC reset of ADC4 restores the thresholds, the channel masks and the flags")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x900));
  guard_pa4(f, 2);
  f.write("AWD3TR", window(0x111, 0x222));
  f.write("AWD3CR", 1u << pa5);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + halves(28));
  REQUIRE(f.isr("AWD2") == 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  for(auto const* reg : {"AWD1TR", "AWD2TR", "AWD3TR"})
  {
    CAPTURE(std::string_view{reg});
    CHECK(f.read(reg) == 0x0FFF'0000);
  }
  CHECK(f.read("AWD2CR") == 0);
  CHECK(f.read("AWD3CR") == 0);
  CHECK(f.read("ISR") == 0);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot after a threshold write mid-conversion keeps the mask, a "
    "fresh machine restored from it compares the next result with the new thresholds")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x800));
  f.write("AWD1TR", window(0xA00, 0x100));
  f.write("CFGR1", awd1en | cont);
  f.write("CHSELRMOD0", 1u << pa4);
  auto const begin = f.start();
  f.run_until(begin + halves(1));
  f.write("AWD1TR", window(0x200, 0x100));
  auto const saved = f.target->snapshot();
  auto const check = [&](adc4_fixture const& g) {
    g.run_until(begin + halves(28));
    CHECK(g.isr("EOC") == 1);
    CHECK(g.isr("AWD1") == 0);
    g.run_until(begin + 2 * halves(28));
    CHECK(g.isr("AWD1") == 1);
  };
  f.target->restore(saved);
  check(f);
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(fresh);
}

namespace
{
constexpr std::uint64_t conversion = 28 * cycle / 2;

[[nodiscard]] constexpr auto
    oversampling(std::uint32_t ratio, std::uint32_t shift, std::uint32_t extra = 0) -> std::uint32_t
{
  return 1u | ratio << 2 | shift << 5 | extra;
}

constexpr std::uint32_t tovs = 1u << 9;
}

TEST_CASE(
    "RM0456 34.4.26: OVSR 000 to 111 sum 2 to 256 conversions, EOC sets with the Nth, each "
    "conversion chained from the stored end of the previous one")
{
  for(std::uint32_t ratio = 0; ratio < 8; ++ratio)
  {
    CAPTURE(ratio);
    auto const         count = 2u << ratio;
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0x123));
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR2", oversampling(ratio, 0));
    auto const begin = f.start();
    if(count > 2)
    {
      f.run_until(begin + conversion + conversion / 2);
      CHECK(f.dues() == std::vector<std::uint64_t>{begin + 2 * conversion});
    }
    f.run_until(begin + count * conversion - early);
    CHECK(f.isr("EOC") == 0);
    CHECK(f.isr("EOS") == 0);
    f.run_until(begin + count * conversion);
    CHECK(f.isr("EOC") == 1);
    CHECK(f.isr("EOS") == 1);
    CHECK(f.read("DR") == ((count * 0x123u) & 0xFFFFu));
  }
}

TEST_CASE(
    "RM0456 34.4.26 Table 335: the sum is shifted right by OVSS with rounding to the nearest, "
    "then truncated to 16 bits")
{
  struct row
  {
    std::uint32_t                ratio;
    std::array<std::uint32_t, 9> expected;
  };
  for(auto const& [ratio, expected] : {
          row{0, {0x1FFE, 0x0FFF, 0x0800, 0x0400, 0x0200, 0x0100, 0x0080, 0x0040, 0x0020}},
          row{3, {0xFFF0, 0x7FF8, 0x3FFC, 0x1FFE, 0x0FFF, 0x0800, 0x0400, 0x0200, 0x0100}},
          row{7, {0xFF00, 0xFF80, 0xFFC0, 0xFFE0, 0xFFF0, 0x7FF8, 0x3FFC, 0x1FFE, 0x0FFF}}
  })
  {
    for(std::uint32_t shift = 0; shift < 9; ++shift)
    {
      CAPTURE(ratio);
      CAPTURE(shift);
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.port().drive(pa4, level_of(0xFFF));
      f.write("CHSELRMOD0", 1u << pa4);
      f.write("CFGR2", oversampling(ratio, shift));
      auto const begin = f.start();
      f.run_until(begin + (2u << ratio) * conversion);
      CHECK(f.read("DR") == expected[shift]);
    }
  }
}

TEST_CASE(
    "RM0456 34.4.26: ALIGN is ignored with the oversampler, RES applies to every conversion "
    "before the sum")
{
  struct row
  {
    std::uint32_t config;
    std::uint64_t halves_per_conversion;
    std::uint32_t expected;
  };
  for(auto const& [config, per, expected] : {
          row{0,                 28, 2 * 0xABCu       },
          row{align,             28, 2 * 0xABCu       },
          row{1u << 2,           24, 2 * (0xABCu >> 2)},
          row{align | (3u << 2), 16, 2 * (0xABCu >> 6)}
  })
  {
    CAPTURE(config);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0xABC));
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR1", config);
    f.write("CFGR2", oversampling(0, 0));
    auto const begin = f.start();
    f.run_until(begin + 2 * halves(per));
    CHECK(f.isr("EOC") == 1);
    CHECK(f.read("DR") == expected);
  }
}

TEST_CASE(
    "RM0456 34.4.26: EOSMP sets after every sampling, EOC after the Nth conversion of a channel "
    "and EOS after N conversions of every channel, intermediate conversions set no OVR")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x300));
  f.port().drive(pa5, level_of(0x500));
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  f.write("CFGR2", oversampling(0, 0));
  auto const begin = f.start();
  f.run_until(begin + halves(3) + early);
  CHECK(f.isr("EOSMP") == 1);
  f.write("ISR", eosmp);
  f.run_until(begin + conversion - early);
  CHECK(f.isr("EOSMP") == 0);
  f.run_until(begin + conversion + halves(3) + early);
  CHECK(f.isr("EOSMP") == 1);
  CHECK(f.isr("EOC") == 0);
  f.run_until(begin + 2 * conversion);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.isr("EOS") == 0);
  CHECK(f.read("DR") == 2 * 0x300u);
  f.run_until(begin + 3 * conversion + early);
  CHECK(f.isr("OVR") == 0);
  f.run_until(begin + 4 * conversion);
  CHECK(f.isr("EOS") == 1);
  CHECK(f.isr("OVR") == 1);
  CHECK(f.read("DR") == 2 * 0x300u);
}

TEST_CASE(
    "RM0456 34.4.26: with DISCEN one ADSTART converts the N conversions of one channel, EOC "
    "clears ADSTART")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x200));
  f.port().drive(pa5, level_of(0x400));
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  f.write("CFGR1", discen);
  f.write("CFGR2", oversampling(1, 0));
  auto const first = f.start();
  f.run_until(first + 4 * conversion - early);
  CHECK(f.cr("ADSTART") == 1);
  f.run_until(first + 4 * conversion);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.isr("EOS") == 0);
  CHECK(f.cpu_read("DR") == 4 * 0x200u);
  auto const second = f.start();
  f.run_until(second + 4 * conversion);
  CHECK(f.isr("EOS") == 1);
  CHECK(f.read("DR") == 4 * 0x400u);
}

namespace
{
void fire(adc4_fixture const& f, std::uint64_t at)
{
  sink_of(f).triggered(trgo, stm32::trigger_edge::rising, at);
}
}

TEST_CASE(
    "RM0456 34.4.26: without TOVS one trigger starts the N conversions of a channel, with TOVS "
    "each trigger starts one, a trigger during a conversion is ignored and ADSTART stays set")
{
  for(auto const per_trigger : {false, true})
  {
    CAPTURE(per_trigger);
    adc4_fixture const f;
    arm(f, exten_rising, 3, 0, false);
    f.port().drive(pa4, level_of(0x180));
    f.write("CFGR2", oversampling(0, 0, per_trigger ? tovs : 0));
    f.write("CR", advregen | aden | adstart);
    auto const conv   = slow_halves(28);
    auto const origin = f.now();
    fire(f, origin);
    f.run_until(origin + slow_latency + conv / 2);
    fire(f, f.now());
    f.run_until(origin + slow_latency + conv + conv / 2);
    CHECK(f.isr("EOC") == 0);
    f.run_until(origin + slow_latency + 2 * conv + us);
    CHECK(f.isr("EOC") == (per_trigger ? 0u : 1u));
    CHECK(f.cr("ADSTART") == 1);
    if(not per_trigger)
    {
      CHECK(f.read("DR") == 2 * 0x180u);
      continue;
    }
    f.run_until(origin + slow_latency + 6 * conv);
    CHECK(f.isr("EOC") == 0);
    auto const second = f.now();
    fire(f, second);
    f.run_until(second + slow_latency + conv + us);
    CHECK(f.isr("EOC") == 1);
    CHECK(f.read("DR") == 2 * 0x180u);
    CHECK(f.cr("ADSTART") == 1);
  }
}

TEST_CASE(
    "RM0456 34.4.26: with TOVS the series moves to the next channel after the Nth trigger of "
    "each, EOS sets with the last")
{
  adc4_fixture const f;
  arm(f, exten_rising, 3, 0, false);
  f.port().drive(pa4, level_of(0x100));
  f.port().drive(pa5, level_of(0x700));
  f.write("CHSELRMOD0", (1u << pa4) | (1u << pa5));
  f.write("CFGR2", oversampling(0, 0, tovs));
  f.write("CR", advregen | aden | adstart);
  auto const conv = slow_halves(28);
  for(auto count = 0; count < 4; ++count)
  {
    CAPTURE(count);
    auto const at = f.now();
    fire(f, at);
    f.run_until(at + slow_latency + conv + us);
    if(count == 1)
    {
      CHECK(f.isr("EOC") == 1);
      CHECK(f.read("DR") == 2 * 0x100u);
      f.write("ISR", eoc | eosmp);
      f.run_until(f.now() + 6 * conv);
      CHECK(f.isr("EOSMP") == 0);
    }
    CHECK(f.isr("EOS") == (count == 3 ? 1u : 0u));
  }
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("DR") == 2 * 0x700u);
}

TEST_CASE(
    "RM0456 34.4.26: with TOVS and a software trigger each ADSTART starts one conversion and "
    "ADSTART clears at its end, inferred")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x240));
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR2", oversampling(0, 0, tovs));
  auto const first = f.start();
  f.run_until(first + conversion);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.isr("EOC") == 0);
  auto const second = f.start();
  f.run_until(second + conversion);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("DR") == 2 * 0x240u);
}

TEST_CASE(
    "RM0456 34.4.26: the watchdogs compare ADC_DR[15:4] of the oversampled result and ignore RES")
{
  struct row
  {
    std::uint32_t resolution;
    std::uint32_t code;
    bool          oversampled;
    bool          flagged;
  };
  for(auto const& [resolution, code, oversampled, flagged] : {
          row{0, 0x800, true,  false},
          row{0, 0x801, true,  true },
          row{1, 0x3FF, true,  true },
          row{1, 0x3FF, false, false}
  })
  {
    CAPTURE(resolution);
    CAPTURE(code);
    CAPTURE(oversampled);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(code));
    f.write("AWD1TR", window(0x800, resolution == 0 ? 0x100 : 0x100));
    f.write("CFGR1", awd1en | (resolution << 2));
    f.write("CHSELRMOD0", 1u << pa4);
    if(oversampled)
    {
      f.write("CFGR2", oversampling(3, 0));
    }
    auto const begin = f.start();
    f.run_until(begin + 16 * halves(oversampled ? (resolution == 0 ? 28 : 24) : 24));
    CHECK(f.isr("EOC") == 1);
    CHECK(f.isr("AWD1") == (flagged ? 1u : 0u));
  }
}

TEST_CASE("RM0456 34.4.26: the watchdogs compare the result truncated to 16 bits")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0xFFF));
  f.write("AWD1TR", window(0xFF8, 0x100));
  f.write("CFGR1", awd1en);
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR2", oversampling(7, 0));
  auto const begin = f.start();
  f.run_until(begin + 256 * conversion);
  CHECK(f.read("DR") == 0xFF00);
  CHECK(f.isr("AWD1") == 0);
}

TEST_CASE("ADSTP during a series drops the sum, the next ADSTART starts a new series")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x300));
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR2", oversampling(1, 0));
  auto const first = f.start();
  f.run_until(first + 2 * conversion + early);
  f.write("CR", advregen | aden | adstart | adstp);
  f.port().drive(pa4, level_of(0x100));
  auto const second = f.start();
  f.run_until(second + 4 * conversion);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("DR") == 4 * 0x100u);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot in the middle of a series restored into a machine never "
    "reset keeps the sum and the count")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x300));
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR2", oversampling(1, 0));
  auto const begin = f.start();
  f.run_until(begin + 2 * conversion + early);
  auto const saved = f.target->snapshot();
  auto const check = [&](adc4_fixture const& g) {
    g.port().drive(pa4, level_of(0x500));
    g.run_until(begin + 4 * conversion - early);
    CHECK(g.isr("EOC") == 0);
    g.run_until(begin + 4 * conversion);
    CHECK(g.isr("EOC") == 1);
    CHECK(g.read("DR") == 3 * 0x300u + 0x500u);
  };
  f.target->restore(saved);
  check(f);
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(fresh);
}

TEST_CASE(
    "RM0456 34.7.5: an OVSS of 9 to 15 shifts by 8 and logs once at ADSTART, a TOVS with CONT "
    "logs as DISCEN with CONT")
{
  {
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0xFFF));
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR2", oversampling(0, 12));
    std::vector<core::log_record> logged;
    auto const                    sink  = recorder(f, logged);
    auto const                    begin = f.start();
    f.run_until(begin + 2 * conversion);
    CHECK(f.read("DR") == 0x20);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("OVSS 9 to 15"));
  }
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR1", cont);
  f.write("CFGR2", tovs);
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  static_cast<void>(f.start());
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("DISCEN or TOVS with CONT"));
}

TEST_CASE(
    "inferred: a threshold write during any conversion of an oversampled series masks the "
    "watchdog for the result of the series, a write before the series or after it does not")
{
  for(auto const& [write_at, flagged] : {
          std::pair{halves(1),              false},
          std::pair{conversion + halves(1), false},
          std::pair{std::uint64_t{0},       true }
  })
  {
    CAPTURE(write_at);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0x900));
    f.write("AWD1TR", window(0x100, 0x080));
    f.write("CFGR1", awd1en);
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR2", oversampling(0, 0));
    if(write_at == 0)
    {
      f.write("AWD1TR", window(0x100, 0x080));
    }
    auto const begin = f.start();
    if(write_at != 0)
    {
      f.run_until(begin + write_at);
      f.write("AWD1TR", window(0x100, 0x080));
    }
    f.run_until(begin + 2 * conversion);
    CHECK(f.isr("EOC") == 1);
    CHECK(f.isr("AWD1") == (flagged ? 1u : 0u));
    if(not flagged)
    {
      static_cast<void>(f.cpu_read("DR"));
      auto const again = f.start();
      f.run_until(again + 2 * conversion);
      CHECK(f.isr("AWD1") == 1);
    }
  }
}

TEST_CASE(
    "inferred: ADDIS and an RCC reset in the middle of a series drop the sum and the count, the "
    "next series needs its own N conversions")
{
  for(auto const reset : {false, true})
  {
    CAPTURE(reset);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.port().drive(pa4, level_of(0x300));
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR2", oversampling(1, 0, tovs));
    auto const first = f.start();
    f.run_until(first + conversion + early);
    CHECK(f.isr("EOC") == 0);
    if(reset)
    {
      f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
      f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
    }
    else
    {
      f.write("CR", advregen | addis);
      f.run_until(f.now() + tdis + early);
      REQUIRE(f.cr("ADEN") == 0);
    }
    static_cast<void>(f.enable());
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR2", oversampling(1, 0, tovs));
    for(auto count = 0; count < 4; ++count)
    {
      CAPTURE(count);
      auto const at = f.start();
      f.run_until(at + conversion + early);
      CHECK(f.isr("EOC") == (count == 3 ? 1u : 0u));
    }
    CHECK(f.read("DR") == 4 * 0x300u);
  }
}

TEST_CASE(
    "RM0456 34.4.17 and 34.4.26: TOVS with CONT converts one conversion per ADSTART with CONT "
    "ignored and logs once")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.port().drive(pa4, level_of(0x240));
  f.write("CHSELRMOD0", 1u << pa4);
  f.write("CFGR1", cont);
  f.write("CFGR2", oversampling(0, 0, tovs));
  std::vector<core::log_record> logged;
  auto const                    sink  = recorder(f, logged);
  auto const                    first = f.start();
  f.run_until(first + conversion);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.isr("EOC") == 0);
  auto const second = f.start();
  f.run_until(second + conversion);
  CHECK(f.cr("ADSTART") == 0);
  CHECK(f.isr("EOC") == 1);
  CHECK(f.read("DR") == 2 * 0x240u);
  f.run_until(second + 4 * conversion);
  CHECK(f.dues().empty());
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("CONT ignored"));
}

TEST_CASE("RM0456 34.4.26: with the oversampler DMAEN raises adc4_dma with the Nth conversion only")
{
  for(auto const mode : {0u, cont})
  {
    CAPTURE(mode);
    adc4_fixture const f;
    slow_enable(f);
    f.port().drive(pa4, 600'000);
    f.write("CHSELRMOD0", 1u << pa4);
    f.write("CFGR1", mode | dmaen);
    f.write("CFGR2", oversampling(0, 0));
    dma_channel(f, 12);
    auto const begin = f.now();
    f.write("CR", advregen | aden | adstart);
    f.run_until(begin + slow_latency + slow_halves(28) + 5 * us);
    CHECK(buffer_words(f, 2) == std::vector<std::uint32_t>{0, 0});
    f.run_until(begin + slow_latency + 2 * slow_halves(28) + 5 * us);
    CHECK(buffer_words(f, 2) == std::vector<std::uint32_t>{2 * code_of(600'000), 0});
    if(mode == 0)
    {
      CHECK(f.cr("ADSTART") == 0);
      f.run_until(f.now() + 6 * slow_halves(28));
      CHECK(buffer_words(f, 2) == std::vector<std::uint32_t>{2 * code_of(600'000), 0});
      CHECK(f.dues().empty());
    }
  }
}

namespace
{
constexpr std::uint32_t vrefen      = 1u << 22;
constexpr std::uint32_t vsensesel   = 1u << 23;
constexpr std::uint32_t vbaten      = 1u << 24;
constexpr std::uint32_t vrefint_in  = 0;
constexpr std::uint32_t vcore_in    = 12;
constexpr std::uint32_t vsense_in   = 13;
constexpr std::uint32_t vbat_in     = 14;
constexpr std::uint32_t dac_in      = 21;
constexpr std::uint32_t ts_cal1     = 0x0BFA'0710;
constexpr std::uint32_t vrefint_cal = 0x0BFA'07A5;

[[nodiscard]] auto factory(std::uint32_t at, std::uint32_t vdda_mv) -> std::uint32_t
{
  auto const word = stm32::factory_halfword(u575(), at);
  REQUIRE(word.has_value());
  return std::min(*word * 3000u / vdda_mv, 16383u) >> 2;
}

[[nodiscard]] auto convert(adc4_fixture const& f, std::uint32_t channel) -> std::uint32_t
{
  f.write("CHSELRMOD0", 1u << channel);
  auto const begin = f.start();
  f.run_until(begin + halves(28));
  REQUIRE(f.isr("EOC") == 1);
  auto const value = f.cpu_read("DR");
  f.write("ISR", eos | eosmp);
  return value;
}

void drive_dac(adc4_fixture const& f, std::uint32_t channel, std::uint32_t mode, std::uint32_t code)
{
  f.put("RCC", "RCC_AHB3ENR", "DAC1EN", 1);
  f.put("DAC1", "MCR", std::format("MODE{}", channel), mode);
  f.put("DAC1", "CR", std::format("EN{}", channel), 1);
  f.run_until(f.now() + 10 * us);
  f.write("DAC1", std::format("DHR12R{}", channel), code);
  f.run_until(f.now() + 10 * us);
}
}

TEST_CASE(
    "RM0456 Table 329 and 34.4.27: VIN[0] reads VREFINT_CAL with VREFEN and VIN[13] TS_CAL1 with "
    "VSENSESEL, each scaled by 3000 / VDDA and cut from 14 to 12 bits, 0 without its enable, "
    "RES shifting it, inferred")
{
  for(std::uint32_t const vdda : {3000u, 3300u})
  {
    for(auto const& [channel, enable, other, word] : {
            std::tuple{vrefint_in, vrefen,    vsensesel, vrefint_cal},
            std::tuple{vsense_in,  vsensesel, vrefen,    ts_cal1    }
    })
    {
      CAPTURE(vdda);
      CAPTURE(channel);
      auto const         expected = factory(word, vdda);
      adc4_fixture const f{u575(), false, true, 1.0, vdda};
      static_cast<void>(f.enable());
      CHECK(convert(f, channel) == 0);
      f.write("CCR", other);
      CHECK(convert(f, channel) == 0);
      f.write("CCR", enable);
      CHECK(convert(f, channel) == expected);
      f.write("CCR", enable | other);
      CHECK(convert(f, channel) == expected);
      for(std::uint32_t res = 1; res < 4; ++res)
      {
        CAPTURE(res);
        f.write("CFGR1", res << 2);
        CHECK(convert(f, channel) == expected >> (2 * res));
      }
    }
  }
}

TEST_CASE(
    "ADSTART converting VREFINT with VREFEN or VSENSE with VSENSESEL on a part with no factory "
    "word is a config_error, with the enable clear or the channel out of the sequence it is not")
{
  auto description = u575();
  for(auto& region : description.regions)
  {
    if(region.content)
    {
      std::erase_if(*region.content, [&](popcorn::region_content const& piece) {
        return region.start + piece.offset == vrefint_cal or region.start + piece.offset == ts_cal1;
      });
    }
  }
  adc4_fixture const f{description};
  static_cast<void>(f.enable());
  auto const start = [](adc4_fixture const& g) {
    static_cast<void>(g.target->mem().write(g.at("ADC4", "CR"), 4, advregen | aden | adstart));
  };
  f.write("CHSELRMOD0", 1u << vrefint_in);
  CHECK(convert(f, vrefint_in) == 0);
  f.write("CCR", vrefen);
  CHECK(convert(f, vsense_in) == 0);
  f.write("CCR", vsensesel);
  CHECK(convert(f, vrefint_in) == 0);
  for(auto const& [enable, channel] : {
          std::pair{vrefen,    vrefint_in},
          std::pair{vsensesel, vsense_in }
  })
  {
    adc4_fixture const g{description};
    static_cast<void>(g.enable());
    g.write("CCR", enable);
    g.write("CHSELRMOD0", 1u << channel);
    CHECK_THROWS_AS(start(g), popcorn::config_error);
  }
}

TEST_CASE(
    "RM0456 Table 329 and ADC_OR 34.7.17: VIN[21] reads dac1_out1 with CHN21SEL 0 and dac1_out2 "
    "with 1, 0 while that DAC output reaches no on-chip peripheral, at the DAC code scaled to "
    "VDDA and the RES shift")
{
  for(std::uint32_t const select : {0u, 1u})
  {
    CAPTURE(select);
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.write("OR", select);
    drive_dac(f, 1, 1, 0x5A5);
    drive_dac(f, 2, 1, 0xA5A);
    CHECK(convert(f, dac_in) == (select == 0 ? 0x5A5u : 0xA5Au));
    f.write("OR", 1 - select);
    CHECK(convert(f, dac_in) == (select == 0 ? 0xA5Au : 0x5A5u));
    f.write("CFGR1", 1u << 2);
    CHECK(convert(f, dac_in) == (select == 0 ? 0xA5Au : 0x5A5u) >> 2);
    f.put("DAC1", "CR", "EN1", 0);
    f.put("DAC1", "CR", "EN2", 0);
    f.run_until(f.now() + us);
    CHECK(convert(f, dac_in) == 0);
  }
  adc4_fixture const f;
  static_cast<void>(f.enable());
  drive_dac(f, 1, 0, 0x5A5);
  CHECK(convert(f, dac_in) == 0);
}

TEST_CASE("a DAC1 level change before the end of the sampling converts, one after it does not")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  drive_dac(f, 1, 1, 0x400);
  f.write("SMPR", 7);
  f.write("CHSELRMOD0", 1u << dac_in);
  auto const begin = f.start();
  f.run_until(begin + halves(1629) - 3 * us);
  f.write("DAC1", "DHR12R1", 0x800);
  f.run_until(begin + halves(1629) + early);
  f.write("DAC1", "DHR12R1", 0xC00);
  f.run_until(begin + halves(1654));
  CHECK(f.read("DR") == 0x800);
}

TEST_CASE(
    "VIN[12], VCORE, converts 0 and logs once at ADSTART, VIN[14] with VBATEN too, a model_error "
    "when strict that keeps ADC_CR, VIN[14] without VBATEN converts 0 and logs nothing")
{
  for(auto const& [channel, enable, phrase] : {
          std::tuple{vcore_in, 0u,     std::string_view{"VCORE"}              },
          std::tuple{vbat_in,  vbaten, std::string_view{"VBATEN with VIN[14]"}}
  })
  {
    CAPTURE(channel);
    {
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.write("CCR", enable);
      f.write("CHSELRMOD0", 1u << channel);
      std::vector<core::log_record> logged;
      auto const                    sink  = recorder(f, logged);
      auto const                    begin = f.start();
      f.run_until(begin + halves(28));
      CHECK(f.read("DR") == 0);
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains(phrase));
      f.write("CR", advregen | aden | adstart);
      CHECK(logged.size() == 1);
    }
    adc4_fixture const f{u575(), true};
    static_cast<void>(f.enable());
    f.write("CCR", enable);
    f.write("CHSELRMOD0", 1u << channel);
    CHECK_THROWS_AS(
        static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden | adstart)),
        popcorn::model_error);
    CHECK(f.cr("ADSTART") == 0);
  }
  adc4_fixture const f;
  static_cast<void>(f.enable());
  std::vector<core::log_record> logged;
  auto const                    sink = recorder(f, logged);
  CHECK(convert(f, vbat_in) == 0);
  CHECK(logged.empty());
}

TEST_CASE(
    "VBATEN set while ADSTART is set with VIN[14] in the sequence logs once, a model_error when "
    "strict_peripherals")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    adc4_fixture const f{u575(), strict};
    static_cast<void>(f.enable());
    f.write("CFGR1", cont);
    f.write("CHSELRMOD0", 1u << vbat_in);
    static_cast<void>(f.start());
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    if(strict)
    {
      CHECK_THROWS_AS(
          static_cast<void>(f.target->mem().write(f.at("ADC4", "CCR"), 4, vbaten)),
          popcorn::model_error);
      CHECK(f.read("CCR") == 0);
      continue;
    }
    f.write("CCR", vbaten);
    f.write("CCR", 0);
    f.write("CCR", vbaten);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("VBATEN with VIN[14]"));
  }
}

TEST_CASE(
    "DS13737 Tables 36 and 109, inferred: VREFINT or VSENSE connected and sampled for less than "
    "12.65 us or 13 us logs once at ADSTART, a model_error when strict, a long sampling or the "
    "enable clear logs nothing")
{
  for(auto const& [channel, enable] : {
          std::tuple{vrefint_in, vrefen   },
          std::tuple{vsense_in,  vsensesel}
  })
  {
    CAPTURE(channel);
    {
      adc4_fixture const f;
      static_cast<void>(f.enable());
      f.write("CHSELRMOD0", 1u << channel);
      std::vector<core::log_record> logged;
      auto const                    sink = recorder(f, logged);
      f.write("CCR", 0);
      static_cast<void>(f.start());
      CHECK(logged.empty());
      f.run_until(f.now() + 2 * halves(28));
      f.write("CCR", enable);
      f.write("SMPR", 7);
      static_cast<void>(f.start());
      CHECK(logged.empty());
      f.run_until(f.now() + halves(1700));
      f.write("SMPR", 0);
      static_cast<void>(f.start());
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains(channel == vrefint_in ? "Table 36" : "Table 109"));
    }
    adc4_fixture const f{u575(), true};
    static_cast<void>(f.enable());
    f.write("CHSELRMOD0", 1u << channel);
    f.write("CCR", enable);
    CHECK_THROWS_AS(
        static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden | adstart)),
        popcorn::model_error);
    CHECK(f.cr("ADSTART") == 0);
  }
}

TEST_CASE(
    "an RCC reset of ADC4 restores ADC_OR and ADC_CCR, a snapshot keeps CHN21SEL and the enables")
{
  adc4_fixture const f;
  static_cast<void>(f.enable());
  f.write("OR", 1);
  f.write("CCR", vrefen | vsensesel | vbaten);
  auto const saved = f.target->snapshot();
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 1);
  f.put("RCC", "RCC_AHB3RSTR", "ADC4RST", 0);
  CHECK(f.read("OR") == 0);
  CHECK(f.read("CCR") == 0);
  adc4_fixture const fresh{no_reset};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(fresh.read("OR") == 1);
  CHECK(fresh.read("CCR") == (vrefen | vsensesel | vbaten));
}

TEST_CASE(
    "VBATEN, VREFEN or VSENSESEL written with ADSTART 0 and the channel already in the sequence "
    "raises nothing under strict, the raise comes at ADSTART")
{
  adc4_fixture const f{u575(), true};
  static_cast<void>(f.enable());
  f.write("CHSELRMOD0", 1u << vbat_in);
  CHECK_NOTHROW(f.write("CCR", vbaten));
  CHECK(f.read("CCR") == vbaten);
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(f.at("ADC4", "CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
  f.write("CCR", 0);
  f.write("CHSELRMOD0", 1u << vrefint_in);
  CHECK_NOTHROW(f.write("CCR", vrefen));
  f.write("CCR", 0);
  f.write("CHSELRMOD0", 1u << vsense_in);
  CHECK_NOTHROW(f.write("CCR", vsensesel));
}

TEST_CASE(
    "VREFEN or VSENSESEL written with ADSTART set and the channel in the sequence is checked at "
    "the write, a missing factory word is a config_error, a short sampling logs once, a "
    "model_error when strict that keeps the old ADC_CCR")
{
  for(auto const& [channel, enable, word] : {
          std::tuple{vrefint_in, vrefen,    vrefint_cal},
          std::tuple{vsense_in,  vsensesel, ts_cal1    }
  })
  {
    CAPTURE(channel);
    auto description = u575();
    for(auto& region : description.regions)
    {
      if(region.content)
      {
        std::erase_if(*region.content, [&](popcorn::region_content const& piece) {
          return region.start + piece.offset == word;
        });
      }
    }
    {
      adc4_fixture const f{description};
      static_cast<void>(f.enable());
      f.write("CFGR1", cont);
      f.write("CHSELRMOD0", 1u << channel);
      static_cast<void>(f.start());
      CHECK_THROWS_AS(
          static_cast<void>(f.target->mem().write(f.at("ADC4", "CCR"), 4, enable)),
          popcorn::config_error);
    }
    for(auto const strict : {false, true})
    {
      CAPTURE(strict);
      adc4_fixture const f{u575(), strict};
      static_cast<void>(f.enable());
      f.write("CFGR1", cont);
      f.write("CHSELRMOD0", 1u << channel);
      static_cast<void>(f.start());
      std::vector<core::log_record> logged;
      auto const                    sink = recorder(f, logged);
      if(strict)
      {
        CHECK_THROWS_AS(
            static_cast<void>(f.target->mem().write(f.at("ADC4", "CCR"), 4, enable)),
            popcorn::model_error);
        CHECK(f.read("CCR") == 0);
        continue;
      }
      f.write("CCR", enable);
      f.write("CCR", 0);
      f.write("CCR", enable);
      REQUIRE(logged.size() == 1);
      CHECK(logged[0].message.contains(channel == vrefint_in ? "Table 36" : "Table 109"));
    }
    adc4_fixture const f;
    static_cast<void>(f.enable());
    f.write("SMPR", 7);
    f.write("CFGR1", cont);
    f.write("CHSELRMOD0", 1u << channel);
    static_cast<void>(f.start());
    std::vector<core::log_record> logged;
    auto const                    sink = recorder(f, logged);
    f.write("CCR", enable);
    CHECK(logged.empty());
  }
}

TEST_CASE(
    "the factory words of the shipped STM32U575 description convert to fixed codes: VREFINT_CAL "
    "6635 reads 1658 at 3000 mV and 1507 at 3300 mV, TS_CAL1 4107 reads 1026 and 933")
{
  for(auto const& [vdda, vrefint_code, sensor_code] : {
          std::tuple{3000u, 1658u, 1026u},
          std::tuple{3300u, 1507u, 933u }
  })
  {
    CAPTURE(vdda);
    adc4_fixture const f{u575(), false, true, 1.0, vdda};
    static_cast<void>(f.enable());
    f.write("CCR", vrefen | vsensesel);
    CHECK(convert(f, vrefint_in) == vrefint_code);
    CHECK(convert(f, vsense_in) == sensor_code);
  }
}
