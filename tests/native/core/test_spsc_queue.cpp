#include <popcorn/core/spsc_queue.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using popcorn::core::spsc_queue;

namespace
{
constexpr auto slots = spsc_queue<std::uint64_t>::segment_slots;

auto counting(std::uint64_t from, std::uint64_t count) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> out(count);
  std::iota(out.begin(), out.end(), from);
  return out;
}

void push_counting(spsc_queue<std::uint64_t>& queue, std::uint64_t from, std::uint64_t count)
{
  for(auto value = from; value < from + count; ++value)
  {
    queue.push(value);
  }
}
}

TEST_CASE("a queue holds one segment empty and pops nothing through its count")
{
  spsc_queue<std::uint64_t> queue;
  CHECK(queue.empty());
  CHECK(queue.pushed() == 0);
  CHECK(queue.segments() == 1);
  std::vector<std::uint64_t> out;
  queue.pop_through(0, out);
  CHECK(out.empty());
  CHECK(queue.contents().empty());
}

TEST_CASE("exactly one segment of values fits one segment, one more links a second")
{
  for(auto const count : {slots, slots + 1})
  {
    CAPTURE(count);
    spsc_queue<std::uint64_t> queue;
    push_counting(queue, 0, count);
    CHECK(queue.pushed() == count);
    CHECK(queue.segments() == (count == slots ? 1U : 2U));
    CHECK(queue.contents() == counting(0, count));
    std::vector<std::uint64_t> out;
    queue.pop_through(count, out);
    CHECK(out == counting(0, count));
    CHECK(queue.empty());
    CHECK(queue.segments() == 1);
  }
}

TEST_CASE("a pop ending on a segment boundary keeps that segment, the next pop frees it")
{
  spsc_queue<std::uint64_t> queue;
  push_counting(queue, 0, 2 * slots);
  std::vector<std::uint64_t> out;
  queue.pop_through(slots, out);
  CHECK(out == counting(0, slots));
  CHECK(queue.segments() == 2);
  CHECK(queue.contents() == counting(slots, slots));
  queue.push(2 * slots);
  CHECK(queue.segments() == 3);
  out.clear();
  queue.pop_through(slots + 1, out);
  CHECK(out == counting(slots, 1));
  CHECK(queue.segments() == 2);
  out.clear();
  queue.pop_through(2 * slots + 1, out);
  CHECK(out == counting(slots + 1, slots));
  CHECK(queue.segments() == 1);
  CHECK(queue.empty());
}

TEST_CASE("pops at any count across many segments hand every value once, in order")
{
  constexpr std::uint64_t    total = 10 * slots + 3;
  spsc_queue<std::uint64_t>  queue;
  std::vector<std::uint64_t> out;
  std::uint64_t              pushed = 0;
  for(std::uint64_t const step : {1U, 255U, 256U, 257U, 513U, 7U})
  {
    push_counting(queue, pushed, step);
    pushed += step;
    CHECK(queue.pushed() == pushed);
    CHECK_FALSE(queue.empty());
    queue.pop_through(pushed - step / 2, out);
  }
  push_counting(queue, pushed, total - pushed);
  CHECK(queue.segments() > 1);
  queue.pop_through(total, out);
  CHECK(out == counting(0, total));
  CHECK(queue.popped() == total);
  CHECK(queue.segments() == 1);
}

TEST_CASE("assign replaces the content and counts on from the first count it names")
{
  spsc_queue<std::uint64_t> queue;
  push_counting(queue, 0, 3 * slots);
  std::vector<std::uint64_t> out;
  queue.pop_through(10, out);
  auto const kept = counting(500, slots + 2);
  queue.assign(40, kept);
  CHECK(queue.popped() == 40);
  CHECK(queue.pushed() == 40 + kept.size());
  CHECK(queue.segments() == 2);
  CHECK(queue.contents() == kept);
  queue.push(9);
  out.clear();
  queue.pop_through(41, out);
  CHECK(out == counting(500, 1));
  out.clear();
  queue.pop_through(queue.pushed(), out);
  auto expected = counting(501, slots + 1);
  expected.push_back(9);
  CHECK(out == expected);
  queue.assign(0, {});
  CHECK(queue.empty());
  CHECK(queue.pushed() == 0);
  CHECK(queue.segments() == 1);
}

TEST_CASE("a million values pushed on one thread and popped on another arrive whole, in order")
{
  constexpr std::uint64_t    total = 1'000'000;
  spsc_queue<std::uint64_t>  queue;
  std::jthread               producer{[&] {
    for(std::uint64_t value = 0; value < total; ++value)
    {
      queue.push(value * 2654435761U);
    }
  }};
  std::vector<std::uint64_t> out;
  std::uint64_t              in_order = 0;
  std::uint64_t              sum      = 0;
  while(out.size() < total or not queue.empty())
  {
    auto const count = queue.pushed();
    auto const from  = out.size();
    queue.pop_through(count, out);
    for(auto index = from; index < out.size(); ++index)
    {
      in_order += out[index] == index * 2654435761U ? 1 : 0;
      sum += out[index];
    }
    if(out.size() == from)
    {
      std::this_thread::yield();
    }
  }
  producer               = {};
  std::uint64_t expected = 0;
  for(std::uint64_t value = 0; value < total; ++value)
  {
    expected += value * 2654435761U;
  }
  CHECK(out.size() == total);
  CHECK(in_order == total);
  CHECK(sum == expected);
  CHECK(queue.segments() == 1);
}
