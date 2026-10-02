#include <popcorn/core/coverage.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

auto fn(std::string_view name) -> address
{
  return sym("branches", name);
}

auto booted() -> fixture
{
  fixture f{m33(), "branches", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto function_range() -> popcorn::address_range
{
  return {
      .name = "branches", .start = fn("branches"), .size = fn("branch_end") + 2 - fn("branches")};
}

void run_branches(fixture const& f, std::uint32_t n, std::string_view entry = "branches")
{
  f.set("pc", fn(entry));
  f.set("r0", n);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
}

auto hits_at(core::coverage_data const& data, address at) -> std::uint32_t
{
  for(auto const& counters : data.blocks)
  {
    if(counters.range.start <= at and at < counters.range.start + counters.range.size)
    {
      return counters.hits[(at - counters.range.start) >> 1];
    }
  }
  return 0;
}

auto edge(core::coverage_data const& data, std::string_view from, std::string_view to)
    -> std::uint64_t
{
  auto const found = std::ranges::find_if(data.edges, [&](core::edge_counter const& e) {
    return e.from == fn(from) and e.to == fn(to);
  });
  return found == data.edges.end() ? 0 : found->hits;
}
}

TEST_CASE("block and edge counters equal the taken counts of each branch side")
{
  auto f = booted();
  f.target->enable_coverage({function_range()});
  run_branches(f, 10);
  auto const data = f.target->coverage();
  CHECK(hits_at(data, fn("branches")) == 1);
  CHECK(hits_at(data, fn("branch_top")) == 9);
  CHECK(hits_at(data, fn("branch_odd")) == 5);
  CHECK(hits_at(data, fn("branch_even")) == 5);
  CHECK(hits_at(data, fn("branch_join")) == 10);
  CHECK(hits_at(data, fn("branch_end")) == 1);
  CHECK(edge(data, "branches", "branch_even") == 1);
  CHECK(edge(data, "branch_top", "branch_odd") == 5);
  CHECK(edge(data, "branch_top", "branch_even") == 4);
  CHECK(edge(data, "branch_odd", "branch_join") == 5);
  CHECK(edge(data, "branch_even", "branch_join") == 5);
  CHECK(edge(data, "branch_join", "branch_top") == 9);
  CHECK(edge(data, "branch_join", "branch_end") == 1);
  CHECK(data.edges.size() == 7);
  CHECK(std::ranges::is_sorted(data.edges, {}, [](core::edge_counter const& e) {
    return std::pair{e.from, e.to};
  }));
}

TEST_CASE("coverage over one function counts nothing outside it and no edge leaves it")
{
  auto f = booted();
  f.target->enable_coverage({function_range()});
  run_branches(f, 4, "outside");
  auto const data = f.target->coverage();
  REQUIRE(data.blocks.size() == 1);
  CHECK(hits_at(data, fn("branches")) == 1);
  auto const range = function_range();
  for(auto const& e : data.edges)
  {
    CHECK(e.from >= range.start);
    CHECK(e.from < range.start + range.size);
    CHECK(e.to >= range.start);
    CHECK(e.to < range.start + range.size);
  }
  CHECK(edge(data, "branches", "branch_even") == 1);
}

TEST_CASE("enabling again keeps the counters of held ranges and zeroes new ones")
{
  auto f = booted();
  f.target->enable_coverage({function_range()});
  run_branches(f, 10);
  auto const top  = fn("branch_top");
  auto const tail = popcorn::address_range{.name = "tail", .start = top, .size = 0x10};
  f.target->enable_coverage({function_range(), tail});
  auto const data = f.target->coverage();
  REQUIRE(data.blocks.size() == 2);
  CHECK(data.blocks[0].hits[(top - data.blocks[0].range.start) >> 1] == 9);
  CHECK(std::ranges::all_of(data.blocks[1].hits, [](std::uint32_t h) { return h == 0; }));
  run_branches(f, 10);
  auto const again = f.target->coverage();
  CHECK(again.blocks[0].hits[(top - again.blocks[0].range.start) >> 1] == 18);
  CHECK(again.blocks[1].hits[0] == 9);
}

TEST_CASE("coverage accumulates across restores and clears on request")
{
  auto       f     = booted();
  auto const taken = f.target->snapshot();
  f.target->enable_coverage({function_range()});
  run_branches(f, 10);
  f.target->restore(taken);
  run_branches(f, 10);
  CHECK(hits_at(f.target->coverage(), fn("branch_join")) == 20);
  CHECK(edge(f.target->coverage(), "branch_join", "branch_top") == 18);
  f.target->disable_coverage();
  CHECK_FALSE(f.target->coverage_enabled());
  run_branches(f, 10);
  CHECK(hits_at(f.target->coverage(), fn("branch_join")) == 20);
  f.target->clear_coverage();
  CHECK(hits_at(f.target->coverage(), fn("branch_join")) == 0);
  CHECK(f.target->coverage().edges.empty());
  CHECK(f.target->coverage().blocks.size() == 1);
}

TEST_CASE("empty ranges cover every executable segment of the loaded images")
{
  auto f = booted();
  f.target->enable_coverage({});
  run_branches(f, 10);
  auto const data = f.target->coverage();
  REQUIRE_FALSE(data.blocks.empty());
  CHECK(hits_at(data, fn("branch_join")) == 10);
}

TEST_CASE("handlers read the counts so far, other callers during a slice cannot")
{
  auto f = booted();
  f.target->enable_coverage({function_range()});
  std::optional<std::uint32_t> seen;
  auto const                   hook =
      f.target->add_intercept(fn("branch_end"), [&](core::intercept_call const& call) {
        seen = hits_at(call.target().coverage(), fn("branch_join"));
        return core::intercept_result::proceed();
      });
  bool       refused = false;
  auto const sink =
      f.target->add_trace_sink({core::trace_kind::stop}, [&](std::span<core::trace_record const>) {
        try
        {
          std::ignore = f.target->coverage();
        }
        catch(popcorn::machine_running const&)
        {
          refused = true;
        }
      });
  run_branches(f, 10);
  CHECK(seen == 10);
  CHECK(refused);
}

TEST_CASE("exception entry and return break the edge chain")
{
  fixture f{m33(), "snapshot", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.target->enable_coverage({});
  core::slice_request request{.exact_instructions = popcorn::icount{100'000}};
  request.max_instructions = popcorn::icount{200'000};
  request.wall_budget      = std::chrono::seconds{60};
  std::ignore              = f.run(request);
  auto const handler       = sym("snapshot", "tick_handler");
  auto const data          = f.target->coverage();
  REQUIRE(hits_at(data, handler) > 50);
  CHECK(std::ranges::none_of(data.edges, [&](core::edge_counter const& e) {
    return e.to == handler;
  }));
}

TEST_CASE("an intercept that returns or stops and resumes leaves no false edge")
{
  fixture f{m33(), "intercepts", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::reset_request);
  auto const seven = sym("intercepts", "seven");
  auto const call  = [&] {
    f.target->begin_call(
        {.entry           = sym("intercepts", "call_seven"),
         .returns         = popcorn::arch::value_kind::i32,
         .abi             = popcorn::arch::float_abi::soft,
         .return_sentinel = 0x3000'0000});
    while(f.run_to_stop().reason != stop_reason::returned)
    {
    }
    return f.target->end_call();
  };
  auto const from_seven = [&](core::coverage_data const& data) {
    return data.edges
         | std::views::filter([&](core::edge_counter const& e) { return e.from == seven; })
         | std::ranges::to<std::vector>();
  };
  f.target->enable_coverage({});
  SUBCASE("returns")
  {
    auto const hook = f.target->add_intercept(seven, [](core::intercept_call const&) {
      return core::intercept_result::returns(
          popcorn::arch::call_arg{.kind = popcorn::arch::value_kind::i32, .bits = 1});
    });
    CHECK(call() == 101);
    auto const data = f.target->coverage();
    CHECK(hits_at(data, seven) == 1);
    CHECK(from_seven(data).empty());
    CHECK(
        data.cuts
        == std::vector<core::cut_counter>{
            {.start = seven, .at = seven, .hits = 1}
    });
  }
  SUBCASE("stop then resume")
  {
    auto const hook = f.target->add_intercept(seven, [](core::intercept_call const&) {
      return core::intercept_result::stop(stop_reason::user);
    });
    CHECK(call() == 107);
    auto const data = f.target->coverage();
    CHECK(hits_at(data, seven) == 2);
    CHECK(
        data.cuts
        == std::vector<core::cut_counter>{
            {.start = seven, .at = seven, .hits = 1}
    });
    auto const edges = from_seven(data);
    REQUIRE(edges.size() == 1);
    CHECK(edges.front().to != seven);
    CHECK(edges.front().hits == 1);
  }
}

TEST_CASE("a block left at an exit after all its counted instructions is cut unless it ended")
{
  fixture    f{m33(), "alu", {}};
  auto const entry = sym("alu", "alu_entry");
  auto const body  = sym("alu", "alu_body");
  auto const loop  = body + 10;

  auto const left = [&](address start, std::uint32_t insns, address pc, bool at_exit) {
    core::coverage_state state;
    state.previous   = start;
    state.open_insns = insns;
    state.leave(*f.module, pc, at_exit);
    return state.cuts;
  };
  using cuts = std::map<std::pair<address, address>, std::uint64_t>;
  cuts const cut_at_body{
      {std::pair{entry, body}, 1}
  };
  CHECK(left(entry, 3, body, true) == cut_at_body);
  CHECK(left(entry, 3, body, false).empty());
  CHECK(left(entry, 4, body, false) == cut_at_body);
  CHECK(left(body, 5, loop, true).empty());
  CHECK(left(entry, 2, body, true).empty());
  CHECK(left(entry, 0, entry, true).empty());
}
