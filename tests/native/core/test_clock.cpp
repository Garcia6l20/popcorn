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
