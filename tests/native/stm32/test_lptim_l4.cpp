#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware   = "l476_l4_rcc_ll";
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
constexpr address          ispr2      = 0xE000'E208;
constexpr std::uint32_t    cmpm       = 1u << 0;
constexpr std::uint32_t    arrm       = 1u << 1;
constexpr std::uint32_t    cmpok      = 1u << 3;
constexpr std::uint32_t    arrok      = 1u << 4;
constexpr std::uint32_t    enable     = 1u << 0;
constexpr std::uint32_t    cntstrt    = 1u << 2;
constexpr std::uint32_t    lptim1_irq = 1u << (65 - 64);
constexpr std::uint64_t    cycle      = 250'000;

constexpr auto presc(std::uint32_t value) -> std::uint32_t
{
  return value << 9;
}

struct rig
{
  std::unique_ptr<stm32_fixture> f;

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  explicit rig(bool strict = false, popcorn::part_description const& desc = l476rc())
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(stm32::board{}, desc);
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(desc, firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
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

}
