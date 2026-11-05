#include "exti_classic.hpp"
#include "stm32_fixture.hpp"
#include "trigger_probe.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/coverage.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
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
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;

constexpr auto          firmware  = std::string_view{"l152xe_l1_exti_ll"};
constexpr std::uint32_t exti      = 0x4001'0400;
constexpr std::uint32_t imr       = exti + 0x00;
constexpr std::uint32_t rtsr      = exti + 0x08;
constexpr std::uint32_t ftsr      = exti + 0x0C;
constexpr std::uint32_t swier     = exti + 0x10;
constexpr std::uint32_t pr        = exti + 0x14;
constexpr std::uint32_t syscfg    = 0x4001'0000;
constexpr std::uint32_t memrmp    = syscfg + 0x00;
constexpr std::uint32_t pmc       = syscfg + 0x04;
constexpr std::uint32_t exticr1   = syscfg + 0x08;
constexpr std::uint32_t exticr4   = syscfg + 0x14;
constexpr std::uint32_t apb2rstr  = 0x4002'3800 + 0x14;
constexpr std::uint32_t syscfgrst = 1u << 0;
constexpr std::uint32_t dbgmcu    = 0xE004'2000;
constexpr std::uint32_t line13    = 1u << 13;
constexpr std::uint32_t exti9_5   = 23;
constexpr std::uint32_t tamper    = 2;
constexpr std::uint32_t comp      = 22;
constexpr std::uint32_t exti0     = 6;

auto parked(
    part_description const& desc    = l152re(),
    std::string_view        name    = firmware,
    core::machine_options   options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{desc, name, std::move(options)};
  f.set("pc", popcorn::testing::sym(name, "idle"));
  return f;
}

auto read(stm32_fixture const& f, std::uint32_t at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void write(stm32_fixture const& f, std::uint32_t at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}

auto cpu(stm32_fixture const& f, std::uint16_t opcode, std::uint32_t at, std::uint32_t value)
    -> std::uint32_t
{
  constexpr std::uint32_t code = 0x2000'0100;
  REQUIRE(f.target->mem().write(code, 2, opcode).has_value());
  auto const set = [&](std::string_view name, std::uint64_t to) {
    f.target->regs().write(*f.target->regs().find(name), to);
  };
  set("r0", at);
  set("r1", value);
  set("pc", code | 1u);
  REQUIRE_FALSE(f.target->run_slice({.max_instructions = icount{1}}).stopped.has_value());
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  return static_cast<std::uint32_t>(f.target->regs().read(*f.target->regs().find("r2")));
}

void cpu_store(stm32_fixture const& f, std::uint32_t at, std::uint32_t value)
{
  static_cast<void>(cpu(f, 0x6001, at, value));
}

auto cpu_load(stm32_fixture const& f, std::uint32_t at) -> std::uint32_t
{
  return cpu(f, 0x6802, at, 0);
}

void clear_pending(stm32_fixture const& f, std::uint32_t irq)
{
  write(f, 0xE000'E280u + 4 * (irq / 32), 1u << (irq % 32));
}

auto port(stm32_fixture const& f, std::string_view name) -> stm32::gpio_port&
{
  auto* const found = f.model_of<stm32::gpio_port>(name);
  REQUIRE(found != nullptr);
  return *found;
}

auto selector(stm32_fixture const& f) -> stm32::exti_selector const&
{
  auto const* const found = f.model_of<stm32::exti_selector>("SYSCFG");
  REQUIRE(found != nullptr);
  return *found;
}

auto irq_pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  return (read(f, 0xE000'E200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
}

void line13_both_edges(stm32_fixture const& f)
{
  write(f, imr, line13);
  write(f, rtsr, line13);
  write(f, ftsr, line13);
}

auto run_breakpoint(part_description const& desc, std::string_view name)
    -> std::vector<std::uint32_t>
{
  stm32_fixture f{desc, name};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  REQUIRE(f.log() == std::vector<std::uint32_t>{0x20});
  port(f, "GPIOA").drive(13, false);
  port(f, "GPIOC").drive(13, false);
  port(f, "GPIOA").drive(0, true);
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  return f.log();
}
}

TEST_CASE("RM0038 10.2: PA0 rises into EXTI0, PC13 falls into EXTI15_10, SWIER enters EXTI0")
{
  auto const expected = std::vector<std::uint32_t>{0x20, 0xe001, 0xe130, 0xb3, 0xe001, 0x5e1, 0, 0};
  CHECK(run_breakpoint(l152re(), firmware) == expected);
  CHECK(run_breakpoint(l152rb(), "l152xb_l1_exti_ll") == expected);
}

TEST_CASE("RM0038 10.3: line 23 exists on Cat.5 and is absent on Cat.1, per the version trait")
{
  for(auto const& [desc, name, lines] : {
          std::tuple{&l152re(), firmware,                              24},
          std::tuple{&l152rb(), std::string_view{"l152xb_l1_exti_ll"}, 23}
  })
  {
    CAPTURE(desc->part);
    auto const f = parked(*desc, name);
    write(f, imr, ~std::uint32_t{});
    CHECK(read(f, imr) == (std::uint32_t{1} << lines) - 1u);
    write(f, swier, 1u << 23);
    CHECK(((read(f, pr) >> 23) & 1u) == (lines == 24 ? 1u : 0u));
    write(f, swier, 1u << 22);
    CHECK(((read(f, pr) >> 22) & 1u) == 1u);
  }
}

TEST_CASE("RM0038 10.3.5: SWIER sets PR of an unmasked line only, reads 1 until PR clears")
{
  auto const f = parked();
  write(f, swier, 1u << 1);
  CHECK(read(f, pr) == 0);
  write(f, swier, 0);
  write(f, imr, 1u << 1);
  write(f, swier, 1u << 1);
  CHECK(read(f, pr) == 1u << 1);
  CHECK(read(f, swier) == 1u << 1);
  write(f, pr, 1u << 1);
  CHECK(read(f, pr) == 0);
  CHECK(read(f, swier) == 0);
}

TEST_CASE("RM0038 10.3.5: a PR write of 1 clears SWIER on a line whose PR is 0, SWIER fires again")
{
  auto const f = parked();
  write(f, swier, 1);
  CHECK(read(f, swier) == 1);
  CHECK(read(f, pr) == 0);
  write(f, pr, 1);
  CHECK(read(f, swier) == 0);
  write(f, imr, 1);
  write(f, swier, 1);
  CHECK(read(f, pr) == 1);
  CHECK(irq_pending(f, exti0));
}

TEST_CASE("RM0038 Tables 49 to 51: every EXTI line 0 to 23 raises its NVIC line only, or none")
{
  auto const& irqs   = l152re().irqs;
  auto const  number = [&](std::string_view name) {
    auto const found = std::ranges::find(irqs, name, &popcorn::irq_desc::name);
    REQUIRE(found != irqs.end());
    return found->number;
  };
  auto const watched = std::to_array<std::string_view>(
      {"EXTI0_IRQn", "EXTI1_IRQn", "EXTI2_IRQn", "EXTI3_IRQn", "EXTI4_IRQn", "EXTI9_5_IRQn",
       "EXTI15_10_IRQn", "TAMPER_STAMP_IRQn", "COMP_IRQn", "PVD_IRQn", "RTC_Alarm_IRQn",
       "USB_FS_WKUP_IRQn", "RTC_WKUP_IRQn", "COMP_ACQ_IRQn"});
  auto const expected = std::to_array<std::string_view>(
      {"EXTI0_IRQn",
       "EXTI1_IRQn",
       "EXTI2_IRQn",
       "EXTI3_IRQn",
       "EXTI4_IRQn",
       "EXTI9_5_IRQn",
       "EXTI9_5_IRQn",
       "EXTI9_5_IRQn",
       "EXTI9_5_IRQn",
       "EXTI9_5_IRQn",
       "EXTI15_10_IRQn",
       "EXTI15_10_IRQn",
       "EXTI15_10_IRQn",
       "EXTI15_10_IRQn",
       "EXTI15_10_IRQn",
       "EXTI15_10_IRQn",
       "",
       "RTC_Alarm_IRQn",
       "USB_FS_WKUP_IRQn",
       "TAMPER_STAMP_IRQn",
       "RTC_WKUP_IRQn",
       "COMP_IRQn",
       "COMP_IRQn",
       ""});
  auto const f = parked();
  write(f, imr, ~std::uint32_t{});
  for(std::uint32_t line = 0; line < expected.size(); ++line)
  {
    CAPTURE(line);
    write(f, swier, 1u << line);
    REQUIRE(read(f, pr) == 1u << line);
    for(auto const name : watched)
    {
      CAPTURE(name);
      CHECK(irq_pending(f, number(name)) == (name == expected[line]));
    }
    write(f, pr, 1u << line);
    for(auto const name : watched)
    {
      clear_pending(f, number(name));
    }
  }
}

TEST_CASE("RM0038 Tables 49 to 51: each EXTI line group raises its NVIC line while a PR bit is set")
{
  auto const f = parked();
  write(f, imr, ~std::uint32_t{});
  for(auto const& [line, irq] : {
          std::pair{5u,  exti9_5},
          std::pair{9u,  exti9_5},
          std::pair{19u, tamper },
          std::pair{22u, comp   },
          std::pair{21u, comp   }
  })
  {
    CAPTURE(line);
    CHECK_FALSE(irq_pending(f, irq));
    write(f, swier, 1u << line);
    CHECK(irq_pending(f, irq));
    write(f, imr, ~(1u << line));
    clear_pending(f, irq);
    CHECK(irq_pending(f, irq));
    write(f, pr, 1u << line);
    clear_pending(f, irq);
    CHECK_FALSE(irq_pending(f, irq));
    write(f, imr, ~std::uint32_t{});
  }
  write(f, swier, 1u << 16);
  CHECK(read(f, pr) == 1u << 16);
}

TEST_CASE("RM0038 8.5.6: EXTICR4 moves line 13 from PA13 to PC13, the other port's pin is ignored")
{
  auto const f = parked();
  write(f, ftsr, line13);
  port(f, "GPIOA").drive(13, false);
  CHECK(read(f, pr) == 0);
  port(f, "GPIOA").drive(13, {});
  line13_both_edges(f);
  port(f, "GPIOC").drive(13, true);
  CHECK(read(f, pr) == 0);
  port(f, "GPIOA").drive(13, false);
  CHECK(read(f, pr) == line13);
  write(f, pr, line13);
  write(f, exticr4, 2u << 4);
  CHECK(read(f, pr) == line13);
  write(f, pr, line13);
  port(f, "GPIOA").drive(13, true);
  CHECK(read(f, pr) == 0);
  port(f, "GPIOC").drive(13, false);
  CHECK(read(f, pr) == line13);
}

TEST_CASE("a selector write that moves a line to a pin of another level is an edge on the line")
{
  auto const f = parked();
  write(f, imr, line13);
  write(f, ftsr, line13);
  port(f, "GPIOA").drive(13, true);
  port(f, "GPIOC").drive(13, false);
  CHECK(read(f, pr) == 0);
  write(f, exticr4, 2u << 4);
  CHECK(read(f, pr) == line13);
  write(f, pr, line13);
  write(f, rtsr, line13);
  write(f, ftsr, 0);
  write(f, exticr4, 0);
  CHECK(read(f, pr) == line13);
}

TEST_CASE("RM0038 8.5.3: PH takes lines 0 to 2, PF and PG exist on Cat.3 to Cat.6 only")
{
  auto const re = parked();
  write(re, exticr1, 0x5555);
  CHECK(selector(re).selected_port(0) == std::optional<std::uint8_t>{'H' - 'A'});
  CHECK(selector(re).selected_port(2) == std::optional<std::uint8_t>{'H' - 'A'});
  CHECK(selector(re).selected_port(3) == std::nullopt);
  write(re, exticr1, 0x7766);
  CHECK(selector(re).selected_port(0) == std::optional<std::uint8_t>{'F' - 'A'});
  CHECK(selector(re).selected_port(3) == std::optional<std::uint8_t>{'G' - 'A'});
  auto const rb = parked(l152rb(), "l152xb_l1_exti_ll");
  write(rb, exticr1, 0x7766);
  CHECK(selector(rb).selected_port(0) == std::nullopt);
  CHECK(selector(rb).selected_port(3) == std::nullopt);
  write(rb, exticr1, 0x0012);
  CHECK(selector(rb).selected_port(0) == std::optional<std::uint8_t>{'C' - 'A'});
  CHECK(selector(rb).selected_port(1) == std::optional<std::uint8_t>{'B' - 'A'});
}

TEST_CASE("an EXTI and SYSCFG snapshot restores the selection, the masks and the pending bits")
{
  auto const f = parked();
  line13_both_edges(f);
  write(f, exticr4, 2u << 4);
  write(f, swier, line13);
  auto const saved = f.target->snapshot();
  for(auto const owner : {"EXTI", "SYSCFG"})
  {
    CAPTURE(owner);
    CHECK_FALSE(
        std::ranges::contains(
            saved.data().models, std::string_view{owner}, &popcorn::core::snapshot_part::owner));
    CHECK(f.target->model(owner)->stateless());
  }
  f.target->reset(reset_kind::power_on);
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  CHECK(read(f, pr) == 0);
  CHECK(read(f, exticr4) == 0);
  f.target->restore(saved);
  CHECK(read(f, exticr4) == 2u << 4);
  CHECK(read(f, imr) == line13);
  CHECK(read(f, pr) == line13);
  CHECK(read(f, swier) == line13);
  CHECK(irq_pending(f, 40));
  write(f, pr, line13);
  port(f, "GPIOC").drive(13, true);
  CHECK(read(f, pr) == line13);
}

TEST_CASE("RM0038 8.5.2: USB_PU reaches the USB pull-up source, SYSCFGRST keeps only MEMRMP")
{
  auto const f = parked();
  popcorn::testing::enable_clocks(
      *f.target, {
                     {"APB2ENR", "SYSCFGEN"}
  });
  auto const* source = f.model_of<stm32::usb_pull_up_source>("SYSCFG");
  REQUIRE(source != nullptr);
  CHECK_FALSE(source->usb_pull_up());
  cpu_store(f, pmc, 1);
  CHECK(source->usb_pull_up());
  write(f, exticr4, 2u << 4);
  f.target->peripheral("SYSCFG").poke(0, 1);
  write(f, apb2rstr, syscfgrst);
  write(f, apb2rstr, 0);
  CHECK_FALSE(source->usb_pull_up());
  CHECK(read(f, exticr4) == 0);
  CHECK(f.target->peripheral("SYSCFG").peek(0) == 1);
}

TEST_CASE("RM0038 8.5.1: MEMRMP is storage, one warning per machine, model_error under strict")
{
  {
    auto const f = parked();
    popcorn::testing::enable_clocks(
        *f.target, {
                       {"APB2ENR", "SYSCFGEN"}
    });
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    cpu_store(f, memrmp, 0x303);
    CHECK(cpu_load(f, memrmp) == 0x003);
    cpu_store(f, memrmp, 0);
    write(f, exticr4, 0);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].kind == core::log_kind::model_warning);
    CHECK(logged[0].reg == "MEMRMP");
  }
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const f               = parked(l152re(), firmware, std::move(options));
  CHECK_THROWS_AS(
      (void)f.target->model("SYSCFG")->on_write(
          f.target->peripheral("SYSCFG"), 0, 4, 0, 3, {.pc = 0, .now = icount{0}}),
      popcorn::model_error);
  CHECK(f.target->peripheral("SYSCFG").peek(0) == 0);
}

TEST_CASE(
    "RM0038 8.5.2: USB_PU is modelled, LCD_CAPA is storage: "
    "one warning, model_error under strict")
{
  constexpr std::uint32_t lcd_capa = 1u << 1;
  {
    auto const f = parked();
    popcorn::testing::enable_clocks(
        *f.target, {
                       {"APB2ENR", "SYSCFGEN"}
    });
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    f.target->enable_coverage({}, true);
    auto const* source = f.model_of<stm32::usb_pull_up_source>("SYSCFG");
    REQUIRE(source != nullptr);
    cpu_store(f, pmc, 1);
    CHECK(source->usb_pull_up());
    cpu_store(f, pmc, 0);
    CHECK_FALSE(source->usb_pull_up());
    CHECK(logged.empty());
    CHECK_FALSE(f.target->model("SYSCFG")->storage_only(pmc - syscfg));
    cpu_store(f, pmc, lcd_capa | 1);
    cpu_store(f, pmc, lcd_capa);
    CHECK(cpu_load(f, pmc) == lcd_capa);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].kind == core::log_kind::model_warning);
    CHECK(logged[0].reg == "PMC");
    CHECK(f.target->coverage().unmodelled.empty());
  }
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const f               = parked(l152re(), firmware, std::move(options));
  REQUIRE(
      f.target->model("SYSCFG")
          ->on_write(
              f.target->peripheral("SYSCFG"), pmc - syscfg, 4, 0, 1, {.pc = 0, .now = icount{0}})
          .has_value());
  CHECK(f.target->peripheral("SYSCFG").peek(pmc - syscfg) == 1);
  CHECK_THROWS_AS(
      (void)f.target->model("SYSCFG")->on_write(
          f.target->peripheral("SYSCFG"), pmc - syscfg, 4, 1, lcd_capa | 1,
          {.pc = 0, .now = icount{0}}),
      popcorn::model_error);
  CHECK(f.target->peripheral("SYSCFG").peek(pmc - syscfg) == 1);
}

TEST_CASE("RM0038 30.16: DBGMCU IDCODE reads 0x10000437, CR and the freeze registers keep to a POR")
{
  auto const f = parked();
  REQUIRE(f.target->model("DBGMCU") != nullptr);
  CHECK(read(f, dbgmcu) == 0x1000'0437u);
  write(f, dbgmcu + 0x04, 0x7);
  write(f, dbgmcu + 0x08, 0x1000);
  write(f, dbgmcu + 0x0C, 0x4);
  f.target->reset(reset_kind::software);
  CHECK(read(f, dbgmcu + 0x04) == 0x7);
  CHECK(read(f, dbgmcu + 0x08) == 0x1000);
  CHECK(read(f, dbgmcu + 0x0C) == 0x4);
  f.target->reset(reset_kind::power_on);
  CHECK(read(f, dbgmcu + 0x04) == 0);
  CHECK(read(f, dbgmcu + 0x08) == 0);
  CHECK(read(f, dbgmcu + 0x0C) == 0);
}

TEST_CASE("RM0038 30.6 to 30.17: the L1 TPIU, ETM and ROM table are RAZ/WI, DBGMCU keeps its model")
{
  auto const blocks = stm32::raz_wi_map(l152re());
  REQUIRE(blocks.size() == 3);
  CHECK(blocks[0].start == 0xE004'0000u);
  CHECK(blocks[1].start == 0xE004'1000u);
  CHECK(blocks[2].start == 0xE00F'F000u);
  auto options   = popcorn::testing::stm32_options();
  options.raz_wi = blocks;
  auto const f   = parked(l152re(), firmware, std::move(options));
  cpu_store(f, 0xE004'0004u, 1);
  CHECK(cpu_load(f, 0xE004'0004u) == 0);
  CHECK(cpu_load(f, 0xE00F'FFD0u) == 0);
  CHECK(read(f, dbgmcu) == 0x1000'0437u);
}

TEST_CASE("RM0038 10.3.2: the EXTI model leaves EXTI_EMR unclaimed, a firmware store lands")
{
  auto const f      = parked();
  auto const claims = f.target->model("EXTI")->claims();
  CHECK_FALSE(std::ranges::contains(claims, std::string_view{"EMR"}));
  CHECK(std::ranges::contains(claims, std::string_view{"PR"}));
  cpu_store(f, exti + 0x04, 2);
  CHECK(cpu_load(f, exti + 0x04) == 2);
  CHECK(read(f, pr) == 0);
}

TEST_CASE(
    "an unshipped Cat.4 part built at configure attaches, its EXTI lines on NVIC 6 and 22"
    * doctest::skip(not popcorn::testing::has_l152rd()))
{
  auto const desc = popcorn::testing::description_from("POPCORN_L152RD_DESC");
  REQUIRE(desc.part == "STM32L152RDTx");
  auto const comp_irq = std::ranges::find(desc.irqs, 22, &popcorn::irq_desc::number);
  REQUIRE(comp_irq != desc.irqs.end());
  CHECK(comp_irq->name == "COMP_IRQn");
  auto const f = parked(desc, "l152xb_l1_exti_ll");
  write(f, imr, ~std::uint32_t{});
  write(f, swier, 1u);
  CHECK(irq_pending(f, 6));
  write(f, swier, 1u << 21);
  CHECK(irq_pending(f, comp));
}

TEST_CASE("the EXTI groups find their NVIC line by number, whatever the description names it")
{
  auto part  = l152re();
  auto named = std::ranges::find(part.irqs, 22, &popcorn::irq_desc::number);
  REQUIRE(named != part.irqs.end());
  named->name  = "COMP_CA_IRQn";
  auto const f = parked(part);
  write(f, imr, ~std::uint32_t{});
  write(f, swier, 1u << 22);
  CHECK(irq_pending(f, comp));
  auto missing = l152re();
  std::erase_if(missing.irqs, [](auto const& irq) { return irq.number == 22; });
  CHECK_THROWS_AS((stm32_fixture{std::move(missing), firmware}), popcorn::config_error);
}

namespace
{
auto with_trigger_probe() -> core::machine_options
{
  auto options = popcorn::testing::stm32_options();
  options.models.insert_or_assign(
      "DAC", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
        return std::make_shared<popcorn::testing::trigger_probe>(ctx);
      });
  return options;
}
}

TEST_CASE(
    "EXTI line 11 drives exti11 inferred: each PC11 edge at its own time, whatever IMR, RTSR and "
    "FTSR, and an EXTICR3 write that moves the line to a pin of another level")
{
  auto const  f     = parked(l152re(), firmware, with_trigger_probe());
  auto* const probe = f.model_of<popcorn::testing::trigger_probe>("DAC");
  REQUIRE(probe != nullptr);
  probe->watched = "exti11";
  probe->listen(true);
  constexpr std::uint32_t exticr3 = syscfg + 0x10;
  write(f, exticr3, 2u << 12);
  auto const rise = f.target->virtual_clock().time().ps + 5'000'123;
  auto const fall = rise + 7'000'456;
  port(f, "GPIOC").drive_at(11, true, popcorn::vtime{rise});
  port(f, "GPIOC").drive_at(11, false, popcorn::vtime{fall});
  f.run_to_stop(
      {.max_instructions = icount{1'000'000}, .until_time = popcorn::vtime{fall + 1'000'000}});
  CHECK(probe->pulses == std::vector<std::uint64_t>{rise});
  CHECK(probe->falls == std::vector<std::uint64_t>{fall});
  CHECK(read(f, pr) == 0);
  auto* const source = stm32::trigger_source_of(probe->context.peers, "exti11");
  REQUIRE(source != nullptr);
  CHECK(source->level("exti11") == std::optional<bool>{false});
  port(f, "GPIOA").drive(11, true);
  CHECK(source->level("exti11") == std::optional<bool>{false});
  auto const moved = f.target->virtual_clock().time().ps;
  write(f, exticr3, 0);
  CHECK(source->level("exti11") == std::optional<bool>{true});
  CHECK(probe->pulses == std::vector<std::uint64_t>{rise, moved});
}

TEST_CASE("trigger_source_of finds EXTI for exti0 to exti15, no source for exti16 or usbsof")
{
  auto const  f     = parked(l152re(), firmware, with_trigger_probe());
  auto* const probe = f.model_of<popcorn::testing::trigger_probe>("DAC");
  REQUIRE(probe != nullptr);
  auto const& peers = probe->context.peers;
  for(auto const* const signal : {"exti0", "exti11", "exti15"})
  {
    CAPTURE(signal);
    CHECK(
        stm32::trigger_source_of(peers, signal)
        == dynamic_cast<stm32::trigger_source*>(f.target->model("EXTI")));
    CHECK(stm32::trigger_source_of(peers, signal) != nullptr);
  }
  for(auto const* const signal : {"exti16", "exti", "usbsof", "11"})
  {
    CAPTURE(signal);
    CHECK(stm32::trigger_source_of(peers, signal) == nullptr);
  }
}

TEST_CASE("a Cat.5 description whose RTSR field covers 23 lines, one less than the bank, throws")
{
  auto part       = l152re();
  auto peripheral = std::ranges::find(
      part.peripherals, std::string_view{"EXTI"}, &popcorn::peripheral_desc::name);
  REQUIRE(peripheral != part.peripherals.end());
  auto rtsr_desc = std::ranges::find(
      peripheral->registers, std::string_view{"RTSR"}, &popcorn::register_desc::name);
  REQUIRE(rtsr_desc != peripheral->registers.end());
  REQUIRE(rtsr_desc->fields.size() == 1);
  rtsr_desc->fields.front().width = 23;
  CHECK_THROWS_WITH_AS(
      (stm32_fixture{std::move(part), firmware}), doctest::Contains("RTSR"), popcorn::config_error);
}
