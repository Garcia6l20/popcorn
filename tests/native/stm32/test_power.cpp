#include "stm32_fixture.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::fault_kind;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint32_t imprecise_bus_error = 1u << 10;
constexpr std::uint32_t handler_mark        = 0xfa17;

auto run_log(
    std::string_view      firmware,
    core::machine_options options = popcorn::testing::stm32_options()) -> std::vector<std::uint32_t>
{
  stm32_fixture f{u575(), firmware, std::move(options)};
  auto const    stopped = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  return f.log();
}

auto field(stm32_fixture const& f, std::string_view reg, std::string_view name) -> std::uint32_t
{
  return f.field("PWR", reg, name);
}
}

TEST_CASE("voltage scale 1 and the EPOD booster become ready after the PWR delay")
{
  auto const log = run_log("pwr_ll");
  REQUIRE(log.size() == 4);
  CHECK(log[0] > 0);
  CHECK(log[0] <= 200);
  CHECK(log[1] > 0);
  CHECK(log[1] <= 200);
  CHECK(log[2] == 3u << 16);
  CHECK(log[3] == 3);
  CHECK(run_log("pwr_ll") == log);
}

TEST_CASE("the PWR factory copies its ready delay: a board changed afterwards does not reach it")
{
  stm32::board board{};
  board.pwr.ready_delay = std::chrono::milliseconds{1};
  auto options          = popcorn::testing::stm32_options();
  options.models        = stm32::factories(board, u575());
  board.pwr.ready_delay = std::chrono::microseconds{50};
  auto const log        = run_log("pwr_ll", std::move(options));
  REQUIRE(log.size() == 4);
  CHECK(log[0] > 200);
  CHECK(log[1] > 200);
}

TEST_CASE("a snapshot taken while VOSRDY is pending restores to the same run")
{
  stm32_fixture f{u575(), "pwr_ll", popcorn::testing::stm32_options()};
  while(field(f, "PWR_VOSR", "VOS") == 0)
  {
    REQUIRE_FALSE(f.run({.max_instructions = icount{4}}).stopped.has_value());
  }
  REQUIRE(field(f, "PWR_VOSR", "VOSRDY") == 0);
  auto const saved = f.target->snapshot();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const expected = f.log();
  f.target->restore(saved);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.log() == expected);
  CHECK(field(f, "PWR_SVMSR", "ACTVOS") == 3);
}

TEST_CASE("FLASH keeps its latency and unlocks with the key pair, LOCK relocks")
{
  auto const log = run_log("flash_ll");
  CHECK(log == std::vector<std::uint32_t>{4, 1, 0, 1});
  CHECK(run_log("flash_ll") == log);
}

TEST_CASE("a wrong FLASH key is an imprecise bus fault")
{
  stm32_fixture f{u575(), "flash_ll"};
  f.set("pc", popcorn::testing::sym("flash_ll", "wrong_key"));
  auto const stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->kind == fault_kind::bus);
  CHECK((stopped.fault->status & imprecise_bus_error) != 0);
  CHECK(f.field("FLASH", "FLASH_NSCR", "LOCK") == 1);
}

TEST_CASE("after a wrong key FLASH_NSCR stays locked until reset")
{
  auto options   = popcorn::testing::stm32_options();
  options.faults = popcorn::arch::fault_policy::enter_handler;
  stm32_fixture f{u575(), "flash_ll", options};
  f.set("pc", popcorn::testing::sym("flash_ll", "wrong_key"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{handler_mark, 1});
  f.target->reset();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{4, 1, 0, 1});
}

TEST_CASE("LL cache maintenance and the ICACHE interrupt on a U575, IDCODE from DBGMCU")
{
  auto const log = run_log("cache_ll");
  REQUIRE(log.size() == 7);
  CHECK(log[0] == 0);
  CHECK(log[1] == 1);
  CHECK(log[2] > 0);
  CHECK(log[3] == 1);
  CHECK(log[5] > 0);
  CHECK(log[6] == 0x3007'6482);
  CHECK(run_log("cache_ll") == log);
}

TEST_CASE("FLASH_NSCR ignores writes while locked and clears STRT once unlocked")
{
  stm32_fixture f{u575(), "flash_ll"};
  auto&         regs = f.target->peripheral("FLASH");
  auto const    nscr = popcorn::find_register(regs.description(), std::string_view{"FLASH_NSCR"});
  auto const  nskeyr = popcorn::find_register(regs.description(), std::string_view{"FLASH_NSKEYR"});
  auto* const model  = f.target->model("FLASH");
  auto const  write  = [&](std::uint32_t offset, std::uint32_t value) {
    return model->on_write(regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}});
  };
  REQUIRE(write(nscr->offset, 1u).has_value());
  CHECK(f.field("FLASH", "FLASH_NSCR", "PG") == 0);
  CHECK(f.field("FLASH", "FLASH_NSCR", "LOCK") == 1);
  REQUIRE(write(nskeyr->offset, 0x4567'0123u).has_value());
  REQUIRE(write(nskeyr->offset, 0xcdef'89abu).has_value());
  CHECK(regs.peek(nskeyr->offset) == 0);
  REQUIRE(write(nscr->offset, 1u | (1u << 16)).has_value());
  CHECK(f.field("FLASH", "FLASH_NSCR", "PG") == 1);
  CHECK(f.field("FLASH", "FLASH_NSCR", "STRT") == 0);
  CHECK(f.field("FLASH", "FLASH_NSCR", "OPTLOCK") == 1);
  CHECK_FALSE(write(nskeyr->offset, 0x4567'0123u).has_value());
}

TEST_CASE("PWR logs what v1 does not model, once")
{
  stm32_fixture                 f{u575(), "flash_ll"};
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto const write = [&](std::string_view peripheral, std::string_view reg, std::uint32_t value) {
    auto&      regs  = f.target->peripheral(peripheral);
    auto const found = popcorn::find_register(regs.description(), reg);
    REQUIRE(found != nullptr);
    REQUIRE(f.target->model(peripheral)
                ->on_write(
                    regs, found->offset, 4, regs.peek(found->offset), value,
                    {.pc = 0, .now = icount{0}})
                .has_value());
  };
  write("PWR", "PWR_CR1", 2);
  write("PWR", "PWR_CR1", 4);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].peripheral == "PWR");
  CHECK(logged[0].reg == "PWR_CR1");
  CHECK(logged[0].message.contains("stop2"));
}

TEST_CASE("a snapshot between the two FLASH keys restores the half-done sequence")
{
  stm32_fixture f{u575(), "flash_ll"};
  auto&         regs = f.target->peripheral("FLASH");
  auto const  nskeyr = popcorn::find_register(regs.description(), std::string_view{"FLASH_NSKEYR"});
  auto* const model  = f.target->model("FLASH");
  auto const  key    = [&](std::uint32_t value) {
    return model->on_write(regs, nskeyr->offset, 4, 0, value, {.pc = 0, .now = icount{0}});
  };
  REQUIRE(key(0x4567'0123u).has_value());
  auto const saved = f.target->snapshot();
  REQUIRE(key(0x1111'1111u).has_value() == false);
  f.target->restore(saved);
  REQUIRE(key(0xcdef'89abu).has_value());
  CHECK(f.field("FLASH", "FLASH_NSCR", "LOCK") == 0);
}

TEST_CASE("the first PWR_CR1 write logs its LPMS mode, Stop 0 included")
{
  stm32_fixture                 f{u575(), "flash_ll"};
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto&      regs = f.target->peripheral("PWR");
  auto const cr1  = popcorn::find_register(regs.description(), std::string_view{"PWR_CR1"});
  REQUIRE(
      f.target->model("PWR")->on_write(regs, cr1->offset, 4, 0, 0, {.pc = 0, .now = icount{0}}));
  REQUIRE(logged.size() == 1);
  CHECK(logged.front().message.contains("stop0"));
}

TEST_CASE("RM0456 10.10.4: active_level() maps every ACTVOS code to a level, range 1 is level 0")
{
  stm32_fixture f{u575(), "pwr_ll", popcorn::testing::stm32_options()};
  auto* const   source = dynamic_cast<stm32::voltage_range_source*>(f.target->model("PWR"));
  REQUIRE(source != nullptr);
  auto&       regs  = f.target->peripheral("PWR");
  auto const* svmsr = popcorn::find_register(regs.description(), std::string_view{"PWR_SVMSR"});
  auto const  active =
      std::ranges::find(svmsr->fields, std::string_view{"ACTVOS"}, &popcorn::field_desc::name);
  REQUIRE(active != svmsr->fields.end());
  auto const shift = 8 * (svmsr->offset % 4) + active->lsb;
  auto const word  = svmsr->offset & ~3u;
  auto const mask  = std::uint32_t{3} << shift;
  for(std::uint32_t code = 0; code < 4; ++code)
  {
    CAPTURE(code);
    regs.poke(word, (regs.peek(word) & ~mask) | (code << shift));
    CHECK(source->active_level() == 3 - code);
  }
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(source->active_level() == 0);
}

TEST_CASE("RM0456 10.10.4: a lower VOS range keeps VOSRDY, a higher one clears it until the delay")
{
  stm32_fixture           f{u575(), "pwr_ll", popcorn::testing::stm32_options()};
  constexpr std::uint32_t loop_at = 0x2000'0100;
  REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
  f.set("pc", loop_at | 1u);
  auto const& pwr    = f.target->peripheral("PWR").description();
  auto const  vosr   = pwr.base + popcorn::find_register(pwr, "PWR_VOSR")->offset;
  auto const  run_us = [&](std::uint64_t us) {
    auto const until = popcorn::vtime{f.target->virtual_clock().time().ps + us * 1'000'000};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = until}).reason
        == stop_reason::time);
  };
  REQUIRE(field(f, "PWR_VOSR", "VOSRDY") == 1);
  REQUIRE(f.target->mem().write(vosr, 4, 3u << 16).has_value());
  CHECK(field(f, "PWR_VOSR", "VOSRDY") == 0);
  CHECK(field(f, "PWR_SVMSR", "ACTVOSRDY") == 0);
  run_us(60);
  CHECK(field(f, "PWR_VOSR", "VOSRDY") == 1);
  CHECK(field(f, "PWR_SVMSR", "ACTVOS") == 3);
  REQUIRE(f.target->mem().write(vosr, 4, 1u << 16).has_value());
  CHECK(field(f, "PWR_VOSR", "VOSRDY") == 1);
  CHECK(field(f, "PWR_SVMSR", "ACTVOSRDY") == 0);
  CHECK(field(f, "PWR_SVMSR", "ACTVOS") == 3);
  run_us(60);
  CHECK(field(f, "PWR_VOSR", "VOSRDY") == 1);
  CHECK(field(f, "PWR_SVMSR", "ACTVOSRDY") == 1);
  CHECK(field(f, "PWR_SVMSR", "ACTVOS") == 1);
}

TEST_CASE("coverage classifies the PWR_SVMSR polls of the voltage change as served")
{
  stm32_fixture f{u575(), "pwr_ll", popcorn::testing::stm32_options()};
  f.target->enable_coverage({}, true);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const found = f.target->coverage().unmodelled;
  CHECK(std::ranges::none_of(found, [](core::unmodelled_access const& access) {
    return access.peripheral == "PWR" and access.reg == "SVMSR";
  }));
}

namespace
{
constexpr auto hal_set_regsel = std::to_array<std::uint16_t>(
    {0x6803, 0xf043, 0x0302, 0x6003, 0x680b, 0xf013, 0x0f02, 0xd101, 0x3a01, 0xd1f9, 0xbe00});
constexpr auto hal_clear_regsel = std::to_array<std::uint16_t>(
    {0x6803, 0xf023, 0x0302, 0x6003, 0x680b, 0xf013, 0x0f02, 0xd001, 0x3a01, 0xd1f9, 0xbe00});
constexpr std::uint32_t hal_timeout_at_msis_4mhz = 50 * (4'000'000 / 1000) / 1000 + 1;
constexpr std::uint64_t us_ps                    = 1'000'000;

auto u575_with_smps() -> popcorn::part_description
{
  auto desc = u575();
  desc.part = "STM32U575ZITxQ";
  return desc;
}

struct supply_rig
{
  stm32_fixture f;

  explicit supply_rig(
      popcorn::part_description desc,
      core::machine_options     options = popcorn::testing::stm32_options())
      : f{std::move(desc), "pwr_ll", std::move(options)}
  {}

  [[nodiscard]] auto at(std::string_view reg) const -> popcorn::address
  {
    auto const& pwr = f.target->peripheral("PWR").description();
    return pwr.base + popcorn::find_register(pwr, reg)->offset;
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(reg), 4, value).has_value());
  }

  void cpu_write(std::string_view reg, std::uint32_t value) const
  {
    auto&      regs  = f.target->peripheral("PWR");
    auto const found = popcorn::find_register(regs.description(), reg);
    REQUIRE(found != nullptr);
    REQUIRE(f.target->model("PWR")
                ->on_write(
                    regs, found->offset, 4, regs.peek(found->offset), value,
                    {.pc = 0, .now = icount{0}})
                .has_value());
  }

  [[nodiscard]] auto regs() const -> std::uint32_t
  {
    return field(f, "PWR_SVMSR", "REGS");
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    constexpr std::uint32_t loop_at = 0x2000'0100;
    REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
    f.set("pc", loop_at | 1u);
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = popcorn::vtime{ps}})
            .reason
        == stop_reason::time);
  }

  [[nodiscard]] auto pending_pwr() const -> std::vector<std::uint64_t>
  {
    auto const& units = f.target->description().peripherals;
    auto const  found =
        std::ranges::find(units, std::string_view{"PWR"}, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    auto const                 owner = static_cast<std::uint32_t>(found - units.begin());
    std::vector<std::uint64_t> due;
    for(auto const& event : f.target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner)
      {
        due.push_back(event.due_ps);
      }
    }
    return due;
  }

  void range_1() const
  {
    write("PWR_VOSR", 3u << 16);
    run_until(now() + 60 * us_ps);
    REQUIRE(field(f, "PWR_SVMSR", "ACTVOS") == 3);
    REQUIRE(pending_pwr().empty());
  }

  auto hal_config_supply(std::span<std::uint16_t const> code) const -> std::uint32_t
  {
    constexpr std::uint32_t code_at = 0x2000'0200;
    for(auto const [index, half] : std::views::enumerate(code))
    {
      REQUIRE(f.target->mem().write(code_at + 2 * index, 2, half).has_value());
    }
    f.set("r0", at("PWR_CR3"));
    f.set("r1", at("PWR_SVMSR"));
    f.set("r2", hal_timeout_at_msis_4mhz);
    f.set("pc", code_at | 1u);
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    return static_cast<std::uint32_t>(f.target->regs().read(*f.target->regs().find("r2")));
  }
};
}

TEST_CASE(
    "RM0456 10.5.2 on a package with SMPS: the HAL_PWREx_ConfigSupply poll sees REGS follow REGSEL")
{
  supply_rig r{u575_with_smps()};
  REQUIRE(r.regs() == 0);
  auto const start = r.now();
  CHECK(r.hal_config_supply(hal_set_regsel) != 0);
  CHECK(r.regs() == 1);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 1);
  CHECK(r.now() - start >= 14 * us_ps);
  CHECK(r.now() - start < 18 * us_ps);
  CHECK(r.hal_config_supply(hal_clear_regsel) != 0);
  CHECK(r.regs() == 0);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 0);
}

TEST_CASE(
    "DS13737 Table 75: the REGS change is due tSMPS or tLDO after the REGSEL write, by ACTVOS")
{
  supply_rig r{u575_with_smps()};
  auto const idle = r.pending_pwr();
  REQUIRE(idle.empty());
  auto written = r.now();
  r.write("PWR_CR3", 2);
  CHECK(r.pending_pwr() == std::vector<std::uint64_t>{written + 14 * us_ps});
  CHECK(r.regs() == 0);
  r.run_until(written + 13 * us_ps);
  CHECK(r.regs() == 0);
  r.run_until(written + 14 * us_ps);
  CHECK(r.regs() == 1);
  written = r.now();
  r.write("PWR_CR3", 0);
  CHECK(r.pending_pwr() == std::vector<std::uint64_t>{written + 16 * us_ps});
  r.run_until(written + 16 * us_ps);
  CHECK(r.regs() == 0);
  r.range_1();
  written = r.now();
  r.write("PWR_CR3", 2);
  CHECK(r.pending_pwr() == std::vector<std::uint64_t>{written + 16 * us_ps});
  r.run_until(written + 16 * us_ps);
  CHECK(r.regs() == 1);
  written = r.now();
  r.write("PWR_CR3", 0);
  CHECK(r.pending_pwr() == std::vector<std::uint64_t>{written + 14 * us_ps});
  r.run_until(written + 14 * us_ps);
  CHECK(r.regs() == 0);
}

TEST_CASE("a REGSEL write superseding a pending switch drops the earlier REGS change")
{
  supply_rig r{u575_with_smps()};
  auto const start = r.now();
  r.write("PWR_CR3", 2);
  r.run_until(start + 4 * us_ps);
  r.write("PWR_CR3", 0);
  r.run_until(start + 8 * us_ps);
  r.write("PWR_CR3", 2);
  r.run_until(start + 14 * us_ps);
  CHECK(r.regs() == 0);
  r.run_until(start + 22 * us_ps);
  CHECK(r.regs() == 1);
  r.write("PWR_CR3", 2 | 4);
  CHECK(r.pending_pwr().empty());
}

TEST_CASE("a system reset with a switch to SMPS pending leaves REGSEL and REGS 0, the LDO")
{
  supply_rig r{u575_with_smps()};
  r.write("PWR_CR3", 2);
  r.f.target->reset(popcorn::reset_kind::software);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 0);
  r.run_until(r.now() + 30 * us_ps);
  CHECK(r.regs() == 0);
  r.write("PWR_CR3", 2);
  r.run_until(r.now() + 14 * us_ps);
  CHECK(r.regs() == 1);
  r.f.target->reset(popcorn::reset_kind::software);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 0);
  CHECK(r.regs() == 0);
}

TEST_CASE("a power-on reset after a switch to SMPS leaves REGSEL and REGS 0, the LDO")
{
  supply_rig r{u575_with_smps()};
  r.write("PWR_CR3", 2);
  r.run_until(r.now() + 14 * us_ps);
  REQUIRE(r.regs() == 1);
  r.f.target->reset(popcorn::reset_kind::power_on);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 0);
  CHECK(r.regs() == 0);
}

TEST_CASE("a snapshot taken while REGS is pending restores the switch at its due time")
{
  supply_rig r{u575_with_smps()};
  auto const written = r.now();
  r.write("PWR_CR3", 2);
  r.run_until(written + 5 * us_ps);
  auto const saved = r.f.target->snapshot();
  r.write("PWR_CR3", 0);
  r.run_until(written + 20 * us_ps);
  REQUIRE(r.regs() == 0);
  r.f.target->restore(saved);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 1);
  CHECK(r.regs() == 0);
  CHECK(r.pending_pwr() == std::vector<std::uint64_t>{written + 14 * us_ps});
  r.run_until(written + 14 * us_ps);
  CHECK(r.regs() == 1);
}

TEST_CASE("RM0456 10.10.3 on a package without SMPS: REGSEL is reserved, REGS stays 0, one warning")
{
  supply_rig                    r{u575()};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  CHECK(r.hal_config_supply(hal_set_regsel) == 0);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 1);
  CHECK(r.regs() == 0);
  CHECK(r.pending_pwr().empty());
  r.cpu_write("PWR_CR3", 2);
  r.write("PWR_CR3", 2);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].reg == "PWR_CR3");
  CHECK(logged[0].message.contains("REGSEL"));
  r.cpu_write("PWR_CR3", 4);
  CHECK(logged.size() == 1);
}

TEST_CASE("on a package without SMPS a REGSEL write is a model_error when strict, REGSEL kept 0")
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  supply_rig r{u575(), std::move(options)};
  auto&      regs  = r.f.target->peripheral("PWR");
  auto const found = popcorn::find_register(regs.description(), std::string_view{"PWR_CR3"});
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->model("PWR")->on_write(
          regs, found->offset, 4, 0, 2 | 4, {.pc = 0, .now = icount{0}})),
      popcorn::model_error);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 0);
  CHECK(field(r.f, "PWR_CR3", "FSTEN") == 0);
  r.cpu_write("PWR_CR3", 4);
  r.write("PWR_CR3", 2);
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 1);
  CHECK(r.regs() == 0);
}

TEST_CASE("on a package without SMPS a firmware write over a debugger-set REGSEL is no model_error")
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  supply_rig                    r{u575(), std::move(options)};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  r.write("PWR_CR3", 2);
  REQUIRE(field(r.f, "PWR_CR3", "REGSEL") == 1);
  CHECK_NOTHROW(r.cpu_write("PWR_CR3", 2 | 4));
  CHECK(field(r.f, "PWR_CR3", "REGSEL") == 1);
  CHECK(field(r.f, "PWR_CR3", "FSTEN") == 1);
  CHECK(r.regs() == 0);
  CHECK(logged.empty());
}

TEST_CASE("a U5 description without PWR_CR3.REGSEL or PWR_SVMSR.REGS throws config_error")
{
  for(auto const& [reg, name] : {
          std::pair{"CR3",   "REGSEL"},
          std::pair{"SVMSR", "REGS"  }
  })
  {
    for(auto desc : {u575(), u575_with_smps()})
    {
      CAPTURE(desc.part);
      CAPTURE(name);
      auto pwr = std::ranges::find(
          desc.peripherals, std::string_view{"PWR"}, &popcorn::peripheral_desc::name);
      REQUIRE(pwr != desc.peripherals.end());
      auto found =
          std::ranges::find(pwr->registers, std::string_view{reg}, &popcorn::register_desc::name);
      REQUIRE(found != pwr->registers.end());
      REQUIRE(
          std::erase_if(found->fields, [&](popcorn::field_desc const& f) { return f.name == name; })
          == 1);
      stm32::register_builtin_models();
      CHECK_THROWS_AS(
          core::machine(desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()),
          popcorn::config_error);
    }
  }
}
