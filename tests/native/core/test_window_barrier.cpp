#include <popcorn/core/window_barrier.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

namespace
{
namespace core = popcorn::core;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

#if defined(__SANITIZE_THREAD__)
constexpr std::uint64_t full_rounds = 20'000;
constexpr std::uint64_t abort_races = 2'000;
#else
constexpr std::uint64_t full_rounds = 1'000'000;
constexpr std::uint64_t abort_races = 100'000;
#endif

struct alignas(64) party_counter
{
  std::uint64_t value = 0;
};

struct round_outcome
{
  std::uint64_t completions = 0;
  std::uint64_t bad_counts  = 0;
  std::uint64_t bad_stamps  = 0;
  std::uint64_t refused     = 0;
};

auto run_rounds(
    std::size_t               parties,
    std::uint64_t             rounds,
    std::chrono::milliseconds late_sleep = {}) -> round_outcome
{
  core::window_barrier       gate{parties};
  std::vector<party_counter> counters(parties);
  std::uint64_t              stamp = 0;
  round_outcome              outcome;
  std::atomic<std::uint64_t> bad_stamps{0};
  std::atomic<std::uint64_t> refused{0};
  auto const                 complete = [&]() noexcept {
    ++outcome.completions;
    for(auto const& counter : counters)
    {
      if(counter.value != outcome.completions)
      {
        ++outcome.bad_counts;
      }
    }
    stamp = outcome.completions;
  };
  {
    std::vector<std::jthread> pool;
    for(std::size_t index = 0; index < parties; ++index)
    {
      pool.emplace_back([&, index] {
        for(std::uint64_t round = 1; round <= rounds; ++round)
        {
          if(index == 0 and late_sleep.count() > 0)
          {
            std::this_thread::sleep_for(late_sleep);
          }
          ++counters[index].value;
          if(not gate.arrive_and_wait(complete))
          {
            refused.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          if(stamp != round)
          {
            bad_stamps.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
  }
  outcome.bad_stamps = bad_stamps.load();
  outcome.refused    = refused.load();
  return outcome;
}

void check_rounds(std::size_t parties, std::uint64_t rounds)
{
  CAPTURE(parties);
  auto const outcome = run_rounds(parties, rounds);
  CHECK(outcome.completions == rounds);
  CHECK(outcome.bad_counts == 0);
  CHECK(outcome.bad_stamps == 0);
  CHECK(outcome.refused == 0);
}
}

TEST_CASE("window barrier: every round sees every party's write and the completion's stamp")
{
  for(auto const parties : {std::size_t{3}, std::size_t{4}, std::size_t{8}})
  {
    check_rounds(parties, full_rounds);
  }
}

TEST_CASE("window barrier: a single party completes every round alone")
{
  check_rounds(1, 1000);
}

TEST_CASE("window barrier: a late party holds the others back and loses nothing")
{
  auto const outcome = run_rounds(3, 20, 5ms);
  CHECK(outcome.completions == 20);
  CHECK(outcome.bad_counts == 0);
  CHECK(outcome.bad_stamps == 0);
  CHECK(outcome.refused == 0);
}

TEST_CASE("window barrier: a waiter sleeps on the futex")
{
  core::window_barrier gate{2};
  std::jthread         waiter{[&] { std::ignore = gate.arrive_and_wait([]() noexcept {}); }};
  auto const           limit = clock_type::now() + 10s;
  while(gate.sleeps() == 0 and clock_type::now() < limit)
  {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(gate.sleeps() >= 1);
  CHECK(gate.arrive_and_wait([]() noexcept {}));
}

TEST_CASE("window barrier: abort releases the waiters and refuses later arrivals")
{
  core::window_barrier gate{3};
  std::atomic<int>     completions{0};
  auto const           complete = [&]() noexcept { completions.fetch_add(1); };
  std::atomic<int>     results[2]{-1, -1};
  {
    std::vector<std::jthread> pool;
    for(int index = 0; index < 2; ++index)
    {
      pool.emplace_back([&, index] { results[index] = gate.arrive_and_wait(complete) ? 1 : 0; });
    }
    while(gate.sleeps() < 2)
    {
      std::this_thread::sleep_for(1ms);
    }
    CHECK(results[0] == -1);
    CHECK(results[1] == -1);
    gate.abort();
  }
  CHECK(results[0] == 0);
  CHECK(results[1] == 0);
  CHECK(gate.aborted());
  for(int later = 0; later < 3; ++later)
  {
    CHECK_FALSE(gate.arrive_and_wait(complete));
  }
  CHECK(completions == 0);
}

TEST_CASE("window barrier: a party arriving while abort runs never waits")
{
  auto const nothing = []() noexcept {};
  for(std::uint64_t round = 0; round < abort_races; ++round)
  {
    core::window_barrier gate{3};
    std::latch           go{2};
    std::jthread         waiting{[&] { std::ignore = gate.arrive_and_wait(nothing); }};
    {
      std::jthread arriving{[&] {
        go.arrive_and_wait();
        std::ignore = gate.arrive_and_wait(nothing);
      }};
      std::jthread aborting{[&] {
        go.arrive_and_wait();
        gate.abort();
      }};
    }
    CHECK_FALSE(gate.arrive_and_wait(nothing));
  }
}

TEST_CASE("window barrier: a last arrival after abort skips the completion")
{
  core::window_barrier gate{1};
  CHECK(gate.arrive_and_wait([]() noexcept {}));
  gate.abort();
  auto ran = false;
  CHECK_FALSE(gate.arrive_and_wait([&]() noexcept { ran = true; }));
  CHECK_FALSE(ran);
}

TEST_CASE("window barrier: usable CPUs and parties")
{
  CHECK(core::usable_cpus() >= 1);
  core::window_barrier const gate{4};
  CHECK(gate.parties() == 4);
}
