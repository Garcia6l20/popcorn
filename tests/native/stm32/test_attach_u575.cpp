#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::stop_reason;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;
}

TEST_CASE("every built-in model attaches to the full description, cross rules included")
{
  stm32::register_builtin_models();
  core::machine target{u575(), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
  stm32::board const board{};
  CHECK(stm32::factories(board, u575()).contains("RCC"));
  CHECK(target.model("RCC") != nullptr);
  auto const trustzone_only = [](std::string_view name) { return name.starts_with("GTZC"); };
  for(auto const& [name, factory] : stm32::factories(board, u575()))
  {
    CAPTURE(name);
    CHECK((target.model(name) != nullptr) != trustzone_only(name));
  }
  core::machine_options wired = popcorn::testing::stm32_options();
  wired.models                = stm32::factories(board, u575());
  CHECK_NOTHROW(core::machine{u575(), popcorn::arch::armv8m::factory(), std::move(wired)});
  auto secured = popcorn::testing::stm32_options();
  REQUIRE(stm32::configure_option_bytes(u575(), {.tzen = true}, secured).has_value());
  core::machine trustzone{u575(), popcorn::arch::armv8m::factory(), std::move(secured)};
  for(auto const& [name, factory] : stm32::factories(board, u575()))
  {
    CAPTURE(name);
    CHECK(trustzone.model(name) != nullptr);
  }
}

TEST_CASE("an LL firmware boots on the CMSIS startup and reads the reset clock tree")
{
  std::vector<std::vector<std::uint32_t>> logs;
  for(int run = 0; run < 2; ++run)
  {
    stm32_fixture f{u575(), "boot_ll"};
    CHECK(f.run_to_stop().reason == stop_reason::user);
    logs.push_back(f.log());
  }
  CHECK(
      logs.front()
      == std::vector<std::uint32_t>{4'000'000, 4'000'000, 4'000'000, 4'000'000, 0x4000'0000});
  CHECK(logs.front() == logs.back());
}
