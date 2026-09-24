#include <popcorn/periph/register_file.hpp>

#include <doctest/doctest.h>

#include <type_traits>

static_assert(not std::is_copy_constructible_v<popcorn::periph::register_file>);
static_assert(not std::is_copy_assignable_v<popcorn::periph::register_file>);
static_assert(not std::is_move_constructible_v<popcorn::periph::register_file>);
static_assert(not std::is_move_assignable_v<popcorn::periph::register_file>);

TEST_CASE("register_file keeps its description and stores words without side effects")
{
  popcorn::peripheral_desc desc;
  desc.name = "UART1";
  desc.size = 0x400;
  popcorn::periph::register_file regs{desc};
  CHECK(&regs.description() == &desc);
  CHECK(regs.peek(0x3fc) == 0);
  regs.poke(0x3fc, 0xdead'beef);
  CHECK(regs.peek(0x3fc) == 0xdead'beef);
  CHECK(regs.peek(0) == 0);
}
