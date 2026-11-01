#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace
{
using popcorn::stop_reason;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::uint32_t sram_base  = 0x2000'0000;
constexpr std::uint32_t ram_marker = 0x5a5a'0001;

auto word_at(stm32_fixture const& f, popcorn::address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void check_smoke(
    popcorn::part_description desc,
    std::string_view          firmware,
    std::uint32_t             idcode,
    std::uint32_t             flash_kib,
    std::uint32_t             sram_kib)
{
  stm32_fixture f{std::move(desc), firmware};
  CHECK(word_at(f, 0x0800'0000) == sram_base + sram_kib * 1024);
  CHECK(f.run_to_stop({.max_instructions = popcorn::icount{10'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{idcode, flash_kib, ram_marker});
  auto const ram_func = sym(firmware, "ram_probe") & ~popcorn::address{1};
  CHECK(ram_func >= sram_base);
  CHECK(ram_func < sram_base + sram_kib * 1024);
}
}

TEST_CASE("the L152RE smoke firmware reads DEV_ID 0x437 and F_SIZE 512 and runs a RAM function")
{
  check_smoke(l152re(), "l152xe_l1_smoke", 0x1000'0437, 512, 32);
}

TEST_CASE("the L152RB smoke firmware reads DEV_ID 0x416 and F_SIZE 128 and runs a RAM function")
{
  check_smoke(l152rb(), "l152xb_l1_smoke", 0x1000'0416, 128, 16);
}
