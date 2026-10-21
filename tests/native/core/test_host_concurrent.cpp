#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::testing::m33;

auto fnv_goal(std::move_only_function<bool()> stop_when) -> core::run_goal
{
  return {.budget = icount{1'000'000}, .stop_when = std::move(stop_when)};
}

auto fnv() -> std::unique_ptr<core::machine>
{
  popcorn::testing::machine_fixture<> f{m33(), "fnv", core::machine_options{}};
  return std::move(f.target);
}

auto one(std::unique_ptr<core::machine> member) -> std::vector<std::unique_ptr<core::machine>>
{
  std::vector<std::unique_ptr<core::machine>> members;
  members.push_back(std::move(member));
  return members;
}

struct result
{
  icount                     instructions{};
  icount                     executed{};
  std::vector<std::uint64_t> regs{};
  std::vector<std::byte>     sram{};

  friend auto operator==(result const&, result const&) -> bool = default;
};

auto fnv_regs(core::machine& m) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> values;
  for(std::string_view const name :
      {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr",
       "pc"})
  {
    auto const found = m.regs().find(name);
    REQUIRE(found.has_value());
    values.push_back(m.regs().read(*found));
  }
  return values;
}

auto sram(core::machine& m) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes(786'432);
  REQUIRE(m.mem().read_block(0x2000'0000, bytes).has_value());
  return bytes;
}

auto observed(core::host& on, core::goal_result const& goal) -> result
{
  return core::access(on, 0, [&](core::machine& m) {
    return result{
        .instructions = goal.stopped.instructions,
        .executed     = goal.executed,
        .regs         = fnv_regs(m),
        .sram         = sram(m),
    };
  });
}

auto alone(core::slice_bounds bounds) -> result
{
  core::host                           on{one(fnv()), bounds};
  core::host_future<core::goal_result> future;
  {
    auto held = on.lock();
    future    = on.run(held, 0, fnv_goal([] { return false; }));
  }
  return observed(on, future.get());
}

auto together(core::slice_bounds bounds) -> std::pair<result, result>
{
  core::host                           first{one(fnv()), bounds};
  core::host                           second{one(fnv()), bounds};
  core::host_future<core::goal_result> a;
  core::host_future<core::goal_result> b;
  std::latch                           posting{2};
  std::latch                           inside{2};
  auto const                           post = [&](core::host& on, auto& future) {
    posting.arrive_and_wait();
    auto held = on.lock();
    future    = on.run(held, 0, fnv_goal([&inside, met = false]() mutable {
                      if(not std::exchange(met, true))
                      {
                        inside.arrive_and_wait();
                      }
                      return false;
                       }));
  };
  {
    std::jthread const ta{[&] { post(first, a); }};
    std::jthread const tb{[&] { post(second, b); }};
  }
  auto const ga = a.get();
  auto const gb = b.get();
  return {observed(first, ga), observed(second, gb)};
}
}

TEST_CASE("two machines running at once on their own hosts each equal one run alone")
{
  for(auto const bounds :
      {core::slice_bounds{.max_instructions = icount{100'000}},
       core::slice_bounds{.max_instructions = icount{1'000}}})
  {
    auto const reference = alone(bounds);
    REQUIRE(reference.instructions.value != 0);
    for(int pair = 0; pair < 5; ++pair)
    {
      auto const [a, b] = together(bounds);
      CHECK(a == reference);
      CHECK(b == reference);
    }
  }
}
