#include <popcorn/core/clock.hpp>
#include <popcorn/core/divisor.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>
#include <vector>

static_assert(not std::is_copy_constructible_v<popcorn::core::clock>);
static_assert(not std::is_move_constructible_v<popcorn::core::clock>);

namespace
{
constexpr std::uint64_t second_ps = 1'000'000'000'000ull;
}

TEST_CASE("clock converts at the instruction rate")
{
  popcorn::core::clock        clock{160'000'000};
  popcorn::core::clock_driver driver{clock};
  CHECK(clock.instruction_hz() == 160'000'000);
  driver.advance({160'000'000});
  CHECK(clock.time().ps == second_ps);
  CHECK(clock.to_icount({2 * second_ps}).value == 320'000'000);
  CHECK(clock.to_icount({second_ps + 1}).value == 160'000'001);

  popcorn::core::clock slow{160'000'000, 2.0};
  CHECK(slow.instruction_hz() == 80'000'000);
}

TEST_CASE("a rate change starts an epoch and keeps earlier time")
{
  popcorn::core::clock        clock{160'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({160'000'000});
  clock.set_core_hz(80'000'000);
  CHECK(clock.time().ps == second_ps);
  driver.advance({80'000'000});
  CHECK(clock.time().ps == 2 * second_ps);
  CHECK(clock.to_icount({3 * second_ps / 2}).value == 200'000'000);

  clock.set_cpi(2.0);
  CHECK(clock.time().ps == 2 * second_ps);
  driver.advance({40'000'000});
  CHECK(clock.time().ps == 3 * second_ps);
}

TEST_CASE("clock domains start unset and keep their handles")
{
  popcorn::core::clock clock{160'000'000};
  auto const           apb1 = clock.domain("apb1");
  auto const           apb2 = clock.domain("apb2");
  CHECK(clock.domain("apb1") == apb1);
  CHECK(apb1 != apb2);
  CHECK(clock.hz(apb1) == 0);
  clock.set_hz(apb1, 40'000'000);
  CHECK(clock.hz(apb1) == 40'000'000);
}

TEST_CASE("a stopped clock domain stores 0 and leaves the core clock running")
{
  popcorn::core::clock clock{160'000'000};
  auto const           apb1 = clock.domain("apb1");
  clock.set_hz(apb1, 40'000'000);
  clock.set_hz(apb1, 0);
  CHECK(clock.hz(apb1) == 0);
  CHECK(clock.core_hz() == 160'000'000);
  clock.set_hz(apb1, 20'000'000);
  CHECK(clock.hz(apb1) == 20'000'000);
}

TEST_CASE("clock_driver advances, jumps, saves and restores the clock")
{
  popcorn::core::clock        clock{160'000'000};
  popcorn::core::clock_driver driver{clock};
  auto const                  apb1 = clock.domain("apb1");
  clock.set_hz(apb1, 40'000'000);
  driver.advance({1'000});
  driver.jump_to({5'000});
  driver.jump_to({5'000});
  CHECK(clock.now().value == 5'000);
  clock.set_core_hz(80'000'000);

  auto const saved = driver.state();
  driver.advance({1'000'000});
  clock.set_core_hz(16'000'000);
  clock.set_hz(apb1, 1);
  auto const late = clock.domain("late");
  clock.set_hz(late, 7);

  driver.restore(saved);
  CHECK(clock.now().value == 5'000);
  CHECK(clock.core_hz() == 80'000'000);
  CHECK(clock.hz(apb1) == 40'000'000);
  CHECK(clock.hz(late) == 0);
  CHECK(clock.domain("late") == late);
  auto after = driver.state();
  after.domain_names.pop_back();
  after.domain_hz.pop_back();
  CHECK(after == saved);

  popcorn::core::clock        fresh{1};
  popcorn::core::clock_driver fresh_driver{fresh};
  fresh_driver.restore(saved);
  CHECK(fresh_driver.state() == saved);
  CHECK(fresh.domain("apb1") == apb1);
}

TEST_CASE("time since power-on adds the time run before resets to the current time")
{
  popcorn::core::clock        clock{4'000'000};
  popcorn::core::clock_driver driver{clock};
  CHECK(clock.since_power_on() == popcorn::vtime{0});
  auto state            = driver.state();
  state.before_reset_ps = 3'000'000;
  driver.restore(state);
  driver.advance({4});
  CHECK(clock.time() == popcorn::vtime{1'000'000});
  CHECK(clock.since_power_on() == popcorn::vtime{4'000'000});
}

TEST_CASE("a retract takes back unrun instructions and holds now and time until passed")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  driver.retract({30});
  CHECK(driver.exact_now().value == 70);
  CHECK(driver.exact_time() == popcorn::vtime{70'000'000});
  CHECK(clock.now().value == 100);
  CHECK(clock.time() == popcorn::vtime{100'000'000});
  driver.advance({10});
  CHECK(driver.exact_now().value == 80);
  CHECK(clock.now().value == 100);
  driver.advance({25});
  CHECK(driver.exact_now().value == 105);
  CHECK(clock.now().value == 105);
  CHECK(clock.time() == popcorn::vtime{105'000'000});
}

TEST_CASE("a second retract inside the hold keeps the larger held value")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  driver.retract({30});
  driver.advance({20});
  driver.retract({15});
  CHECK(driver.exact_now().value == 75);
  CHECK(clock.now().value == 100);
  CHECK(clock.time() == popcorn::vtime{100'000'000});
}

TEST_CASE("a retract never goes back past the start of the epoch")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  clock.set_core_hz(2'000'000);
  driver.advance({5});
  driver.retract({30});
  CHECK(driver.exact_now().value == 100);
  CHECK(driver.exact_time() == popcorn::vtime{100'000'000});
}

TEST_CASE("time holds through a rate change made inside the hold")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  driver.retract({50});
  clock.set_core_hz(4'000'000);
  CHECK(clock.time() == popcorn::vtime{100'000'000});
  CHECK(driver.exact_time() == popcorn::vtime{50'000'000});
  driver.advance({200});
  CHECK(clock.time() == popcorn::vtime{100'000'000});
  driver.advance({4});
  CHECK(clock.time() == popcorn::vtime{101'000'000});
}

TEST_CASE("a restore drops the held values")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  auto const                  saved = driver.state();
  driver.advance({100});
  driver.retract({30});
  driver.restore(saved);
  CHECK(clock.now().value == 0);
  CHECK(clock.time() == popcorn::vtime{0});
}

TEST_CASE("the clock state carries the hold, and a restore reinstates it")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  driver.retract({30});
  auto const saved = driver.state();
  CHECK(saved.held == popcorn::icount{100});
  CHECK(saved.held_time == popcorn::vtime{100'000'000});
  driver.advance({50});
  driver.restore(saved);
  CHECK(driver.exact_now().value == 70);
  CHECK(clock.now().value == 100);
  CHECK(clock.time() == popcorn::vtime{100'000'000});
}

TEST_CASE("the hold clears once execution passes it, by advance or jump")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  auto const unheld = [&] {
    auto state      = driver.state();
    state.held      = {};
    state.held_time = {};
    return state;
  };
  driver.retract({30});
  driver.advance({29});
  CHECK(driver.state().held == popcorn::icount{100});
  driver.advance({1});
  CHECK(driver.state() == unheld());
  CHECK(driver.state().held == popcorn::icount{0});

  driver.retract({10});
  driver.jump_to({95});
  CHECK(driver.state().held == popcorn::icount{100});
  driver.jump_to({100});
  CHECK(driver.state().held == popcorn::icount{0});
  CHECK(driver.state().held_time == popcorn::vtime{0});
}

TEST_CASE("the hold stays while time is still held after a rate change")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  driver.retract({50});
  clock.set_core_hz(4'000'000);
  driver.advance({50});
  CHECK(driver.state().held_time == popcorn::vtime{100'000'000});
  driver.advance({150});
  CHECK(driver.state().held == popcorn::icount{0});
  CHECK(driver.state().held_time == popcorn::vtime{0});
}

TEST_CASE("the hold stays while the count is still held after a slower rate")
{
  popcorn::core::clock        clock{1'000'000};
  popcorn::core::clock_driver driver{clock};
  driver.advance({100});
  driver.retract({50});
  clock.set_core_hz(500'000);
  driver.advance({25});
  CHECK(driver.state().held == popcorn::icount{100});
  CHECK(clock.now().value == 100);
  driver.advance({25});
  CHECK(driver.state().held == popcorn::icount{0});
}

namespace
{
using u128 = unsigned __int128;

struct lcg
{
  std::uint64_t state{0x9e3779b97f4a7c15ull};

  auto next() -> std::uint64_t
  {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state ^ (state >> 29);
  }
};

auto divisors() -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> out{
      1,
      2,
      3,
      7,
      10,
      255,
      256,
      1'000'000,
      6250,
      12'500,
      16'000'000,
      48'000'000,
      80'000'000,
      160'000'000,
      160'000'001,
      250'000'000,
      999'999'999,
      1'000'000'000,
      1'000'000'000'000ull,
      2'000'000'000'000ull,
      65'536ull * 1'000'000'000'000ull,
      std::uint64_t{1} << 32,
      (std::uint64_t{1} << 63) - 1,
      std::uint64_t{1} << 63,
      (std::uint64_t{1} << 63) + 1,
      std::numeric_limits<std::uint64_t>::max() - 1,
      std::numeric_limits<std::uint64_t>::max()};
  lcg random;
  for(int i = 0; i < 64; ++i)
  {
    out.push_back((random.next() >> (random.next() % 64)) | 1);
  }
  return out;
}

auto dividends(std::uint64_t d, lcg& random) -> std::vector<u128>
{
  auto const        wide = u128{d};
  std::vector<u128> out{
      0,
      1,
      wide - 1,
      wide,
      wide + 1,
      2 * wide - 1,
      2 * wide,
      u128{std::numeric_limits<std::uint64_t>::max()},
      u128{1} << 64,
      (u128{1} << 64) + 1,
      (wide << 64) - 1,
      wide << 64,
      (wide << 64) + 1,
      ~u128{0}};
  for(std::uint64_t k : {3ull, 1000ull, 1'000'000'000ull, 0x123456789abcdefull})
  {
    for(int delta = -1; delta <= 1; ++delta)
    {
      out.push_back(wide * k + static_cast<u128>(static_cast<__int128>(delta)));
    }
  }
  for(int i = 0; i < 400; ++i)
  {
    auto const bits = random.next() % 129;
    auto const word = (u128{random.next()} << 64) | random.next();
    out.push_back(bits == 128 ? word : word >> (128 - bits));
  }
  return out;
}
}

TEST_CASE("divisor gives the low 64 bits of n / d over a sweep of divisors and dividends")
{
  lcg  random;
  auto compared = 0u;
  for(auto const d : divisors())
  {
    popcorn::core::divisor const divide{d};
    CHECK(divide.value() == d);
    for(auto const n : dividends(d, random))
    {
      auto const expected = static_cast<std::uint64_t>(n / d);
      if(divide.quotient(n) != expected)
      {
        FAIL(
            "quotient differs, d="
            << d
            << " high="
            << static_cast<std::uint64_t>(n >> 64)
            << " low="
            << static_cast<std::uint64_t>(n));
      }
      ++compared;
    }
  }
  CHECK(compared > 30'000);
}

TEST_CASE("a default divisor divides by 1")
{
  popcorn::core::divisor const one;
  CHECK(one.quotient(12345) == 12345);
  CHECK(one.quotient(u128{1} << 64) == 0);
}

TEST_CASE("clock time equals the exact conversion over clocks, cpis and instruction counts")
{
  lcg  random;
  auto compared = 0u;
  for(std::uint64_t const core_hz :
      {1ull, 3ull, 16'000'000ull, 48'000'000ull, 80'000'000ull, 160'000'000ull, 160'000'001ull,
       250'000'000ull, 1'000'000'000ull, 3'000'000'007ull})
  {
    for(double const cpi : {0.5, 1.0, 1.5, 2.0, 3.0, 7.25})
    {
      if(static_cast<double>(core_hz) / cpi < 1.0)
      {
        continue;
      }
      auto const hz = static_cast<std::uint64_t>(std::llround(static_cast<double>(core_hz) / cpi));
      auto const period = hz / std::gcd(hz, second_ps);
      std::vector<std::uint64_t> counts{0, 1, 2, hz - 1, hz, hz + 1, 1ull << 40, ~0ull};
      for(std::uint64_t whole : {1ull, 2ull, 3ull, 1000ull, 12345ull})
      {
        for(int delta = -1; delta <= 1; ++delta)
        {
          counts.push_back(
              whole * period + static_cast<std::uint64_t>(static_cast<std::int64_t>(delta)));
        }
      }
      for(int i = 0; i < 40; ++i)
      {
        counts.push_back(random.next() >> (random.next() % 64));
      }
      for(auto const count : counts)
      {
        popcorn::core::clock        clock{core_hz, cpi};
        popcorn::core::clock_driver driver{clock};
        CHECK(clock.instruction_hz() == hz);
        driver.jump_to({count});
        auto const expected = popcorn::vtime_from_instructions(count, hz).ps;
        if(clock.time().ps != expected or driver.exact_time().ps != expected)
        {
          FAIL("time differs, core_hz=" << core_hz << " cpi=" << cpi << " count=" << count);
        }
        ++compared;
      }
    }
  }
  CHECK(compared > 3000);
}

TEST_CASE("clock time follows rate changes and a restore like the exact conversion")
{
  popcorn::core::clock        clock{160'000'000};
  popcorn::core::clock_driver driver{clock};
  std::uint64_t               epoch_ps    = 0;
  std::uint64_t               epoch_count = 0;
  std::uint64_t               count       = 0;
  std::uint64_t               hz          = 160'000'000;
  lcg                         random;
  auto const                  start_epoch = [&] {
    epoch_ps += popcorn::vtime_from_instructions(count - epoch_count, hz).ps;
    epoch_count = count;
  };
  for(int step = 0; step < 300; ++step)
  {
    count += random.next() % 5'000'000;
    driver.jump_to({count});
    CHECK(
        clock.time().ps == epoch_ps + popcorn::vtime_from_instructions(count - epoch_count, hz).ps);
    if(step % 3 == 0)
    {
      auto const next = 1 + random.next() % 400'000'000;
      if(static_cast<double>(next) / clock.cpi() >= 1.0)
      {
        start_epoch();
        clock.set_core_hz(next);
        hz = static_cast<std::uint64_t>(std::llround(static_cast<double>(next) / clock.cpi()));
      }
    }
    else if(step % 3 == 1)
    {
      auto const cpi = 0.5 + static_cast<double>(random.next() % 700) / 100.0;
      if(static_cast<double>(clock.core_hz()) / cpi >= 1.0)
      {
        start_epoch();
        clock.set_cpi(cpi);
        hz = static_cast<std::uint64_t>(std::llround(static_cast<double>(clock.core_hz()) / cpi));
      }
    }
    CHECK(clock.instruction_hz() == hz);
  }

  popcorn::core::clock        copy{1};
  popcorn::core::clock_driver restoring{copy};
  restoring.restore(driver.state());
  CHECK(copy.instruction_hz() == hz);
  CHECK(copy.time().ps == clock.time().ps);
  driver.jump_to({count + 123'457});
  restoring.jump_to({count + 123'457});
  CHECK(copy.time().ps == clock.time().ps);
}
