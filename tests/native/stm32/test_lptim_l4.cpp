#include "stm32_fixture.hpp"
#include "trigger_probe.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l476rc;
using popcorn::testing::set_debug_freeze;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::trigger_probe;

constexpr std::string_view firmware   = "l476_l4_rcc_ll";
constexpr std::string_view lptim_ll   = "l476_l4_lptim_ll";
constexpr address          lptim1     = 0x4000'7C00;
constexpr address          lptim2     = 0x4000'9400;
constexpr address          isr        = 0x00;
constexpr address          icr        = 0x04;
constexpr address          ier        = 0x08;
constexpr address          cfgr       = 0x0C;
constexpr address          cr         = 0x10;
constexpr address          cmp        = 0x14;
constexpr address          arr        = 0x18;
constexpr address          cnt        = 0x1C;
constexpr address          option     = 0x20;
constexpr address          apb1enr1   = 0x4002'1058;
constexpr address          apb1enr2   = 0x4002'105C;
constexpr address          apb1rstr1  = 0x4002'1038;
constexpr address          apb1rstr2  = 0x4002'103C;
constexpr address          rcc_cfgr   = 0x4002'1008;
constexpr address          ispr2      = 0xE000'E208;
constexpr std::uint32_t    cmpm       = 1u << 0;
constexpr std::uint32_t    arrm       = 1u << 1;
constexpr std::uint32_t    cmpok      = 1u << 3;
constexpr std::uint32_t    arrok      = 1u << 4;
constexpr std::uint32_t    enable     = 1u << 0;
constexpr std::uint32_t    sngstrt    = 1u << 1;
constexpr std::uint32_t    cntstrt    = 1u << 2;
constexpr std::uint32_t    preload    = 1u << 22;
constexpr std::uint32_t    wavpol     = 1u << 21;
constexpr std::uint32_t    wave       = 1u << 20;
constexpr std::uint32_t    exttrig    = 1u << 2;
constexpr std::uint32_t    timout     = 1u << 19;
constexpr std::uint32_t    rising     = 1u << 17;
constexpr std::uint32_t    falling    = 2u << 17;
constexpr std::uint32_t    both_edges = 3u << 17;
constexpr address          ahb2enr    = 0x4002'104C;
constexpr std::uint32_t    lptim1_irq = 1u << (65 - 64);
constexpr std::uint64_t    cycle      = 250'000;
constexpr std::uint64_t    lsi_tick   = 31'250'000;
constexpr std::uint64_t    us         = 1'000'000;

constexpr auto presc(std::uint32_t value) -> std::uint32_t
{
  return value << 9;
}

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  trigger_probe*                 probe{};
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  explicit rig(
      bool                             strict = false,
      popcorn::part_description const& desc   = l476rc(),
      bool                             listen = false)
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(stm32::board{}, desc);
    options.strict_peripherals = strict;
    if(listen)
    {
      options.models.insert_or_assign(
          "DAC1", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
            return std::make_shared<trigger_probe>(ctx);
          });
    }
    f = std::make_unique<stm32_fixture>(desc, firmware, std::move(options));
    if(listen)
    {
      probe = f->model_of<trigger_probe>("DAC1");
      REQUIRE(probe != nullptr);
      probe->watched = "lptim1_out";
      probe->listen(true);
    }
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    write(apb1enr1, 1u << 31);
    write(apb1enr2, 1u << 5);
  }

  [[nodiscard]] auto read(address at) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(address at, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at, 4, value).has_value());
  }

  void idle_to(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto pending(std::string_view unit = "LPTIM1") const -> std::optional<std::uint64_t>
  {
    auto const& units = f->target->description().peripherals;
    auto const  found = std::ranges::find(units, unit, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    auto const owner  = static_cast<std::uint32_t>(found - units.begin());
    auto const events = f->target->snapshot().data().events.pending;
    auto const event  = std::ranges::find_if(
        events, [&](auto const& one) { return one.id.owner == owner and one.id.kind == 0; });
    return event == events.end() ? std::nullopt : std::optional{event->due_ps};
  }

  [[nodiscard]] auto warnings(std::string_view text) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.message.find(text) != std::string::npos;
    });
  }

  [[nodiscard]] auto out_level() const -> std::optional<bool>
  {
    auto* const source = dynamic_cast<stm32::trigger_source*>(f->target->model("LPTIM1"));
    REQUIRE(source != nullptr);
    return source->level("lptim1_out");
  }

  [[nodiscard]] auto saved() const -> std::vector<std::byte>
  {
    auto out = std::vector<std::byte>{};
    f->target->model("LPTIM1")->save(out);
    return out;
  }

  void settle_values(std::uint32_t compare, std::uint32_t reload) const
  {
    write(lptim1 + arr, reload);
    write(lptim1 + cmp, compare);
    idle_to(now_ps() + 4 * cycle);
    write(lptim1 + icr, cmpok | arrok);
  }

  [[nodiscard]] auto started(
      std::uint32_t compare,
      std::uint32_t reload,
      std::uint32_t mode   = cntstrt,
      std::uint32_t config = 0,
      std::uint32_t irqs   = 0) const -> std::uint64_t
  {
    write(lptim1 + cfgr, config);
    write(lptim1 + ier, irqs);
    write(lptim1 + cr, enable);
    idle_to(now_ps() + 300 * cycle);
    settle_values(compare, reload);
    auto const at = now_ps();
    write(lptim1 + cr, enable | mode);
    return at;
  }
};

struct firmware_run
{
  std::unique_ptr<stm32_fixture>      f;
  std::vector<std::uint64_t>          compares;
  std::vector<std::uint64_t>          reloads;
  std::vector<core::intercept_handle> hooks;

  explicit firmware_run(std::string_view entry)
  {
    f = std::make_unique<stm32_fixture>(l476rc(), lptim_ll, popcorn::testing::stm32_options());
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(lptim_ll, "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym(lptim_ll, entry));
    record("on_compare", compares);
    record("on_reload", reloads);
  }

  void record(std::string_view symbol, std::vector<std::uint64_t>& into)
  {
    hooks.push_back(
        f->target->add_intercept(sym(lptim_ll, symbol), [&into](core::intercept_call const& c) {
          into.push_back(c.target().virtual_clock().time().ps);
          return core::intercept_result::proceed();
        }));
  }

  [[nodiscard]] auto started() const -> std::uint64_t
  {
    auto const stopped = f->run_to_stop(
        {.max_instructions = icount{1'000'000}, .until = sym(lptim_ll, "on_started")});
    REQUIRE(stopped.reason == stop_reason::address);
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{1'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }
};

auto gaps(std::vector<std::uint64_t> const& at) -> std::vector<std::uint64_t>
{
  auto out = std::vector<std::uint64_t>{};
  for(std::size_t index = 1; index < at.size(); ++index)
  {
    out.push_back(at[index] - at[index - 1]);
  }
  return out;
}

TEST_CASE("ES0250 2.18.3: CMPM is due one kernel clock after CNT reaches CMP, CMP 6 ARR 10")
{
  rig        r;
  auto const t = r.started(6, 10);
  CHECK(r.pending() == t + 3 * cycle + 6 * cycle + cycle);
}

TEST_CASE(
    "RM0351 34.4.5: with PRESC 3 ARRM is due 8 kernel clocks per count, plus the ES0250 "
    "2.18.3 clock unprescaled, inferred")
{
  rig        r;
  auto const t = r.started(20, 10, cntstrt, presc(3));
  CHECK(r.pending() == t + 3 * cycle + 10 * 8 * cycle + cycle);
}

TEST_CASE("RM0351 34.4.6 note: CNT reads 0 until 3 kernel clocks after CNTSTRT, then counts")
{
  rig        r;
  auto const t = r.started(20, 30);
  r.idle_to(t + 3 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  r.idle_to(t + 4 * cycle);
  CHECK(r.read(lptim1 + cnt) == 1);
  r.idle_to(t + 13 * cycle);
  CHECK(r.read(lptim1 + cnt) == 10);
}

TEST_CASE("RM0351 34.4.12: the start waits two prescaled clocks after ENABLE, inferred")
{
  rig r;
  r.write(lptim1 + cfgr, presc(2));
  r.write(lptim1 + cr, enable);
  auto const t = r.now_ps();
  r.write(lptim1 + cr, enable | cntstrt);
  CHECK(r.pending() == t + 8 * cycle + cycle);
}

TEST_CASE("RM0351 34.4.10: CMPOK sets 3 kernel clocks after the CMP write, inferred")
{
  rig r;
  r.write(lptim1 + cr, enable);
  auto const t = r.now_ps();
  r.write(lptim1 + cmp, 5);
  CHECK(r.pending() == t + 3 * cycle);
  r.idle_to(t + 2 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpok) == 0);
  r.idle_to(t + 3 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpok) == cmpok);
  CHECK(r.read(lptim1 + cmp) == 5);
}

TEST_CASE("RM0351 34.4.10: with PRESC 0 an ARR write while counting applies at ARROK, CNT goes on")
{
  rig        r;
  auto const t = r.started(20, 10);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 25 * cycle);
  REQUIRE(r.read(lptim1 + cnt) == 3);
  r.write(lptim1 + icr, cmpm | arrm);
  r.write(lptim1 + arr, 20);
  r.idle_to(a + 28 * cycle);
  CHECK(r.read(lptim1 + cnt) == 6);
  CHECK((r.read(lptim1 + isr) & arrok) == arrok);
  CHECK(r.pending() == a + 42 * cycle + cycle);
}

TEST_CASE("inferred: a CMPOK or ARROK at or after a counter tick keeps that tick's CMPM and ARRM")
{
  for(auto const& [written, value, flag, from] : {
          std::tuple{arr, 12u, cmpm, 3u},
          std::tuple{arr, 10u, cmpm, 4u},
          std::tuple{cmp, 20u, arrm, 7u},
          std::tuple{cmp, 20u, arrm, 8u},
  })
  {
    CAPTURE(written);
    CAPTURE(from);
    rig        r;
    auto const t     = r.started(flag == cmpm ? 6 : 20, 10);
    auto const a     = t + 3 * cycle;
    auto const match = a + (flag == cmpm ? 7 : 11) * cycle;
    r.idle_to(a + from * cycle);
    r.write(lptim1 + written, value);
    auto const ok = a + (from + 3) * cycle;
    r.idle_to(ok);
    if(ok < match)
    {
      CHECK(r.pending() == match);
      r.idle_to(match);
    }
    CHECK((r.read(lptim1 + isr) & flag) == flag);
    r.write(lptim1 + icr, flag);
    CHECK((r.read(lptim1 + isr) & flag) == 0);
    if(value == 12)
    {
      r.idle_to(a + 19 * cycle);
      CHECK((r.read(lptim1 + isr) & cmpm) == 0);
      r.idle_to(a + 20 * cycle);
      CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
    }
  }
}

TEST_CASE("inferred: a CMP applied on the tick of its value matches one period later")
{
  rig        r;
  auto const t = r.started(6, 10);
  auto const a = t + 3 * cycle;
  r.idle_to(a);
  r.write(lptim1 + cmp, 3);
  r.idle_to(a + 5 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(a + 15 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("RM0351 34.6: LPTIM1_IRQn rises at the CMPM due with no register read in between")
{
  rig        r;
  auto const t   = r.started(6, 10, cntstrt, 0, cmpm);
  auto const due = t + 10 * cycle;
  REQUIRE(r.pending() == due);
  r.idle_to(due - cycle);
  CHECK((r.read(ispr2) & lptim1_irq) == 0);
  r.idle_to(due);
  CHECK((r.read(ispr2) & lptim1_irq) == lptim1_irq);
  r.write(lptim1 + icr, cmpm);
  r.write(ispr2 - 0x100 + 0x180, lptim1_irq);
  CHECK((r.read(ispr2) & lptim1_irq) == 0);
}

TEST_CASE("RM0351 34.6 note: an IER bit set after its flag raises no IRQ")
{
  rig        r;
  auto const t = r.started(6, 10);
  r.idle_to(t + 12 * cycle);
  REQUIRE((r.read(lptim1 + isr) & cmpm) == cmpm);
  r.write(lptim1 + cr, 0);
  r.write(lptim1 + ier, cmpm);
  r.write(lptim1 + cr, enable);
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(r.now_ps() + 30 * cycle);
  CHECK((r.read(ispr2) & lptim1_irq) == 0);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE(
    "checklist 53 policy: IER, CFGR and OR writes with ENABLE 1 are kept out, logged once, "
    "RM0351 34.4.12")
{
  for(auto const at : {ier, cfgr, option})
  {
    CAPTURE(at);
    rig r;
    r.write(lptim1 + cr, enable);
    r.write(lptim1 + at, 1);
    r.write(lptim1 + at, 2);
    CHECK(r.read(lptim1 + at) == 0);
    CHECK(r.warnings("with CR.ENABLE 1: the register keeps its value") == 1);
    rig strict{true};
    strict.write(lptim1 + cr, enable);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(lptim1 + at, 4, 1)), popcorn::model_error);
    CHECK(strict.read(lptim1 + at) == 0);
    strict.write(lptim1 + cr, 0);
    strict.write(lptim1 + at, 1);
    CHECK(strict.read(lptim1 + at) == 1);
  }
}

TEST_CASE(
    "checklist 53 policy: CMP and ARR writes with ENABLE 0 are kept out, logged once, "
    "RM0351 34.7.6 and 34.7.7")
{
  for(auto const at : {cmp, arr})
  {
    CAPTURE(at);
    rig        r;
    auto const reset = r.read(lptim1 + at);
    r.write(lptim1 + at, 7);
    r.write(lptim1 + at, 8);
    CHECK(r.read(lptim1 + at) == reset);
    CHECK(r.warnings("write with CR.ENABLE 0: the register keeps its value") == 1);
    rig strict{true};
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(lptim1 + at, 4, 7)), popcorn::model_error);
    CHECK(strict.read(lptim1 + at) == reset);
  }
}

TEST_CASE(
    "checklist 53 policy: a second CMP write before CMPOK replaces the value, logged once, "
    "inferred, RM0351 34.4.10")
{
  rig r;
  r.write(lptim1 + cr, enable);
  r.write(lptim1 + cmp, 4);
  r.write(lptim1 + cmp, 5);
  CHECK(r.warnings("before its OK flag") == 1);
  r.idle_to(r.now_ps() + 4 * cycle);
  CHECK(r.read(lptim1 + cmp) == 5);
  rig strict{true};
  strict.write(lptim1 + cr, enable);
  strict.write(lptim1 + cmp, 4);
  CHECK_THROWS_AS(
      static_cast<void>(strict.f->target->mem().write(lptim1 + cmp, 4, 5)), popcorn::model_error);
  CHECK(strict.read(lptim1 + cmp) == 4);
}

TEST_CASE("checklist 27: an ICR write at the CMPM due time leaves CMPM 0 until the next match")
{
  rig        r;
  auto const t   = r.started(6, 10);
  auto const due = t + 10 * cycle;
  r.idle_to(due);
  REQUIRE(r.now_ps() == due);
  r.write(lptim1 + icr, cmpm);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(due + 10 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(due + 11 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("RM0351 34.4.7: SNGSTRT counts once to ARR, sets ARRM once and stops at 0")
{
  rig        r;
  auto const t = r.started(2, 5, sngstrt, presc(1));
  auto const a = t + 3 * cycle;
  r.idle_to(a + 11 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == arrm);
  CHECK(r.pending() == a + 12 * cycle);
  r.write(lptim1 + icr, cmpm | arrm);
  r.idle_to(a + 80 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  CHECK((r.read(lptim1 + isr) & (cmpm | arrm)) == 0);
  CHECK(r.pending() == std::nullopt);
  auto const again = r.now_ps();
  r.write(lptim1 + cr, enable | sngstrt);
  r.idle_to(again + 7 * cycle);
  CHECK(r.read(lptim1 + cnt) == 2);
}

TEST_CASE("inferred: an OK on the ARR tick or the wrap of a one-shot run keeps ARRM and the stop")
{
  for(auto const& [written, value, from] : {
          std::tuple{cmp, 2u, 2u},
          std::tuple{cmp, 2u, 3u},
          std::tuple{arr, 7u, 3u},
          std::tuple{cmp, 4u, 3u},
  })
  {
    CAPTURE(written);
    CAPTURE(from);
    rig        r;
    auto const t = r.started(2, 5, sngstrt);
    auto const a = t + 3 * cycle;
    r.idle_to(a + from * cycle);
    if(value == 4)
    {
      r.write(lptim1 + icr, cmpm);
    }
    r.write(lptim1 + written, value);
    r.idle_to(a + 6 * cycle);
    CHECK((r.read(lptim1 + isr) & arrm) == arrm);
    CHECK((r.read(lptim1 + isr) & cmpm) == (value == 4 ? 0 : cmpm));
    r.write(lptim1 + icr, cmpm | arrm);
    r.idle_to(a + 30 * cycle);
    CHECK(r.read(lptim1 + cnt) == 0);
    CHECK((r.read(lptim1 + isr) & (cmpm | arrm)) == 0);
    CHECK(r.pending() == std::nullopt);
  }
}

TEST_CASE("checklist 31: an LPTIM1RST pulse resets the registers and withdraws the pending event")
{
  rig        r;
  auto const t = r.started(6, 10, cntstrt, 0, cmpm);
  auto const a = t + 3 * cycle;
  REQUIRE(r.pending() == a + 7 * cycle);
  r.idle_to(a + 2 * cycle);
  r.write(apb1rstr1, 1u << 31);
  r.write(apb1rstr1, 0);
  CHECK(r.pending() == std::nullopt);
  CHECK(r.read(lptim1 + cr) == 0);
  CHECK(r.read(lptim1 + cmp) == 0);
  CHECK(r.read(lptim1 + arr) == 1);
  CHECK(r.read(lptim1 + ier) == 0);
  r.write(lptim1 + cr, enable);
  r.write(lptim1 + arr, 10);
  r.write(lptim1 + cmp, 2);
  r.idle_to(a + 5 * cycle);
  r.write(lptim1 + icr, cmpok | arrok);
  auto const s = r.now_ps();
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(a + 8 * cycle);
  CHECK(r.read(lptim1 + isr) == 0);
  CHECK(r.pending() == s + 6 * cycle);
  r.idle_to(s + 6 * cycle);
  CHECK(r.read(lptim1 + isr) == cmpm);
  CHECK((r.read(ispr2) & lptim1_irq) == 0);
}

TEST_CASE("RM0351 6.4.13 and 6.4.14: LPTIM1RST and LPTIM2RST each reset only their own LPTIM")
{
  for(auto const& [unit, base, reset, bit] : {
          std::tuple{std::string_view{"LPTIM1"}, lptim1, apb1rstr1, 1u << 31},
          std::tuple{std::string_view{"LPTIM2"}, lptim2, apb1rstr2, 1u << 5 },
  })
  {
    CAPTURE(unit);
    rig r;
    for(auto const at : {lptim1, lptim2})
    {
      r.write(at + cr, enable);
      r.write(at + cmp, 5);
    }
    auto const other = base == lptim1 ? lptim2 : lptim1;
    REQUIRE(r.pending(unit).has_value());
    r.write(reset, bit);
    r.write(reset, 0);
    CHECK(r.pending(unit) == std::nullopt);
    CHECK(r.read(base + cr) == 0);
    CHECK(r.read(base + cmp) == 0);
    CHECK(r.read(other + cr) == enable);
    CHECK(r.read(other + cmp) == 5);
    r.idle_to(r.now_ps() + 4 * cycle);
    CHECK(r.read(base + isr) == 0);
    CHECK(r.read(other + isr) == cmpok);
  }
}

TEST_CASE("inferred: a PCLK1 rate change keeps CNT and the tick's elapsed kernel clocks")
{
  rig        r;
  auto const t = r.started(5, 100, cntstrt, presc(2));
  auto const x = t + 13 * cycle;
  r.idle_to(x);
  REQUIRE(r.read(lptim1 + cnt) == 2);
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (4u << 8));
  CHECK(r.pending() == x + 22 * cycle);
  r.idle_to(x + 3 * cycle);
  CHECK(r.read(lptim1 + cnt) == 2);
  r.idle_to(x + 4 * cycle);
  CHECK(r.read(lptim1 + cnt) == 3);
  r.idle_to(x + 22 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("inferred: a CMPM in its delay at a PCLK1 rate change sets one new kernel clock later")
{
  rig        r;
  auto const t = r.started(2, 100, cntstrt, presc(2));
  auto const x = t + 11 * cycle;
  r.idle_to(x);
  REQUIRE(r.read(lptim1 + cnt) == 2);
  REQUIRE((r.read(lptim1 + isr) & cmpm) == 0);
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (4u << 8));
  CHECK(r.pending() == x + 2 * cycle);
  r.idle_to(x + 2 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

auto started_at_half_pclk1(rig const& r, std::uint32_t compare) -> std::uint64_t
{
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (4u << 8));
  r.write(lptim1 + cr, enable);
  r.idle_to(r.now_ps() + 300 * cycle);
  r.write(lptim1 + arr, 100);
  r.write(lptim1 + cmp, compare);
  r.idle_to(r.now_ps() + 8 * cycle);
  r.write(lptim1 + icr, cmpok | arrok);
  auto const t = r.now_ps();
  r.write(lptim1 + cr, enable | cntstrt);
  return t;
}

TEST_CASE("inferred: a rate drop in the start's first tick keeps CMPM of count 0, no stale floor")
{
  rig        r;
  auto const t = started_at_half_pclk1(r, 0);
  CHECK(r.pending() == t + 8 * cycle);
  r.idle_to(t + 7 * cycle);
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (7u << 8));
  CHECK(r.pending() == t + 15 * cycle);
  r.idle_to(t + 15 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("inferred: a rate rise after a CMP applied on its tick keeps CMPM a period later")
{
  rig        r;
  auto const t = started_at_half_pclk1(r, 6);
  auto const a = t + 6 * cycle;
  r.idle_to(a + 4 * cycle);
  r.write(lptim1 + cmp, 5);
  r.idle_to(a + 11 * cycle);
  REQUIRE(r.read(lptim1 + cnt) == 5);
  REQUIRE((r.read(lptim1 + isr) & cmpok) == cmpok);
  r.write(rcc_cfgr, r.read(rcc_cfgr) & ~(7u << 8));
  CHECK(r.pending() > a + 100 * cycle);
  r.idle_to(a + 14 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
}

TEST_CASE(
    "inferred: an OK on the one-shot wrap stops there, a later rate change leaves CNT 0 and "
    "nothing pending")
{
  rig        r;
  auto const t = r.started(2, 5, sngstrt, presc(2));
  auto const a = t + 3 * cycle;
  r.idle_to(a + 21 * cycle);
  r.write(lptim1 + cmp, 3);
  r.idle_to(a + 25 * cycle);
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (4u << 8));
  CHECK((r.read(lptim1 + isr) & (arrm | cmpok)) == (arrm | cmpok));
  CHECK(r.read(lptim1 + cnt) == 0);
  CHECK(r.pending() == std::nullopt);
  r.idle_to(a + 60 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
}

TEST_CASE("inferred: a stopped kernel clock freezes CNT and a pending CMPOK, both resume")
{
  rig        r;
  auto const t = r.started(30, 100);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 5 * cycle);
  r.write(lptim1 + cmp, 40);
  r.idle_to(a + 6 * cycle);
  r.write(apb1enr1, 0);
  r.idle_to(a + 50 * cycle);
  CHECK(r.read(lptim1 + cnt) == 6);
  CHECK((r.read(lptim1 + isr) & cmpok) == 0);
  CHECK(r.pending() == std::nullopt);
  r.write(apb1enr1, 1u << 31);
  r.idle_to(a + 51 * cycle);
  CHECK(r.read(lptim1 + cnt) == 7);
  CHECK((r.read(lptim1 + isr) & cmpok) == 0);
  r.idle_to(a + 52 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpok) == cmpok);
}

TEST_CASE("RM0351 34.3 Table 216: LPTIM2 has no CFGR.ENC, bit 24 reads 0, LPTIM1 keeps it")
{
  rig r;
  r.write(lptim1 + cfgr, 1u << 24);
  r.write(lptim2 + cfgr, 1u << 24);
  CHECK(r.read(lptim1 + cfgr) == 1u << 24);
  CHECK(r.read(lptim2 + cfgr) == 0);
  r.write(lptim2 + option, 3);
  CHECK(r.read(lptim2 + option) == 3);
}

TEST_CASE("checklist 39: LPTIM1 needs CFGR.ENC")
{
  auto       desc  = l476rc();
  auto const found = std::ranges::find(
      desc.peripherals, std::string_view{"LPTIM1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  auto owner =
      std::ranges::find(found->registers, std::string_view{"CFGR"}, &popcorn::register_desc::name);
  REQUIRE(owner != found->registers.end());
  REQUIRE(std::erase_if(owner->fields, [](auto const& one) { return one.name == "ENC"; }) == 1);
  CHECK_THROWS_AS(rig(false, desc), popcorn::config_error);
}

TEST_CASE("a snapshot mid period restores the model state and its pending event")
{
  rig        r;
  auto const t = r.started(6, 10);
  r.idle_to(t + 12 * cycle);
  r.write(lptim1 + icr, cmpm);
  r.idle_to(t + 15 * cycle);
  auto const saved = r.f->target->snapshot();
  rig        other;
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  CHECK(other.pending() == r.pending());
  CHECK((other.read(lptim1 + isr) & cmpm) == 0);
  for(auto const* one : {&r, &other})
  {
    CHECK(one->read(lptim1 + cnt) == 1);
    one->idle_to(t + 22 * cycle);
    CHECK(one->read(lptim1 + isr) == (cmpm | arrm));
  }
}

TEST_CASE("a machine reset gives ARR 1, the counter then wraps at 1")
{
  rig r;
  static_cast<void>(r.started(6, 10));
  r.f->target->reset();
  r.f->set("pc", sym(firmware, "idle"));
  r.write(apb1enr1, 1u << 31);
  CHECK(r.read(lptim1 + arr) == 1);
  CHECK(r.read(lptim1 + isr) == 0);
  CHECK(r.pending() == std::nullopt);
  r.write(lptim1 + cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const t = r.now_ps();
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(t + 4 * cycle);
  CHECK(r.read(lptim1 + cnt) == 1);
  r.idle_to(t + 5 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
}

TEST_CASE("a machine reset after PPRE1 /2 takes the reset kernel rate, CMPM on the 4 MHz count")
{
  rig r;
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (4u << 8));
  static_cast<void>(r.started(6, 10));
  r.f->target->reset();
  r.f->set("pc", sym(firmware, "idle"));
  r.write(lptim1 + cr, enable);
  r.write(lptim1 + arr, 10);
  r.write(lptim1 + cmp, 6);
  CHECK(r.pending() == std::nullopt);
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK(r.read(lptim1 + isr) == 0);
  auto const e = r.now_ps();
  r.write(apb1enr1, 1u << 31);
  CHECK(r.pending() == e + 3 * cycle);
  r.idle_to(e + 4 * cycle);
  CHECK(r.read(lptim1 + isr) == (cmpok | arrok));
  r.write(lptim1 + icr, cmpok | arrok);
  auto const t = r.now_ps();
  r.write(lptim1 + cr, enable | cntstrt);
  CHECK(r.pending() == t + 3 * cycle + 6 * cycle + cycle);
  r.idle_to(t + 10 * cycle);
  CHECK(r.read(lptim1 + isr) == cmpm);
}

TEST_CASE("checklist 53: each CFGR and OR function the model lacks logs once, raises under strict")
{
  for(auto const& [at, bits] : std::vector<std::pair<address, std::uint32_t>>{
          {cfgr,   1u << 17 | 1u << 13},
          {cfgr,   1u << 17 | 7u << 13},
          {cfgr,   1u << 0            },
          {cfgr,   1u << 23           },
          {cfgr,   1u << 24           },
          {option, 1u << 0            }
  })
  {
    CAPTURE(at);
    CAPTURE(bits);
    rig r;
    r.write(lptim1 + at, bits);
    r.write(lptim1 + cr, enable);
    r.write(lptim1 + cr, enable | cntstrt);
    r.write(lptim1 + cr, enable);
    r.write(lptim1 + cr, 0);
    r.write(lptim1 + cr, enable);
    r.write(lptim1 + cr, enable | cntstrt);
    CHECK(r.warnings("at a counter start: not modelled") == 1);
    rig strict{true};
    strict.write(lptim1 + at, bits);
    strict.write(lptim1 + cr, enable);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(lptim1 + cr, 4, enable | cntstrt)),
        popcorn::model_error);
    CHECK(strict.pending() == std::nullopt);
  }
}

TEST_CASE("RM0351 34.4.10: with PRELOAD 1 a CMP write while counting applies at the next wrap")
{
  rig        r;
  auto const t = r.started(2, 10, cntstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + cmp, 6);
  r.idle_to(a + 4 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpok) == cmpok);
  r.write(lptim1 + icr, cmpm | cmpok);
  CHECK(r.pending() == a + 11 * cycle);
  r.idle_to(a + 11 * cycle);
  CHECK(r.pending() == a + 18 * cycle);
  r.idle_to(a + 17 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(a + 18 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("RM0351 34.4.10: with PRELOAD 1 an ARR write while counting applies at the next wrap")
{
  rig        r;
  auto const t = r.started(20, 10, cntstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 2 * cycle);
  r.write(lptim1 + arr, 15);
  r.idle_to(a + 11 * cycle);
  CHECK((r.read(lptim1 + isr) & (arrm | arrok)) == (arrm | arrok));
  r.write(lptim1 + icr, arrm | arrok);
  CHECK(r.pending() == a + 27 * cycle);
  r.idle_to(a + 26 * cycle);
  CHECK(r.read(lptim1 + cnt) == 15);
  CHECK((r.read(lptim1 + isr) & arrm) == 0);
}

TEST_CASE("RM0351 34.4.10: with PRELOAD 1 a CMP write applies at its OK before the start")
{
  rig r;
  r.write(lptim1 + cfgr, preload);
  r.write(lptim1 + cr, enable);
  r.idle_to(r.now_ps() + 300 * cycle);
  r.settle_values(5, 10);
  auto const t = r.now_ps();
  r.write(lptim1 + cr, enable | cntstrt);
  CHECK(r.pending() == t + 3 * cycle + 5 * cycle + cycle);
}

TEST_CASE("inferred: with PRELOAD 1 a second CMP write waits for its own OK and wrap")
{
  rig        r;
  auto const t = r.started(2, 10, cntstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + cmp, 6);
  r.idle_to(a + 5 * cycle);
  r.write(lptim1 + icr, cmpm | cmpok);
  r.idle_to(a + 9 * cycle);
  r.write(lptim1 + cmp, 8);
  r.idle_to(a + 17 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(a + 18 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
  r.write(lptim1 + icr, cmpm | cmpok);
  r.idle_to(a + 30 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(a + 31 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("inferred: with PRELOAD 1 a second ARR write waits for its own OK and wrap")
{
  rig        r;
  auto const t = r.started(20, 10, cntstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + arr, 12);
  r.idle_to(a + 5 * cycle);
  r.write(lptim1 + icr, arrok);
  r.idle_to(a + 9 * cycle);
  r.write(lptim1 + arr, 15);
  r.idle_to(a + 12 * cycle);
  r.write(lptim1 + icr, arrm | arrok);
  r.idle_to(a + 23 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == 0);
  r.idle_to(a + 24 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == arrm);
  r.write(lptim1 + icr, arrm);
  r.idle_to(a + 39 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == 0);
  r.idle_to(a + 40 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == arrm);
}

TEST_CASE("checklist 64: with PRELOAD 1 a CMP write in the third period applies at its next wrap")
{
  for(auto const written : {23u, 30u})
  {
    CAPTURE(written);
    rig        r;
    auto const t = r.started(2, 10, cntstrt, preload);
    auto const a = t + 3 * cycle;
    r.idle_to(a + written * cycle);
    r.write(lptim1 + cmp, 6);
    r.idle_to(a + (written + 4) * cycle);
    r.write(lptim1 + icr, cmpm | cmpok);
    r.idle_to(a + 39 * cycle);
    CHECK((r.read(lptim1 + isr) & cmpm) == 0);
    r.idle_to(a + 40 * cycle);
    CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
  }
}

TEST_CASE("checklist 64: with PRELOAD 1 an ARR write in the third period applies at its next wrap")
{
  rig        r;
  auto const t = r.started(20, 10, cntstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 23 * cycle);
  r.write(lptim1 + arr, 15);
  r.idle_to(a + 27 * cycle);
  r.write(lptim1 + icr, arrm | arrok);
  r.idle_to(a + 33 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == arrm);
  r.write(lptim1 + icr, arrm);
  r.idle_to(a + 48 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == 0);
  r.idle_to(a + 49 * cycle);
  CHECK((r.read(lptim1 + isr) & arrm) == arrm);
}

TEST_CASE("inferred: with PRELOAD 1 an OK in the start latency waits for the first wrap")
{
  rig r;
  r.write(lptim1 + cfgr, preload);
  r.write(lptim1 + cr, enable);
  r.idle_to(r.now_ps() + 300 * cycle);
  r.settle_values(0, 10);
  auto const w = r.now_ps();
  r.write(lptim1 + cmp, 5);
  r.idle_to(w + cycle);
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(w + 5 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
  r.write(lptim1 + icr, cmpm | cmpok);
  r.idle_to(w + 20 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(w + 21 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("inferred: with PRELOAD 1 and ARR preloaded, a CMP OK on the wrap applies there too")
{
  rig        r;
  auto const t = r.started(2, 10, cntstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + arr, 10);
  r.idle_to(a + 4 * cycle);
  r.write(lptim1 + icr, cmpm);
  r.idle_to(a + 8 * cycle);
  r.write(lptim1 + cmp, 6);
  r.idle_to(a + 17 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(a + 18 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
}

TEST_CASE("inferred: a CNTSTRT due on the one-shot wrap restarts with the preloaded CMP")
{
  rig        r;
  auto const t = r.started(2, 5, sngstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + cmp, 0);
  r.idle_to(a + 3 * cycle);
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(a + 5 * cycle);
  r.write(lptim1 + icr, cmpm | cmpok);
  r.idle_to(a + 6 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.idle_to(a + 7 * cycle);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
  r.idle_to(a + 9 * cycle);
  CHECK(r.read(lptim1 + cnt) == 3);
}

TEST_CASE("inferred: ENABLE 0 applies a CMP value waiting for the wrap with PRELOAD 1")
{
  rig        r;
  auto const t = r.started(2, 10, cntstrt, preload);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + cmp, 6);
  r.idle_to(a + 5 * cycle);
  r.write(lptim1 + cr, 0);
  r.write(lptim1 + icr, cmpm | cmpok);
  r.write(lptim1 + cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const s = r.now_ps();
  r.write(lptim1 + cr, enable | cntstrt);
  CHECK(r.pending() == s + 3 * cycle + 6 * cycle + cycle);
}

TEST_CASE(
    "RM0351 34.4.7: SNGSTRT while counting continuously stops at the wrap after the switch, its "
    "latency inferred")
{
  for(auto const& [written, stop] : {
          std::tuple{1u, 6u },
          std::tuple{4u, 12u},
  })
  {
    CAPTURE(written);
    rig        r;
    auto const t = r.started(20, 5);
    auto const a = t + 3 * cycle;
    r.idle_to(a + written * cycle);
    r.write(lptim1 + cr, enable | sngstrt);
    CHECK(r.pending() == a + std::min(written + 3, 6u) * cycle);
    r.idle_to(a + (written + 3) * cycle);
    CHECK(r.pending() == a + stop * cycle);
    r.idle_to(a + (stop - 1) * cycle);
    CHECK(r.read(lptim1 + cnt) == 5);
    r.idle_to(a + (stop + 2) * cycle);
    CHECK(r.read(lptim1 + cnt) == 0);
    CHECK(r.pending() == std::nullopt);
  }
}

TEST_CASE("RM0351 34.4.7: CNTSTRT in one-shot mode keeps the counter going past the wrap")
{
  rig        r;
  auto const t = r.started(20, 5, sngstrt);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(a + 4 * cycle);
  CHECK(r.pending() == a + 6 * cycle);
  r.idle_to(a + 20 * cycle);
  CHECK(r.read(lptim1 + cnt) == 2);
}

TEST_CASE("inferred: a CNTSTRT reaching a counter the one-shot wrap stopped starts it")
{
  rig        r;
  auto const t = r.started(20, 5, sngstrt);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 5 * cycle);
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(a + 7 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  CHECK(r.pending() == a + 8 * cycle);
  r.idle_to(a + 10 * cycle);
  CHECK(r.read(lptim1 + cnt) == 2);
  r.idle_to(a + 20 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
}

TEST_CASE("inferred: CNTSTRT while counting continuously is discarded, no restart, no warning")
{
  rig        r;
  auto const t = r.started(20, 5);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + cr, enable | cntstrt);
  CHECK(r.pending() == a + 6 * cycle);
  r.idle_to(a + 3 * cycle);
  CHECK(r.read(lptim1 + cnt) == 3);
  CHECK(r.logged.empty());
}

TEST_CASE("inferred: a kernel rate change keeps the kernel clocks left before a mode switch")
{
  rig        r;
  auto const t = r.started(20, 100);
  auto const x = t + 4 * cycle;
  r.idle_to(x);
  r.write(lptim1 + cr, enable | sngstrt);
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (4u << 8));
  CHECK(r.pending() == x + 6 * cycle);
}

TEST_CASE("inferred: a SNGSTRT written with the kernel clock stopped lands 3 clocks after it runs")
{
  rig        r;
  auto const t = r.started(20, 5);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(apb1enr1, 0);
  r.write(lptim1 + cr, enable | sngstrt);
  r.idle_to(a + 10 * cycle);
  CHECK(r.pending() == std::nullopt);
  auto const e = r.now_ps();
  r.write(apb1enr1, 1u << 31);
  CHECK(r.pending() == e + 3 * cycle);
  r.idle_to(e + 6 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  CHECK(r.pending() == std::nullopt);
}

TEST_CASE(
    "checklist 53 policy: a second CNTSTRT or SNGSTRT before the switch replaces it, inferred")
{
  rig        r;
  auto const t = r.started(20, 5);
  auto const a = t + 3 * cycle;
  r.idle_to(a + cycle);
  r.write(lptim1 + cr, enable | sngstrt);
  r.write(lptim1 + cr, enable | cntstrt);
  r.write(lptim1 + cr, enable | cntstrt);
  CHECK(r.warnings("before the previous one reaches the counter") == 1);
  r.idle_to(a + 20 * cycle);
  CHECK(r.read(lptim1 + cnt) == 2);
  rig        strict{true};
  auto const s = strict.started(20, 5);
  strict.idle_to(s + 4 * cycle);
  strict.write(lptim1 + cr, enable | sngstrt);
  CHECK_THROWS_AS(
      static_cast<void>(strict.f->target->mem().write(lptim1 + cr, 4, enable | cntstrt)),
      popcorn::model_error);
}

TEST_CASE("ES0250 2.18.3: lptim1_out rises one kernel clock after CMP and falls after ARR")
{
  for(auto const scale : {1u, 2u})
  {
    CAPTURE(scale);
    rig        r{false, l476rc(), true};
    auto const t = r.started(2, 5, cntstrt, presc(scale == 1 ? 0 : 1));
    auto const a = t + 3 * cycle;
    auto const k = std::uint64_t{scale} * cycle;
    r.idle_to(a + 12 * k + 2 * cycle);
    CHECK(r.probe->pulses == std::vector{a + 2 * k + cycle, a + 8 * k + cycle});
    CHECK(r.probe->falls == std::vector{a + 5 * k + cycle, a + 11 * k + cycle});
  }
}

TEST_CASE("RM0351 34.4.9: WAVPOL inverts lptim1_out at once, also with ENABLE 0")
{
  rig        r{false, l476rc(), true};
  auto const x = r.now_ps();
  r.write(lptim1 + cfgr, wavpol);
  CHECK(r.probe->pulses == std::vector{x});
  CHECK(r.out_level() == true);
  r.idle_to(x + 2 * cycle);
  r.write(lptim1 + cfgr, 0);
  CHECK(r.probe->falls == std::vector{x + 2 * cycle});
  CHECK(r.out_level() == false);
  r.probe->pulses.clear();
  r.probe->falls.clear();
  auto const t = r.started(2, 5, cntstrt, wavpol);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 7 * cycle);
  CHECK(r.probe->falls == std::vector{a + 3 * cycle});
  CHECK(r.probe->pulses.size() == 2);
  CHECK(r.probe->pulses.back() == a + 6 * cycle);
}

TEST_CASE(
    "RM0351 34.4.9: one-pulse falls at ARR, set-once keeps its level and discards a restart, "
    "the software restart inferred")
{
  for(auto const config : {0u, wave})
  {
    CAPTURE(config);
    rig        r{false, l476rc(), true};
    auto const t = r.started(2, 5, sngstrt, config);
    auto const a = t + 3 * cycle;
    r.idle_to(a + 20 * cycle);
    CHECK(r.probe->pulses == std::vector{a + 3 * cycle});
    CHECK(r.probe->falls.size() == (config == wave ? 0u : 1u));
    CHECK(r.out_level() == (config == wave));
    CHECK(r.read(lptim1 + cnt) == 0);
    r.write(lptim1 + cr, enable | sngstrt);
    r.idle_to(a + 25 * cycle);
    CHECK(r.read(lptim1 + cnt) == (config == wave ? 0u : 2u));
    r.idle_to(a + 30 * cycle);
    CHECK(r.probe->pulses.size() == (config == wave ? 1u : 2u));
    if(config == wave)
    {
      auto const x = r.now_ps();
      r.write(lptim1 + cr, 0);
      CHECK(r.probe->falls == std::vector{x});
    }
  }
}

TEST_CASE("inferred: set-once latches lptim1_out with no sink listening and CMPM already set")
{
  rig r;
  static_cast<void>(r.started(2, 5));
  r.idle_to(r.now_ps() + 20 * cycle);
  REQUIRE((r.read(lptim1 + isr) & (cmpm | arrm)) == (cmpm | arrm));
  r.write(lptim1 + cr, 0);
  r.write(lptim1 + cfgr, wave);
  r.write(lptim1 + cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const t = r.now_ps();
  r.write(lptim1 + cr, enable | sngstrt);
  r.idle_to(t + 30 * cycle);
  CHECK(r.out_level() == true);
  CHECK(r.read(lptim1 + cnt) == 0);
}

TEST_CASE("inferred: ENABLE 0 drops lptim1_out at the write")
{
  rig        r{false, l476rc(), true};
  auto const t = r.started(2, 5, cntstrt);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 4 * cycle);
  auto const x = r.now_ps();
  r.write(lptim1 + cr, 0);
  r.idle_to(x + 5 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  CHECK(r.probe->falls == std::vector{x});
}

TEST_CASE("inferred: an LPTIM1RST pulse drops a high lptim1_out at the write")
{
  rig        r{false, l476rc(), true};
  auto const t = r.started(2, 5, cntstrt);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 4 * cycle);
  auto const x = r.now_ps();
  r.write(apb1rstr1, 1u << 31);
  r.write(apb1rstr1, 0);
  CHECK(r.probe->falls == std::vector{x});
  CHECK(r.out_level() == false);
}

TEST_CASE("each LPTIM drives its own output signal")
{
  rig r;
  for(auto const& [unit, own, other] : {
          std::tuple{"LPTIM1", "lptim1_out", "lptim2_out"},
          std::tuple{"LPTIM2", "lptim2_out", "lptim1_out"},
  })
  {
    CAPTURE(unit);
    auto* const source = dynamic_cast<stm32::trigger_source*>(r.f->target->model(unit));
    REQUIRE(source != nullptr);
    CHECK(source->produces(own));
    CHECK_FALSE(source->produces(other));
  }
}

TEST_CASE("a sink that starts listening after a long span sees edges from then on only")
{
  rig r{false, l476rc(), true};
  r.probe->listen(false);
  auto const t = r.started(2, 5, cntstrt);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 6000 * cycle + cycle);
  r.probe->listening = true;
  CHECK(r.read(lptim1 + cnt) == 1);
  CHECK(r.probe->pulses.empty());
  CHECK(r.probe->falls.empty());
  r.idle_to(a + 6012 * cycle);
  CHECK(r.probe->pulses == std::vector{a + 6003 * cycle, a + 6009 * cycle});
  CHECK(r.probe->falls == std::vector{a + 6006 * cycle, a + 6012 * cycle});
}

TEST_CASE("a snapshot restored under a listening sink gives it the next lptim1_out edges")
{
  rig        r{false, l476rc(), true};
  auto const t = r.started(2, 5, cntstrt);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 4 * cycle);
  auto const saved = r.f->target->snapshot();
  rig        other{false, l476rc(), true};
  other.probe->listen(false);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.probe->listening = true;
  other.idle_to(a + 13 * cycle);
  CHECK(other.probe->pulses == std::vector{a + 9 * cycle});
  CHECK(other.probe->falls == std::vector{a + 6 * cycle, a + 12 * cycle});
}

TEST_CASE("inferred: CMP equal to ARR keeps lptim1_out inactive")
{
  rig        r{false, l476rc(), true};
  auto const t = r.started(5, 5, cntstrt);
  r.idle_to(t + 30 * cycle);
  CHECK((r.read(lptim1 + isr) & (cmpm | arrm)) == (cmpm | arrm));
  CHECK(r.probe->pulses.empty());
  CHECK(r.out_level() == false);
}

TEST_CASE("RM0351 34.4.6 note: LL firmware on LSI, CMPM 19 ticks after CNTSTRT, then every 32")
{
  firmware_run run{"lptim1_continuous"};
  auto const   started = run.started();
  CHECK(run.finish() == std::vector<std::uint32_t>{10, 10});
  REQUIRE(run.compares.size() == 10);
  REQUIRE(run.reloads.size() == 10);
  CHECK(run.compares.front() - started > 19 * lsi_tick - us);
  CHECK(run.compares.front() - started < 19 * lsi_tick + us);
  CHECK(gaps(run.compares) == std::vector<std::uint64_t>(9, 32 * lsi_tick));
  CHECK(gaps(run.reloads) == std::vector<std::uint64_t>(9, 32 * lsi_tick));
  CHECK(run.reloads.front() - run.compares.front() >= 16 * lsi_tick);
  CHECK(run.reloads.front() - run.compares.front() < 16 * lsi_tick + us);
}

TEST_CASE("RM0351 34.4.7: LL firmware SNGSTRT gives one CMPM and one ARRM, then CNT 0")
{
  firmware_run run{"lptim1_one_shot"};
  auto const   started = run.started();
  CHECK(run.finish() == std::vector<std::uint32_t>{1, 1, 0});
  REQUIRE(run.reloads.size() == 1);
  REQUIRE(run.compares.size() == 1);
  CHECK(run.reloads.front() - started > 35 * lsi_tick - us);
  CHECK(run.reloads.front() - started < 35 * lsi_tick + us);
}
}

TEST_CASE("RM0351 48.16.4: DBG_LPTIM1_STOP freezes CNT, tick phase and CMPM due while held")
{
  rig        r;
  auto const t = r.started(6, 100, cntstrt, presc(2), cmpm);
  auto const a = t + 3 * cycle;
  REQUIRE(r.pending() == a + 25 * cycle);
  auto const x = a + 17 * cycle;
  r.idle_to(x);
  REQUIRE(r.read(lptim1 + cnt) == 4);
  set_debug_freeze(*r.f, "DBG_LPTIM1_STOP", true);
  r.f->target->hold_core(true);
  CHECK(r.pending() == std::nullopt);
  r.idle_to(x + 40 * cycle);
  CHECK(r.read(lptim1 + cnt) == 4);
  CHECK((r.read(lptim1 + isr) & cmpm) == 0);
  r.f->target->hold_core(false);
  auto const span = r.now_ps() - x;
  CHECK(r.pending() == a + 25 * cycle + span);
  r.idle_to(x + span + 2 * cycle);
  CHECK(r.read(lptim1 + cnt) == 4);
  r.idle_to(x + span + 3 * cycle);
  CHECK(r.read(lptim1 + cnt) == 5);
  r.idle_to(a + 25 * cycle + span);
  CHECK((r.read(lptim1 + isr) & cmpm) == cmpm);
  CHECK((r.read(ispr2) & lptim1_irq) == lptim1_irq);
}

TEST_CASE("RM0351 48.16.5: DBG_LPTIM2_STOP freezes LPTIM2 only, LPTIM1 counts on while held")
{
  rig r;
  r.write(lptim2 + cr, enable);
  r.idle_to(r.now_ps() + 300 * cycle);
  r.write(lptim2 + arr, 100);
  r.write(lptim2 + cmp, 30);
  r.idle_to(r.now_ps() + 4 * cycle);
  r.write(lptim2 + icr, cmpok | arrok);
  static_cast<void>(r.started(30, 100));
  r.write(lptim2 + cr, enable | cntstrt);
  r.idle_to(r.now_ps() + 5 * cycle);
  auto const first  = r.pending();
  auto const second = r.pending("LPTIM2");
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  auto const x  = r.now_ps();
  auto const c1 = r.read(lptim1 + cnt);
  auto const c2 = r.read(lptim2 + cnt);
  set_debug_freeze(*r.f, "DBG_LPTIM2_STOP", true);
  r.f->target->hold_core(true);
  CHECK(r.pending() == first);
  CHECK(r.pending("LPTIM2") == std::nullopt);
  r.idle_to(x + 10 * cycle);
  CHECK(r.read(lptim1 + cnt) == c1 + 10);
  CHECK(r.read(lptim2 + cnt) == c2);
  r.f->target->hold_core(false);
  CHECK(r.pending("LPTIM2") == *second + 10 * cycle);
}

TEST_CASE("RM0351 48.16.4: DBG_LPTIM1_STOP 0 leaves LPTIM1 counting while the core is held")
{
  rig        r;
  auto const t = r.started(30, 100);
  auto const a = t + 3 * cycle;
  r.idle_to(a + 5 * cycle);
  set_debug_freeze(*r.f, "DBG_LPTIM1_STOP", false);
  r.f->target->hold_core(true);
  CHECK(r.pending() == a + 31 * cycle);
  r.idle_to(a + 15 * cycle);
  CHECK(r.read(lptim1 + cnt) == 15);
  r.f->target->hold_core(false);
}

TEST_CASE("inferred: a PCLK1 rate change while held keeps LPTIM1 frozen, the release runs at it")
{
  rig        r;
  auto const t = r.started(30, 100);
  auto const a = t + 3 * cycle;
  auto const x = a + 5 * cycle;
  r.idle_to(x);
  set_debug_freeze(*r.f, "DBG_LPTIM1_STOP", true);
  r.f->target->hold_core(true);
  r.write(rcc_cfgr, r.read(rcc_cfgr) | (4u << 8));
  r.idle_to(x + 20 * cycle);
  CHECK(r.read(lptim1 + cnt) == 5);
  CHECK(r.pending() == std::nullopt);
  r.f->target->hold_core(false);
  CHECK(r.pending() == r.now_ps() + 26 * 2 * cycle);
}

TEST_CASE("RM0351 34.4.14: the DBG_LPTIM1_STOP hold is kept across an LPTIM1RST pulse")
{
  rig r;
  set_debug_freeze(*r.f, "DBG_LPTIM1_STOP", true);
  r.f->target->hold_core(true);
  r.write(apb1rstr1, 1u << 31);
  r.write(apb1rstr1, 0);
  r.write(lptim1 + cr, enable);
  r.write(lptim1 + arr, 10);
  r.idle_to(r.now_ps() + 50 * cycle);
  CHECK((r.read(lptim1 + isr) & arrok) == 0);
  CHECK(r.pending() == std::nullopt);
  r.f->target->hold_core(false);
  auto const x = r.now_ps();
  CHECK(r.pending() == x + 3 * cycle);
  r.idle_to(x + 3 * cycle);
  CHECK((r.read(lptim1 + isr) & arrok) == arrok);
}

TEST_CASE("RM0351 48.16.4: a power-on reset clears DBG_LPTIM1_STOP, LPTIM1 runs while held")
{
  rig r;
  set_debug_freeze(*r.f, "DBG_LPTIM1_STOP", true);
  r.f->target->hold_core(true);
  r.f->target->reset(popcorn::reset_kind::power_on);
  r.write(apb1enr1, 1u << 31);
  r.write(lptim1 + cr, enable);
  auto const x = r.now_ps();
  r.write(lptim1 + arr, 10);
  CHECK(r.pending() == x + 3 * cycle);
  r.idle_to(x + 3 * cycle);
  CHECK((r.read(lptim1 + isr) & arrok) == arrok);
  r.f->target->hold_core(false);
}

TEST_CASE("RM0351 48.16.4: a system reset keeps DBG_LPTIM1_STOP, LPTIM1 stays frozen while held")
{
  rig r;
  set_debug_freeze(*r.f, "DBG_LPTIM1_STOP", true);
  r.f->target->hold_core(true);
  r.f->target->reset(popcorn::reset_kind::software);
  r.write(apb1enr1, 1u << 31);
  r.write(lptim1 + cr, enable);
  r.write(lptim1 + arr, 10);
  r.idle_to(r.now_ps() + 50 * cycle);
  CHECK((r.read(lptim1 + isr) & arrok) == 0);
  CHECK(r.pending() == std::nullopt);
  r.f->target->hold_core(false);
  auto const x = r.now_ps();
  CHECK(r.pending() == x + 3 * cycle);
}

namespace
{
auto etr_pin(rig const& r, std::string_view port, std::uint8_t pin, std::uint32_t af)
    -> stm32::gpio_port&
{
  auto const base = port == "GPIOA" ? address{0x4800'0000} : address{0x4800'0400};
  r.write(ahb2enr, r.read(ahb2enr) | (port == "GPIOA" ? 1u : 2u));
  auto* const found = r.f->model_of<stm32::gpio_port>(port);
  REQUIRE(found != nullptr);
  found->drive(pin, false);
  r.write(base, (r.read(base) & ~(3u << (2 * pin))) | (2u << (2 * pin)));
  r.write(base + 0x20, (r.read(base + 0x20) & ~(15u << (4 * pin))) | (af << (4 * pin)));
  return *found;
}

auto lptim1_etr(rig const& r) -> stm32::gpio_port&
{
  return etr_pin(r, "GPIOB", 6, 1);
}
}

TEST_CASE("RM0351 34.4.6: a rising PB6 LPTIM1_ETR edge starts CNTSTRT two clocks later, EXTTRIG")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(6, 100, cntstrt, rising, exttrig);
  r.idle_to(t + 20 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  CHECK(r.read(lptim1 + isr) == 0);
  auto const e = r.now_ps() + 2 * cycle;
  pins.drive_at(6, true, vtime{e});
  r.idle_to(e);
  CHECK((r.read(lptim1 + isr) & exttrig) == exttrig);
  CHECK((r.read(ispr2) & lptim1_irq) == lptim1_irq);
  CHECK(r.pending() == e + 2 * cycle + 6 * cycle + cycle);
  r.idle_to(e + 2 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  r.idle_to(e + 3 * cycle);
  CHECK(r.read(lptim1 + cnt) == 1);
}

TEST_CASE("RM0351 34.7.4 TRIGEN: 10 starts on the falling edge only, 11 on either edge")
{
  for(auto const& [config, first_starts] : {
          std::pair{falling,    false},
          std::pair{both_edges, true }
  })
  {
    CAPTURE(config);
    rig        r;
    auto&      pins = lptim1_etr(r);
    auto const t    = r.started(6, 100, cntstrt, config);
    auto const e    = t + 20 * cycle;
    pins.drive_at(6, true, vtime{e});
    pins.drive_at(6, false, vtime{e + 10 * cycle});
    r.idle_to(e + 5 * cycle);
    CHECK(r.read(lptim1 + cnt) == (first_starts ? 3u : 0u));
    r.idle_to(e + 15 * cycle);
    CHECK(r.read(lptim1 + cnt) == (first_starts ? 13u : 3u));
  }
}

TEST_CASE("RM0351 34.4.6: a trigger while counting is ignored and sets no EXTTRIG")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(60, 100, cntstrt, rising);
  auto const e    = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  pins.drive_at(6, false, vtime{e + 5 * cycle});
  pins.drive_at(6, true, vtime{e + 10 * cycle});
  r.idle_to(e + 6 * cycle);
  r.write(lptim1 + icr, exttrig);
  r.idle_to(e + 20 * cycle);
  CHECK(r.read(lptim1 + isr) == 0);
  CHECK(r.read(lptim1 + cnt) == 18);
}

TEST_CASE("RM0351 34.4.8: with TIMOUT 1 a trigger while counting restarts CNT from 0")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(30, 100, cntstrt, rising | timout, cmpm);
  auto const e    = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  pins.drive_at(6, false, vtime{e + 5 * cycle});
  pins.drive_at(6, true, vtime{e + 20 * cycle});
  r.idle_to(e + 6 * cycle);
  r.write(lptim1 + icr, exttrig);
  r.idle_to(e + 20 * cycle);
  CHECK((r.read(lptim1 + isr) & exttrig) == exttrig);
  CHECK(r.pending() == e + 22 * cycle + 31 * cycle);
  r.idle_to(e + 22 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  r.idle_to(e + 25 * cycle);
  CHECK(r.read(lptim1 + cnt) == 3);
}

TEST_CASE("RM0351 34.7.4 TRGFLT: 01, 10 and 11 delay EXTTRIG and the start by 2, 4 and 8 clocks")
{
  for(std::uint32_t code = 1; code <= 3; ++code)
  {
    CAPTURE(code);
    rig        r;
    auto&      pins  = lptim1_etr(r);
    auto const delay = (1u << code) * cycle;
    auto const t     = r.started(6, 100, cntstrt, rising | code << 6, cmpm);
    auto const e     = t + 20 * cycle;
    pins.drive_at(6, true, vtime{e});
    r.idle_to(e + delay - cycle);
    CHECK((r.read(lptim1 + isr) & exttrig) == 0);
    r.idle_to(e + delay);
    CHECK((r.read(lptim1 + isr) & exttrig) == exttrig);
    CHECK(r.pending() == e + delay + 9 * cycle);
  }
}

TEST_CASE("RM0351 34.4.7: SNGSTRT with a trigger counts once to ARR, the next trigger restarts it")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(3, 10, sngstrt, rising);
  auto const e    = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  pins.drive_at(6, false, vtime{e + 4 * cycle});
  pins.drive_at(6, true, vtime{e + 6 * cycle});
  pins.drive_at(6, false, vtime{e + 30 * cycle});
  pins.drive_at(6, true, vtime{e + 40 * cycle});
  r.idle_to(e + 8 * cycle);
  CHECK(r.read(lptim1 + cnt) == 6);
  r.idle_to(e + 20 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
  r.write(lptim1 + icr, exttrig | arrm | cmpm);
  r.idle_to(e + 45 * cycle);
  CHECK((r.read(lptim1 + isr) & exttrig) == exttrig);
  CHECK(r.read(lptim1 + cnt) == 3);
}

TEST_CASE("RM0351 34.4.7: with WAVE 1 only the first trigger starts the one-shot run")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(3, 10, sngstrt, rising | wave);
  auto const e    = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  pins.drive_at(6, false, vtime{e + 30 * cycle});
  pins.drive_at(6, true, vtime{e + 40 * cycle});
  r.idle_to(e + 20 * cycle);
  r.write(lptim1 + icr, exttrig | arrm | cmpm);
  r.idle_to(e + 45 * cycle);
  CHECK(r.read(lptim1 + isr) == 0);
  CHECK(r.read(lptim1 + cnt) == 0);
}

TEST_CASE("inferred: a trigger before CNTSTRT or SNGSTRT starts nothing and sets no EXTTRIG")
{
  rig   r;
  auto& pins = lptim1_etr(r);
  r.write(lptim1 + cfgr, rising);
  r.write(lptim1 + cr, enable);
  r.idle_to(r.now_ps() + 300 * cycle);
  auto const e = r.now_ps() + 2 * cycle;
  pins.drive_at(6, true, vtime{e});
  r.idle_to(e + 10 * cycle);
  CHECK(r.read(lptim1 + isr) == 0);
  r.write(lptim1 + cr, enable | cntstrt);
  r.idle_to(e + 20 * cycle);
  CHECK(r.read(lptim1 + cnt) == 0);
}

TEST_CASE("RM0351 34.4.6: a rising PA5 LPTIM2_ETR edge starts LPTIM2, LPTIM1 stays at 0")
{
  rig   r;
  auto& pins = etr_pin(r, "GPIOA", 5, 14);
  for(auto const base : {lptim1, lptim2})
  {
    r.write(base + cfgr, rising);
    r.write(base + cr, enable);
  }
  r.idle_to(r.now_ps() + 300 * cycle);
  for(auto const base : {lptim1, lptim2})
  {
    r.write(base + arr, 100);
    r.idle_to(r.now_ps() + 4 * cycle);
    r.write(base + cr, enable | cntstrt);
  }
  auto const e = r.now_ps() + 2 * cycle;
  pins.drive_at(5, true, vtime{e});
  r.idle_to(e + 12 * cycle);
  CHECK(r.read(lptim2 + cnt) == 10);
  CHECK((r.read(lptim2 + isr) & exttrig) == exttrig);
  CHECK(r.read(lptim1 + cnt) == 0);
  CHECK((r.read(lptim1 + isr) & exttrig) == 0);
}

TEST_CASE(
    "RM0351 34.4.4: at TRGFLT 11 a 2-clock pulse while counting with TIMOUT 1 restarts nothing")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(60, 100, cntstrt, rising | timout | 3u << 6);
  auto const e    = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  pins.drive_at(6, false, vtime{e + 10 * cycle});
  pins.drive_at(6, true, vtime{e + 20 * cycle});
  pins.drive_at(6, false, vtime{e + 22 * cycle});
  r.idle_to(e + 12 * cycle);
  REQUIRE(r.read(lptim1 + cnt) == 2);
  r.write(lptim1 + icr, exttrig);
  r.idle_to(e + 40 * cycle);
  CHECK((r.read(lptim1 + isr) & exttrig) == 0);
  CHECK(r.read(lptim1 + cnt) == 30);
}

TEST_CASE(
    "RM0351 34.7.4 TRGFLT: a pulse shorter than 2 or 8 clocks starts nothing, one of 2 or 8 does")
{
  for(std::uint32_t code : {1u, 3u})
  {
    CAPTURE(code);
    auto const n = std::uint64_t{1} << code;
    for(auto const width : {n - 1, n})
    {
      CAPTURE(width);
      rig        r;
      auto&      pins = lptim1_etr(r);
      auto const t    = r.started(60, 100, cntstrt, rising | code << 6);
      auto const e    = t + 20 * cycle;
      pins.drive_at(6, true, vtime{e});
      pins.drive_at(6, false, vtime{e + width * cycle});
      r.idle_to(e + 30 * cycle);
      auto const started = width == n;
      CHECK((r.read(lptim1 + isr) & exttrig) == (started ? exttrig : 0u));
      CHECK(r.read(lptim1 + cnt) == (started ? 30u - n - 2u : 0u));
    }
  }
}

TEST_CASE("RM0351 34.4.4: with TRIGEN 11 and TRGFLT 11 a 2-clock pulse starts nothing")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(60, 100, cntstrt, both_edges | 3u << 6);
  auto const e    = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  pins.drive_at(6, false, vtime{e + 2 * cycle});
  r.idle_to(e + 30 * cycle);
  CHECK(r.read(lptim1 + isr) == 0);
  CHECK(r.read(lptim1 + cnt) == 0);
}

TEST_CASE("an LPTIM1RST pulse with PB6 high keeps the ETR level, TRIGEN 10 starts on its fall")
{
  for(auto const high : {true, false})
  {
    CAPTURE(high);
    rig   r;
    auto& pins = lptim1_etr(r);
    pins.drive(6, high);
    r.write(apb1rstr1, 1u << 31);
    r.write(apb1rstr1, 0);
    auto const t = r.started(60, 100, cntstrt, falling);
    auto const e = t + 20 * cycle;
    pins.drive_at(6, false, vtime{e});
    r.idle_to(e + 12 * cycle);
    CHECK((r.read(lptim1 + isr) & exttrig) == (high ? exttrig : 0u));
    CHECK(r.read(lptim1 + cnt) == (high ? 10u : 0u));
  }
}
