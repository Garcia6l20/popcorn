#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152re;
using popcorn::testing::set_debug_freeze;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware  = std::string_view{"l152xe_l1_iwdg_ll"};
constexpr std::uint64_t ms        = 1'000'000'000;
constexpr std::uint64_t lsi_hz    = 37'000;
constexpr std::uint64_t ps_per_s  = 1'000'000'000'000;
constexpr std::uint64_t lsi_cycle = ps_per_s / lsi_hz + 1;

constexpr auto lsi_ticks(std::uint64_t cycles) -> std::uint64_t
{
  return (cycles * ps_per_s + lsi_hz - 1) / lsi_hz;
}

auto entered(
    std::string_view      entry,
    core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{l152re(), firmware, std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{10'000'000}, .until = sym(firmware, "main")}).reason
      == stop_reason::address);
  f.set("pc", sym(firmware, entry));
  return f;
}

struct calls
{
  std::vector<std::uint64_t> at;
  core::intercept_handle     hook;

  calls(stm32_fixture& f, std::string_view function)
      : hook{
            f.target->add_intercept(sym(firmware, function), [this](core::intercept_call const& c) {
              at.push_back(c.target().virtual_clock().time().ps);
              return core::intercept_result::proceed();
            })}
  {}
};

struct bus
{
  stm32_fixture& f;

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(at(peripheral, reg), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(peripheral, reg), 4, value).has_value());
  }
};

auto run_for(stm32_fixture const& f, std::uint64_t ps) -> popcorn::stop
{
  auto const until = vtime{f.target->virtual_clock().time().ps + ps};
  for(;;)
  {
    auto const slice = f.run(
        {.max_instructions = icount{1'000'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = until});
    if(slice.stopped)
    {
      return *slice.stopped;
    }
  }
}

void lsi_on(stm32_fixture& f)
{
  bus const b{f};
  b.write("RCC", "CSR", b.read("RCC", "CSR") | 1u);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  REQUIRE(f.field("RCC", "CSR", "LSIRDY") == 1);
}

auto bite_time(stm32_fixture const& f) -> std::uint64_t
{
  auto const stopped = run_for(f, 60'000 * ms);
  REQUIRE(stopped.reason == stop_reason::reset_request);
  return f.target->virtual_clock().time().ps;
}

auto with_iwdg(part_description desc, auto&& change) -> part_description
{
  auto found = std::ranges::find(
      desc.peripherals, std::string_view{"IWDG"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  change(found->registers);
  return desc;
}

auto status_of(std::vector<popcorn::register_desc>& registers) -> popcorn::register_desc&
{
  auto found = std::ranges::find(registers, std::string_view{"SR"}, &popcorn::register_desc::name);
  REQUIRE(found != registers.end());
  return *found;
}
}

TEST_CASE(
    "a refreshed L1 IWDG keeps running, the last refresh plus RL ticks of /32 requests a watchdog "
    "reset, RL and not the RL + 1 of RM0038 Table 92 by the refresh deviation of limits.md")
{
  std::uint64_t first = 0;
  for(int run = 0; run < 2; ++run)
  {
    auto       f = entered("main");
    calls      refreshes{f, "refresh"};
    auto const stopped = f.run_to_stop();
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::reset_request);
    CHECK(stopped.detail.find("watchdog") != std::string::npos);
    REQUIRE(refreshes.at.size() == 51);
    auto const now      = f.target->virtual_clock().time().ps;
    auto const expected = refreshes.at.back() + lsi_ticks(1'000 * 32);
    CHECK(now >= expected);
    CHECK(now < expected + lsi_cycle);
    CHECK(f.field("RCC", "CSR", "LSIRDY") == 1);
    if(run == 0)
    {
      first = now;
    }
    else
    {
      CHECK(now == first);
    }
  }
}

TEST_CASE("under reset_and_continue the L1 firmware restarts and reads RCC_CSR.IWDGRSTF")
{
  auto options             = popcorn::testing::stm32_options();
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  auto       f             = entered("main", std::move(options));
  auto const stopped       = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{1});
  CHECK(f.field("RCC", "CSR", "IWDGRSTF") == 1);
}

TEST_CASE("LL updates: PVU, then PVU and RVU set at the writes, both clear, PR and RL read back")
{
  auto       f       = entered("updates");
  auto const stopped = run_for(f, 10 * ms);
  REQUIRE(stopped.reason == stop_reason::time);
  CHECK(f.log() == std::vector<std::uint32_t>{1, 3, 0, 6, 500});
}

TEST_CASE("RM0038 21.4.4: PVU and RVU clear 5 LSI cycles after the write")
{
  auto f = entered("idle");
  bus  b{f};
  lsi_on(f);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "PR", 3);
  b.write("IWDG", "RLR", 100);
  CHECK(b.read("IWDG", "SR") == 3);
  REQUIRE(run_for(f, lsi_ticks(5) - lsi_cycle / 2).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 3);
  REQUIRE(run_for(f, lsi_cycle / 2).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 0);
  CHECK(b.read("IWDG", "PR") == 3);
  CHECK(b.read("IWDG", "RLR") == 100);
}

TEST_CASE("RM0038 21.3.2: PR and RLR ignore writes until the 0x5555 key")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  b.write("IWDG", "PR", 3);
  b.write("IWDG", "RLR", 100);
  CHECK(b.read("IWDG", "PR") == 0);
  CHECK(b.read("IWDG", "RLR") == 0xFFF);
  CHECK(b.read("IWDG", "SR") == 0);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "KR", 0xAAAA);
  b.write("IWDG", "PR", 3);
  CHECK(b.read("IWDG", "PR") == 0);
  CHECK(b.read("IWDG", "KR") == 0);
}

TEST_CASE("RM0038 21.4.2: PR 110 and 111 both divide by 256, 101 by 128")
{
  for(auto const& [pr, divider] : {
          std::pair{5u, 128u},
          std::pair{6u, 256u},
          std::pair{7u, 256u}
  })
  {
    CAPTURE(pr);
    auto f = entered("idle");
    bus  b{f};
    lsi_on(f);
    b.write("IWDG", "KR", 0x5555);
    b.write("IWDG", "PR", pr);
    REQUIRE(run_for(f, ms).reason == stop_reason::time);
    REQUIRE(b.read("IWDG", "SR") == 0);
    b.write("IWDG", "KR", 0xCCCC);
    auto const started  = f.target->virtual_clock().time().ps;
    auto const took     = bite_time(f) - started;
    auto const expected = lsi_ticks(std::uint64_t{0xFFF} * divider);
    CHECK(took >= expected);
    CHECK(took < expected + lsi_ticks(divider));
  }
}

TEST_CASE(
    "with no window, a refresh at once after the start bites RL ticks later, RL and not the RL + 1 "
    "of RM0038 Table 92 by the refresh deviation of limits.md")
{
  auto f = entered("idle");
  bus  b{f};
  lsi_on(f);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "RLR", 100);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  REQUIRE(b.read("IWDG", "SR") == 0);
  b.write("IWDG", "KR", 0xCCCC);
  b.write("IWDG", "KR", 0xAAAA);
  auto const refreshed = f.target->virtual_clock().time().ps;
  auto const took      = bite_time(f) - refreshed;
  auto const expected  = lsi_ticks(100 * 4);
  CHECK(took >= expected);
  CHECK(took < expected + lsi_ticks(4));
}

TEST_CASE("RM0038 6.2.12: starting the IWDG forces the LSI on, an LSION 1 then 0 does not stop it")
{
  auto f = entered("idle");
  bus  b{f};
  CHECK(f.field("RCC", "CSR", "LSION") == 0);
  b.write("IWDG", "KR", 0xCCCC);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  CHECK(f.field("RCC", "CSR", "LSIRDY") == 1);
  CHECK(f.field("RCC", "CSR", "LSION") == 0);
  b.write("RCC", "CSR", b.read("RCC", "CSR") | 1u);
  b.write("RCC", "CSR", b.read("RCC", "CSR") & ~1u);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  CHECK(f.field("RCC", "CSR", "LSION") == 0);
  CHECK(f.field("RCC", "CSR", "LSIRDY") == 1);
}

TEST_CASE("RM0038 21.3.3: with DBG_IWDG_STOP the watchdog counts running time only")
{
  auto const started = [] {
    auto f = entered("idle");
    bus  b{f};
    set_debug_freeze(f, "DBG_IWDG_STOP", true);
    b.write("IWDG", "KR", 0xCCCC);
    REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
    return f;
  };
  auto       twin      = started();
  auto const reference = bite_time(twin);

  auto f = started();
  f.target->hold_core(true);
  REQUIRE(run_for(f, 1'000 * ms).reason == stop_reason::time);
  f.target->hold_core(false);
  CHECK(bite_time(f) == reference + 1'000 * ms);

  auto running = started();
  set_debug_freeze(running, "DBG_IWDG_STOP", false);
  running.target->hold_core(true);
  CHECK(bite_time(running) == reference);
}

TEST_CASE("a snapshot between refresh and expiry restores to the same L1 watchdog stop")
{
  auto f = entered("main");
  REQUIRE(run_for(f, 300 * ms).reason == stop_reason::time);
  auto const saved   = f.target->snapshot();
  auto const stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::reset_request);
  auto const at = f.target->virtual_clock().time();

  auto other = entered("main");
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  REQUIRE(other.run_to_stop().reason == stop_reason::reset_request);
  CHECK(other.target->virtual_clock().time() == at);
}

TEST_CASE("checklist 39: the L1 IWDG row refuses WINR, EWCR, WVU, EWU and EWIF, needs PVU and RVU")
{
  CHECK_NOTHROW(stm32_fixture(l152re(), firmware));
  auto const add_register = [](std::string_view name, std::uint32_t offset) {
    return [=](std::vector<popcorn::register_desc>& registers) {
      auto& added  = registers.emplace_back();
      added.name   = name;
      added.offset = offset;
      added.width  = 32;
    };
  };
  auto const add_field = [](std::string_view name, std::uint8_t lsb) {
    return [=](std::vector<popcorn::register_desc>& registers) {
      status_of(registers).fields.push_back({.name = std::string{name}, .lsb = lsb, .width = 1});
    };
  };
  auto const drop_field = [](std::string_view name) {
    return [=](std::vector<popcorn::register_desc>& registers) {
      std::erase_if(status_of(registers).fields, [&](popcorn::field_desc const& one) {
        return one.name == name;
      });
    };
  };
  CHECK_THROWS_AS(
      stm32_fixture(with_iwdg(l152re(), add_register("WINR", 0x10)), firmware),
      popcorn::config_error);
  CHECK_THROWS_AS(
      stm32_fixture(with_iwdg(l152re(), add_register("EWCR", 0x14)), firmware),
      popcorn::config_error);
  for(auto const& [name, lsb] : {
          std::pair{std::string_view{"WVU"},  std::uint8_t{2} },
          std::pair{std::string_view{"EWU"},  std::uint8_t{3} },
          std::pair{std::string_view{"EWIF"}, std::uint8_t{14}}
  })
  {
    CAPTURE(name);
    CHECK_THROWS_AS(
        stm32_fixture(with_iwdg(l152re(), add_field(name, lsb)), firmware), popcorn::config_error);
  }
  for(auto const name : {std::string_view{"PVU"}, std::string_view{"RVU"}})
  {
    CAPTURE(name);
    CHECK_THROWS_AS(
        stm32_fixture(with_iwdg(l152re(), drop_field(name)), firmware), popcorn::config_error);
  }
}
