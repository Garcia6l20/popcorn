#include "stm32_fixture.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string_view>
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

constexpr auto firmware = std::string_view{"l476_l4_pwr_ll"};

auto pwr_desc() -> popcorn::peripheral_desc const&
{
  return *std::ranges::find(
      l476rc().peripherals, std::string_view{"PWR"}, &popcorn::peripheral_desc::name);
}

auto offset_of(std::string_view reg) -> std::uint32_t
{
  auto const* const found = popcorn::find_register(pwr_desc(), reg);
  REQUIRE(found != nullptr);
  return found->offset;
}

auto bits_of(std::string_view reg, std::string_view name) -> popcorn::field_desc const&
{
  auto const* const found = popcorn::find_register(pwr_desc(), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return *bits;
}

void set(
    stm32_fixture const& f,
    std::string_view     reg,
    std::string_view     name,
    std::uint32_t        value,
    popcorn::initiator   by = popcorn::initiator::cpu)
{
  auto const& bits   = bits_of(reg, name);
  auto const  offset = offset_of(reg);
  auto const  mask   = ((std::uint32_t{1} << bits.width) - 1u) << bits.lsb;
  auto&       regs   = f.target->peripheral("PWR");
  auto const  word   = regs.peek(offset);
  REQUIRE(f.target->model("PWR")
              ->on_write(
                  regs, offset, 4, word, (word & ~mask) | (value << bits.lsb),
                  {.pc = 0, .now = icount{0}, .by = by})
              .has_value());
}

auto level(stm32_fixture const& f) -> std::uint32_t
{
  auto const* const source =
      dynamic_cast<stm32::voltage_range_source const*>(f.target->model("PWR"));
  REQUIRE(source != nullptr);
  return source->active_level();
}

void idle_for(stm32_fixture const& f, std::chrono::microseconds span)
{
  auto const now     = f.target->virtual_clock().time();
  auto const stopped = f.run_to_stop(
      {.max_instructions = icount{1'000'000},
       .until_time       = vtime{now.ps + static_cast<std::uint64_t>(span.count()) * 1'000'000}});
  REQUIRE(stopped.reason == stop_reason::time);
}

auto parked(core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{l476rc(), firmware, std::move(options)};
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  return f;
}

auto strict_options() -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  return options;
}

constexpr auto functions = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"CR1", "LPR"  },
    {"CR2", "PVDE" },
    {"CR2", "PVME1"},
    {"CR2", "PVME2"},
    {"CR2", "PVME3"},
    {"CR2", "PVME4"},
    {"CR3", "EWUP1"},
    {"CR3", "EWUP2"},
    {"CR3", "EWUP3"},
    {"CR3", "EWUP4"},
    {"CR3", "EWUP5"},
    {"CR3", "APC"  },
});
}

TEST_CASE("RM0351 5.1.8: the LL sequence polls VOSF for Range 2 then Range 1, then DBP")
{
  stm32_fixture f{l476rc(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 7);
  CHECK(log[0] == 0x0200u);
  CHECK(log[1] > 0);
  CHECK(log[2] == 0x0400u);
  CHECK(log[3] > 0);
  CHECK(log[4] == 0x0200u);
  CHECK(log[5] == 0x0100u);
  CHECK(log[6] == 0xA5C3u);
  CHECK(f.field("PWR", "SR2", "VOSF") == 0);
  CHECK(level(f) == 0);
}

TEST_CASE("DS10198 Table 53: VOSF reads 1 from the VOS write to the ready delay in both directions")
{
  auto f = parked();
  CHECK(f.field("PWR", "CR1", "VOS") == 1);
  CHECK(f.field("PWR", "SR2", "VOSF") == 0);
  CHECK(level(f) == 0);
  set(f, "CR1", "VOS", 2);
  CHECK(f.field("PWR", "SR2", "VOSF") == 1);
  CHECK(level(f) == 0);
  idle_for(f, std::chrono::microseconds{40});
  CHECK(f.field("PWR", "SR2", "VOSF") == 1);
  CHECK(level(f) == 0);
  idle_for(f, std::chrono::microseconds{20});
  CHECK(f.field("PWR", "SR2", "VOSF") == 0);
  CHECK(level(f) == 1);
  set(f, "CR1", "VOS", 1);
  CHECK(f.field("PWR", "SR2", "VOSF") == 1);
  CHECK(level(f) == 1);
  idle_for(f, std::chrono::microseconds{60});
  CHECK(f.field("PWR", "SR2", "VOSF") == 0);
  CHECK(level(f) == 0);
  set(f, "CR1", "DBP", 1);
  CHECK(f.field("PWR", "SR2", "VOSF") == 0);
}

TEST_CASE("RM0351 5.4.1: VOS 00 and 11 cannot be written, the range stays, one warning")
{
  auto                          f = parked();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  set(f, "CR1", "VOS", 0);
  set(f, "CR1", "VOS", 3);
  CHECK(f.field("PWR", "CR1", "VOS") == 1);
  CHECK(f.field("PWR", "SR2", "VOSF") == 0);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].reg == "CR1");
  for(auto const forbidden : {0u, 3u})
  {
    CAPTURE(forbidden);
    auto       strict = parked(strict_options());
    auto&      regs   = strict.target->peripheral("PWR");
    auto const word   = regs.peek(0);
    CHECK_THROWS_AS(
        (void)strict.target->model("PWR")->on_write(
            regs, 0, 4, word, (word & ~(3u << 9)) | (forbidden << 9), {.pc = 0, .now = icount{0}}),
        popcorn::model_error);
    CHECK(strict.field("PWR", "CR1", "VOS") == 1);
  }
}

TEST_CASE("RM0351 5.4.1: a debugger write of VOS 00 keeps the range with no log record")
{
  auto                          f = parked();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  set(f, "CR1", "VOS", 0, popcorn::initiator::debugger);
  CHECK(f.field("PWR", "CR1", "VOS") == 1);
  CHECK(logged.empty());
}

TEST_CASE("RM0351 5.4.1: Stop 2 by LPMS 010 is named once and a deep-sleep WFI idles")
{
  auto                          f = parked();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  f.set("pc", popcorn::testing::sym(firmware, "stop2"));
  idle_for(f, std::chrono::microseconds{200});
  CHECK(f.field("PWR", "CR1", "LPMS") == 2);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].reg == "CR1");
  CHECK(logged[0].message.contains("LPMS selects stop2"));
  idle_for(f, std::chrono::microseconds{200});
  CHECK(logged.size() == 1);
}

TEST_CASE("RM0351 5.4.1: LPMS names Stop 1, Standby and Shutdown at its first change")
{
  constexpr auto modes = std::to_array<std::pair<std::uint32_t, std::string_view>>({
      {1, "stop1"   },
      {3, "standby" },
      {4, "shutdown"},
      {7, "shutdown"},
  });
  for(auto const& [value, name] : modes)
  {
    CAPTURE(value);
    auto                          f = parked();
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    set(f, "CR1", "LPMS", value);
    set(f, "CR1", "LPMS", 0);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains(std::format("LPMS selects {}", name)));
  }
}

TEST_CASE("RM0351 5.4: a function enable the model does not run warns once per field")
{
  for(auto const& [reg, name] : functions)
  {
    CAPTURE(name);
    auto                          f = parked();
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    set(f, reg, name, 0);
    CHECK(logged.empty());
    set(f, reg, name, 1, popcorn::initiator::debugger);
    set(f, reg, name, 0, popcorn::initiator::debugger);
    CHECK(logged.empty());
    set(f, reg, name, 1);
    CHECK(f.field("PWR", reg, name) == 1);
    set(f, reg, name, 0);
    set(f, reg, name, 1);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].kind == core::log_kind::model_warning);
    CHECK(logged[0].reg == reg);
    CHECK(logged[0].message.starts_with(name));
  }
}

TEST_CASE("RM0351 5.4: a strict function enable raises, the register stays, a set bit is kept")
{
  for(auto const& [reg, name] : functions)
  {
    CAPTURE(name);
    auto        strict = parked(strict_options());
    auto&       regs   = strict.target->peripheral("PWR");
    auto const  offset = offset_of(reg);
    auto const  word   = regs.peek(offset);
    auto const& bits   = bits_of(reg, name);
    CHECK_THROWS_AS(
        (void)strict.target->model("PWR")->on_write(
            regs, offset, 4, word, word | 1u << bits.lsb, {.pc = 0, .now = icount{0}}),
        popcorn::model_error);
    CHECK(regs.peek(offset) == word);
    set(strict, reg, name, 1, popcorn::initiator::debugger);
    CHECK(strict.field("PWR", reg, name) == 1);
    set(strict, reg, name, 1);
    CHECK(strict.field("PWR", reg, name) == 1);
  }
}

TEST_CASE("RM0351 5.4.1 and 5.4.2: DBP, USV, IOSV, PLS and EIWUL are stored with no log record")
{
  auto                          f = parked(strict_options());
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  CHECK(f.field("PWR", "CR3", "EIWUL") == 1);
  set(f, "CR1", "DBP", 1);
  set(f, "CR2", "USV", 1);
  set(f, "CR2", "IOSV", 1);
  set(f, "CR2", "PLS", 5);
  set(f, "CR3", "EIWUL", 0);
  set(f, "CR3", "RRS", 1);
  CHECK(f.field("PWR", "CR1", "DBP") == 1);
  CHECK(f.field("PWR", "CR2", "USV") == 1);
  CHECK(f.field("PWR", "CR2", "IOSV") == 1);
  CHECK(f.field("PWR", "CR2", "PLS") == 5);
  CHECK(f.field("PWR", "CR3", "EIWUL") == 0);
  CHECK(f.field("PWR", "CR3", "RRS") == 1);
  CHECK(logged.empty());
}

TEST_CASE("RM0351 5.4.3, 5.4.8, 5.4.9 and 5.4.11: the description names the PWR bits RM0351 names")
{
  constexpr auto reserved = std::to_array<std::pair<std::string_view, std::string_view>>({
      {"CR3",   "EWF" },
      {"PUCRA", "PU14"},
      {"PDCRA", "PD13"},
      {"PDCRA", "PD15"},
      {"PDCRB", "PD4" },
  });
  for(auto const& [reg, name] : reserved)
  {
    CAPTURE(name);
    auto const* const found = popcorn::find_register(pwr_desc(), reg);
    REQUIRE(found != nullptr);
    CHECK(
        std::ranges::find(found->fields, name, &popcorn::field_desc::name) == found->fields.end());
  }
  CHECK(bits_of("CR3", "EIWUL").lsb == 15);
  CHECK(bits_of("PUCRA", "PU15").lsb == 15);
  CHECK(bits_of("PDCRA", "PD14").lsb == 14);
  CHECK(bits_of("PDCRB", "PD5").lsb == 5);
  CHECK(popcorn::find_register(pwr_desc(), "PUCRA")->fields.size() == 15);
  CHECK(popcorn::find_register(pwr_desc(), "PDCRA")->fields.size() == 14);
  CHECK(popcorn::find_register(pwr_desc(), "PDCRB")->fields.size() == 15);
}

TEST_CASE("RM0351 6.4.13: a PWRRST pulse resets every PWR register and the range")
{
  auto f = parked();
  set(f, "CR1", "VOS", 2);
  set(f, "CR2", "USV", 1);
  set(f, "CR3", "EIWUL", 0);
  auto const base = pwr_desc().base;
  REQUIRE(f.target->mem()
              .write(base + offset_of("PUCRA"), 4, 1u << bits_of("PUCRA", "PU0").lsb)
              .has_value());
  REQUIRE(f.target->mem()
              .write(base + offset_of("CR4"), 4, 1u << bits_of("CR4", "VBE").lsb)
              .has_value());
  REQUIRE(f.field("PWR", "PUCRA", "PU0") == 1);
  REQUIRE(f.field("PWR", "CR4", "VBE") == 1);
  idle_for(f, std::chrono::microseconds{60});
  REQUIRE(level(f) == 1);
  auto const& rcc = *std::ranges::find(
      l476rc().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  auto const* const reset = popcorn::find_register(rcc, "APB1RSTR1");
  REQUIRE(reset != nullptr);
  auto const bit =
      std::ranges::find(reset->fields, std::string_view{"PWRRST"}, &popcorn::field_desc::name);
  REQUIRE(bit != reset->fields.end());
  auto const at = rcc.base + reset->offset;
  REQUIRE(f.target->mem().write(at, 4, std::uint32_t{1} << bit->lsb).has_value());
  REQUIRE(f.target->mem().write(at, 4, 0).has_value());
  CHECK(f.field("PWR", "CR1", "VOS") == 1);
  CHECK(f.field("PWR", "CR2", "USV") == 0);
  CHECK(f.field("PWR", "CR3", "EIWUL") == 1);
  CHECK(f.field("PWR", "PUCRA", "PU0") == 0);
  CHECK(f.field("PWR", "CR4", "VBE") == 0);
  CHECK(level(f) == 0);
}
