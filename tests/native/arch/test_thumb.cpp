#include <popcorn/arch/armv8m/thumb.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace
{
namespace thumb = popcorn::arch::armv8m::thumb;
using popcorn::address;
using thumb::ending;

constexpr address at = 0x0800'1000;

auto narrow(std::uint16_t bits) -> thumb::insn
{
  return {.at = at, .bits = bits, .size = 2};
}

auto wide(std::uint16_t first, std::uint16_t second, address where = at) -> thumb::insn
{
  return {.at = where, .bits = std::uint32_t{first} << 16 | second, .size = 4};
}

auto little_endian(std::initializer_list<std::uint16_t> halves) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  for(auto const half : halves)
  {
    bytes.push_back(static_cast<std::byte>(half & 0xff));
    bytes.push_back(static_cast<std::byte>(half >> 8));
  }
  return bytes;
}

struct expected_row
{
  std::string_view name;
  thumb::insn      instruction;
  ending           kind;
  bool             conditional = false;
};
}

TEST_CASE("size is 4 exactly for first halfwords from 0xe800")
{
  for(std::uint32_t first = 0; first <= 0xffff; ++first)
  {
    auto const expected = first >= 0xe800 ? 4 : 2;
    if(thumb::size(static_cast<std::uint16_t>(first)) != expected)
    {
      FAIL(std::format("first halfword {:#06x}", first));
    }
  }
}

TEST_CASE("read decodes little-endian halfwords and refuses a cut instruction")
{
  std::array const code{
      std::byte{0x80}, std::byte{0xf3}, std::byte{0x11}, std::byte{0x88}, std::byte{0x70}};
  CHECK(thumb::read(at, code) == wide(0xf380, 0x8811));
  CHECK(thumb::read(at, std::span{code}.subspan(4)) == std::nullopt);
  CHECK(thumb::read(at, std::span{code}.first(3)) == std::nullopt);
  std::array const bx{std::byte{0x70}, std::byte{0x47}};
  CHECK(thumb::read(at, bx) == narrow(0x4770));
}

TEST_CASE("every block-ending row classifies with its kind")
{
  std::array const rows{
      expected_row{"b t1", narrow(0xd0fe), ending::direct, true},
      expected_row{"b t2", narrow(0xe7fd), ending::direct},
      expected_row{"cbz", narrow(0xb128), ending::direct, true},
      expected_row{"b t3", wide(0xf47f, 0xaffb), ending::direct, true},
      expected_row{"b t4", wide(0xf001, 0xb856), ending::direct},
      expected_row{"bl", wide(0xf001, 0xf854), ending::direct},
      expected_row{"bx lr", narrow(0x4770), ending::indirect},
      expected_row{"blx r3", narrow(0x4798), ending::indirect},
      expected_row{"bxns lr", narrow(0x4774), ending::indirect},
      expected_row{"blxns r3", narrow(0x479c), ending::indirect},
      expected_row{"add pc, r1", narrow(0x448f), ending::indirect},
      expected_row{"mov pc, lr", narrow(0x46f7), ending::indirect},
      expected_row{"pop {r4, pc}", narrow(0xbd10), ending::indirect},
      expected_row{"ldmia.w r0, {r1, pc}", wide(0xe890, 0x8002), ending::indirect},
      expected_row{"ldmdb r0, {r1, pc}", wide(0xe910, 0x8002), ending::indirect},
      expected_row{"pop.w {r4-r11, pc}", wide(0xe8bd, 0x8ff0), ending::indirect},
      expected_row{"ldr pc, [sp], #4", wide(0xf85d, 0xfb04), ending::indirect},
      expected_row{"ldr.w pc, [r0, #8]", wide(0xf8d0, 0xf008), ending::indirect},
      expected_row{"ldr.w pc, [r0, r1]", wide(0xf850, 0xf001), ending::indirect},
      expected_row{"ldr.w pc, literal", wide(0xf8df, 0xf000), ending::indirect},
      expected_row{"tbb", wide(0xe8d0, 0xf001), ending::indirect},
      expected_row{"tbh", wide(0xe8d0, 0xf011), ending::indirect},
      expected_row{"svc", narrow(0xdf01), ending::exception},
      expected_row{"bkpt", narrow(0xbe02), ending::exception},
      expected_row{"udf", narrow(0xde03), ending::exception},
      expected_row{"udf.w", wide(0xf7f0, 0xa004), ending::exception},
      expected_row{"yield", narrow(0xbf10), ending::system},
      expected_row{"wfe", narrow(0xbf20), ending::system},
      expected_row{"wfi", narrow(0xbf30), ending::system},
      expected_row{"yield.w", wide(0xf3af, 0x8001), ending::system},
      expected_row{"wfe.w", wide(0xf3af, 0x8002), ending::system},
      expected_row{"wfi.w", wide(0xf3af, 0x8003), ending::system},
      expected_row{"isb", wide(0xf3bf, 0x8f6f), ending::system},
      expected_row{"cpsie i", narrow(0xb662), ending::system},
      expected_row{"cpsid i", narrow(0xb672), ending::system},
      expected_row{"msr basepri, r0", wide(0xf380, 0x8811), ending::system},
      expected_row{"vmsr fpscr, r0", wide(0xeee1, 0x0a10), ending::system},
      expected_row{"vmsr fpexc, r0", wide(0xeee8, 0x0a10), ending::system},
      expected_row{"vlstm sp", wide(0xec2d, 0x0a00), ending::system},
      expected_row{"vlldm sp", wide(0xec3d, 0x0a00), ending::system},
      expected_row{"blx t2", wide(0xf000, 0xc000), ending::exception},
  };
  for(auto const& row : rows)
  {
    CAPTURE(row.name);
    auto const result = thumb::classify(row.instruction);
    CHECK(result.kind == row.kind);
    CHECK(result.conditional == row.conditional);
    CHECK(result.target.has_value() == (row.kind == ending::direct));
  }
}

TEST_CASE("near misses do not end a block")
{
  std::array const rows{
      expected_row{"nop", narrow(0xbf00), ending::none},
      expected_row{"sev", narrow(0xbf40), ending::none},
      expected_row{"nop.w", wide(0xf3af, 0x8000), ending::none},
      expected_row{"sev.w", wide(0xf3af, 0x8004), ending::none},
      expected_row{"dsb", wide(0xf3bf, 0x8f4f), ending::none},
      expected_row{"dmb", wide(0xf3bf, 0x8f5f), ending::none},
      expected_row{"sg", wide(0xe97f, 0xe97f), ending::none},
      expected_row{"add r1, r2", narrow(0x4411), ending::none},
      expected_row{"ldr.w r0, [r1, #8]", wide(0xf8d1, 0x0008), ending::none},
      expected_row{"pop {r4, r5}", narrow(0xbc30), ending::none},
      expected_row{"mrs r0, basepri", wide(0xf3ef, 0x8011), ending::none},
      expected_row{"vmsr fpinst, r0", wide(0xeee9, 0x0a10), ending::none},
      expected_row{"it eq", narrow(0xbf08), ending::none},
      expected_row{"moveq r0, r1", narrow(0x4608), ending::none},
      expected_row{"smc", wide(0xf7f0, 0x8000), ending::none},
      expected_row{"vlstm with a nonzero low halfword", wide(0xec2d, 0x0a01), ending::none},
  };
  for(auto const& row : rows)
  {
    CAPTURE(row.name);
    CHECK(thumb::classify(row.instruction) == thumb::flow{});
  }
}

TEST_CASE("direct branch targets")
{
  CHECK(thumb::classify(narrow(0xd0fe)).target == at);
  CHECK(thumb::classify(narrow(0xd080)).target == at + 4 - 256);
  CHECK(thumb::classify(narrow(0xe7fd)).target == at - 2);
  CHECK(thumb::classify(narrow(0xb3f8)).target == at + 4 + 126);
  CHECK(thumb::classify(wide(0xf07f, 0xafff, 0)).target == 0x0010'0002);
  CHECK(thumb::classify(wide(0xf400, 0x8000, 0x0020'0000)).target == 0x0010'0004);
  CHECK(thumb::classify(wide(0xf100, 0xf800, 0x0020'0004)).target == 0x0030'0008);
  CHECK(thumb::classify(wide(0xf001, 0xf854, 0x10e)).target == 0x11ba);
  CHECK(thumb::classify(wide(0xf47f, 0xaffb, 0x106)).target == 0x100);
  CHECK(thumb::classify(wide(0xf3ff, 0x97ff, 0x0100'0000)).target == 0x0200'0002);
  CHECK(thumb::classify(wide(0xf400, 0x9000, 0x0100'0000)).target == 0x4);
  CHECK(thumb::classify(wide(0xf000, 0xc000)).target == std::nullopt);
}

TEST_CASE("every table row's own value classifies as the first row matching it")
{
  auto const                 rows = thumb::block_end_rows();
  std::set<std::string_view> names;
  for(auto const& row : rows)
  {
    CAPTURE(row.name);
    CHECK_FALSE(row.name.empty());
    CHECK(names.insert(row.name).second);
    CHECK((row.value & row.mask) == row.value);
    thumb::insn const own{.at = at, .bits = row.value, .size = row.size};
    auto const first = std::ranges::find_if(rows, [&](thumb::block_end_row const& candidate) {
      return candidate.size == row.size and (row.value & candidate.mask) == candidate.value;
    });
    REQUIRE(first != rows.end());
    CHECK(thumb::classify(own).kind == first->kind);
    CHECK(thumb::classify(own).conditional == (first->kind != ending::none and first->conditional));
  }
}

TEST_CASE("is_it matches IT and not the hints")
{
  CHECK(thumb::is_it(narrow(0xbf08)));
  CHECK(thumb::is_it(narrow(0xbf1c)));
  CHECK_FALSE(thumb::is_it(narrow(0xbf00)));
  CHECK_FALSE(thumb::is_it(narrow(0xbf30)));
  CHECK_FALSE(thumb::is_it(wide(0xf3af, 0x8008)));
}

TEST_CASE("last_ends_block classifies the last whole instruction of a run")
{
  auto const ends = [](std::initializer_list<std::uint16_t> halves) {
    return thumb::last_ends_block(at, little_endian(halves));
  };
  CHECK(ends({0x2001}) == false);
  CHECK(ends({0x2001, 0xd1fe}) == true);
  CHECK(ends({0x4770}) == true);
  CHECK(ends({0x2001, 0xbd00}) == true);
  CHECK(ends({0xbf08}) == false);
  CHECK(ends({0xbf08, 0x4770}) == true);
  CHECK(ends({0xbf08, 0x4608}) == false);
  CHECK(ends({0x2001, 0xf000, 0xf800}) == true);
  CHECK(ends({0xf000, 0xf800, 0xf3af, 0x8000}) == false);
  CHECK(ends({0xf3af, 0x8000, 0xf85d, 0xfb04}) == true);
  CHECK_FALSE(ends({}).has_value());
  CHECK_FALSE(ends({0x2001, 0xf000}).has_value());
  auto const odd = little_endian({0x2001, 0x4770});
  CHECK_FALSE(thumb::last_ends_block(at, std::span{odd}.first(3)).has_value());
}
