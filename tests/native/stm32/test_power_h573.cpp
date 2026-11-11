#include "h573_secure_rig.hpp"
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
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <string>
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
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;

constexpr std::uint32_t loop_at = 0x2000'0100;
constexpr std::uint64_t ns      = 1'000;
constexpr std::uint64_t us      = 1'000'000;

struct rig
{
  stm32_fixture f;

  explicit rig(
      std::string_view      firmware = "h573_pwr_ll",
      core::machine_options options  = popcorn::testing::stm32_options())
      : f{h573(), firmware, std::move(options)}
  {
    REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
    f.set("pc", loop_at | 1u);
  }

  [[nodiscard]] auto at(std::string_view name) const -> popcorn::address
  {
    auto const found = popcorn::find_register(f.target->peripheral("PWR").description(), name);
    REQUIRE(found != nullptr);
    return f.target->peripheral("PWR").description().base + found->offset;
  }

  [[nodiscard]] auto read(std::string_view name) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(at(name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view name, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(name), 4, value).has_value());
  }

  [[nodiscard]] auto get(std::string_view reg, std::string_view field) const -> std::uint32_t
  {
    return f.field("PWR", reg, field);
  }

  void run_for(std::uint64_t ps) const
  {
    auto const until = vtime{f.target->virtual_clock().time().ps + ps};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = until}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto level() const -> std::uint32_t
  {
    auto* const source = f.model_of<stm32::voltage_range_source>("PWR");
    REQUIRE(source != nullptr);
    return source->active_level();
  }

  [[nodiscard]] auto pending() const -> std::size_t
  {
    return f.target->snapshot().data().events.pending.size();
  }
};
}

TEST_CASE("RM0481 10.11.5: VOS0 clears VOSRDY, the poll loop ends after the delay, ACTVOS reads 3")
{
  stm32_fixture f{h573(), "h573_pwr_ll", popcorn::testing::stm32_options()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 6);
  CHECK(log[0] == 0);
  CHECK(log[1] > 0);
  CHECK(log[1] <= 400);
  CHECK(log[2] == 3u << 4);
  CHECK(log[3] == 3);
  CHECK(log[4] == 1);
  CHECK(log[5] == 1);
  stm32_fixture again{h573(), "h573_pwr_ll", popcorn::testing::stm32_options()};
  REQUIRE(again.run_to_stop().reason == stop_reason::user);
  CHECK(again.log() == log);
}

TEST_CASE("RM0481 10.11.6: PWR_VOSSR resets to 0x2008, VOSRDY and ACTVOSRDY set at VOS3")
{
  rig r;
  CHECK(r.read("PWR_VOSSR") == 0x2008);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 0);
  CHECK(r.level() == 3);
}

TEST_CASE("STM32H573 silicon: VOS3 to VOS0 steps ACTVOS every 10.4 us and is ready after 36 us")
{
  rig        r;
  auto const idle = r.pending();
  r.write("PWR_VOSCR", 3u << 4);
  CHECK(r.read("PWR_VOSSR") == 0x4000);
  CHECK(r.level() == 3);
  CHECK(r.pending() == idle + 1);
  r.run_for(10'300 * ns);
  CHECK(r.read("PWR_VOSSR") == 0x4000);
  r.run_for(200 * ns);
  CHECK(r.read("PWR_VOSSR") == 0x8000);
  CHECK(r.level() == 3);
  r.run_for(10'200 * ns);
  CHECK(r.read("PWR_VOSSR") == 0x8000);
  r.run_for(200 * ns);
  CHECK(r.read("PWR_VOSSR") == 0xC000);
  CHECK(r.level() == 3);
  r.run_for(15'000 * ns);
  CHECK(r.read("PWR_VOSSR") == 0xC000);
  CHECK(r.level() == 3);
  r.run_for(200 * ns);
  CHECK(r.read("PWR_VOSSR") == 0xE008);
  CHECK(r.level() == 0);
}

TEST_CASE("a lower VOS keeps VOSRDY, RM0481 10.11.6, and steps down at the measured upward rate")
{
  rig r;
  r.write("PWR_VOSCR", 3u << 4);
  r.run_for(40 * us);
  REQUIRE(r.read("PWR_VOSSR") == 0xE008);
  auto const idle = r.pending();
  r.write("PWR_VOSCR", 1u << 4);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 0);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 2);
  CHECK(r.level() == 0);
  CHECK(r.pending() == idle + 1);
  r.run_for(10'500 * ns);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 0);
  CHECK(r.level() == 0);
  r.run_for(15'000 * ns);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 0);
  r.run_for(200 * ns);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 1);
  CHECK(r.level() == 2);
}

TEST_CASE("a VOS write that changes nothing starts no delay")
{
  rig        r;
  auto const idle = r.pending();
  r.write("PWR_VOSCR", 0);
  CHECK(r.pending() == idle);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
}

TEST_CASE("each VOS write restarts the steps: a stale step or ready event changes nothing")
{
  rig r;
  r.write("PWR_VOSCR", 3u << 4);
  r.run_for(2 * us);
  r.write("PWR_VOSCR", 0);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 0);
  r.run_for(2 * us);
  r.write("PWR_VOSCR", 3u << 4);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 1);
  r.run_for(8 * us);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 1);
  r.run_for(6 * us);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 2);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 0);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 0);
  r.run_for(21'900 * ns);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 3);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 0);
  r.run_for(200 * ns);
  CHECK(r.read("PWR_VOSSR") == 0xE008);
  CHECK(r.level() == 0);
}

TEST_CASE("a reset sets the FLASH voltage range back to VOS3 for the next VOS change")
{
  rig r;
  r.write("PWR_VOSCR", 3u << 4);
  r.run_for(40 * us);
  REQUIRE(r.level() == 0);
  r.f.target->reset();
  r.f.set("pc", loop_at | 1u);
  CHECK(r.level() == 3);
  r.write("PWR_VOSCR", 3u << 4);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 0);
  CHECK(r.level() == 3);
}

TEST_CASE("active_level() maps every H5 ACTVOS code to a level, VOS0 is level 0")
{
  rig   r;
  auto& regs  = r.f.target->peripheral("PWR");
  auto  vossr = popcorn::find_register(regs.description(), std::string_view{"PWR_VOSSR"});
  REQUIRE(vossr != nullptr);
  auto const& fields = vossr->fields;
  auto const  active =
      std::ranges::find(fields, std::string_view{"ACTVOS"}, &popcorn::field_desc::name);
  REQUIRE(active != fields.end());
  auto const shift = 8 * (vossr->offset % 4) + active->lsb;
  auto const word  = vossr->offset & ~3u;
  auto const mask  = std::uint32_t{3} << shift;
  for(std::uint32_t code = 0; code < 4; ++code)
  {
    CAPTURE(code);
    regs.poke(word, (regs.peek(word) & ~mask) | (code << shift));
    CHECK(r.level() == 3 - code);
  }
}

TEST_CASE("a snapshot taken between two ACTVOS steps restores to the same run")
{
  rig r;
  r.write("PWR_VOSCR", 3u << 4);
  auto const at_write = r.f.target->snapshot();
  r.run_for(12 * us);
  REQUIRE(r.get("PWR_VOSSR", "ACTVOS") == 2);
  auto const between = r.f.target->snapshot();
  r.run_for(30 * us);
  CHECK(r.read("PWR_VOSSR") == 0xE008);
  r.f.target->restore(between);
  CHECK(r.read("PWR_VOSSR") == 0x8000);
  CHECK(r.level() == 3);
  r.run_for(9 * us);
  CHECK(r.read("PWR_VOSSR") == 0xC000);
  r.run_for(21 * us);
  CHECK(r.read("PWR_VOSSR") == 0xE008);
  CHECK(r.level() == 0);
  r.f.target->restore(at_write);
  CHECK(r.read("PWR_VOSSR") == 0x4000);
  r.run_for(40 * us);
  CHECK(r.read("PWR_VOSSR") == 0xE008);
}

TEST_CASE("RM0481 10.11.7: PWR_BDCR survives every reset but a power-on one")
{
  using popcorn::reset_kind;
  for(auto const [tzen, kind] : std::views::cartesian_product(
          std::array{false, true},
          std::array{
              reset_kind::pin, reset_kind::software, reset_kind::watchdog,
              reset_kind::window_watchdog, reset_kind::low_power, reset_kind::option_load,
              reset_kind::peripheral, reset_kind::brown_out, reset_kind::power_on}))
  {
    CAPTURE(tzen);
    CAPTURE(static_cast<int>(kind));
    rig r{
        "h573_pwr_ll", tzen ? popcorn::testing::h573_secure_rig::options({.tzen = true})
                            : popcorn::testing::stm32_options()};
    r.write("PWR_BDCR", 0x303);
    r.write("PWR_WUCR", 0x1);
    if(kind == reset_kind::peripheral)
    {
      r.f.target->model("PWR")->reset(r.f.target->peripheral("PWR"), kind);
    }
    else
    {
      r.f.target->reset(kind);
    }
    auto const kept = kind == reset_kind::power_on ? 0u : 1u;
    CHECK(r.get("PWR_BDCR", "BREN") == kept);
    CHECK(r.get("PWR_BDCR", "MONEN") == kept);
    CHECK(r.get("PWR_BDCR", "VBE") == kept);
    CHECK(r.get("PWR_BDCR", "VBRS") == kept);
    if(kind != reset_kind::peripheral)
    {
      CHECK(r.get("PWR_WUCR", "WUPEN1") == 0);
    }
  }
}

namespace
{
constexpr auto backup_functions = std::to_array<std::pair<std::string_view, std::uint32_t>>({
    {"BREN",  1u << 0},
    {"MONEN", 1u << 1},
    {"VBE",   1u << 8},
});
}

TEST_CASE("RM0481 10.11.7: a firmware enable of a backup function warns once per field")
{
  for(auto const& [name, bit] : backup_functions)
  {
    CAPTURE(name);
    rig                           r;
    std::vector<core::log_record> logged;
    auto const                    sink =
        r.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    auto&      regs   = r.f.target->peripheral("PWR");
    auto const offset = r.at("PWR_BDCR") - r.f.target->peripheral("PWR").description().base;
    auto const cpu    = [&](std::uint32_t value) {
      REQUIRE(r.f.target->model("PWR")
                  ->on_write(regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}})
                  .has_value());
    };
    r.write("PWR_BDCR", bit);
    r.write("PWR_BDCR", 0);
    CHECK(logged.empty());
    cpu(bit);
    CHECK(r.get("PWR_BDCR", name) == 1);
    cpu(0);
    cpu(bit);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].kind == core::log_kind::model_warning);
    CHECK(logged[0].reg == "PWR_BDCR");
    CHECK(logged[0].message.starts_with(name));
  }
}

TEST_CASE("RM0481 10.11.7: a strict backup function enable raises and keeps the word")
{
  for(auto const& [name, bit] : backup_functions)
  {
    CAPTURE(name);
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = true;
    rig        r{"h573_pwr_ll", std::move(options)};
    auto&      regs   = r.f.target->peripheral("PWR");
    auto const offset = r.at("PWR_BDCR") - regs.description().base;
    CHECK_THROWS_AS(
        static_cast<void>(r.f.target->model("PWR")->on_write(
            regs, offset, 4, 0, bit, {.pc = 0, .now = icount{0}})),
        popcorn::model_error);
    CHECK(r.get("PWR_BDCR", name) == 0);
    r.write("PWR_BDCR", bit);
    CHECK(r.get("PWR_BDCR", name) == 1);
    auto const seen = r.f.target->mem().read(r.at("PWR_BDCR"), 4);
    REQUIRE(seen.has_value());
    CHECK((*seen & bit) == bit);
  }
}

TEST_CASE("RM0481 10.11.7: RCC_BDCR.VSWRST clears every field of PWR_BDCR")
{
  rig        r;
  auto&      rcc   = r.f.target->peripheral("RCC");
  auto const found = popcorn::find_register(rcc.description(), std::string_view{"RCC_BDCR"});
  auto const vswrst =
      std::ranges::find(found->fields, std::string_view{"VSWRST"}, &popcorn::field_desc::name);
  auto const at = rcc.description().base + found->offset;
  r.write("PWR_BDCR", 0x303);
  REQUIRE(r.f.target->mem().write(at, 4, 1u << vswrst->lsb).has_value());
  for(auto const name : {"BREN", "MONEN", "VBE", "VBRS"})
  {
    CAPTURE(name);
    CHECK(r.get("PWR_BDCR", name) == 0);
  }
  REQUIRE(r.f.target->mem().write(at, 4, 0).has_value());
  r.write("PWR_BDCR", 0x303);
  CHECK(r.get("PWR_BDCR", "BREN") == 1);
}

TEST_CASE("RM0481 10.11.11: PWR_SCCR keeps BYPASS and its write-once lock over a system reset")
{
  using popcorn::reset_kind;
  struct row
  {
    reset_kind kind;
    bool       kept;
  };
  constexpr std::array rows{
      row{reset_kind::pin,             true },
      row{reset_kind::software,        true },
      row{reset_kind::watchdog,        true },
      row{reset_kind::window_watchdog, true },
      row{reset_kind::low_power,       true },
      row{reset_kind::option_load,     true },
      row{reset_kind::brown_out,       false},
      row{reset_kind::power_on,        false},
  };
  for(auto const [tzen, one] : std::views::cartesian_product(std::array{false, true}, rows))
  {
    CAPTURE(tzen);
    CAPTURE(static_cast<int>(one.kind));
    rig r{
        "h573_pwr_ll", tzen ? popcorn::testing::h573_secure_rig::options({.tzen = true})
                            : popcorn::testing::stm32_options()};
    r.write("PWR_SCCR", 1);
    REQUIRE(r.get("PWR_SCCR", "BYPASS") == 1);
    r.f.target->reset(one.kind);
    CHECK(r.get("PWR_SCCR", "BYPASS") == (one.kept ? 1u : 0u));
    if(tzen)
    {
      continue;
    }
    r.write("PWR_SCCR", one.kept ? 0 : 1);
    CHECK(r.get("PWR_SCCR", "BYPASS") == 1);
  }
}

TEST_CASE("RM0481 10.11.1: the first PWR_PMCR write logs its LPMS mode, 1 is standby")
{
  rig                           r;
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  r.write("PWR_PMCR", 1);
  r.write("PWR_PMCR", 0);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].peripheral == "PWR");
  CHECK(logged[0].reg == "PWR_PMCR");
  CHECK(logged[0].message.contains("standby"));
}

TEST_CASE("RM0481 10.11.1: LPMS 0 is stop0 and SVOS is storage")
{
  rig                           r;
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  r.write("PWR_PMCR", 3u << 2);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("stop0"));
  CHECK(r.get("PMCR", "SVOS") == 3);
}

TEST_CASE("the H5 has no EPOD booster: a PMCR.BOOSTE write is stored and starts no delay")
{
  rig        r;
  auto const idle = r.pending();
  r.write("PWR_PMCR", 1u << 12);
  CHECK(r.get("PWR_PMCR", "BOOSTE") == 1);
  CHECK(r.pending() == idle);
  r.run_for(100 * us);
  CHECK(r.get("PWR_PMCR", "BOOSTE") == 1);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
}

TEST_CASE("RM0481 10.9: PWR_SECCFGR is RAZ/WI with TrustZone off")
{
  rig        r;
  auto&      regs   = r.f.target->peripheral("PWR");
  auto const seccfg = popcorn::find_register(regs.description(), std::string_view{"PWR_SECCFGR"});
  REQUIRE(seccfg != nullptr);
  auto const stored = regs.peek(seccfg->offset);
  CHECK(r.read("PWR_SECCFGR") == 0);
  r.write("PWR_SECCFGR", 0xffff'ffff);
  CHECK(r.read("PWR_SECCFGR") == 0);
  CHECK(regs.peek(seccfg->offset) == stored);
}

TEST_CASE("coverage classifies PWR_VOSSR as served and USBSCR as unimplemented")
{
  rig r;
  r.f.target->enable_coverage({}, true);
  constexpr popcorn::address stub = 0x2000'0200;
  REQUIRE(r.f.target->mem().write(stub, 4, 0xE7FE'6802).has_value());
  for(auto const name : {"PWR_VOSSR", "USBSCR"})
  {
    r.f.set("r0", r.at(name));
    r.f.set("pc", stub | 1u);
    r.run_for(us);
  }
  auto const found = r.f.target->coverage().unmodelled;
  REQUIRE(found.size() == 1);
  CHECK(found.front().peripheral == "PWR");
  CHECK(found.front().reg == "USBSCR");
  CHECK(found.front().reason == popcorn::core::unmodelled_reason::unimplemented);
}

TEST_CASE(
    "RM0481 10.9 Table 114: a PWR_SECCFGR group bit makes its registers WI to Non-secure writes, "
    "Non-secure reads see them, Table 114 against 10.9.1 RAZ")
{
  struct row
  {
    std::string_view bit;
    std::string_view reg;
    std::string_view field;
  };
  for(auto const& one : std::initializer_list<row>{
          {"LPMSEC",  "PWR_PMCR",   "SVOS"    },
          {"VUSBSEC", "PWR_USBSCR", "USB33DEN"},
          {"VBSEC",   "PWR_BDCR",   "BREN"    },
          {"VBSEC",   "PWR_DBPCR",  "DBP"     },
          {"RETSEC",  "PWR_IORETR", "IORETEN" },
          {"SCMSEC",  "PWR_SCCR",   "BYPASS"  },
          {"SCMSEC",  "PWR_VMCR",   "PVDE"    },
  })
  {
    std::string const name{one.reg};
    CAPTURE(name);
    popcorn::testing::h573_secure_rig const r;
    auto const                              value = r.bit("PWR", one.reg, one.field);
    r.write("PWR", "PWR_SECCFGR", r.bit("PWR", "PWR_SECCFGR", one.bit), true);
    auto const stored = r.peek("PWR", one.reg);
    r.write("PWR", one.reg, stored ^ value, false);
    CHECK(r.peek("PWR", one.reg) == stored);
    CHECK(r.read("PWR", one.reg, false) == stored);
    r.write("PWR", one.reg, stored ^ value, true);
    CHECK(r.peek("PWR", one.reg) == (stored ^ value));
    CHECK(r.read("PWR", one.reg, false) == (stored ^ value));
  }
}

TEST_CASE(
    "RM0481 10.9 Table 114: WUP3SEC makes the pin 3 fields of PWR_WUCR RAZ/WI to Non-secure "
    "accesses, pin 2 stays open")
{
  popcorn::testing::h573_secure_rig const r;
  auto const                              pin3 = r.bit("PWR", "PWR_WUCR", "WUPEN3")
                                               | r.bit("PWR", "PWR_WUCR", "WUPP3")
                                               | r.bit("PWR", "PWR_WUCR", "WUPPUPD3");
  auto const                              pin2 = r.bit("PWR", "PWR_WUCR", "WUPEN2")
                                               | r.bit("PWR", "PWR_WUCR", "WUPP2")
                                               | r.bit("PWR", "PWR_WUCR", "WUPPUPD2");
  r.write("PWR", "PWR_WUCR", pin3, true);
  r.write("PWR", "PWR_SECCFGR", r.bit("PWR", "PWR_SECCFGR", "WUP3SEC"), true);
  CHECK(r.read("PWR", "PWR_WUCR", false) == 0);
  CHECK(r.read("PWR", "PWR_WUCR", true) == pin3);
  r.write("PWR", "PWR_WUCR", pin2, false);
  CHECK(r.peek("PWR", "PWR_WUCR") == (pin3 | pin2));
  CHECK(r.read("PWR", "PWR_WUCR", false) == pin2);
}

TEST_CASE(
    "RM0481 10.9 Table 114: SYSCLKSEC makes PWR_VOSCR.VOS RAZ/WI and the UCPD1 SEC bit "
    "PWR_UCPDR RAZ/WI to Non-secure accesses")
{
  popcorn::testing::h573_secure_rig const r;
  auto const                              vos = r.bit("PWR", "PWR_VOSCR", "VOS");
  r.write("PWR", "PWR_VOSCR", vos & 0x10u, true);
  r.write("RCC", "RCC_SECCFGR", r.bit("RCC", "RCC_SECCFGR", "SYSCLKSEC"), true);
  CHECK(r.read("PWR", "PWR_VOSCR", false) == 0);
  r.write("PWR", "PWR_VOSCR", 0, false);
  CHECK((r.peek("PWR", "PWR_VOSCR") & vos) == (vos & 0x10u));
  auto const ucpd = r.bit("PWR", "PWR_UCPDR", "UCPD_DBDIS");
  r.write("PWR", "PWR_UCPDR", ucpd, true);
  CHECK(r.read("PWR", "PWR_UCPDR", false) == ucpd);
  r.write("GTZC1_TZSC", "SECCFGR2", r.bit("GTZC1_TZSC", "SECCFGR2", "UCPDSEC"), true);
  CHECK(r.read("PWR", "PWR_UCPDR", false) == 0);
  r.write("PWR", "PWR_UCPDR", 0, false);
  CHECK(r.peek("PWR", "PWR_UCPDR") == ucpd);
}

TEST_CASE("RM0481 10.9: a Non-secure PWR_SECCFGR write is WI, a Non-secure read sees it")
{
  popcorn::testing::h573_secure_rig const r;
  auto const                              lpm = r.bit("PWR", "PWR_SECCFGR", "LPMSEC");
  r.write("PWR", "PWR_SECCFGR", lpm, true);
  r.write("PWR", "PWR_SECCFGR", 0, false);
  CHECK(r.peek("PWR", "PWR_SECCFGR") == lpm);
  CHECK(r.read("PWR", "PWR_SECCFGR", false) == lpm);
}

TEST_CASE("RM0481: PWR_PRIVCFGR needs a privileged access and its SPRIV bit a Secure one")
{
  popcorn::testing::h573_secure_rig const r;
  using popcorn::bus_attrs;
  using popcorn::initiator;
  constexpr bus_attrs secure_priv{.by = initiator::cpu, .secure = true, .privileged = true};
  constexpr bus_attrs nonsecure_priv{.by = initiator::cpu, .secure = false, .privileged = true};
  constexpr bus_attrs nonsecure_unpriv{.by = initiator::cpu, .secure = false, .privileged = false};
  r.write_as("PWR", "PWR_PRIVCFGR", 0x3, nonsecure_unpriv);
  CHECK(r.peek("PWR", "PWR_PRIVCFGR") == 0);
  r.write_as("PWR", "PWR_PRIVCFGR", 0x3, nonsecure_priv);
  CHECK(r.peek("PWR", "PWR_PRIVCFGR") == 0x2);
  r.write_as("PWR", "PWR_PRIVCFGR", 0x3, secure_priv);
  CHECK(r.peek("PWR", "PWR_PRIVCFGR") == 0x3);
}

TEST_CASE("RM0481: PWR_SECCFGR is privileged with SPRIV, a Non-secure write raises PWRF")
{
  popcorn::testing::h573_secure_rig const r;
  using popcorn::bus_attrs;
  using popcorn::initiator;
  constexpr bus_attrs secure_priv{.by = initiator::cpu, .secure = true, .privileged = true};
  constexpr bus_attrs secure_unpriv{.by = initiator::cpu, .secure = true, .privileged = false};
  constexpr bus_attrs nonsecure_priv{.by = initiator::cpu, .secure = false, .privileged = true};
  auto const          bits = std::uint32_t{1};
  CHECK(r.read_as("PWR", "PWR_SECCFGR", nonsecure_priv) == 0);
  CHECK_FALSE(r.illegal_flag("PWRF"));
  r.write_as("PWR", "PWR_SECCFGR", bits, secure_unpriv);
  CHECK(r.peek("PWR", "PWR_SECCFGR") == bits);
  r.write_as("PWR", "PWR_SECCFGR", 0, secure_unpriv);
  r.write_as("PWR", "PWR_PRIVCFGR", 0x1, secure_priv);
  r.write_as("PWR", "PWR_SECCFGR", bits, secure_unpriv);
  CHECK(r.peek("PWR", "PWR_SECCFGR") == 0);
  CHECK_FALSE(r.illegal_flag("PWRF"));
  r.write_as("PWR", "PWR_SECCFGR", bits, nonsecure_priv);
  CHECK(r.peek("PWR", "PWR_SECCFGR") == 0);
  CHECK(r.illegal_flag("PWRF"));
}

TEST_CASE("an H573 description without PWR_VOSCR.VOS throws config_error at TZEN=0 and 1")
{
  auto desc = h573();
  auto pwr =
      std::ranges::find(desc.peripherals, std::string_view{"PWR"}, &popcorn::peripheral_desc::name);
  REQUIRE(pwr != desc.peripherals.end());
  auto vos =
      std::ranges::find(pwr->registers, std::string_view{"VOSCR"}, &popcorn::register_desc::name);
  REQUIRE(vos != pwr->registers.end());
  REQUIRE(
      std::erase_if(vos->fields, [](popcorn::field_desc const& f) { return f.name == "VOS"; })
      == 1);
  stm32::register_builtin_models();
  CHECK_THROWS_AS(
      core::machine(desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()),
      popcorn::config_error);
  CHECK_THROWS_AS(
      core::machine(
          desc, popcorn::arch::armv8m::factory(),
          popcorn::testing::h573_secure_rig::options({.tzen = true})),
      popcorn::config_error);
}
