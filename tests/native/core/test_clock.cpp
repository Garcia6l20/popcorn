#include <popcorn/core/clock.hpp>

#include <doctest/doctest.h>

#include <type_traits>

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
