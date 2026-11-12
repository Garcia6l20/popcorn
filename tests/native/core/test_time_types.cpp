#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>
#include <vector>

static_assert(std::is_trivially_copyable_v<popcorn::core::event>);
static_assert(std::is_standard_layout_v<popcorn::core::event>);
static_assert(not std::is_copy_constructible_v<popcorn::core::scheduler>);
static_assert(not std::is_move_constructible_v<popcorn::core::scheduler>);

namespace
{
constexpr auto never = std::numeric_limits<std::uint64_t>::max();
}

TEST_CASE("events are plain data")
{
  popcorn::core::event const e{
      .at = {100},
        .seq = 7, .id = {.owner = 2, .kind = 1},
        .payload = 42
  };
  auto const copy = e;
  CHECK(copy == e);
}

TEST_CASE("an empty queue has no deadline")
{
  popcorn::core::scheduler const queue;
  CHECK_FALSE(queue.next_deadline().has_value());
  CHECK(queue.earliest().value == never);
  CHECK(queue.pending().empty());
  CHECK(queue.next_seq() == 0);
}

TEST_CASE("events pop by due count, then by seq, and the cached deadline follows")
{
  popcorn::core::scheduler queue;
  auto const               late  = queue.schedule({500}, {1, 9}, 0);
  auto const               first = queue.schedule({100}, {1, 1}, 10);
  auto const               tie   = queue.schedule({100}, {2, 1}, 20);
  CHECK(queue.earliest().value == 100);
  CHECK(queue.next_deadline()->value == 100);
  CHECK(first < tie);

  CHECK_FALSE(queue.pop_due({99}).has_value());
  CHECK(queue.pop_due({100})->seq == first);
  CHECK(queue.pop_due({100})->seq == tie);
  CHECK_FALSE(queue.pop_due({100}).has_value());
  CHECK(queue.earliest().value == 500);

  queue.schedule({50}, {3, 0}, 0);
  CHECK(queue.earliest().value == 50);
  queue.cancel(late);
  queue.cancel(late);
  queue.cancel(12'345);
  CHECK(queue.pending().size() == 1);
}

TEST_CASE("event ports stamp their owner and cannot cancel other models' events")
{
  popcorn::core::scheduler  queue;
  popcorn::core::event_port uart{queue, 3};
  popcorn::core::event_port timer{queue, 4};
  auto const                byte_done = uart.schedule({200}, 1, 0x55);
  auto const                overflow  = timer.schedule({300}, 2, 0);
  CHECK(queue.pending()[0].id == popcorn::core::event_id{3, 1});
  CHECK(queue.pending()[0].payload == 0x55);

  timer.cancel(byte_done);
  CHECK(queue.pending().size() == 2);
  uart.cancel(byte_done);
  CHECK(queue.pending().size() == 1);
  CHECK(queue.pending()[0].seq == overflow);
  CHECK(queue.earliest().value == 300);
}

TEST_CASE("restore rewinds the queue and the seq counter for a deterministic replay")
{
  popcorn::core::scheduler queue;
  queue.schedule({100}, {1, 1}, 0);
  queue.schedule({100}, {2, 1}, 0);
  std::vector<popcorn::core::event> const saved{queue.pending().begin(), queue.pending().end()};
  auto const                              saved_seq = queue.next_seq();

  auto const after = queue.schedule({10}, {5, 5}, 0);
  static_cast<void>(queue.pop_due({1'000}));
  CHECK(queue.earliest().value == 100);

  queue.restore(saved, saved_seq);
  CHECK(std::ranges::equal(queue.pending(), saved));
  CHECK(queue.earliest().value == 100);
  CHECK(queue.schedule({10}, {5, 5}, 0) == after);
}

namespace
{
struct keyed_queue
{
  popcorn::core::clock        clock{4'000'000};
  popcorn::core::clock_driver driver{clock};
  popcorn::core::scheduler    queue;

  keyed_queue()
  {
    driver.on_rate_change([this] { queue.rekey(clock); });
  }
};

constexpr popcorn::core::event_id owner{.owner = 1, .kind = 0};
}

TEST_CASE("a time-keyed event moves with the core rate, an instruction-keyed one stays")
{
  keyed_queue q;
  q.driver.advance({100});
  auto const timed = q.queue.schedule_at(popcorn::vtime{100'000'000}, owner, 1, q.clock);
  auto const fixed = q.queue.schedule(popcorn::icount{400}, owner, 2);
  CHECK(q.queue.pending()[0].seq == timed);
  CHECK(q.queue.pending()[0].at.value == 400);
  CHECK(q.queue.pending()[0].kind == popcorn::core::deadline::time);
  CHECK(q.queue.pending()[1].seq == fixed);
  q.clock.set_core_hz(8'000'000);
  CHECK(q.queue.pending()[0].seq == fixed);
  CHECK(q.queue.pending()[0].at.value == 400);
  CHECK(q.queue.pending()[1].seq == timed);
  CHECK(q.queue.pending()[1].at.value == 100 + 600);
  CHECK(q.queue.earliest().value == 400);
  q.clock.set_cpi(2.0);
  CHECK(q.queue.pending()[0].seq == timed);
  CHECK(q.queue.pending()[0].at.value == 400);
  CHECK(q.queue.pending()[1].seq == fixed);
}

TEST_CASE("a time-keyed and an instruction-keyed event on one count keep scheduling order")
{
  keyed_queue q;
  auto const  timed = q.queue.schedule_at(popcorn::vtime{25'000'000}, owner, 1, q.clock);
  auto const  fixed = q.queue.schedule(popcorn::icount{100}, owner, 2);
  CHECK(q.queue.pending()[0].seq == timed);
  q.clock.set_cpi(1.0);
  CHECK(q.queue.pending()[0].seq == timed);
  CHECK(q.queue.pending()[1].seq == fixed);
  CHECK(q.queue.pending()[0].at == q.queue.pending()[1].at);
}

TEST_CASE("a time-keyed event already due, or due after a slow-down, never moves before now")
{
  keyed_queue q;
  q.driver.advance({1000});
  auto const past = q.queue.schedule_at(popcorn::vtime{1'000}, owner, 1, q.clock);
  CHECK(q.queue.pending()[0].seq == past);
  CHECK(q.queue.pending()[0].at.value == 1000);
  q.clock.set_core_hz(1'000'000);
  CHECK(q.queue.pending()[0].at.value == 1000);
  CHECK(q.queue.pop_due(popcorn::icount{1000}).has_value());
}

TEST_CASE("the rate-change hook runs once per change, after the new epoch starts")
{
  popcorn::core::clock        clock{4'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({4});
  std::vector<std::uint64_t> seen;
  driver.on_rate_change([&] {
    seen.push_back(clock.core_hz());
    CHECK(clock.time() == popcorn::vtime{1'000'000});
  });
  clock.set_core_hz(8'000'000);
  clock.set_cpi(2.0);
  CHECK(seen == std::vector<std::uint64_t>{8'000'000, 8'000'000});
  driver.on_rate_change({});
  clock.set_core_hz(4'000'000);
  CHECK(seen.size() == 2);
}

namespace
{
constexpr std::uint64_t period_ps = 250'000;

auto seqs_of(popcorn::core::scheduler const& queue) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> out;
  for(auto const& one : queue.pending())
  {
    out.push_back(one.seq);
  }
  return out;
}
}

TEST_CASE("time events due inside one instruction fire by their exact time, not posting order")
{
  keyed_queue q;
  auto const  late  = q.queue.schedule_at(popcorn::vtime{10 * period_ps - 1}, owner, 1, q.clock);
  auto const  early = q.queue.schedule_at(popcorn::vtime{9 * period_ps + 1}, owner, 2, q.clock);
  REQUIRE(q.queue.pending()[0].at == q.queue.pending()[1].at);
  CHECK(seqs_of(q.queue) == std::vector<std::uint64_t>{early, late});
  CHECK(q.queue.pop_due(popcorn::icount{10})->payload == 2);
  CHECK(q.queue.pop_due(popcorn::icount{10})->payload == 1);
}

TEST_CASE("an earlier host lane event fires before a local one of its count, a tie keeps the lane")
{
  keyed_queue q;
  auto const  local = q.queue.schedule_at(popcorn::vtime{10 * period_ps - 1}, owner, 1, q.clock);
  auto const  host  = q.queue.schedule_at(
      popcorn::vtime{9 * period_ps + 1}, owner, 2, popcorn::core::host_lane, q.clock);
  auto const host_tie = q.queue.schedule_at(
      popcorn::vtime{10 * period_ps - 1}, owner, 3, popcorn::core::host_lane, q.clock);
  auto const local_tie = q.queue.schedule_at(popcorn::vtime{10 * period_ps - 1}, owner, 4, q.clock);
  CHECK(seqs_of(q.queue) == std::vector<std::uint64_t>{host, local, local_tie, host_tie});
}

TEST_CASE("a time event on an instruction boundary keeps posting order with an instruction one")
{
  keyed_queue q;
  auto const  fixed  = q.queue.schedule(popcorn::icount{10}, owner, 1);
  auto const  edge   = q.queue.schedule_at(popcorn::vtime{10 * period_ps}, owner, 2, q.clock);
  auto const  inside = q.queue.schedule_at(popcorn::vtime{10 * period_ps - 1}, owner, 3, q.clock);
  CHECK(q.queue.pending()[2].order_ps == popcorn::core::at_boundary);
  CHECK(seqs_of(q.queue) == std::vector<std::uint64_t>{inside, fixed, edge});
}

TEST_CASE("an unnumbered event stays behind every event of its count, even one due later")
{
  keyed_queue q;
  std::ignore = q.queue.schedule_unnumbered(
      popcorn::vtime{9 * period_ps + 1}, owner, 1, popcorn::core::host_lane, q.clock);
  auto const later = q.queue.schedule_at(popcorn::vtime{10 * period_ps - 1}, owner, 2, q.clock);
  CHECK(
      seqs_of(q.queue)
      == std::vector<std::uint64_t>{later, popcorn::core::scheduler::unnumbered_seq});
  q.clock.set_cpi(1.5);
  REQUIRE(q.queue.pending()[0].at == q.queue.pending()[1].at);
  CHECK(q.queue.pending().back().seq == popcorn::core::scheduler::unnumbered_seq);
  CHECK(q.queue.pending().back().order_ps == popcorn::core::at_boundary);
}

TEST_CASE("a rate change that puts two time events on one count orders them by exact time")
{
  keyed_queue q;
  auto const  second = q.queue.schedule_at(popcorn::vtime{11 * period_ps}, owner, 1, q.clock);
  auto const  first  = q.queue.schedule_at(popcorn::vtime{10 * period_ps}, owner, 2, q.clock);
  CHECK(seqs_of(q.queue) == std::vector<std::uint64_t>{first, second});
  CHECK(q.queue.pending()[0].order_ps == popcorn::core::at_boundary);
  q.clock.set_core_hz(1'000'000);
  REQUIRE(q.queue.pending()[0].at == q.queue.pending()[1].at);
  CHECK(q.queue.pending()[0].order_ps == 10 * period_ps);
  CHECK(q.queue.pending()[1].order_ps == 11 * period_ps);
  CHECK(seqs_of(q.queue) == std::vector<std::uint64_t>{first, second});
}

TEST_CASE("a time event due at the largest picosecond is on its boundary after a rate change")
{
  keyed_queue q;
  q.driver.advance({100});
  q.clock.set_cpi(2.0);
  constexpr auto last = std::numeric_limits<std::uint64_t>::max();
  std::ignore         = q.queue.schedule_at(popcorn::vtime{last}, owner, 1, q.clock);
  std::ignore         = q.queue.schedule_at(popcorn::vtime{last}, owner, 2, 1, q.clock);
  REQUIRE(q.queue.pending().size() == 2);
  CHECK(q.queue.pending()[0].order_ps == popcorn::core::at_boundary);
  CHECK(q.queue.pending()[1].order_ps == popcorn::core::at_boundary);
  q.clock.set_cpi(1.0);
  CHECK(q.queue.pending()[0].order_ps == popcorn::core::at_boundary);
}

TEST_CASE("at a core clock with a fractional period the last picosecond of a count is its boundary")
{
  popcorn::core::clock        clock{3'000'000};
  popcorn::core::clock_driver driver{clock};
  popcorn::core::scheduler    queue;
  constexpr std::uint64_t     boundary = 3'333'333;
  std::ignore = queue.schedule_at(popcorn::vtime{boundary}, owner, 1, clock);
  std::ignore = queue.schedule_at(popcorn::vtime{boundary - 1}, owner, 2, clock);
  std::ignore = queue.schedule_at(popcorn::vtime{boundary + 1}, owner, 3, clock);
  REQUIRE(queue.pending().size() == 3);
  auto const find = [&](std::uint64_t payload) -> popcorn::core::event const& {
    return *std::ranges::find(queue.pending(), payload, &popcorn::core::event::payload);
  };
  CHECK(find(1).at.value == 10);
  CHECK(find(1).order_ps == popcorn::core::at_boundary);
  CHECK(find(2).at.value == 10);
  CHECK(find(2).order_ps == boundary - 1);
  CHECK(find(3).at.value == 11);
  CHECK(find(3).order_ps == boundary + 1);
}

TEST_CASE("restore sorts the saved events by count, exact time, lane and seq")
{
  keyed_queue q;
  std::ignore = q.queue.schedule_at(popcorn::vtime{10 * period_ps - 1}, owner, 1, q.clock);
  std::ignore = q.queue.schedule_at(
      popcorn::vtime{9 * period_ps + 1}, owner, 2, popcorn::core::host_lane, q.clock);
  std::ignore = q.queue.schedule(popcorn::icount{10}, owner, 3);
  std::ignore = q.queue.schedule_at(popcorn::vtime{9 * period_ps + 5}, owner, 4, 1, q.clock);
  std::vector<popcorn::core::event> const sorted{
      q.queue.pending().begin(), q.queue.pending().end()};
  std::vector<popcorn::core::event> const reversed{sorted.rbegin(), sorted.rend()};
  popcorn::core::scheduler                restored;
  restored.restore(reversed, q.queue.next_seq(), q.queue.next_lane_seq());
  CHECK(std::ranges::equal(restored.pending(), sorted));
  std::vector<std::uint64_t> payloads;
  for(auto const& one : sorted)
  {
    payloads.push_back(one.payload);
  }
  CHECK(payloads == std::vector<std::uint64_t>{2, 4, 1, 3});
}
