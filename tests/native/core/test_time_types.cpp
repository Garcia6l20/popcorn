#include <popcorn/core/scheduler.hpp>

#include <doctest/doctest.h>

#include <limits>
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
