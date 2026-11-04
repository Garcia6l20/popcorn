#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
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
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware  = std::string_view{"l476_l4_iwdg_ll"};
constexpr std::uint64_t ms        = 1'000'000'000;
constexpr std::uint64_t lsi_hz    = 32'000;
constexpr std::uint64_t ps_per_s  = 1'000'000'000'000;
constexpr std::uint64_t lsi_cycle = ps_per_s / lsi_hz + 1;
constexpr std::uint32_t iwdg_sw   = 1u << 16;

constexpr auto lsi_ticks(std::uint64_t cycles) -> std::uint64_t
{
  return (cycles * ps_per_s + lsi_hz - 1) / lsi_hz;
}

auto entered(
    std::string_view        entry,
    core::machine_options   options = popcorn::testing::stm32_options(),
    part_description const& desc    = l476rc()) -> stm32_fixture
{
  stm32_fixture f{desc, firmware, std::move(options)};
  if(entry != "main")
  {
    f.set("pc", sym(firmware, entry));
  }
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

void program_option(stm32_fixture& f, std::uint32_t optr)
{
  bus const b{f};
  b.write("FLASH", "KEYR", 0x4567'0123u);
  b.write("FLASH", "KEYR", 0xCDEF'89ABu);
  b.write("FLASH", "OPTKEYR", 0x0819'2A3Bu);
  b.write("FLASH", "OPTKEYR", 0x4C5D'6E7Fu);
  b.write("FLASH", "OPTR", optr);
  b.write("FLASH", "CR", b.read("FLASH", "CR") | (1u << 17));
  REQUIRE(f.field("FLASH", "CR", "OPTSTRT") == 0);
  f.target->reset(popcorn::reset_kind::option_load);
  f.set("pc", sym(firmware, "idle"));
}

auto with_unit(std::string_view unit, std::function<void(popcorn::peripheral_desc&)> const& change)
    -> part_description
{
  auto desc  = l476rc();
  auto found = std::ranges::find(desc.peripherals, unit, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  change(*found);
  return desc;
}

auto register_of(popcorn::peripheral_desc& unit, std::string_view name) -> popcorn::register_desc&
{
  auto found = std::ranges::find(unit.registers, name, &popcorn::register_desc::name);
  REQUIRE(found != unit.registers.end());
  return *found;
}
}

TEST_CASE("a refreshed L4 IWDG keeps running, the last refresh plus RL ticks of /32 bites")
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
}

TEST_CASE("under reset_and_continue the L4 firmware restarts and reads RCC_CSR.IWDGRSTF")
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

TEST_CASE("RM0351 36.4.4: LL sets RVU then WVU, both clear 5 LSI cycles later, WIN reads back")
{
  auto       f       = entered("windowed");
  auto const stopped = run_for(f, 10 * ms);
  REQUIRE(stopped.reason == stop_reason::time);
  CHECK(f.log() == std::vector<std::uint32_t>{3, 7, 0, 500});
  auto g = entered("idle");
  bus  b{g};
  lsi_on(g);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "WINR", 300);
  CHECK(b.read("IWDG", "SR") == 4);
  REQUIRE(run_for(g, lsi_ticks(5) - lsi_cycle / 2).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 4);
  REQUIRE(run_for(g, lsi_cycle / 2).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 0);
}

TEST_CASE("RM0351 36.4.2: PR 110 and 111 both divide by 256, 101 by 128")
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

TEST_CASE("RM0351 36.3.2: a refresh while the counter is above WIN bites at once")
{
  auto f = entered("idle");
  bus  b{f};
  lsi_on(f);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "WINR", 0x100);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0xCCCC);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0xAAAA);
  auto const stopped = run_for(f, ms);
  CAPTURE(stopped.detail);
  CHECK(stopped.reason == stop_reason::reset_request);
}

TEST_CASE("RM0351 36.3.3: with FLASH_OPTR.IWDG_SW 0 the IWDG starts at reset without a key")
{
  for(auto const hardware : {true, false})
  {
    CAPTURE(hardware);
    auto       f = entered("idle");
    bus        b{f};
    auto const optr = b.read("FLASH", "OPTR");
    REQUIRE((optr & iwdg_sw) != 0);
    program_option(f, hardware ? optr & ~iwdg_sw : optr);
    CHECK(f.field("FLASH", "OPTR", "IWDG_SW") == (hardware ? 0u : 1u));
    auto const started = f.target->virtual_clock().time().ps;
    auto const stopped = run_for(f, lsi_ticks(0xFFF * 4) + 10 * ms);
    if(hardware)
    {
      REQUIRE(stopped.reason == stop_reason::reset_request);
      auto const took = f.target->virtual_clock().time().ps - started;
      CHECK(took >= lsi_ticks(0xFFF * 4));
      CHECK(took < lsi_ticks(0xFFF * 4) + lsi_ticks(4) + lsi_cycle);
      CHECK(f.field("RCC", "CSR", "LSIRDY") == 1);
    }
    else
    {
      CHECK(stopped.reason == stop_reason::time);
    }
  }
}

TEST_CASE("checklist 39: the L4 IWDG row needs WINR, refuses EWCR, needs FLASH_OPTR.IWDG_SW")
{
  auto const no_window = with_unit("IWDG", [](popcorn::peripheral_desc& unit) {
    std::erase_if(unit.registers, [](auto const& r) { return r.name == "WINR"; });
  });
  CHECK_THROWS_WITH_AS(
      entered("idle", popcorn::testing::stm32_options(), no_window), doctest::Contains("WINR"),
      popcorn::config_error);
  auto const early = with_unit("IWDG", [](popcorn::peripheral_desc& unit) {
    auto added   = register_of(unit, "WINR");
    added.name   = "EWCR";
    added.offset = 0x14;
    unit.registers.push_back(added);
  });
  CHECK_THROWS_WITH_AS(
      entered("idle", popcorn::testing::stm32_options(), early), doctest::Contains("EWCR"),
      popcorn::config_error);
  auto const no_option = with_unit("FLASH", [](popcorn::peripheral_desc& unit) {
    auto& optr = register_of(unit, "OPTR");
    REQUIRE(std::erase_if(optr.fields, [](auto const& one) { return one.name == "IWDG_SW"; }) == 1);
  });
  CHECK_THROWS_WITH_AS(
      entered("idle", popcorn::testing::stm32_options(), no_option), doctest::Contains("IWDG_SW"),
      popcorn::config_error);
}
