#include <popcorn/core/types.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <optional>

TEST_CASE("vtime_from_instructions converts at 160 MHz")
{
  CHECK(popcorn::vtime_from_instructions(1, 160'000'000).ps == 6'250);
  CHECK(popcorn::vtime_from_instructions(160'000'000, 160'000'000).ps == 1'000'000'000'000ull);
  CHECK(
      popcorn::vtime_from_instructions(std::uint64_t{1} << 32, 160'000'000).ps
      == 26'843'545'600'000ull);
}

TEST_CASE("vtime_from_instructions is exact at non-divisor clocks")
{
  CHECK(popcorn::vtime_from_instructions(1, 110'000'000).ps == 9'090);
  CHECK(popcorn::vtime_from_instructions(3, 110'000'000).ps == 27'272);
  CHECK(
      popcorn::vtime_from_instructions(std::uint64_t{1} << 32, 110'000'000).ps
      == 39'045'157'236'363ull);

  CHECK(popcorn::vtime_from_instructions(1, 32'768).ps == 30'517'578);
  CHECK(popcorn::vtime_from_instructions(3, 32'768).ps == 91'552'734);
  CHECK(
      popcorn::vtime_from_instructions(std::uint64_t{1} << 32, 32'768).ps
      == 131'072'000'000'000'000ull);
}

TEST_CASE("stop_reason and fault_kind format their identifier with {}")
{
  CHECK(std::format("{}", popcorn::stop_reason::watchpoint) == "watchpoint");
  CHECK(std::format("{}", popcorn::fault_kind::mem_manage) == "mem_manage");
}

TEST_CASE("stop aggregates a reason, a location, counters and an optional fault")
{
  popcorn::stop const s{
      .reason = popcorn::stop_reason::fault,
      .where  = {.pc = 0x0800'1000, .symbol = "main", .offset = 4, .file = "main.c", .line = 12},
      .instructions = {42},
      .time         = popcorn::vtime_from_instructions(42, 160'000'000),
      .fault =
          popcorn::fault_info{
                 .kind          = popcorn::fault_kind::bus,
                 .fault_address = 0x2000'0000,
                 .status        = 1,
                 .detail        = 2},
  };

  CHECK(s.reason == popcorn::stop_reason::fault);
  CHECK(s.where.pc == 0x0800'1000);
  CHECK(s.instructions.value == 42);
  CHECK(s.fault.has_value());
  CHECK(s.fault->kind == popcorn::fault_kind::bus);
}

TEST_CASE("to_owned copies a location's symbol and file text")
{
  popcorn::location const view{
      .pc = 0x0800'1000, .symbol = "main", .offset = 4, .file = "main.c", .line = 12};
  popcorn::owned_location const owned = popcorn::to_owned(view);

  CHECK(owned.pc == view.pc);
  CHECK(owned.symbol == "main");
  CHECK(owned.offset == view.offset);
  CHECK(owned.file == "main.c");
  CHECK(owned.line == view.line);
}

TEST_CASE("stop with no fault leaves the optional empty")
{
  popcorn::stop const s{
      .reason       = popcorn::stop_reason::returned,
      .where        = {.pc = 0, .symbol = {}, .offset = 0, .file = {}, .line = 0},
      .instructions = {0},
      .time         = {0},
      .fault        = std::nullopt,
  };

  CHECK_FALSE(s.fault.has_value());
}

TEST_CASE("a default stop has an empty detail")
{
  CHECK(popcorn::stop{}.detail.empty());
}

TEST_CASE("icount_from_vtime rounds up and inverts vtime_from_instructions")
{
  CHECK(popcorn::icount_from_vtime({1'000'000'000'000ull}, 160'000'000).value == 160'000'000);
  CHECK(popcorn::icount_from_vtime({1}, 160'000'000).value == 1);
  CHECK(popcorn::icount_from_vtime({0}, 160'000'000).value == 0);
  for(std::uint64_t const hz : {160'000'000ull, 110'000'000ull, 32'768ull})
  {
    for(std::uint64_t const n : {1ull, 3ull, 1'000'003ull, 1ull << 32})
    {
      CAPTURE(hz);
      CAPTURE(n);
      CHECK(popcorn::icount_from_vtime(popcorn::vtime_from_instructions(n, hz), hz).value == n);
    }
  }
}
