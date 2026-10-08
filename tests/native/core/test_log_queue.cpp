#include "log_queue.hpp"

#include <doctest/doctest.h>
#include <poll.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using popcorn::vtime;
using popcorn::core::log_queue;
using popcorn::core::log_record;
using popcorn::core::loop_waker;

constexpr std::uint64_t producers    = 4;
constexpr std::uint64_t per_producer = 2'000;

auto record_of(std::uint64_t producer, std::uint64_t index) -> log_record
{
  return {.offset = static_cast<popcorn::address>(producer), .message = std::to_string(index)};
}

TEST_CASE("records of each producer come out in order, none lost under the bound")
{
  log_queue                            queue{producers * per_producer};
  std::array<std::uint64_t, producers> next{};
  std::atomic<std::uint64_t>           done{0};
  std::uint64_t                        taken = 0;
  std::jthread                         consumer{[&] {
    while(done.load() < producers or taken < producers * per_producer)
    {
      auto const popped = queue.pop();
      if(not popped)
      {
        std::this_thread::yield();
        continue;
      }
      auto const producer = popped->record.offset;
      CHECK(popped->record.message == std::to_string(next.at(producer)));
      CHECK(popped->time.ps == next.at(producer));
      ++next.at(producer);
      ++taken;
    }
  }};
  {
    std::latch                started{producers};
    std::vector<std::jthread> threads;
    for(std::uint64_t producer = 0; producer < producers; ++producer)
    {
      threads.emplace_back([&, producer] {
        started.arrive_and_wait();
        for(std::uint64_t index = 0; index < per_producer; ++index)
        {
          queue.push(record_of(producer, index), vtime{index});
        }
        ++done;
      });
    }
  }
  consumer.join();
  CHECK(taken == producers * per_producer);
  CHECK(queue.take_dropped() == 0);
  CHECK_FALSE(queue.pop().has_value());
}

TEST_CASE("past the bound the first records stay and the rest are counted once")
{
  log_queue queue{4096};
  for(std::uint64_t index = 0; index < 5000; ++index)
  {
    queue.push(record_of(0, index), vtime{index});
  }
  CHECK(queue.take_dropped() == 904);
  CHECK(queue.take_dropped() == 0);
  std::uint64_t kept = 0;
  while(auto const popped = queue.pop())
  {
    CHECK(popped->record.message == std::to_string(kept));
    ++kept;
  }
  CHECK(kept == 4096);
}

TEST_CASE("take_all takes what is queued now and restore_front puts the rest back first")
{
  log_queue queue;
  queue.push(record_of(0, 0), vtime{0});
  queue.push(record_of(0, 1), vtime{1});
  auto taken = queue.take_all();
  queue.push(record_of(0, 2), vtime{2});
  REQUIRE(taken.size() == 2);
  taken.pop_front();
  queue.restore_front(std::move(taken));
  CHECK(queue.pop()->record.message == "1");
  CHECK(queue.pop()->record.message == "2");
  CHECK_FALSE(queue.pop().has_value());
}

TEST_CASE("the first record into an empty queue wakes the waker, later ones do not")
{
  auto      waker = std::make_shared<loop_waker>();
  log_queue queue;
  queue.wake_through(waker, 42);
  queue.push(record_of(0, 0), vtime{0});
  queue.push(record_of(0, 1), vtime{1});
  CHECK(waker->drain() == std::vector<std::uint64_t>{42});
  while(queue.pop())
  {
  }
  queue.push(record_of(0, 2), vtime{2});
  CHECK(waker->drain() == std::vector<std::uint64_t>{42});
  waker.reset();
  queue.push(record_of(0, 3), vtime{3});
}

TEST_CASE("records queued before a waker wake it when it is set, and again after a restore")
{
  log_queue queue;
  queue.push(record_of(0, 0), vtime{0});
  auto waker = std::make_shared<loop_waker>();
  queue.wake_through(waker, 7);
  for(std::uint64_t index = 1; index <= 10; ++index)
  {
    queue.push(record_of(0, index), vtime{index});
  }
  pollfd ready{.fd = waker->fd(), .events = POLLIN, .revents = 0};
  CHECK(::poll(&ready, 1, 0) == 1);
  CHECK(waker->drain() == std::vector<std::uint64_t>{7});
  queue.restore_front(queue.take_all());
  CHECK(waker->drain() == std::vector<std::uint64_t>{7});
}

TEST_CASE("a failed flush that restores a dropped count wakes the waker once")
{
  log_queue queue{1};
  queue.push(record_of(0, 0), vtime{0});
  queue.push(record_of(0, 1), vtime{1});
  auto waker = std::make_shared<loop_waker>();
  queue.wake_through(waker, 9);
  std::ignore  = waker->drain();
  auto batch   = queue.take_all();
  auto dropped = queue.take_dropped();
  REQUIRE(dropped == 1);
  queue.restore_dropped(dropped);
  queue.restore_front(std::move(batch));
  CHECK(waker->drain() == std::vector<std::uint64_t>{9});
}
}
