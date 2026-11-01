#include "stm32_fixture.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;

constexpr auto firmware = std::string_view{"l152xe_l1_pwr_ll"};

auto pwr_desc() -> popcorn::peripheral_desc const&
{
  return *std::ranges::find(
      l152re().peripherals, std::string_view{"PWR"}, &popcorn::peripheral_desc::name);
}

auto bits_of(std::string_view reg, std::string_view name) -> popcorn::field_desc const&
{
  auto const* const found = popcorn::find_register(pwr_desc(), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return *bits;
}

void set(stm32_fixture const& f, std::string_view reg, std::string_view name, std::uint32_t value)
{
  auto const& bits   = bits_of(reg, name);
  auto const  offset = popcorn::find_register(pwr_desc(), reg)->offset;
  auto const  mask   = ((std::uint32_t{1} << bits.width) - 1u) << bits.lsb;
  auto&       regs   = f.target->peripheral("PWR");
  auto const  word   = regs.peek(offset);
  REQUIRE(f.target->model("PWR")
              ->on_write(
                  regs, offset, 4, word, (word & ~mask) | (value << bits.lsb),
                  {.pc = 0, .now = icount{0}})
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
  stm32_fixture f{l152re(), firmware, std::move(options)};
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  return f;
}
}

TEST_CASE("RM0038 5.4: the LL sequence polls VOSF for Range 1 then Range 3, DBP by bit-band")
{
  stm32_fixture f{l152re(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 6);
  CHECK(log[0] == 0x1000u);
  CHECK(log[1] > 0);
  CHECK(log[2] == 0x0800u);
  CHECK(log[3] > 0);
  CHECK(log[4] == 0x1800u);
  CHECK(log[5] == 0x0100u);
  CHECK(f.field("PWR", "CSR", "VOSF") == 0);
  CHECK(level(f) == 2);
}

TEST_CASE("RM0038 5.4.1: VOSF reads 1 from the VOS write to the ready delay, the level then moves")
{
  auto f = parked();
  CHECK(f.field("PWR", "CR", "VOS") == 2);
  CHECK(f.field("PWR", "CSR", "VOSF") == 0);
  CHECK(level(f) == 1);
  set(f, "CR", "VOS", 1);
  CHECK(f.field("PWR", "CSR", "VOSF") == 1);
  CHECK(level(f) == 1);
  idle_for(f, std::chrono::microseconds{60});
  CHECK(f.field("PWR", "CSR", "VOSF") == 0);
  CHECK(level(f) == 0);
  set(f, "CR", "VOS", 3);
  CHECK(f.field("PWR", "CSR", "VOSF") == 1);
  CHECK(level(f) == 0);
  idle_for(f, std::chrono::microseconds{60});
  CHECK(f.field("PWR", "CSR", "VOSF") == 0);
  CHECK(level(f) == 2);
  set(f, "CR", "DBP", 1);
  CHECK(f.field("PWR", "CSR", "VOSF") == 0);
}

TEST_CASE("RM0038 5.4.1: VOS 00 is forbidden, the write keeps the range and warns once")
{
  auto                          f = parked();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  set(f, "CR", "VOS", 0);
  set(f, "CR", "VOS", 0);
  CHECK(f.field("PWR", "CR", "VOS") == 2);
  CHECK(f.field("PWR", "CSR", "VOSF") == 0);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].reg == "CR");
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto       strict          = parked(std::move(options));
  auto&      regs            = strict.target->peripheral("PWR");
  auto const word            = regs.peek(0);
  CHECK_THROWS_AS(
      (void)strict.target->model("PWR")->on_write(
          regs, 0, 4, word, word & ~(3u << 11), {.pc = 0, .now = icount{0}}),
      popcorn::model_error);
  CHECK(strict.field("PWR", "CR", "VOS") == 2);
}

TEST_CASE("RM0038 5.4.1: the first PDDS change names the low-power mode, once")
{
  auto                          f = parked();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  set(f, "CR", "DBP", 1);
  CHECK(logged.empty());
  set(f, "CR", "PDDS", 1);
  set(f, "CR", "PDDS", 0);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("PDDS selects standby"));
}

TEST_CASE("RM0038 5.4.1: a debugger write of VOS 00 keeps the range with no log record")
{
  auto                          f = parked();
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto&      regs = f.target->peripheral("PWR");
  auto const word = regs.peek(0);
  REQUIRE(f.target->model("PWR")
              ->on_write(
                  regs, 0, 4, word, word & ~(3u << 11),
                  {.pc = 0, .now = icount{0}, .by = popcorn::initiator::debugger})
              .has_value());
  CHECK(f.field("PWR", "CR", "VOS") == 2);
  CHECK(logged.empty());
}

TEST_CASE("RM0038 5.4.1: a PWRRST pulse keeps FWU and ULP and resets VOS")
{
  auto f = parked();
  set(f, "CR", "FWU", 1);
  set(f, "CR", "ULP", 1);
  set(f, "CR", "VOS", 1);
  idle_for(f, std::chrono::microseconds{60});
  auto const& rcc = *std::ranges::find(
      l152re().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  auto const* const reset = popcorn::find_register(rcc, "APB1RSTR");
  auto const        bit =
      std::ranges::find(reset->fields, std::string_view{"PWRRST"}, &popcorn::field_desc::name);
  REQUIRE(bit != reset->fields.end());
  auto const at = rcc.base + reset->offset;
  REQUIRE(f.target->mem().write(at, 4, std::uint32_t{1} << bit->lsb).has_value());
  REQUIRE(f.target->mem().write(at, 4, 0).has_value());
  CHECK(f.field("PWR", "CR", "FWU") == 1);
  CHECK(f.field("PWR", "CR", "ULP") == 1);
  CHECK(f.field("PWR", "CR", "VOS") == 2);
  CHECK(level(f) == 1);
}

TEST_CASE("RM0038 5.4.1: CSBF and CWUF read as 0 after a write of 1")
{
  auto f = parked();
  set(f, "CR", "CWUF", 1);
  set(f, "CR", "CSBF", 1);
  auto const& cr = *popcorn::find_register(pwr_desc(), "CR");
  for(auto const name : {"CWUF", "CSBF"})
  {
    auto const bits =
        std::ranges::find(cr.fields, std::string_view{name}, &popcorn::field_desc::name);
    REQUIRE(bits != cr.fields.end());
    CHECK((f.target->mem().read(pwr_desc().base, 4).value() >> bits->lsb & 1u) == 0);
  }
}
