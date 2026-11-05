#include "debug_freeze.hpp"
#include "exti_classic.hpp"
#include "exti_rows.hpp"
#include "read_protection.hpp"
#include "stm32_fixture.hpp"

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
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;

constexpr auto          firmware  = std::string_view{"l476_l4_exti_ll"};
constexpr std::uint32_t exti      = 0x4001'0400;
constexpr std::uint32_t imr1      = exti + 0x00;
constexpr std::uint32_t emr1      = exti + 0x04;
constexpr std::uint32_t rtsr1     = exti + 0x08;
constexpr std::uint32_t ftsr1     = exti + 0x0C;
constexpr std::uint32_t swier1    = exti + 0x10;
constexpr std::uint32_t pr1       = exti + 0x14;
constexpr std::uint32_t imr2      = exti + 0x20;
constexpr std::uint32_t rtsr2     = exti + 0x28;
constexpr std::uint32_t ftsr2     = exti + 0x2C;
constexpr std::uint32_t swier2    = exti + 0x30;
constexpr std::uint32_t pr2       = exti + 0x34;
constexpr std::uint32_t syscfg    = 0x4001'0000;
constexpr std::uint32_t memrmp    = syscfg + 0x00;
constexpr std::uint32_t exticr1   = syscfg + 0x08;
constexpr std::uint32_t cfgr1     = syscfg + 0x04;
constexpr std::uint32_t exticr4   = syscfg + 0x14;
constexpr std::uint32_t scsr      = syscfg + 0x18;
constexpr std::uint32_t cfgr2     = syscfg + 0x1C;
constexpr std::uint32_t swpr      = syscfg + 0x20;
constexpr std::uint32_t skr       = syscfg + 0x24;
constexpr std::uint32_t rcc_cfgr  = 0x4002'1000 + 0x08;
constexpr std::uint32_t dbgmcu    = 0xE004'2000;
constexpr std::uint32_t apb2rstr  = 0x4002'1000 + 0x40;
constexpr std::uint32_t syscfgrst = 1u << 0;
constexpr std::uint32_t exti0     = 6;
constexpr std::uint32_t pvd_pvm   = 1;

auto parked(
    part_description const& desc    = l476rc(),
    core::machine_options   options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{desc, firmware, std::move(options)};
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
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

auto cpu_store(stm32_fixture const& f, std::uint32_t at, std::uint32_t value) -> popcorn::stop
{
  constexpr std::uint32_t code = 0x2000'0100;
  REQUIRE(f.target->mem().write(code, 2, 0x6001).has_value());
  REQUIRE(f.target->mem().write(code + 2, 2, 0xbe00).has_value());
  f.set("r0", at);
  f.set("r1", value);
  f.set("pc", code | 1u);
  return f.run_to_stop({.max_instructions = icount{4}});
}

auto irq_pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  return (read(f, 0xE000'E200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
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

constexpr auto watched = std::to_array<std::uint32_t>({1, 2, 3, 6, 7, 8, 9, 10, 23, 40, 41, 64});

auto pending_irqs(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const irq : watched)
  {
    if(irq_pending(f, irq))
    {
      out.push_back(irq);
    }
  }
  return out;
}

void clear_watched(stm32_fixture const& f)
{
  for(auto const irq : watched)
  {
    clear_pending(f, irq);
  }
}

auto with_register_fields(std::string_view reg, auto const& edit) -> part_description
{
  auto part       = l476rc();
  auto peripheral = std::ranges::find(
      part.peripherals, std::string_view{"EXTI"}, &popcorn::peripheral_desc::name);
  REQUIRE(peripheral != part.peripherals.end());
  auto found = std::ranges::find(peripheral->registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != peripheral->registers.end());
  edit(*found);
  return part;
}
}

TEST_CASE("RM0351 14.3: PA0 rises into EXTI0 on NVIC 6, SWIER2 line 35 enters PVD_PVM on NVIC 1")
{
  stm32_fixture f{l476rc(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  REQUIRE(f.log() == std::vector<std::uint32_t>{0, 0xff82'0001, 0x8f});
  port(f, "GPIOA").drive(0, true);
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0, 0xff82'0001, 0x8f, 0xe001, 0xb3, 0x3508, 0, 0});
}

TEST_CASE("RM0351 9.2.3: PH0 takes line 0 at code 7, code 7 on line 2 selects no port")
{
  auto const f = parked();
  write(f, exticr1, 0xffff);
  CHECK(read(f, exticr1) == 0x7777u);
  auto const* const selector = f.model_of<stm32::exti_selector>("SYSCFG");
  REQUIRE(selector != nullptr);
  CHECK(selector->selected_port(0) == std::optional<std::uint8_t>{'H' - 'A'});
  CHECK(selector->selected_port(1) == std::optional<std::uint8_t>{'H' - 'A'});
  CHECK(selector->selected_port(2) == std::nullopt);
  CHECK(selector->selected_port(3) == std::nullopt);
  write(f, exticr1, 0x0607);
  CHECK(selector->selected_port(2) == std::optional<std::uint8_t>{'G' - 'A'});
  write(f, 0x4800'0000, 0xabff'fffc);
  write(f, 0x4800'1c00, 0);
  write(f, imr1, 0xff82'0005);
  write(f, rtsr1, 0x5);
  port(f, "GPIOA").drive(0, true);
  CHECK(read(f, pr1) == 0);
  port(f, "GPIOH").drive(0, true);
  CHECK(read(f, pr1) == 1u);
  CHECK(irq_pending(f, exti0));
}

TEST_CASE("RM0351 14.5: IMR1 and IMR2 reset to the direct lines, the edge registers lack them")
{
  auto const f = parked();
  f.target->reset(popcorn::reset_kind::power_on);
  CHECK(read(f, imr1) == 0xff82'0000u);
  CHECK(read(f, imr2) == 0x0000'0087u);
  for(auto const at : {rtsr1, ftsr1})
  {
    write(f, at, ~std::uint32_t{});
    CHECK(read(f, at) == 0x007d'ffffu);
  }
  for(auto const at : {rtsr2, ftsr2})
  {
    write(f, at, 0xff);
    CHECK(read(f, at) == 0x78u);
  }
  write(f, imr2, ~std::uint32_t{});
  CHECK(read(f, imr2) == 0xffu);
  write(f, imr1, 0);
  CHECK(read(f, imr1) == 0);
  write(f, imr2, 0);
  write(f, swier1, ~std::uint32_t{});
  CHECK(read(f, pr1) == 0);
  write(f, swier2, ~std::uint32_t{});
  CHECK(read(f, pr2) == 0);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(read(f, imr1) == 0xff82'0000u);
  CHECK(read(f, imr2) == 0x0000'0087u);
  CHECK(read(f, rtsr2) == 0);
  CHECK(read(f, swier1) == 0);
}

TEST_CASE("RM0351 Table 61: every EXTI line 0 to 39 raises its NVIC line only, a direct line none")
{
  auto const expected = std::to_array<std::optional<std::uint32_t>>({
      6, 7,  8,  9,  10, 23, 23, 23, 23, 23, 40, 40, 40, 40, 40, 40, 1, {}, 41, 2,
      3, 64, 64, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, 1,  1, 1,  1,  {},
  });
  auto const f        = parked();
  write(f, imr1, ~std::uint32_t{});
  write(f, imr2, 0xff);
  for(std::uint32_t line = 0; line < expected.size(); ++line)
  {
    CAPTURE(line);
    auto const swier = line < 32 ? swier1 : swier2;
    auto const pr    = line < 32 ? pr1 : pr2;
    auto const bit   = 1u << (line % 32);
    write(f, swier, bit);
    CHECK(read(f, pr) == (expected[line] ? bit : 0u));
    CHECK(read(f, swier) == (expected[line] ? bit : 0u));
    CHECK(
        pending_irqs(f)
        == (expected[line] ? std::vector<std::uint32_t>{*expected[line]}
                           : std::vector<std::uint32_t>{}));
    write(f, pr, bit);
    CHECK(read(f, pr) == 0);
    CHECK(read(f, swier) == 0);
    clear_watched(f);
  }
}

TEST_CASE("RM0351 Table 61: PVD_PVM stays raised while line 16 or a line 35 to 38 is pending")
{
  auto const f = parked();
  write(f, imr1, ~std::uint32_t{});
  write(f, imr2, 0xff);
  write(f, swier2, 1u << 3);
  CHECK(read(f, pr2) == 1u << 3);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, swier1, 1u << 16);
  write(f, pr2, 1u << 3);
  clear_pending(f, pvd_pvm);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, swier2, 1u << 6);
  write(f, pr1, 1u << 16);
  clear_pending(f, pvd_pvm);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, pr2, 1u << 6);
  clear_pending(f, pvd_pvm);
  CHECK_FALSE(irq_pending(f, pvd_pvm));
  CHECK(pending_irqs(f).empty());
}

TEST_CASE(
    "RM0351 14.3: a direct line latches nothing and raises no NVIC line, its IMR bit is storage")
{
  auto const f     = parked();
  auto*      input = f.model_of<stm32::exti_line_input>("EXTI");
  REQUIRE(input != nullptr);
  write(f, imr1, 0xff82'0000u | (1u << 18));
  write(f, rtsr1, 1u << 18);
  for(auto const line : {std::uint8_t{17}, std::uint8_t{23}, std::uint8_t{31}})
  {
    CAPTURE(line);
    input->line_changed(line, true);
    input->line_changed(line, false);
  }
  input->line_changed(39, true);
  input->line_changed(40, true);
  CHECK(read(f, pr1) == 0);
  CHECK(read(f, pr2) == 0);
  CHECK(pending_irqs(f).empty());
  input->line_changed(18, true);
  CHECK(read(f, pr1) == 1u << 18);
  CHECK(pending_irqs(f) == std::vector<std::uint32_t>{41});
  write(f, rtsr2, 1u << 4);
  write(f, imr2, 1u << 4);
  input->line_changed(36, true);
  CHECK(read(f, pr2) == 1u << 4);
  CHECK(irq_pending(f, pvd_pvm));
  write(f, imr1, 1u << 23);
  CHECK(read(f, imr1) == 1u << 23);
  write(f, emr1, 1u << 23);
  CHECK(read(f, emr1) == 1u << 23);
}

TEST_CASE("a description whose edge register has a direct line's bit, or lacks a line, throws")
{
  auto const tr17 = with_register_fields("RTSR1", [](popcorn::register_desc& reg) {
    auto added = reg.fields.front();
    added.name = "TR17";
    added.lsb  = 17;
    reg.fields.push_back(added);
  });
  try
  {
    stm32_fixture f{tr17, firmware};
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& error)
  {
    CHECK(std::string_view{error.what()}.contains("RTSR1"));
  }
  auto const no_pr38 = with_register_fields("PR2", [](popcorn::register_desc& reg) {
    std::erase_if(reg.fields, [](auto const& field) { return field.name == "PIF38"; });
  });
  CHECK_THROWS_WITH_AS(
      (stm32_fixture{no_pr38, firmware}), doctest::Contains("PR2"), popcorn::config_error);
  auto const imr2_short = with_register_fields("IMR2", [](popcorn::register_desc& reg) {
    std::erase_if(reg.fields, [](auto const& field) { return field.name == "MR39"; });
  });
  CHECK_THROWS_WITH_AS(
      (stm32_fixture{imr2_short, firmware}), doctest::Contains("IMR2"), popcorn::config_error);
}

TEST_CASE(
    "RM0351 9.2: the L4 SYSCFG MEMRMP, CFGR1, CFGR2 and SWPR are storage, one warning each, "
    "FB_MODE raises under strict, SYSCFGRST keeps MEMRMP, SCSR, CFGR2 and SWPR inferred")
{
  {
    auto const                    f = parked();
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(cpu_store(f, cfgr1, 0x7c00'0101u).reason == stop_reason::user);
    REQUIRE(cpu_store(f, cfgr1, 0x7c00'0001u).reason == stop_reason::user);
    REQUIRE(cpu_store(f, memrmp, 0x1).reason == stop_reason::user);
    REQUIRE(cpu_store(f, cfgr2, 0x1).reason == stop_reason::user);
    REQUIRE(cpu_store(f, swpr, 0x8000'0001u).reason == stop_reason::user);
    REQUIRE(cpu_store(f, exticr1, 0x2).reason == stop_reason::user);
    std::vector<std::string_view> regs;
    for(auto const& record : logged)
    {
      CHECK(record.kind == core::log_kind::model_warning);
      regs.push_back(record.reg);
    }
    CHECK(regs == std::vector<std::string_view>{"CFGR1", "MEMRMP", "CFGR2", "SWPR"});
    auto& unit = f.target->peripheral("SYSCFG");
    unit.poke(0x18, 0x3);
    write(f, apb2rstr, syscfgrst);
    write(f, apb2rstr, 0);
    CHECK(read(f, exticr1) == 0);
    CHECK(unit.peek(0x00) == 0x1u);
    CHECK(unit.peek(0x04) == 0x7c00'0001u);
    CHECK(unit.peek(0x18) == 0x3u);
    CHECK(unit.peek(0x1c) == 0x1u);
    CHECK(unit.peek(0x20) == 0x8000'0001u);
  }
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const f               = parked(l476rc(), std::move(options));
  CHECK_THROWS_AS((void)cpu_store(f, memrmp, 0x100), popcorn::model_error);
  CHECK(f.target->peripheral("SYSCFG").peek(0) == 0);
}

namespace
{
constexpr address sram2      = 0x1000'0000;
constexpr address sram2_last = sram2 + 0x7ffc;
constexpr address sram1_mark = 0x2001'0000;

void unlock(stm32_fixture const& f, std::initializer_list<std::uint32_t> keys = {0xca, 0x53})
{
  for(auto const key : keys)
  {
    REQUIRE(cpu_store(f, skr, key).reason == stop_reason::user);
  }
}

void mark(stm32_fixture const& f)
{
  for(auto const at : {sram2, sram2_last})
  {
    write(f, at, 0x5a5a'0000u | (at & 0xffffu));
  }
  write(f, sram1_mark, 0xa5a5'0003u);
}

auto now(stm32_fixture const& f) -> std::uint64_t
{
  return f.target->virtual_clock().time().ps;
}

auto syscfg_events(stm32_fixture const& f) -> std::vector<std::uint64_t>
{
  auto const& units = f.target->description().peripherals;
  auto const  owner = static_cast<std::uint32_t>(
      std::ranges::find(units, std::string_view{"SYSCFG"}, &popcorn::peripheral_desc::name)
      - units.begin());
  std::vector<std::uint64_t> out;
  for(auto const& event : f.target->snapshot().data().events.pending)
  {
    if(event.id.owner == owner)
    {
      out.push_back(event.due_ps);
    }
  }
  return out;
}

void run_until(stm32_fixture const& f, std::uint64_t ps)
{
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  f.run_to_stop({.max_instructions = icount{100'000}, .until_time = popcorn::vtime{ps}});
}

auto erased(stm32_fixture const& f) -> bool
{
  return read(f, sram2) == 0 and read(f, sram2_last) == 0;
}
}

TEST_CASE(
    "RM0351 9.2.7: HAL_SYSCFG_SRAM2Erase sets SRAM2BSY at the next instruction, clears it after "
    "the erase, SRAM2 reads 0 and SRAM1 is kept, inferred")
{
  stm32_fixture f{l476rc(), "l476_l4_syscfg_hal"};
  REQUIRE(f.run_to_stop({.max_instructions = icount{200'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0x3, 0x5a5a'0001u, 1, 0, 0, 0, 0xa5a5'0003u});
}

TEST_CASE(
    "RM0351 9.2.7: the erase ends 8192 HCLK periods after the SRAM2ER write at the write's HCLK, "
    "inferred, a later HCLK change keeps the due time")
{
  for(auto const& [hpre, hz] : {
          std::pair{0x0u, 4'000'000u},
          std::pair{0x8u, 2'000'000u}
  })
  {
    CAPTURE(hpre);
    auto const f = parked();
    write(f, rcc_cfgr, hpre << 4);
    mark(f);
    unlock(f);
    CHECK(syscfg_events(f).empty());
    REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
    auto const at  = now(f);
    auto const due = at + (8192ull * 1'000'000'000'000ull + hz - 1) / hz;
    CHECK(read(f, scsr) == 0x3u);
    CHECK(syscfg_events(f) == std::vector<std::uint64_t>{due});
    write(f, rcc_cfgr, 0);
    CHECK(syscfg_events(f) == std::vector<std::uint64_t>{due});
    run_until(f, due - 1'000'000'000'000ull / hz);
    CHECK(read(f, scsr) == 0x3u);
    CHECK_FALSE(erased(f));
    run_until(f, due + 1);
    CHECK(read(f, scsr) == 0);
    CHECK(erased(f));
    CHECK(read(f, sram1_mark) == 0xa5a5'0003u);
  }
}

TEST_CASE(
    "RM0351 9.2.10: SRAM2ER sets only after 0xCA then 0x53 in SKR, a wrong key locks it again, "
    "one key sequence starts one erase")
{
  for(auto const keys : {
          std::initializer_list<std::uint32_t>{},
          std::initializer_list<std::uint32_t>{0xca},
          std::initializer_list<std::uint32_t>{0x53},
          std::initializer_list<std::uint32_t>{0xca, 0x00, 0x53},
          std::initializer_list<std::uint32_t>{0xca, 0x53, 0x53}
  })
  {
    CAPTURE(keys.size());
    auto const f = parked();
    mark(f);
    unlock(f, keys);
    CHECK(read(f, skr) == 0);
    REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
    CHECK(read(f, scsr) == 0);
    CHECK(syscfg_events(f).empty());
    run_until(f, now(f) + 10'000'000'000ull);
    CHECK_FALSE(erased(f));
  }
  auto const f = parked();
  mark(f);
  unlock(f);
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  REQUIRE(cpu_store(f, scsr, 0).reason == stop_reason::user);
  CHECK(read(f, scsr) == 0x3u);
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  CHECK(syscfg_events(f).size() == 1);
  run_until(f, now(f) + 10'000'000'000ull);
  CHECK(erased(f));
  mark(f);
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  CHECK(read(f, scsr) == 0);
  CHECK(syscfg_events(f).empty());
}

TEST_CASE(
    "a CPU write to SRAM2 during the erase lands and is zeroed at its end, a SYSCFGRST pulse "
    "keeps the running erase and locks the key, inferred")
{
  auto const f = parked();
  mark(f);
  unlock(f);
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  REQUIRE(cpu_store(f, sram2 + 0x100, 0x1234'5678u).reason == stop_reason::user);
  CHECK(read(f, sram2 + 0x100) == 0x1234'5678u);
  auto const due = syscfg_events(f);
  REQUIRE(due.size() == 1);
  write(f, exticr1, 0x2);
  unlock(f, {0xca});
  write(f, apb2rstr, syscfgrst);
  write(f, apb2rstr, 0);
  CHECK(read(f, scsr) == 0x3u);
  CHECK(read(f, exticr1) == 0);
  CHECK(syscfg_events(f) == due);
  run_until(f, due.front() + 1);
  CHECK(read(f, scsr) == 0);
  CHECK(erased(f));
  CHECK(read(f, sram2 + 0x100) == 0);
  mark(f);
  unlock(f, {0x53});
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  CHECK(read(f, scsr) == 0);
  unlock(f);
  write(f, apb2rstr, syscfgrst);
  write(f, apb2rstr, 0);
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  CHECK(read(f, scsr) == 0);
  unlock(f);
  f.target->reset(popcorn::reset_kind::software);
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  CHECK(read(f, scsr) == 0);
}

TEST_CASE("an L4 SYSCFG snapshot saves the key stage and a running erase, the restore completes it")
{
  auto const f = parked();
  CHECK_FALSE(f.target->model("SYSCFG")->stateless());
  mark(f);
  unlock(f);
  auto const keyed = f.target->snapshot();
  CHECK(
      std::ranges::contains(
          keyed.data().models, std::string_view{"SYSCFG"}, &popcorn::core::snapshot_part::owner));
  f.target->reset(popcorn::reset_kind::power_on);
  f.target->restore(keyed);
  REQUIRE(cpu_store(f, scsr, 1).reason == stop_reason::user);
  CHECK(read(f, scsr) == 0x3u);
  auto const running = f.target->snapshot();
  auto const due     = syscfg_events(f);
  REQUIRE(due.size() == 1);
  run_until(f, due.front() + 1);
  REQUIRE(erased(f));
  f.target->restore(running);
  CHECK(read(f, scsr) == 0x3u);
  CHECK_FALSE(erased(f));
  CHECK(syscfg_events(f) == due);
  run_until(f, due.front() + 1);
  CHECK(read(f, scsr) == 0);
  CHECK(erased(f));
}

TEST_CASE("RM0351 3.5.1: a read protection regression writes 0 to SRAM2 at once, inferred")
{
  auto const f = parked();
  mark(f);
  auto* const listener = f.model_of<stm32::read_protection_listener>("SYSCFG");
  REQUIRE(listener != nullptr);
  listener->on_read_protection_regression();
  CHECK(erased(f));
  CHECK(read(f, scsr) == 0);
  CHECK(read(f, sram1_mark) == 0xa5a5'0003u);
}

TEST_CASE("RM0351 9.2.6: EXTICR4 moves line 13 from PA13 to PC13 on the L4")
{
  auto const f = parked();
  write(f, 0x4800'0800, 0);
  write(f, imr1, 0xff82'0000u | (1u << 13));
  write(f, ftsr1, 1u << 13);
  port(f, "GPIOC").drive(13, true);
  port(f, "GPIOC").drive(13, false);
  CHECK(read(f, pr1) == 0);
  write(f, exticr4, 2u << 4);
  write(f, pr1, 1u << 13);
  port(f, "GPIOC").drive(13, true);
  port(f, "GPIOC").drive(13, false);
  CHECK(read(f, pr1) == 1u << 13);
}

TEST_CASE(
    "RM0351 48.16: the L4 DBGMCU IDCODE reads 0x10076415, DBG_TIM2_STOP freezes TIM2, the "
    "freeze registers keep to a power-on reset")
{
  auto const f = parked();
  CHECK(read(f, dbgmcu) == 0x1007'6415u);
  auto const* const freeze = f.model_of<stm32::debug_freeze>("DBGMCU");
  REQUIRE(freeze != nullptr);
  CHECK(freeze->frozen("DBG_TIM2_STOP") == std::optional<bool>{false});
  write(f, dbgmcu + 0x08, 1u);
  CHECK(freeze->frozen("DBG_TIM2_STOP") == std::optional<bool>{true});
  CHECK(freeze->frozen("DBG_TIM3_STOP") == std::optional<bool>{false});
  write(f, dbgmcu + 0x10, 1u << 11);
  CHECK(freeze->frozen("DBG_TIM1_STOP") == std::optional<bool>{true});
  f.target->reset(popcorn::reset_kind::software);
  CHECK(freeze->frozen("DBG_TIM2_STOP") == std::optional<bool>{true});
  f.target->reset(popcorn::reset_kind::power_on);
  CHECK(freeze->frozen("DBG_TIM2_STOP") == std::optional<bool>{false});
}

TEST_CASE("the L4 EXTI row serves the L476, the L1 variant does not attach to its description")
{
  auto const f = parked();
  CHECK(f.model_of<stm32::exti_line_input>("EXTI") != nullptr);
  stm32::register_builtin_models();
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(stm32::board{}, l476rc());
  options.models.insert_or_assign("EXTI", stm32::exti_classic_factory(stm32::l1_exti_24));
  CHECK_THROWS_AS(
      (core::machine{l476rc(), popcorn::arch::armv8m::factory(), std::move(options)}),
      popcorn::config_error);
}
