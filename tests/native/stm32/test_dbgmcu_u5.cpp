#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <cstdint>

namespace
{
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint32_t dbgmcu_base = 0xE004'4000;
constexpr std::uint32_t idcode      = dbgmcu_base + 0x00;
constexpr std::uint32_t cr          = dbgmcu_base + 0x04;
constexpr std::uint32_t apb1lfzr    = dbgmcu_base + 0x08;
constexpr std::uint32_t apb2fzr     = dbgmcu_base + 0x10;
constexpr std::uint32_t u575_rev_u  = 0x3007'6482;

auto strict_idle() -> stm32_fixture
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  stm32_fixture f{u575(), "rcc_ll", options};
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  return f;
}

auto read(stm32_fixture const& f, std::uint32_t at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return *value;
}

void write(stm32_fixture const& f, std::uint32_t at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}
}

TEST_CASE("DBGMCU_IDCODE reads DEV_ID 0x482 and REV_ID 0x3007, a write leaves it")
{
  auto const f = strict_idle();
  REQUIRE(f.target->model("DBGMCU") != nullptr);
  CHECK(read(f, idcode) == u575_rev_u);
  auto const id = read(f, idcode);
  CHECK((id & 0xFFFu) == 0x482u);
  CHECK((id >> 16) == 0x3007u);
  write(f, idcode, 0);
  CHECK(read(f, idcode) == u575_rev_u);
}

TEST_CASE("DBGMCU_CR and the freeze registers keep what is written until a power-on reset")
{
  auto const f = strict_idle();
  write(f, cr, 0x6);
  write(f, apb1lfzr, 0x1);
  write(f, apb2fzr, 0x0002'0000);
  CHECK(read(f, cr) == 0x6);
  CHECK(read(f, apb1lfzr) == 0x1);
  CHECK(read(f, apb2fzr) == 0x0002'0000);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(read(f, cr) == 0x6);
  CHECK(read(f, apb2fzr) == 0x0002'0000);
  f.target->reset(popcorn::reset_kind::power_on);
  CHECK(read(f, cr) == 0);
  CHECK(read(f, apb1lfzr) == 0);
  CHECK(read(f, apb2fzr) == 0);
  CHECK(read(f, idcode) == u575_rev_u);
}
