#include "stm32_fixture.hpp"

#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t ms        = 1'000'000'000;
constexpr std::uint64_t us        = 1'000'000;
constexpr std::uint64_t lsi_cycle = 1'000'000'000'000 / 32'000;

auto entered(
    std::string_view      entry,
    core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{u575(), "iwdg_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("iwdg_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("iwdg_ll", entry));
  return f;
}

struct calls
{
  std::vector<std::uint64_t> at;
  core::intercept_handle     hook;

  calls(stm32_fixture& f, std::string_view function)
      : hook{f.target->add_intercept(
            sym("iwdg_ll", function),
            [this](core::intercept_call const& c) {
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
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
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
  return f.run_to_stop(
      {.max_instructions = icount{1'000'000'000},
       .until_time       = vtime{f.target->virtual_clock().time().ps + ps}});
}
}

TEST_CASE("a refreshed IWDG keeps running, the last refresh plus 1 s requests a watchdog reset")
{
  std::uint64_t first = 0;
  for(int run = 0; run < 2; ++run)
  {
    auto       f = entered("main");
    calls      refreshes{f, "refresh"};
    bus        b{f};
    auto const stopped = f.run_to_stop();
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::reset_request);
    CHECK(stopped.detail.find("watchdog") != std::string::npos);
    REQUIRE(refreshes.at.size() == 51);
    CHECK(refreshes.at.back() - refreshes.at.front() > 40 * ms);
    auto const now      = f.target->virtual_clock().time().ps;
    auto const expected = refreshes.at.back() + 1'000 * ms;
    CHECK(now >= expected);
    CHECK(now < expected + lsi_cycle);
    CHECK(((b.read("RCC", "RCC_BDCR") >> 26) & 0b11u) == 0b10u);
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

TEST_CASE("under reset_and_continue the firmware restarts and reads IWDGRSTF")
{
  auto options             = popcorn::testing::stm32_options();
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  auto       f             = entered("main", std::move(options));
  auto const stopped       = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{1});
  CHECK(f.field("RCC", "RCC_CSR", "IWDGRSTF") == 1);
}

TEST_CASE("a refresh with the counter above the window requests a watchdog reset at once")
{
  auto       f = entered("window");
  calls      refreshes{f, "refresh"};
  auto const stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::reset_request);
  REQUIRE(refreshes.at.size() == 2);
  CHECK(f.target->virtual_clock().time().ps - refreshes.at.back() < ms / 100);
}

TEST_CASE("the early wake-up interrupt fires when the counter reaches EWIT - 1")
{
  auto       f = entered("early_wakeup");
  calls      refreshes{f, "refresh"};
  calls      irq{f, "IWDG_IRQHandler"};
  auto const stopped = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0});
  REQUIRE(irq.at.size() == 1);
  auto const after = irq.at.front() - refreshes.at.back();
  CHECK(after >= 101 * ms);
  CHECK(after < 101 * ms + lsi_cycle);
}

TEST_CASE("PR, RLR and WINR ignore writes until the 0x5555 key, then set their update flag")
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
  b.write("IWDG", "PR", 3);
  CHECK(b.read("IWDG", "PR") == 3);
  CHECK(b.read("IWDG", "SR") == 1);
  b.write("IWDG", "PR", 5);
  CHECK(b.read("IWDG", "PR") == 3);
  b.write("IWDG", "KR", 0);
  b.write("IWDG", "RLR", 100);
  CHECK(b.read("IWDG", "RLR") == 0xFFF);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 0);
  CHECK(((b.read("RCC", "RCC_BDCR") >> 26) & 0b11u) == 0b10u);
}

TEST_CASE("the started IWDG counts 0xFFF ticks of the reset prescaler, then bites")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "RLR", 100);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0xCCCC);
  auto const started = f.target->virtual_clock().time().ps;
  auto const stopped = run_for(f, 2'000 * ms);
  REQUIRE(stopped.reason == stop_reason::reset_request);
  auto const took     = f.target->virtual_clock().time().ps - started;
  auto const expected = std::uint64_t{0xFFF} * 4 * lsi_cycle;
  CHECK(took >= expected);
  CHECK(took < expected + 200'000'000);
}

TEST_CASE("a snapshot between refresh and expiry restores to the same watchdog stop")
{
  auto f = entered("main");
  REQUIRE(run_for(f, 500 * ms).reason == stop_reason::time);
  auto const saved   = f.target->snapshot();
  auto const stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::reset_request);
  auto const at = f.target->virtual_clock().time();

  f.target->restore(saved);
  REQUIRE(f.run_to_stop().reason == stop_reason::reset_request);
  CHECK(f.target->virtual_clock().time() == at);

  auto other = entered("main");
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  REQUIRE(other.run_to_stop().reason == stop_reason::reset_request);
  CHECK(other.target->virtual_clock().time() == at);
}

TEST_CASE("the started IWDG keeps the LSI on until the next reset, whatever LSION says")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  CHECK(f.field("RCC", "RCC_BDCR", "LSIRDY") == 1);
  CHECK(f.field("RCC", "RCC_BDCR", "LSION") == 0);
  b.write("RCC", "RCC_BDCR", b.read("RCC", "RCC_BDCR") | (1u << 26));
  b.write("RCC", "RCC_BDCR", b.read("RCC", "RCC_BDCR") & ~(1u << 26));
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  CHECK(f.field("RCC", "RCC_BDCR", "LSIRDY") == 1);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(f.field("RCC", "RCC_BDCR", "LSIRDY") == 0);
  f.set("pc", sym("iwdg_ll", "idle"));
  b.write("RCC", "RCC_BDCR", b.read("RCC", "RCC_BDCR") | (1u << 26));
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  REQUIRE(f.field("RCC", "RCC_BDCR", "LSIRDY") == 1);
  b.write("IWDG", "KR", 0xAAAA);
  REQUIRE(run_for(f, 3'000 * ms).reason == stop_reason::time);
}

TEST_CASE("a core clock change keeps the watchdog deadline in virtual time")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  auto const started = f.target->virtual_clock().time().ps;
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  auto const icscr1 = b.read("RCC", "RCC_ICSCR1") & ~(0xFu << 28);
  b.write("RCC", "RCC_ICSCR1", icscr1 | (1u << 28) | (1u << 23));
  REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
  b.write("RCC", "RCC_ICSCR1", icscr1 | (4u << 28) | (1u << 23));
  auto const stopped = run_for(f, 2'000 * ms);
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::reset_request);
  auto const took     = f.target->virtual_clock().time().ps - started;
  auto const expected = 100 * us + std::uint64_t{0xFFF} * 4 * lsi_cycle;
  CHECK(took >= expected);
  CHECK(took < expected + lsi_cycle);
}

TEST_CASE("a prescaler change applies when PVU clears, the expiry follows the new divider")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  auto const started = f.target->virtual_clock().time().ps;
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "PR", 3);
  auto const cleared = f.target->virtual_clock().time().ps + 6 * lsi_cycle;
  auto const ticks   = (cleared - started - 100 * us) / (4 * lsi_cycle);
  auto const stopped = run_for(f, 5'000 * ms);
  REQUIRE(stopped.reason == stop_reason::reset_request);
  auto const expected = cleared + (0xFFF - ticks) * 32 * lsi_cycle;
  auto const now      = f.target->virtual_clock().time().ps;
  CAPTURE(now - started);
  CHECK(now + 4 * lsi_cycle >= expected);
  CHECK(now < expected + lsi_cycle);
}

TEST_CASE("a window change reloads the counter from RL when WVU clears")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "RLR", 100);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "WINR", 50);
  CHECK(f.field("IWDG", "SR", "WVU") == 1);
  auto const cleared = f.target->virtual_clock().time().ps + 6 * lsi_cycle;
  auto const stopped = run_for(f, 100 * ms);
  REQUIRE(stopped.reason == stop_reason::reset_request);
  auto const expected = cleared + 100 * 4 * lsi_cycle;
  auto const now      = f.target->virtual_clock().time().ps;
  CHECK(now >= expected);
  CHECK(now < expected + lsi_cycle);
}

TEST_CASE("a snapshot with PVU and WVU set restores the update deadlines exactly")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "PR", 1);
  b.write("IWDG", "WINR", 0x800);
  REQUIRE(b.read("IWDG", "SR") == 0b101);
  auto const saved = f.target->snapshot();
  REQUIRE(run_for(f, 3 * lsi_cycle).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 0b101);
  REQUIRE(run_for(f, 4 * lsi_cycle).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 0);
  REQUIRE(run_for(f, 2'000 * ms).reason == stop_reason::reset_request);
  auto const at = f.target->virtual_clock().time();

  f.target->restore(saved);
  CHECK(b.read("IWDG", "SR") == 0b101);
  REQUIRE(run_for(f, 2'000 * ms).reason == stop_reason::reset_request);
  CHECK(f.target->virtual_clock().time() == at);

  auto other = entered("idle");
  bus  ob{other};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(ob.read("IWDG", "SR") == 0b101);
  REQUIRE(run_for(other, 2'000 * ms).reason == stop_reason::reset_request);
  CHECK(other.target->virtual_clock().time() == at);
}

TEST_CASE("an update flag clears at its deadline across a core clock change")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "PR", 1);
  REQUIRE(run_for(f, 3 * lsi_cycle).reason == stop_reason::time);
  auto const icscr1 = b.read("RCC", "RCC_ICSCR1") & ~(0xFu << 28);
  b.write("RCC", "RCC_ICSCR1", icscr1 | (1u << 28) | (1u << 23));
  REQUIRE(run_for(f, 2 * lsi_cycle).reason == stop_reason::time);
  CHECK(f.field("IWDG", "SR", "PVU") == 1);
  REQUIRE(run_for(f, 2 * lsi_cycle).reason == stop_reason::time);
  CHECK(f.field("IWDG", "SR", "PVU") == 0);
}

TEST_CASE("an LSI rate change keeps the partial division sequence")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  auto const started = f.target->virtual_clock().time().ps;
  REQUIRE(run_for(f, 100 * us + 3202 * lsi_cycle + lsi_cycle / 2).reason == stop_reason::time);
  auto const changed = f.target->virtual_clock().time().ps;
  auto const cycles  = (changed - started - 100 * us) / lsi_cycle;
  REQUIRE(cycles == 3202);
  b.write("RCC", "RCC_BDCR", b.read("RCC", "RCC_BDCR") | (1u << 28));
  constexpr std::uint64_t slow     = 1'000'000'000'000 / 250;
  auto const              expected = changed + ((0xFFF - 800) * 4 - 2) * slow;
  auto const              stopped  = run_for(f, 100'000 * ms);
  REQUIRE(stopped.reason == stop_reason::reset_request);
  auto const now = f.target->virtual_clock().time().ps;
  CHECK(now >= expected);
  CHECK(now < expected + slow);
}
