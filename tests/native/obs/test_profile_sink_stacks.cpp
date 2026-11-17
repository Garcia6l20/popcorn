#include "profile_rig.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>

#include <doctest/doctest.h>

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
using namespace popcorn::testing::profile_rig;
using obs::block_exit;
using obs::block_shape;
using obs::frame_kind;

using shapes = std::map<address, block_shape>;

[[maybe_unused]] auto const builtin_arch = (popcorn::arch::armv8m::register_builtin(), true);

auto call_at(address next, address function) -> block_shape
{
  return {.exit = block_exit::call, .next = next, .function = function};
}

auto ret_at(address next, address function) -> block_shape
{
  return {.exit = block_exit::ret, .next = next, .function = function};
}

auto plain(address function) -> block_shape
{
  return {.function = function};
}

auto stacked(std::vector<core::trace_record> const& records, shapes code, std::uint32_t depth = 256)
    -> obs::profile_result
{
  return fed(
      records, {.code =
                    [code = std::move(code)](address pc, std::uint32_t) {
                      auto const found = code.find(pc);
                      return found != code.end() ? found->second : block_shape{.function = pc};
                    },
                .max_call_depth = depth});
}

auto node(obs::profile_result const& result, std::vector<std::pair<frame_kind, std::uint32_t>> path)
    -> std::optional<obs::profile_frame>
{
  std::uint32_t at = 0;
  for(auto const& [kind, key] : path)
  {
    auto const found = std::ranges::find_if(
        std::views::iota(std::size_t{1}, result.frames.size()), [&](std::size_t index) {
          auto const& candidate = result.frames[index];
          return candidate.parent == at and candidate.kind == kind and candidate.key == key;
        });
    if(found == std::views::iota(std::size_t{1}, result.frames.size()).end())
    {
      return std::nullopt;
    }
    at = static_cast<std::uint32_t>(*found);
  }
  return result.frames[at];
}

auto function_totals(obs::profile_result const& result, address function)
    -> std::pair<std::uint64_t, std::uint64_t>
{
  std::uint64_t self      = 0;
  std::uint64_t inclusive = 0;
  for(auto const [index, frame] : std::views::enumerate(result.frames))
  {
    if(index == 0 or frame.kind == frame_kind::exception or frame.key != function)
    {
      continue;
    }
    self += frame.self;
    auto nested = false;
    for(auto up = frame.parent; up != 0; up = result.frames[up].parent)
    {
      auto const& above = result.frames[up];
      nested            = nested or (above.kind != frame_kind::exception and above.key == function);
    }
    if(not nested)
    {
      inclusive += frame.inclusive;
    }
  }
  return {self, inclusive};
}
}

TEST_CASE("calls nest frames and returns pop to the caller")
{
  auto const result = stacked(
      {
          block(0x100, 3, 3), block(0x200, 2, 2), block(0x300, 4, 4), block(0x204, 1, 1),
          block(0x104, 5, 5)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x200, call_at(0x204, 0x200)},
       {0x300, ret_at(0x302, 0x300)},
       {0x204, ret_at(0x206, 0x200)},
       {0x104, plain(0x100)}});
  auto const main = node(
      result, {
                  {frame_kind::entry, 0x100}
  });
  auto const a = node(
      result, {
                  {frame_kind::entry, 0x100},
                  {frame_kind::call,  0x200}
  });
  auto const b = node(
      result,
      {
          {frame_kind::entry, 0x100},
          {frame_kind::call,  0x200},
          {frame_kind::call,  0x300}
  });
  REQUIRE(main);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(main->self == 8);
  CHECK(main->inclusive == 15);
  CHECK(main->entries == 1);
  CHECK(a->self == 3);
  CHECK(a->inclusive == 7);
  CHECK(a->entries == 1);
  CHECK(b->self == 4);
  CHECK(b->inclusive == 4);
  CHECK(result.frames.size() == 4);
  CHECK(result.frames[0].inclusive == 15);
  CHECK(result.calls == 2);
  CHECK(result.unmatched_returns == 0);
}

TEST_CASE("recursion nests a frame per level and unwinds level by level")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x200, 2, 2), block(0x200, 2, 2), block(0x300, 1, 1),
          block(0x204, 1, 1), block(0x204, 1, 1), block(0x104, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x200, call_at(0x204, 0x200)},
       {0x300, ret_at(0x302, 0x200)},
       {0x204, ret_at(0x206, 0x200)},
       {0x104, plain(0x100)}});
  auto const outer = node(
      result, {
                  {frame_kind::entry, 0x100},
                  {frame_kind::call,  0x200}
  });
  auto const inner = node(
      result,
      {
          {frame_kind::entry, 0x100},
          {frame_kind::call,  0x200},
          {frame_kind::call,  0x200}
  });
  REQUIRE(outer);
  REQUIRE(inner);
  CHECK(outer->self == 3);
  CHECK(inner->self == 3);
  CHECK(outer->inclusive == 7);
  CHECK(result.unmatched_returns == 0);
  CHECK(function_totals(result, 0x200) == std::pair<std::uint64_t, std::uint64_t>{7, 7});
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100}
  })
          ->self
      == 2);
}

TEST_CASE("a return that matches no frame is counted and the code goes on in an entry node")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x200, 2, 2), block(0x900, 3, 3)
  },
      {{0x100, call_at(0x104, 0x100)}, {0x200, ret_at(0x202, 0x200)}, {0x900, plain(0x900)}});
  CHECK(result.unmatched_returns == 1);
  auto const after = node(
      result, {
                  {frame_kind::entry, 0x900}
  });
  REQUIRE(after);
  CHECK(after->self == 3);
  CHECK(after->entries == 1);
}

TEST_CASE("a call or a return that falls through moves no frame")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x104, 1, 1), block(0x108, 1, 1), block(0x10c, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x104, ret_at(0x108, 0x100)},
       {0x108, plain(0x100)},
       {0x10c, plain(0x100)}});
  CHECK(result.calls == 0);
  CHECK(result.unmatched_returns == 0);
  CHECK(result.frames.size() == 2);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100}
  })
          ->self
      == 4);
}

TEST_CASE("an exception between a call and its callee keeps the call, its return pops no frame")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), enter(30, 0), block(0x500, 2, 2), block(0x600, 3, 3),
          block(0x504, 1, 1), leave(30, 0), block(0x200, 4, 4), block(0x204, 1, 1),
          block(0x104, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x500, call_at(0x504, 0x500)},
       {0x600, ret_at(0x602, 0x600)},
       {0x504, ret_at(0x506, 0x500)},
       {0x200, plain(0x200)},
       {0x204, ret_at(0x206, 0x200)},
       {0x104, plain(0x100)}});
  CHECK(result.calls == 2);
  CHECK(result.unmatched_returns == 0);
  auto const handler = node(
      result,
      {
          {frame_kind::entry,     0x100},
          {frame_kind::exception, 30   },
          {frame_kind::entry,     0x500}
  });
  REQUIRE(handler);
  CHECK(handler->self == 3);
  CHECK(handler->inclusive == 6);
  auto const callee = node(
      result, {
                  {frame_kind::entry, 0x100},
                  {frame_kind::call,  0x200}
  });
  REQUIRE(callee);
  CHECK(callee->self == 5);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100}
  })
          ->self
      == 2);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry,     0x100},
              {frame_kind::exception, 30   }
  })
          ->entries
      == 1);
}

TEST_CASE("an exit with no exception frame clears the stack")
{
  auto const result = stacked(
      {
          block(0x500, 1, 1), block(0x600, 1, 1), leave(30, 0), block(0x100, 2, 2)
  },
      {{0x500, call_at(0x504, 0x500)}, {0x600, plain(0x600)}, {0x100, plain(0x100)}});
  auto const after = node(
      result, {
                  {frame_kind::entry, 0x100}
  });
  REQUIRE(after);
  CHECK(after->self == 2);
}

TEST_CASE("a cut clears the stack and a later return matches nothing")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x200, 1, 1), time_to(10), block(0x300, 1, 1),
          block(0x104, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x200, plain(0x200)},
       {0x300, ret_at(0x302, 0x300)},
       {0x104, plain(0x100)}});
  CHECK(result.cuts == 1);
  CHECK(result.unmatched_returns == 1);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x300}
  })
          ->self
      == 1);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100}
  })
          ->self
      == 2);
}

TEST_CASE("calls past the depth limit stay in the caller and are counted")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x200, 1, 1), block(0x300, 1, 1), block(0x400, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x200, call_at(0x204, 0x200)},
       {0x300, call_at(0x304, 0x300)},
       {0x400, plain(0x400)}},
      2);
  CHECK(result.calls == 2);
  CHECK(result.deep_calls == 1);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100},
              {frame_kind::call,  0x200},
              {frame_kind::call,  0x300}
  })
          ->self
      == 2);
}

TEST_CASE("a block that resumes inside the previous one moves no frame")
{
  auto const result = stacked(
      {
          block(0x100, 4, 4), block(0x106, 2, 2), block(0x200, 1, 1)
  },
      {{0x100, call_at(0x10c, 0x100)}, {0x106, call_at(0x10c, 0x100)}, {0x200, plain(0x200)}});
  CHECK(result.calls == 1);
  auto const callee = node(
      result, {
                  {frame_kind::entry, 0x100},
                  {frame_kind::call,  0x200}
  });
  REQUIRE(callee);
  CHECK(callee->entries == 1);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100}
  })
          ->self
      == 6);
}

TEST_CASE("a return inside a handler never pops the frames of the code it interrupted")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x200, 1, 1), enter(30, 0), block(0x500, 1, 1),
          block(0x104, 1, 1), leave(30, 0), block(0x204, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x200, plain(0x200)},
       {0x500, ret_at(0x502, 0x500)},
       {0x104, plain(0x100)},
       {0x204, plain(0x200)}});
  CHECK(result.unmatched_returns == 1);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100},
              {frame_kind::call,  0x200}
  })
          ->self
      == 2);
}

TEST_CASE("a block's shape is kept per pc and instruction count")
{
  auto const result =
      fed({block(0x100, 2, 2), block(0x200, 1, 1), block(0x100, 3, 3), block(0x200, 1, 1)},
          {.code = [](address pc, std::uint32_t instructions) {
            return pc == 0x100 and instructions == 3 ? call_at(0x10c, 0x100) : plain(pc);
          }});
  CHECK(result.calls == 1);
}

TEST_CASE("the return of a call past the depth limit pops nothing and the rest still match")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x200, 1, 1), block(0x300, 1, 1), block(0x400, 1, 1),
          block(0x304, 1, 1), block(0x204, 1, 1), block(0x104, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x200, call_at(0x204, 0x200)},
       {0x300, call_at(0x304, 0x300)},
       {0x400, ret_at(0x402, 0x400)},
       {0x304, ret_at(0x306, 0x300)},
       {0x204, ret_at(0x206, 0x200)},
       {0x104, plain(0x100)}},
      2);
  CHECK(result.unmatched_returns == 0);
  CHECK(result.deep_calls == 1);
  CHECK(result.calls == 2);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100},
              {frame_kind::call,  0x200},
              {frame_kind::call,  0x300}
  })
          ->self
      == 3);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100},
              {frame_kind::call,  0x200}
  })
          ->self
      == 2);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100}
  })
          ->self
      == 2);
}

TEST_CASE("recursion past the depth limit unwinds with no unmatched return")
{
  auto const result = stacked(
      {
          block(0x100, 1, 1), block(0x200, 1, 1), block(0x200, 1, 1), block(0x200, 1, 1),
          block(0x200, 1, 1), block(0x300, 1, 1), block(0x204, 1, 1), block(0x204, 1, 1),
          block(0x204, 1, 1), block(0x204, 1, 1), block(0x104, 1, 1)
  },
      {{0x100, call_at(0x104, 0x100)},
       {0x200, call_at(0x204, 0x200)},
       {0x300, ret_at(0x302, 0x200)},
       {0x204, ret_at(0x206, 0x200)},
       {0x104, plain(0x100)}},
      2);
  CHECK(result.unmatched_returns == 0);
  CHECK(result.deep_calls == 3);
  CHECK(result.calls == 2);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100},
              {frame_kind::call,  0x200}
  })
          ->self
      == 2);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100},
              {frame_kind::call,  0x200},
              {frame_kind::call,  0x200}
  })
          ->self
      == 7);
  CHECK(
      node(
          result,
          {
              {frame_kind::entry, 0x100}
  })
          ->self
      == 2);
}

TEST_CASE("no code reader, no call tree")
{
  auto const result = fed({block(0x100, 1, 1)});
  CHECK(result.frames.empty());
  CHECK(result.calls == 0);
}

TEST_CASE(
    "the FreeRTOS example builds a call tree, a switched-in task counted in the interrupted node")
{
  auto       f      = spi_machine();
  auto       reader = obs::machine_code_reader(*f->target);
  auto const idle   = sym("spi_flash", "prvIdleTask");
  auto const check  = sym("spi_flash", "prvCheckTasksWaitingTermination");
  CHECK(reader(check, 1).function == check);
  CHECK(reader(0x2000'0000, 1).function == 0x2000'0000);
  profiled p{*f->target, {.code = obs::machine_code_reader(*f->target)}};
  f->run_for(100 * ms_ps);
  auto const result = p.take();
  CHECK(result.unmatched_returns == 147);
  CHECK(result.calls == 1'427'929);
  CHECK(result.deep_calls == 0);
  CHECK(
      function_totals(result, idle)
      == std::pair<std::uint64_t, std::uint64_t>{3'606'334, 9'931'817});
  CHECK(
      function_totals(result, check)
      == std::pair<std::uint64_t, std::uint64_t>{9'954'065, 9'968'483});
  CHECK(result.frames[0].inclusive == result.instructions);
}

TEST_CASE("the call tree does not depend on where slices end between blocks")
{
  auto     whole = spi_machine();
  profiled one{*whole->target, {.code = obs::machine_code_reader(*whole->target)}};
  whole->run_for(20 * ms_ps);
  auto const expected = one.take();

  auto       sliced = spi_machine();
  profiled   many{*sliced->target, {.code = obs::machine_code_reader(*sliced->target)}};
  auto const end    = vtime{sliced->now().ps + 20 * ms_ps};
  auto       slices = 0;
  for(;;)
  {
    ++slices;
    auto const ran = sliced->run(
        {.max_instructions = icount{5'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = end});
    if(ran.stopped)
    {
      REQUIRE(ran.stopped->reason == stop_reason::time);
      break;
    }
  }
  CHECK(slices > 100);
  auto const got = many.take();
  CHECK(got.frames == expected.frames);
  CHECK(got.calls == expected.calls);
  CHECK(got.unmatched_returns == expected.unmatched_returns);
  CHECK(expected.calls > 0);
}
