#include "exceptions.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace
{
namespace armv8m = popcorn::arch::armv8m;
using armv8m::exception_state;

auto reference_best(exception_state const& exceptions, bool synchronous_only)
    -> std::optional<std::uint32_t>
{
  std::optional<std::uint32_t> best;
  for(std::uint32_t number = armv8m::exc_nmi; number < exceptions.priority.size(); ++number)
  {
    auto const enabled = number < armv8m::exc_irq0 or armv8m::test_bit(exceptions.enabled, number);
    if(not enabled
       or not armv8m::test_bit(exceptions.pending, number)
       or (synchronous_only and not armv8m::synchronous(number)))
    {
      continue;
    }
    if(not best or armv8m::priority_of(exceptions, number) < armv8m::priority_of(exceptions, *best))
    {
      best = number;
    }
  }
  return best;
}

auto reference_any(exception_state const& exceptions, std::uint32_t count, bool synchronous_only)
    -> bool
{
  for(std::uint32_t number = 2; number < count; ++number)
  {
    auto const enabled = number < 16 or armv8m::test_bit(exceptions.enabled, number);
    if(armv8m::test_bit(exceptions.pending, number)
       and enabled
       and (not synchronous_only or armv8m::synchronous(number)))
    {
      return true;
    }
  }
  return false;
}

auto seed() -> std::uint64_t
{
  char const* const text = std::getenv("POPCORN_PENDING_SCAN_SEED");
  return text == nullptr ? 0x5eed'0005u : std::stoull(text, nullptr, 0);
}

auto random_words(std::mt19937_64& random, std::size_t count, int density)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out(count);
  for(auto& word : out)
  {
    for(auto bit = 0; bit < 32; ++bit)
    {
      if(static_cast<int>(random() % 100) < density)
      {
        word |= 1u << bit;
      }
    }
  }
  return out;
}

auto random_state(std::mt19937_64& random) -> exception_state
{
  auto const count = static_cast<std::uint32_t>(16 + random() % 481);
  auto const words = (count + 31) / 32;
  auto const sized = [&] {
    auto const pick = random() % 8;
    return pick == 0 ? words - random() % words : pick == 1 ? words + 1 + random() % 2 : words;
  };
  std::vector<std::uint8_t> priority(count);
  auto const                levels = 1 + random() % 8;
  for(auto& value : priority)
  {
    value = static_cast<std::uint8_t>((random() % levels) << 5);
  }
  auto const density = static_cast<int>(random() % 30);
  return {
      .priority = std::move(priority),
      .enabled  = random_words(random, sized(), 50 + density),
      .pending  = random_words(random, sized(), 1 + density),
      .active   = std::vector<std::uint32_t>(words, 0),
      .level    = std::vector<std::uint32_t>(words, 0),
  };
}
}

TEST_CASE("the word scan picks the same pending exception as the per-number loop")
{
  auto const      from = seed();
  std::mt19937_64 random{from};
  for(auto round = 0; round < 100'000; ++round)
  {
    auto const state            = random_state(random);
    auto const synchronous_only = (random() & 1u) != 0;
    auto const count            = static_cast<std::uint32_t>(state.priority.size());
    INFO(std::format("POPCORN_PENDING_SCAN_SEED={:#x} round {} count {}", from, round, count));
    REQUIRE(
        armv8m::best_pending(state, synchronous_only) == reference_best(state, synchronous_only));
    REQUIRE(
        armv8m::any_pending(state, count, synchronous_only)
        == reference_any(state, count, synchronous_only));
  }
}

TEST_CASE("the word scan keeps the lowest number among equal priorities")
{
  exception_state state{
      .priority = std::vector<std::uint8_t>(80, 0x40),
      .enabled  = {0xffff'ffffu, 0xffff'ffffu, 0xffff'ffffu},
      .pending  = {1u << 20,     1u << 1,      1u << 15    },
      .active   = {0,            0,            0           },
      .level    = {0,            0,            0           },
  };
  CHECK(armv8m::best_pending(state) == 20u);
  state.priority[79] = 0x20;
  CHECK(armv8m::best_pending(state) == 79u);
  state.pending = {(1u << 2) | (1u << 11), 0, 0};
  CHECK(armv8m::best_pending(state) == 2u);
  CHECK(armv8m::best_pending(state, true) == 11u);
  state.pending = {1u << 1, 0, 0};
  CHECK_FALSE(armv8m::best_pending(state).has_value());
  CHECK_FALSE(armv8m::any_pending(state, 80));
}
