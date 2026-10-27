#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::stop_reason;
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;

constexpr auto attached = std::to_array<std::string_view>({
    "ADC1",   "ADC2",   "ADCC",   "CRC",   "DBGMCU", "DCACHE", "EXTI",   "FLASH",   "GPDMA1",
    "GPDMA2", "GPIOA",  "GPIOB",  "GPIOC", "GPIOD",  "GPIOE",  "GPIOF",  "GPIOG",   "GPIOH",
    "GPIOI",  "HASH",   "I2C1",   "I2C2",  "I2C3",   "I2C4",   "ICACHE", "IWDG",    "LPUART",
    "PWR",    "RCC",    "RNG",    "RTC",   "SAI1",   "SAI2",   "SPI1",   "SPI2",    "SPI3",
    "SPI4",   "SPI5",   "SPI6",   "TAMP",  "TIM1",   "TIM12",  "TIM13",  "TIM14",   "TIM15",
    "TIM16",  "TIM17",  "TIM2",   "TIM3",  "TIM4",   "TIM5",   "TIM6",   "TIM7",    "TIM8",
    "UART12", "UART4",  "UART5",  "UART7", "UART8",  "UART9",  "USART1", "USART10", "USART11",
    "USART2", "USART3", "USART6",
});

constexpr auto matched_without_model = std::to_array<std::string_view>(
    {"GTZC1_MPCBB1", "GTZC1_MPCBB2", "GTZC1_MPCBB3", "GTZC1_TZIC", "GTZC1_TZSC"});

constexpr auto without_row = std::to_array<std::string_view>({"SBS", "USB"});

constexpr std::uint32_t sram_end = 0x2000'0000 + 640 * 1024;

auto word_at(stm32_fixture const& f, popcorn::address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto contains(auto const& list, std::string_view name) -> bool
{
  return std::ranges::find(list, name) != list.end();
}
}

TEST_CASE("the H573 description is the H573 and its trustzone units match the GTZC row only")
{
  CHECK(h573().part == "STM32H573AIIx");
  stm32::register_builtin_models();
  stm32::board const board{};
  auto const         factories = stm32::factories(board, h573());
  for(auto const name : matched_without_model)
  {
    CAPTURE(name);
    CHECK(factories.contains(std::string{name}));
  }
  for(auto const name : without_row)
  {
    CAPTURE(name);
    CHECK_FALSE(factories.contains(std::string{name}));
  }
  CHECK_FALSE(factories.contains("SYS"));
  CHECK(std::ranges::none_of(h573().peripherals, [](auto const& unit) {
    return unit.name == "SYS";
  }));
  auto expected = std::vector<std::string>{attached.begin(), attached.end()};
  expected.insert(expected.end(), matched_without_model.begin(), matched_without_model.end());
  std::ranges::sort(expected);
  std::vector<std::string> rows;
  for(auto const& [name, factory] : factories)
  {
    rows.push_back(name);
  }
  std::ranges::sort(rows);
  CHECK(rows == expected);
}

TEST_CASE("every H573 peripheral with a built-in row has a model, and the rest have none")
{
  stm32::register_builtin_models();
  core::machine target{h573(), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
  for(auto const& unit : h573().peripherals)
  {
    CAPTURE(unit.name);
    std::string_view const base = unit.name.starts_with("SEC_")
                                    ? std::string_view{unit.name}.substr(4)
                                    : std::string_view{unit.name};
    CHECK((target.model(unit.name) != nullptr) == contains(attached, base));
  }
  for(auto const name : attached)
  {
    CAPTURE(name);
    CHECK(target.model(std::string{name}) != nullptr);
  }
  for(auto const name : matched_without_model)
  {
    CAPTURE(name);
    CHECK(target.model(std::string{name}) == nullptr);
  }
  for(auto const name : without_row)
  {
    CAPTURE(name);
    CHECK(target.model(std::string{name}) == nullptr);
  }
}

TEST_CASE("the RNG attaches on the H573 with the reference manual reset values")
{
  stm32_fixture f{h573(), "h573_boot_ll"};
  REQUIRE(f.target->model("RNG") != nullptr);
  auto const& rng = f.target->peripheral("RNG");
  CHECK(rng.peek(0x00) == 0x0080'0d00);
  CHECK(rng.peek(0x04) == 0);
}

TEST_CASE("an LL firmware boots on the H5 CMSIS startup and reads the reset clock tree")
{
  std::vector<std::vector<std::uint32_t>> logs;
  for(int run = 0; run < 2; ++run)
  {
    stm32_fixture f{h573(), "h573_boot_ll"};
    CHECK(word_at(f, 0x0800'0000) == sram_end);
    CHECK(f.run_to_stop().reason == stop_reason::user);
    logs.push_back(f.log());
  }
  REQUIRE(logs.front().size() == 5);
  CHECK(
      std::vector<std::uint32_t>(logs.front().begin() + 1, logs.front().end())
      == std::vector<std::uint32_t>{32'000'000, 32'000'000, 32'000'000, 0x8});
  CHECK(logs.front() == logs.back());
}
