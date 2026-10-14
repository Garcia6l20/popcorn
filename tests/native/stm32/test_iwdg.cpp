#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
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
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::debug_freeze_set;
using popcorn::testing::set_debug_freeze;
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

std::uint64_t slicing = 1'000'000'000;

auto run_for(stm32_fixture const& f, std::uint64_t ps) -> popcorn::stop
{
  auto const until = vtime{f.target->virtual_clock().time().ps + ps};
  for(;;)
  {
    auto const slice = f.run(
        {.max_instructions = icount{slicing},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = until});
    if(slice.stopped)
    {
      return *slice.stopped;
    }
  }
}

struct sliced
{
  explicit sliced(std::uint64_t instructions) : saved{std::exchange(slicing, instructions)}
  {}
  sliced(sliced const&)                    = delete;
  auto operator=(sliced const&) -> sliced& = delete;
  ~sliced()
  {
    slicing = saved;
  }

  std::uint64_t saved;
};
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

TEST_CASE("a snapshot with PVU, WVU and EWU set restores the update deadlines exactly")
{
  constexpr std::uint32_t updating = 0b1101;
  auto const              early_at = [](stm32_fixture const& f, bus const& b) {
    auto const stopped =
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .stop_when = [&] {
                         return (b.read("IWDG", "SR") & (1u << 14)) != 0;
                       }});
    REQUIRE(stopped.reason == stop_reason::user);
    return f.target->virtual_clock().time();
  };
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "PR", 1);
  b.write("IWDG", "WINR", 0x800);
  b.write("IWDG", "EWCR", (1u << 15) | 0xFF0);
  REQUIRE(b.read("IWDG", "SR") == updating);
  auto const saved = f.target->snapshot();
  REQUIRE(run_for(f, 3 * lsi_cycle).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == updating);
  REQUIRE(run_for(f, 4 * lsi_cycle).reason == stop_reason::time);
  CHECK(b.read("IWDG", "SR") == 0);
  auto const cleared = f.target->virtual_clock().time().ps;
  auto const early   = early_at(f, b);
  CHECK(early.ps - cleared > 3 * ms);
  CHECK(early.ps - cleared < 5 * ms);
  REQUIRE(run_for(f, 2'000 * ms).reason == stop_reason::reset_request);
  auto const at = f.target->virtual_clock().time();

  f.target->restore(saved);
  CHECK(b.read("IWDG", "SR") == updating);
  CHECK(early_at(f, b) == early);
  REQUIRE(run_for(f, 2'000 * ms).reason == stop_reason::reset_request);
  CHECK(f.target->virtual_clock().time() == at);

  auto other = entered("idle");
  bus  ob{other};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(ob.read("IWDG", "SR") == updating);
  CHECK(early_at(other, ob) == early);
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

namespace
{
constexpr std::uint32_t ewu  = 1u << 3;
constexpr std::uint32_t ewif = 1u << 14;
constexpr std::uint32_t ewie = 1u << 15;

struct early_rig
{
  stm32_fixture f = entered("idle");
  bus           b{f};

  early_rig()
  {
    b.write("IWDG", "KR", 0xCCCC);
    b.write("IWDG", "KR", 0x5555);
  }

  [[nodiscard]] auto status() const -> std::uint32_t
  {
    return b.read("IWDG", "SR");
  }

  [[nodiscard]] auto irq_pending() const -> bool
  {
    auto const word = f.target->mem().read(0xe000'e200u, 4);
    REQUIRE(word.has_value());
    return (*word >> 27 & 1u) != 0;
  }

  void clear_irq() const
  {
    REQUIRE(f.target->mem().write(0xe000'e280u, 4, 1u << 27).has_value());
  }

  template <typename Busy> void write_early(std::uint32_t value, Busy const& while_busy) const
  {
    b.write("IWDG", "EWCR", value);
    REQUIRE((status() & ewu) != 0);
    auto steps = 0;
    while((status() & ewu) != 0)
    {
      REQUIRE(steps < 100);
      while_busy();
      REQUIRE(run_for(f, 5 * us).reason == stop_reason::time);
      ++steps;
    }
    CHECK(steps * 5 * us >= 6 * lsi_cycle);
    REQUIRE(run_for(f, us).reason == stop_reason::time);
  }

  void write_early(std::uint32_t value) const
  {
    write_early(value, [] {});
  }
};
}

TEST_CASE("a new EWIE applies when EWU clears, not at the write")
{
  early_rig r;
  r.write_early(0xFFF);
  CHECK((r.status() & ewif) == 0);
  r.write_early(ewie | 0xFFF, [&] { REQUIRE((r.status() & ewif) == 0); });
  CHECK((r.status() & ewif) != 0);
  CHECK(r.irq_pending());
}

TEST_CASE("a new EWIT applies when EWU clears, not at the write")
{
  early_rig r;
  r.write_early(ewie | 2);
  CHECK((r.status() & ewif) == 0);
  r.write_early(ewie | 0xFFF, [&] { REQUIRE((r.status() & ewif) == 0); });
  CHECK((r.status() & ewif) != 0);
}

TEST_CASE("with EWIF set, the IWDG IRQ follows EWIE once EWU clears")
{
  early_rig r;
  r.write_early(ewie | 0xFFF);
  REQUIRE((r.status() & ewif) != 0);
  r.write_early(0xFFF);
  r.clear_irq();
  REQUIRE_FALSE(r.irq_pending());
  r.write_early(ewie | 0xFFF, [&] { REQUIRE_FALSE(r.irq_pending()); });
  CHECK(r.irq_pending());
}

TEST_CASE("KR reloads replace the expiry and early wakeup events instead of queueing more")
{
  auto f = entered("idle");
  bus  b{f};
  b.write("IWDG", "KR", 0xCCCC);
  b.write("IWDG", "KR", 0x5555);
  b.write("IWDG", "PR", 1);
  b.write("IWDG", "RLR", 2048);
  b.write("IWDG", "EWCR", (1u << 15) | 204u);
  REQUIRE(run_for(f, ms).reason == stop_reason::time);
  REQUIRE(b.read("IWDG", "SR") == 0);
  auto const queued = [&] { return f.target->snapshot().data().events.pending.size(); };
  auto const before = queued();
  for(int reload = 0; reload < 1000; ++reload)
  {
    b.write("IWDG", "KR", 0xAAAA);
  }
  CHECK(queued() == before);
  CHECK(f.target->pending_events() == before);
  REQUIRE(run_for(f, 460 * ms).reason == stop_reason::time);
  CHECK(f.field("IWDG", "SR", "EWIF") == 0);
  REQUIRE(run_for(f, 2 * ms).reason == stop_reason::time);
  CHECK(f.field("IWDG", "SR", "EWIF") == 1);
}

namespace
{
constexpr std::uint64_t bite_after = std::uint64_t{0xFFF} * 4 * lsi_cycle;

auto started_idle(core::machine_options options = popcorn::testing::stm32_options())
    -> stm32_fixture
{
  auto f = entered("idle", std::move(options));
  bus{f}.write("IWDG", "KR", 0xCCCC);
  return f;
}

auto bite_time(stm32_fixture const& f, std::uint64_t within = 10'000 * ms) -> std::uint64_t
{
  auto const stopped = run_for(f, within);
  REQUIRE(stopped.reason == stop_reason::reset_request);
  return f.target->virtual_clock().time().ps;
}
}

TEST_CASE("a held core without DBG_IWDG_STOP is bitten on time and stays held")
{
  auto       f       = started_idle();
  auto const started = f.target->virtual_clock().time().ps;
  f.target->hold_core(true);
  auto const bitten = bite_time(f);
  CHECK(bitten - started >= bite_after);
  CHECK(bitten - started < bite_after + 200 * ms);
  CHECK(f.target->core_held());

  auto options             = popcorn::testing::stm32_options();
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  auto continuing          = started_idle(std::move(options));
  continuing.target->hold_core(true);
  auto const resets = continuing.target->resets();
  REQUIRE(run_for(continuing, 1'000 * ms).reason == stop_reason::time);
  CHECK(continuing.target->resets() == resets + 1);
  CHECK(continuing.target->core_held());
}

TEST_CASE("with DBG_IWDG_STOP the watchdog counts running time only")
{
  auto const by = [](std::uint64_t instructions) {
    sliced const scope{instructions};
    auto         f = started_idle();
    set_debug_freeze(f, "DBG_IWDG_STOP", true);
    REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
    f.target->hold_core(true);
    REQUIRE(run_for(f, 1'000 * ms).reason == stop_reason::time);
    f.target->hold_core(false);
    return bite_time(f);
  };
  CHECK(by(997) == by(1'000'000'000));
  auto twin = started_idle();
  set_debug_freeze(twin, "DBG_IWDG_STOP", true);
  REQUIRE(run_for(twin, 100 * ms).reason == stop_reason::time);
  auto const reference = bite_time(twin);

  auto f = started_idle();
  set_debug_freeze(f, "DBG_IWDG_STOP", true);
  REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
  f.target->hold_core(true);
  REQUIRE(run_for(f, 1'000 * ms).reason == stop_reason::time);
  f.target->hold_core(false);
  CHECK(bite_time(f) == reference + 1'000 * ms);
}

TEST_CASE("a hold released at once leaves the watchdog to the picosecond")
{
  auto twin = started_idle();
  set_debug_freeze(twin, "DBG_IWDG_STOP", true);
  REQUIRE(run_for(twin, 100 * ms + lsi_cycle / 3).reason == stop_reason::time);
  auto const reference = bite_time(twin);

  auto f = started_idle();
  set_debug_freeze(f, "DBG_IWDG_STOP", true);
  REQUIRE(run_for(f, 100 * ms + lsi_cycle / 3).reason == stop_reason::time);
  f.target->hold_core(true);
  f.target->hold_core(false);
  CHECK(bite_time(f) == reference);
}

TEST_CASE("clock changes during a frozen hold shift the bite by the held span only")
{
  SUBCASE("core clock")
  {
    auto twin = started_idle();
    set_debug_freeze(twin, "DBG_IWDG_STOP", true);
    REQUIRE(run_for(twin, ms).reason == stop_reason::time);
    auto const reference = bite_time(twin);

    auto f = started_idle();
    bus  b{f};
    set_debug_freeze(f, "DBG_IWDG_STOP", true);
    REQUIRE(run_for(f, ms).reason == stop_reason::time);
    f.target->hold_core(true);
    auto const icscr1 = b.read("RCC", "RCC_ICSCR1") & ~(0xFu << 28);
    b.write("RCC", "RCC_ICSCR1", icscr1 | (1u << 28) | (1u << 23));
    REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
    b.write("RCC", "RCC_ICSCR1", icscr1 | (4u << 28) | (1u << 23));
    REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
    f.target->hold_core(false);
    auto const bitten = bite_time(f);
    CHECK(bitten >= reference + 200 * ms);
    CHECK(bitten < reference + 200 * ms + lsi_cycle);
  }
  SUBCASE("LSI rate")
  {
    constexpr std::uint64_t slow = 1'000'000'000'000 / 250;
    auto                    f    = started_idle();
    bus                     b{f};
    auto const              started = f.target->virtual_clock().time().ps;
    set_debug_freeze(f, "DBG_IWDG_STOP", true);
    REQUIRE(run_for(f, 100 * us + 3202 * lsi_cycle + lsi_cycle / 2).reason == stop_reason::time);
    auto const changed = f.target->virtual_clock().time().ps;
    REQUIRE((changed - started - 100 * us) / lsi_cycle == 3202);
    f.target->hold_core(true);
    REQUIRE(run_for(f, 200 * ms).reason == stop_reason::time);
    b.write("RCC", "RCC_BDCR", b.read("RCC", "RCC_BDCR") | (1u << 28));
    REQUIRE(run_for(f, 300 * ms).reason == stop_reason::time);
    f.target->hold_core(false);
    auto const expected = changed + 500 * ms + ((0xFFF - 800) * 4 - 2) * slow;
    auto const bitten   = bite_time(f, 100'000 * ms);
    CHECK(bitten >= expected);
    CHECK(bitten < expected + slow);
  }
}

TEST_CASE("a frozen watchdog restores frozen and bites as the original")
{
  auto f = started_idle();
  set_debug_freeze(f, "DBG_IWDG_STOP", true);
  REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
  f.target->hold_core(true);
  REQUIRE(run_for(f, 300 * ms).reason == stop_reason::time);
  auto const saved = f.target->snapshot();
  REQUIRE(run_for(f, 300 * ms).reason == stop_reason::time);
  f.target->hold_core(false);
  auto const reference = bite_time(f);

  auto other = entered("idle");
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.target->core_held());
  REQUIRE(run_for(other, 300 * ms).reason == stop_reason::time);
  other.target->hold_core(false);
  CHECK(bite_time(other) == reference);
}

TEST_CASE("DBG_IWDG_STOP survives a watchdog reset of a held core and clears on power-on")
{
  auto f = started_idle();
  set_debug_freeze(f, "DBG_IWDG_STOP", true);
  f.target->hold_core(true);
  f.target->reset(popcorn::reset_kind::watchdog);
  CHECK(debug_freeze_set(f, "DBG_IWDG_STOP"));
  CHECK(f.target->core_held());
  f.target->reset(popcorn::reset_kind::power_on);
  CHECK_FALSE(debug_freeze_set(f, "DBG_IWDG_STOP"));
  CHECK(f.target->core_held());
}

TEST_CASE("a prescaler update in flight at the hold completes after the release")
{
  auto const run = [](bool hold) {
    auto f = started_idle();
    bus  b{f};
    set_debug_freeze(f, "DBG_IWDG_STOP", true);
    REQUIRE(run_for(f, ms).reason == stop_reason::time);
    b.write("IWDG", "KR", 0x5555);
    b.write("IWDG", "PR", 3);
    REQUIRE(run_for(f, 3 * lsi_cycle).reason == stop_reason::time);
    REQUIRE((b.read("IWDG", "SR") & 1u) != 0);
    if(hold)
    {
      f.target->hold_core(true);
      REQUIRE(run_for(f, 100 * ms).reason == stop_reason::time);
      CHECK((b.read("IWDG", "SR") & 1u) != 0);
      f.target->hold_core(false);
    }
    return bite_time(f, 20'000 * ms);
  };
  CHECK(run(true) == run(false) + 100 * ms);
}

TEST_CASE("a watchdog started while the core is held starts counting at the release")
{
  auto const by = [](std::uint64_t instructions) {
    sliced const scope{instructions};
    auto         f = entered("idle");
    bus          b{f};
    set_debug_freeze(f, "DBG_IWDG_STOP", true);
    REQUIRE(run_for(f, ms).reason == stop_reason::time);
    f.target->hold_core(true);
    b.write("IWDG", "KR", 0xCCCC);
    REQUIRE(run_for(f, 1'000 * ms).reason == stop_reason::time);
    f.target->hold_core(false);
    return bite_time(f);
  };
  CHECK(by(997) == by(1'000'000'000));
  auto const run = [](bool hold) {
    auto f = entered("idle");
    bus  b{f};
    set_debug_freeze(f, "DBG_IWDG_STOP", true);
    REQUIRE(run_for(f, ms).reason == stop_reason::time);
    if(hold)
    {
      f.target->hold_core(true);
    }
    b.write("IWDG", "KR", 0xCCCC);
    if(hold)
    {
      REQUIRE(run_for(f, 1'000 * ms).reason == stop_reason::time);
      f.target->hold_core(false);
    }
    return bite_time(f);
  };
  CHECK(run(true) == run(false) + 1'000 * ms - 100 * us);
}

namespace
{
auto without_freeze_field() -> popcorn::part_description
{
  auto desc = u575();
  auto dbg  = std::ranges::find(
      desc.peripherals, std::string_view{"DBGMCU"}, &popcorn::peripheral_desc::name);
  REQUIRE(dbg != desc.peripherals.end());
  for(auto& reg : dbg->registers)
  {
    std::erase_if(
        reg.fields, [](popcorn::field_desc const& field) { return field.name == "DBG_IWDG_STOP"; });
  }
  return desc;
}

auto without_dbgmcu() -> popcorn::part_description
{
  auto desc = u575();
  std::erase_if(
      desc.peripherals, [](popcorn::peripheral_desc const& unit) { return unit.name == "DBGMCU"; });
  return desc;
}

auto entered_on(popcorn::part_description const& desc, core::machine_options options)
    -> stm32_fixture
{
  stm32_fixture f{desc, "iwdg_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("iwdg_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("iwdg_ll", "idle"));
  return f;
}
}

TEST_CASE("a hold with no freeze field to read warns once and the watchdog runs on")
{
  struct absent
  {
    popcorn::part_description desc;
    std::string_view          says;
  };
  for(auto const& [desc, says] : {
          absent{without_freeze_field(), "DBG_IWDG_STOP"},
          absent{without_dbgmcu(),       "no DBGMCU"    }
  })
  {
    CAPTURE(says);
    auto                          f = entered_on(desc, popcorn::testing::stm32_options());
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    bus{f}.write("IWDG", "KR", 0xCCCC);
    auto const started = f.target->virtual_clock().time().ps;
    f.target->hold_core(true);
    f.target->hold_core(false);
    f.target->hold_core(true);
    auto const bitten = bite_time(f);
    CHECK(bitten - started < bite_after + 200 * ms);
    std::erase_if(logged, [](core::log_record const& record) {
      return record.kind != core::log_kind::model_warning;
    });
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains(says));

    auto strict               = popcorn::testing::stm32_options();
    strict.strict_peripherals = true;
    auto refused              = entered_on(desc, std::move(strict));
    CHECK_THROWS_AS(refused.target->hold_core(true), popcorn::model_error);
    CHECK_FALSE(refused.target->core_held());
    CHECK(run_for(refused, ms).reason == stop_reason::time);
  }
}
