#include "loop_waker.hpp"

#include <doctest/doctest.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

namespace
{
using popcorn::core::loop_waker;

constexpr std::uint64_t notifiers  = 8;
constexpr std::uint64_t per_thread = 5'000;

auto readable(int fd, int timeout_ms) -> bool
{
  pollfd watched{.fd = fd, .events = POLLIN, .revents = 0};
  return ::poll(&watched, 1, timeout_ms) == 1 and (watched.revents & POLLIN) != 0;
}

TEST_CASE("every notified token is drained once, on readiness only, none stranded")
{
  loop_waker                 waker;
  std::vector<std::uint64_t> seen(notifiers * per_thread, 0);
  std::atomic<bool>          notifying{true};
  std::uint64_t              drained = 0;
  std::jthread               drainer{[&] {
    for(;;)
    {
      if(not readable(waker.fd(), 1'000))
      {
        if(not notifying.load())
        {
          return;
        }
        continue;
      }
      for(auto const token : waker.drain())
      {
        ++seen.at(token);
        ++drained;
      }
    }
  }};
  {
    std::latch                started{notifiers};
    std::vector<std::jthread> threads;
    for(std::uint64_t thread = 0; thread < notifiers; ++thread)
    {
      threads.emplace_back([&, thread] {
        started.arrive_and_wait();
        for(std::uint64_t index = 0; index < per_thread; ++index)
        {
          waker.notify(thread * per_thread + index);
        }
      });
    }
  }
  notifying.store(false);
  drainer.join();
  CHECK(drained == notifiers * per_thread);
  CHECK(std::ranges::all_of(seen, [](std::uint64_t count) { return count == 1; }));
  CHECK_FALSE(readable(waker.fd(), 0));
  CHECK(waker.drain().empty());
}

TEST_CASE("a spurious drain returns nothing and never blocks")
{
  loop_waker waker;
  CHECK(waker.drain().empty());
  waker.notify(7);
  CHECK(waker.drain() == std::vector<std::uint64_t>{7});
  CHECK(waker.drain().empty());
}
}
