#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace
{
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware    = "l476_l4_smoke";
constexpr std::uint32_t sram1_base  = 0x2000'0000;
constexpr std::uint32_t sram1_size  = 96 * 1024;
constexpr std::uint32_t sram2_base  = 0x1000'0000;
constexpr std::uint32_t sram2_size  = 32 * 1024;
constexpr std::uint32_t vector_size = (16 + 82) * 4;

auto word_at(stm32_fixture const& f, popcorn::address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto run_smoke() -> std::vector<std::uint32_t>
{
  stm32_fixture f{l476rc(), firmware};
  CHECK(f.run_to_stop({.max_instructions = popcorn::icount{10'000}}).reason == stop_reason::user);
  return f.log();
}
}

TEST_CASE("the L476RC smoke reads DEV_ID 0x415 and F_SIZE 256 and uses both SRAMs and the FPU")
{
  auto const log = run_smoke();
  REQUIRE(log.size() == 9);
  CHECK(
      std::vector<std::uint32_t>{log.begin(), log.begin() + 5}
      == std::vector<std::uint32_t>{0x1007'6415, 256, 0x5a5a'0001, 0x5a5a'0002, 13});
  auto const ram_func = sym(firmware, "ram_probe");
  CHECK(ram_func >= sram1_base);
  CHECK(ram_func < sram1_base + sram1_size);
  auto const sram2_word = sym(firmware, "sram2_word");
  CHECK(sram2_word >= sram2_base);
  CHECK(sram2_word < sram2_base + sram2_size);
}

TEST_CASE("the L476RC smoke firmware enables GPIOA in RCC and drives PA5 high then low in ODR")
{
  auto const log = run_smoke();
  REQUIRE(log.size() == 9);
  CHECK(log[5] == 0x0000'0001);
  CHECK(log[6] == 0xABFF'F7FF);
  CHECK(log[7] == 0x0000'0020);
  CHECK(log[8] == 0x0000'0000);
}

TEST_CASE("word 97 of the L476RC smoke vector table holds FPU_IRQHandler and word 0 the SRAM1 top")
{
  stm32_fixture f{l476rc(), firmware};
  CHECK(sym(firmware, "g_pfnVectors") == 0x0800'0000);
  CHECK(word_at(f, 0x0800'0000) == sram1_base + sram1_size);
  CHECK(word_at(f, 0x0800'0000 + vector_size - 4) == (sym(firmware, "FPU_IRQHandler") | 1u));
}
