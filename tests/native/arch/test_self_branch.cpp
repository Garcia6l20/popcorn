#include "arch_fixture.hpp"

#include "../../../native/src/core/machine_impl.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::sym;

constexpr address       narrow_loop  = 0x2004'0000;
constexpr address       wide_loop    = 0x2004'0010;
constexpr address       icsr         = 0xE000'ED04;
constexpr address       syst_csr     = 0xE000'E010;
constexpr address       syst_rvr     = 0xE000'E014;
constexpr address       syst_cvr     = 0xE000'E018;
constexpr address       demcr        = 0xE000'EDFC;
constexpr address       dwt_ctrl     = 0xE000'1000;
constexpr address       dwt_cyccnt   = 0xE000'1004;
constexpr std::uint64_t ps_per_instr = 250'000;
constexpr auto          long_wall    = std::chrono::seconds{60};

enum class leg
{
  fast,
  coverage,
  ring_blocks,
  probe,
  spin,
};

auto fn(std::string_view name) -> address
{
  return sym("timers", name);
}

void put(arch_fixture const& f, address at, std::uint32_t value, std::uint32_t size = 4)
{
  REQUIRE(f.target->mem().write(at, size, value));
}

auto get(arch_fixture const& f, address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto skips(arch_fixture const& f) -> std::uint64_t
{
  return core::machine_internals::counters(*f.target).self_branch_skips;
}

struct spinning
{
  arch_fixture                      f;
  std::optional<core::probe_handle> probe{};
};

auto spinning_at(address loop, leg how) -> spinning
{
  core::machine_options options;
  options.trace_capacity = 16384;
  if(how == leg::spin)
  {
    options.spin_threshold = 64;
  }
  spinning s{
      .f = arch_fixture{m33(), "timers", std::move(options)}
  };
  auto& f = s.f;
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.set("pc", fn("timers_setup"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  put(f, narrow_loop, 0xe7fe, 2);
  put(f, wide_loop, 0xf7ff, 2);
  put(f, wide_loop + 2, 0xbffe, 2);
  put(f, demcr, 1u << 24);
  put(f, dwt_ctrl, 1);
  put(f, syst_rvr, 3999);
  put(f, syst_cvr, 0);
  put(f, syst_csr, 7);
  f.set("basepri", 0x40);
  f.set("pc", loop | 1u);
  switch(how)
  {
    case leg::coverage:
      f.target->enable_coverage({
          {.name = "loops", .start = narrow_loop, .size = 0x20}
      });
      break;
    case leg::ring_blocks:
      f.target->set_ring_blocks(true);
      break;
    case leg::probe:
      s.probe = f.target->add_probe(loop);
      break;
    default:
      break;
  }
  return s;
}

struct outputs
{
  std::optional<stop_reason> reason;
  std::uint64_t              instructions{};
  std::uint64_t              executed{};
  std::uint64_t              now{};
  std::uint64_t              ps{};
  std::uint64_t              pc{};
  std::uint32_t              cvr{};
  std::uint32_t              icsr{};
  std::uint32_t              cyccnt{};
  std::uint32_t              ticks{};

  friend auto operator==(outputs const&, outputs const&) -> bool = default;
};

auto run_once(arch_fixture const& f, core::slice_request request) -> outputs
{
  request.wall_budget = long_wall;
  outputs       out;
  std::uint64_t executed = 0;
  for(;;)
  {
    auto const slice = f.run(request);
    executed += slice.executed.value;
    if(slice.stopped)
    {
      out.reason       = slice.stopped->reason;
      out.instructions = slice.stopped->instructions.value;
      break;
    }
    if(request.budget_left)
    {
      request.budget_left = icount{request.budget_left->value - slice.executed.value};
    }
    if(request.exact_instructions)
    {
      request.exact_instructions = icount{request.exact_instructions->value - slice.executed.value};
    }
  }
  out.executed = executed;
  out.now      = f.target->virtual_clock().now().value;
  out.ps       = f.target->virtual_clock().time().ps;
  out.pc       = f.reg("pc");
  out.cvr      = get(f, syst_cvr);
  out.icsr     = get(f, icsr);
  out.cyccnt   = get(f, dwt_cyccnt);
  out.ticks    = f.word(fn("ticks"));
  return out;
}

auto tick_entries(arch_fixture const& f) -> std::vector<std::uint64_t>
{
  auto&                      ring = f.target->trace();
  std::uint64_t              at   = ring.origin().value;
  std::vector<std::uint64_t> out;
  for(auto const& record : ring.records())
  {
    at += record.dt;
    if(record.kind == core::trace_kind::irq and record.flags == 1 and record.aux == 15)
    {
      out.push_back(at);
    }
  }
  return out;
}

struct history
{
  std::vector<outputs>       phases;
  std::vector<std::uint64_t> entries;
  std::uint64_t              skips{};
};

auto run_phases(address loop, leg how) -> history
{
  auto        s = spinning_at(loop, how);
  auto const& f = s.f;
  history     h;
  h.phases.push_back(run_once(f, {.budget_left = icount{50'000}}));
  auto const until = vtime{f.target->virtual_clock().time().ps + 30'001 * ps_per_instr};
  h.phases.push_back(run_once(f, {.max_instructions = icount{1'000'000}, .until_time = until}));
  h.phases.push_back(run_once(f, {.exact_instructions = icount{12'345}}));
  f.set("basepri", 0);
  h.phases.push_back(run_once(f, {.budget_left = icount{20'003}}));
  h.phases.push_back(run_once(f, {.max_instructions = icount{777}, .budget_left = icount{9'000}}));
  h.entries = tick_entries(f);
  h.skips   = skips(f);
  return h;
}
}

TEST_CASE("a b . skipped to each event and bound gives the outputs of the run executing it")
{
  for(auto const loop : {narrow_loop, wide_loop})
  {
    CAPTURE(loop);
    auto const fast = run_phases(loop, leg::fast);
    auto const slow = run_phases(loop, leg::coverage);
    CHECK(fast.skips > 0);
    CHECK(slow.skips == 0);
    REQUIRE(fast.phases.size() == slow.phases.size());
    for(std::size_t index = 0; index < fast.phases.size(); ++index)
    {
      CAPTURE(index);
      auto const& a = fast.phases[index];
      auto const& b = slow.phases[index];
      CHECK(a.reason == b.reason);
      CHECK(a.instructions == b.instructions);
      CHECK(a.executed == b.executed);
      CHECK(a.now == b.now);
      CHECK(a.ps == b.ps);
      CHECK(a.pc == b.pc);
      CHECK(a.cvr == b.cvr);
      CHECK(a.icsr == b.icsr);
      CHECK(a.cyccnt == b.cyccnt);
      CHECK(a.ticks == b.ticks);
    }
    CHECK(fast.phases[0].reason == stop_reason::budget);
    CHECK(fast.phases[0].ticks == 0);
    CHECK((fast.phases[0].icsr & (1u << 26)) != 0);
    CHECK(fast.phases[1].reason == stop_reason::time);
    CHECK(fast.phases[2].reason == stop_reason::budget);
    CHECK(fast.phases[2].executed == 12'345);
    CHECK(fast.phases[3].ticks > fast.phases[2].ticks + 3);
    CHECK(fast.entries.size() >= 5);
    CHECK(fast.entries == slow.entries);
  }
}

TEST_CASE("a probe, ring block records or spin detection on the b . keep every iteration")
{
  SUBCASE("a probe counts each iteration")
  {
    auto       s   = spinning_at(narrow_loop, leg::probe);
    auto const out = run_once(s.f, {.budget_left = icount{10'000}});
    CHECK(out.reason == stop_reason::budget);
    CHECK(skips(s.f) == 0);
    CHECK(s.probe->hits->count() == out.executed);
  }
  SUBCASE("ring block records")
  {
    auto const fast = spinning_at(narrow_loop, leg::fast);
    auto const out  = run_once(fast.f, {.budget_left = icount{10'000}});
    auto const s    = spinning_at(narrow_loop, leg::ring_blocks);
    CHECK(run_once(s.f, {.budget_left = icount{10'000}}) == out);
    CHECK(skips(s.f) == 0);
    CHECK(skips(fast.f) > 0);
  }
  SUBCASE("spin detection still stops the run")
  {
    auto const s   = spinning_at(narrow_loop, leg::spin);
    auto const out = run_once(s.f, {.budget_left = icount{10'000}});
    CHECK(out.reason == stop_reason::spin);
    CHECK(skips(s.f) == 0);
  }
}

TEST_CASE("an until_return at the b . stops as the run executing it does, also when resumed there")
{
  auto const run = [](leg how) {
    auto                 s  = spinning_at(narrow_loop, how);
    auto const           sp = s.f.reg("sp");
    std::vector<outputs> out;
    out.push_back(run_once(s.f, {.budget_left = icount{1'000}}));
    out.push_back(run_once(
        s.f, {
                 .budget_left  = icount{10'000},
                 .until_return = core::frame_return{.ret = narrow_loop, .sp = sp}
    }));
    return out;
  };
  auto const fast = run(leg::fast);
  CHECK(fast == run(leg::coverage));
  CHECK(fast[1].reason == stop_reason::returned);
}

TEST_CASE("coverage counts every iteration of the b .")
{
  auto       s   = spinning_at(narrow_loop, leg::coverage);
  auto const out = run_once(s.f, {.budget_left = icount{10'000}});
  CHECK(skips(s.f) == 0);
  auto const data = s.f.target->coverage();
  REQUIRE(data.blocks.size() == 1);
  REQUIRE(not data.blocks[0].hits.empty());
  CHECK(data.blocks[0].hits[0] == out.executed);
}

TEST_CASE("a b . under a masked SysTick spins one virtual second in one skip per tick")
{
  auto       s      = spinning_at(narrow_loop, leg::fast);
  auto const until  = vtime{s.f.target->virtual_clock().time().ps + 1'000'000'000'000};
  auto const runs   = core::machine_internals::counters(*s.f.target).engine_runs;
  auto const out    = run_once(s.f, {.max_instructions = icount{8'000'000}, .until_time = until});
  auto const engine = core::machine_internals::counters(*s.f.target).engine_runs - runs;
  CHECK(out.reason == stop_reason::time);
  CHECK(out.executed == 4'000'000);
  CHECK(engine <= 1010);
  CHECK(skips(s.f) >= 990);
}

TEST_CASE("a b . that an IT block makes conditional and fails falls through, resumed there")
{
  for(auto const how : {leg::fast, leg::coverage})
  {
    auto s = spinning_at(narrow_loop, how);
    REQUIRE(run_once(s.f, {.budget_left = icount{100}}).pc == narrow_loop);
    s.f.set("xpsr", (s.f.reg("xpsr") & ~std::uint64_t{0xf000'0000}) | (1u << 11));
    auto const before = skips(s.f);
    auto const out    = run_once(s.f, {.budget_left = icount{1}});
    CHECK(out.pc == narrow_loop + 2);
    CHECK(skips(s.f) == before);
  }
}
