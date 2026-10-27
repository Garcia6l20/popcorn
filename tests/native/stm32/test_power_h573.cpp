#include "stm32_fixture.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
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
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;

constexpr std::uint32_t loop_at = 0x2000'0100;
constexpr std::uint64_t us      = 1'000'000;

struct rig
{
  stm32_fixture f;

  explicit rig(std::string_view firmware = "h573_pwr_ll")
      : f{h573(), firmware, popcorn::testing::stm32_options()}
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

TEST_CASE("a VOS write clears both ready bits at once and the delay sets them with ACTVOS")
{
  rig        r;
  auto const idle = r.pending();
  r.write("PWR_VOSCR", 3u << 4);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 0);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 0);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 0);
  CHECK(r.level() == 3);
  CHECK(r.pending() == idle + 1);
  r.run_for(49 * us);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 0);
  CHECK(r.level() == 3);
  r.run_for(2 * us);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 3);
  CHECK(r.level() == 0);
}

TEST_CASE("RM0481 10.11.6: a lower VOS keeps VOSRDY, ACTVOSRDY clears until the delay")
{
  rig r;
  r.write("PWR_VOSCR", 3u << 4);
  r.run_for(60 * us);
  REQUIRE(r.get("PWR_VOSSR", "VOSRDY") == 1);
  REQUIRE(r.get("PWR_VOSSR", "ACTVOS") == 3);
  auto const idle = r.pending();
  r.write("PWR_VOSCR", 1u << 4);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOSRDY") == 0);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 3);
  CHECK(r.pending() == idle + 1);
  r.run_for(60 * us);
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

TEST_CASE("a second VOS write supersedes the first: the stale due time changes nothing")
{
  rig r;
  r.write("PWR_VOSCR", 3u << 4);
  r.run_for(30 * us);
  r.write("PWR_VOSCR", 2u << 4);
  r.run_for(25 * us);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 0);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 0);
  r.run_for(30 * us);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 2);
  CHECK(r.level() == 1);
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

TEST_CASE("a snapshot taken while VOSRDY is pending restores to the same run")
{
  rig r;
  r.write("PWR_VOSCR", 3u << 4);
  auto const saved = r.f.target->snapshot();
  r.run_for(60 * us);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 3);
  r.f.target->restore(saved);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 0);
  r.run_for(60 * us);
  CHECK(r.get("PWR_VOSSR", "VOSRDY") == 1);
  CHECK(r.get("PWR_VOSSR", "ACTVOS") == 3);
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
