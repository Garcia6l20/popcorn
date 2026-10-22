#include "dwarf_macros.hpp"
#include "macro_sections.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using popcorn::image::macro_entry;
using popcorn::image::macro_index;
using popcorn::image::detail::read_macros;
using popcorn::testing::dwarf_bytes;
using popcorn::testing::macro_section;
using popcorn::testing::synthetic_info;
using popcorn::testing::synthetic_unit;

namespace
{
auto entries_of(macro_index const& index, std::string_view name) -> std::vector<macro_entry>
{
  return index.entries
       | std::views::filter([&](macro_entry const& entry) { return entry.name == name; })
       | std::ranges::to<std::vector>();
}

auto one_unit(macro_section const& macros, std::uint32_t at) -> macro_index
{
  synthetic_info const info{{{.name = "a.c", .macros = at}}};
  return read_macros(info.sections(), macros.bytes()).index;
}
}

TEST_CASE("an undef after a define leaves the name out of the unit")
{
  macro_section macros;
  auto const    at = macros.unit();
  macros.define("GONE 5");
  macros.undef("GONE");
  macros.define("KEPT 1");
  macros.end();
  auto const index = one_unit(macros, at);
  CHECK(entries_of(index, "GONE").empty());
  CHECK(entries_of(index, "KEPT").size() == 1);
}

TEST_CASE("a redefinition keeps the last definition")
{
  macro_section macros;
  auto const    at = macros.unit();
  macros.define("MODE 1");
  macros.define("MODE 2");
  macros.end();
  auto const mode = entries_of(one_unit(macros, at), "MODE");
  REQUIRE(mode.size() == 1);
  CHECK(mode.front().text == "2");
}

TEST_CASE("an import runs in place and a second import of the same unit adds nothing")
{
  macro_section macros;
  auto const    shared = macros.unit();
  macros.define("SHARED 7");
  macros.define("LATE 1");
  macros.end();
  auto const once = macros.unit();
  macros.define("LATE 0");
  macros.import(shared);
  macros.end();
  auto const twice = macros.unit();
  macros.define("LATE 0");
  macros.import(shared);
  macros.start_file();
  macros.import(shared);
  macros.end_file();
  macros.end();

  synthetic_info const info{
      {{.name = "once.c", .macros = once}, {.name = "twice.c", .macros = twice}}
  };
  auto const scan = read_macros(info.sections(), macros.bytes());
  CHECK(scan.units_decoded == 3);
  auto const late = entries_of(scan.index, "LATE");
  REQUIRE(late.size() == 1);
  CHECK(late.front().text == "1");
  CHECK(late.front().units == std::vector<std::uint32_t>{0, 1});
  CHECK(scan.index.entries.size() == 2);
}

TEST_CASE("a diamond of imports runs the shared unit once per path, with no cycle")
{
  macro_section macros;
  auto const    root = macros.unit();
  macros.define("ROOT 1");
  macros.end();
  auto const bottom = macros.unit();
  macros.define("BOTTOM 1");
  macros.end();
  auto const left = macros.unit();
  macros.import(bottom);
  macros.define("LEFT 1");
  macros.end();
  auto const right = macros.unit();
  macros.import(bottom);
  macros.define("RIGHT 1");
  macros.end();
  auto const top = macros.unit();
  macros.import(left);
  macros.import(right);
  macros.end();

  synthetic_info const info{
      {{.name = "root.c", .macros = root}, {.name = "top.c", .macros = top}}
  };
  auto const index = read_macros(info.sections(), macros.bytes()).index;
  for(auto const* name : {"BOTTOM", "LEFT", "RIGHT"})
  {
    auto const found = entries_of(index, name);
    REQUIRE(found.size() == 1);
    CHECK(found.front().units == std::vector<std::uint32_t>{1});
  }
}

TEST_CASE("an import of offset 0 is skipped when a compile unit's own macros start there")
{
  macro_section macros;
  auto const    first = macros.unit(5, false, true);
  REQUIRE(first == 0);
  macros.define("FIRST_ONLY 1");
  macros.import(0);
  macros.end();
  auto const shared = macros.unit();
  macros.define("SHARED 1");
  macros.end();
  auto const second = macros.unit(5, false, true);
  macros.import(0);
  macros.import(shared);
  macros.define("SECOND_ONLY 1");
  macros.end();

  synthetic_info const info{
      {{.name = "first.c", .macros = first}, {.name = "second.c", .macros = second}}
  };
  auto const index = read_macros(info.sections(), macros.bytes()).index;
  CHECK(index.units == std::vector<std::string>{"first.c", "second.c"});
  auto const first_only = entries_of(index, "FIRST_ONLY");
  REQUIRE(first_only.size() == 1);
  CHECK(first_only.front().units == std::vector<std::uint32_t>{0});
  auto const second_only = entries_of(index, "SECOND_ONLY");
  REQUIRE(second_only.size() == 1);
  CHECK(second_only.front().units == std::vector<std::uint32_t>{1});
  CHECK(entries_of(index, "SHARED").size() == 1);
  CHECK(index.entries.size() == 3);
}

TEST_CASE("an import of offset 0 runs when no compile unit's own macros start there")
{
  macro_section macros;
  auto const    shared = macros.unit();
  REQUIRE(shared == 0);
  macros.define("AT_ZERO 1");
  macros.end();
  auto const root = macros.unit();
  macros.import(0);
  macros.end();
  auto const found = entries_of(one_unit(macros, root), "AT_ZERO");
  REQUIRE(found.size() == 1);
  CHECK(found.front().units == std::vector<std::uint32_t>{0});
}

TEST_CASE("define_strp reads its text from .debug_str")
{
  dwarf_bytes str;
  str.cstr("unused");
  auto const text_at = str.size();
  str.cstr("FROM_STR 42");

  macro_section macros;
  auto const    at = macros.unit();
  macros.strp(text_at);
  macros.end();
  synthetic_info const info{{{.name = "a.c", .macros = at}}};
  auto const           found =
      entries_of(read_macros(info.sections(str.bytes), macros.bytes()).index, "FROM_STR");
  REQUIRE(found.size() == 1);
  CHECK(found.front().text == "42");
}

TEST_CASE("a strx define in a shared unit resolves through each importing unit's base")
{
  dwarf_bytes str;
  auto const  a_at = str.size();
  str.cstr("A 1");
  auto const b_at = str.size();
  str.cstr("B 2");

  dwarf_bytes offsets;
  offsets.u32(12);
  offsets.u16(5);
  offsets.u16(0);
  offsets.u32(static_cast<std::uint32_t>(a_at));
  offsets.u32(0);
  offsets.u32(static_cast<std::uint32_t>(b_at));

  macro_section macros;
  auto const    shared = macros.unit();
  macros.strx(0);
  macros.end();
  auto const first = macros.unit();
  macros.import(shared);
  macros.end();
  auto const second = macros.unit();
  macros.import(shared);
  macros.end();

  synthetic_info const info{
      {{.name = "first.c", .macros = first, .str_offsets_base = 8},
       {.name = "second.c", .macros = second, .str_offsets_base = 16}}
  };
  auto const index = read_macros(info.sections(str.bytes, offsets.bytes), macros.bytes()).index;
  auto const a     = entries_of(index, "A");
  auto const b     = entries_of(index, "B");
  REQUIRE(a.size() == 1);
  REQUIRE(b.size() == 1);
  CHECK(a.front().units == std::vector<std::uint32_t>{0});
  CHECK(b.front().units == std::vector<std::uint32_t>{1});
}

TEST_CASE("a strx define of a 64-bit unit reads 8-byte .debug_str_offsets entries")
{
  dwarf_bytes str;
  str.cstr("pad");
  auto const text_at = str.size();
  str.cstr("WIDE_INDEX 9");

  dwarf_bytes offsets;
  offsets.u32(0xFFFF'FFFFu);
  offsets.u64(12);
  offsets.u16(5);
  offsets.u16(0);
  offsets.u64(0);
  offsets.u64(text_at);

  macro_section macros;
  auto const    at = macros.unit();
  macros.strx(1);
  macros.end();
  synthetic_info const info{{{.name = "a.c", .macros = at, .str_offsets_base = 16, .is64 = true}}};
  auto const           found = entries_of(
      read_macros(info.sections(str.bytes, offsets.bytes), macros.bytes()).index, "WIDE_INDEX");
  REQUIRE(found.size() == 1);
  CHECK(found.front().text == "9");
}

TEST_CASE("a unit name strx out of range spoils the macro heads only, not the line scan")
{
  synthetic_info const info{{{.name = "", .name_strx = 999}}};
  CHECK(popcorn::image::detail::scan_compile_units(info.sections()).units.empty());
  CHECK_THROWS_AS(
      std::ignore = popcorn::image::detail::unit_heads(info.sections()), popcorn::image_error);
}

TEST_CASE("define_strx resolves through the unit's nonzero str_offsets_base")
{
  dwarf_bytes str;
  str.cstr("pad");
  auto const name_at = str.size();
  str.cstr("unit.c");
  auto const text_at = str.size();
  str.cstr("BY_INDEX 3");

  dwarf_bytes offsets;
  offsets.u32(999);
  offsets.u32(999);
  offsets.u32(999);
  offsets.u32(999);
  offsets.u32(static_cast<std::uint32_t>(name_at));
  offsets.u32(static_cast<std::uint32_t>(text_at));

  macro_section macros;
  auto const    at = macros.unit();
  macros.strx(1);
  macros.end();
  synthetic_info const info{{{.name = "", .macros = at, .name_strx = 0, .str_offsets_base = 16}}};
  auto const index = read_macros(info.sections(str.bytes, offsets.bytes), macros.bytes()).index;
  CHECK(index.units == std::vector<std::string>{"unit.c"});
  auto const found = entries_of(index, "BY_INDEX");
  REQUIRE(found.size() == 1);
  CHECK(found.front().text == "3");
}

TEST_CASE("a 64-bit offset-size header reads 8-byte offsets for strp and import")
{
  dwarf_bytes str;
  str.cstr("WIDE 64");

  macro_section macros;
  auto const    leaf = macros.unit(5, true, true);
  macros.strp(0);
  macros.end();
  auto const root = macros.unit(5, true);
  macros.import(leaf);
  macros.end();
  synthetic_info const info{{{.name = "a.c", .macros = root}}};
  auto const           found =
      entries_of(read_macros(info.sections(str.bytes), macros.bytes()).index, "WIDE");
  REQUIRE(found.size() == 1);
  CHECK(found.front().text == "64");
}

TEST_CASE("a GNU version 4 unit reached through DW_AT_GNU_macros is read")
{
  macro_section macros;
  auto const    at = macros.unit(4);
  macros.define("GNU_FOUR 4");
  macros.end();
  synthetic_info const info{{{.name = "a.c", .macros = at, .gnu_macros = true}}};
  CHECK(entries_of(read_macros(info.sections(), macros.bytes()).index, "GNU_FOUR").size() == 1);
}

TEST_CASE("a vendor opcode is skipped by the forms of the operands table")
{
  constexpr std::uint8_t dw_form_data2  = 0x05;
  constexpr std::uint8_t dw_form_string = 0x08;
  macro_section          macros;
  auto const             at = macros.unit_with_operands(0xe5, {dw_form_data2, dw_form_string});
  macros.raw(0xe5);
  macros.raw(0x34);
  macros.raw(0x12);
  for(char const c : std::string_view{"skipped"})
  {
    macros.raw(static_cast<std::uint8_t>(c));
  }
  macros.raw(0);
  macros.define("AFTER 1");
  macros.end();
  CHECK(entries_of(one_unit(macros, at), "AFTER").size() == 1);
}

TEST_CASE("a function-like define keeps its parameters in the text, a bare define is empty")
{
  macro_section macros;
  auto const    at = macros.unit();
  macros.define("SQUARE(x) ((x) * (x))");
  macros.define("BARE");
  macros.define("SPACED (1)");
  macros.end();
  auto const index  = one_unit(macros, at);
  auto const square = entries_of(index, "SQUARE");
  REQUIRE(square.size() == 1);
  CHECK(square.front().function_like);
  CHECK(square.front().text == "(x) ((x) * (x))");
  auto const bare = entries_of(index, "BARE");
  REQUIRE(bare.size() == 1);
  CHECK_FALSE(bare.front().function_like);
  CHECK(bare.front().text.empty());
  auto const spaced = entries_of(index, "SPACED");
  REQUIRE(spaced.size() == 1);
  CHECK_FALSE(spaced.front().function_like);
  CHECK(spaced.front().text == "(1)");
}

TEST_CASE("a partial unit is not listed and units without macros are skipped")
{
  macro_section macros;
  auto const    at = macros.unit();
  macros.define("ONLY 1");
  macros.end();
  synthetic_info const info{
      {{.name = "partial.c", .macros = at, .partial = true},
       {.name = "plain.c"},
       {.name = "full.c", .macros = at}}
  };
  auto const index = read_macros(info.sections(), macros.bytes()).index;
  CHECK(index.units == std::vector<std::string>{"full.c"});
}

TEST_CASE("the consts firmware ends each unit with its own definitions")
{
  char const* const path = std::getenv("POPCORN_CONSTS_ELF");
  REQUIRE(path != nullptr);
  auto const index = popcorn::image::read_macros(path);
  REQUIRE(index.units.size() == 2);
  CHECK(index.units[0].ends_with("consts_a.c"));
  CHECK(index.units[1].ends_with("consts_b.c"));

  auto const limit = entries_of(index, "CONSTS_LIMIT");
  REQUIRE(limit.size() == 1);
  CHECK(limit.front().text == "16");
  CHECK(limit.front().units == std::vector<std::uint32_t>{0, 1});

  auto const mode = entries_of(index, "CONSTS_MODE");
  REQUIRE(mode.size() == 2);
  CHECK(mode[0].text == "1");
  CHECK(mode[0].units == std::vector<std::uint32_t>{0});
  CHECK(mode[1].text == "2");
  CHECK(mode[1].units == std::vector<std::uint32_t>{1});

  CHECK(entries_of(index, "CONSTS_GONE").empty());
  auto const square = entries_of(index, "CONSTS_SQUARE");
  REQUIRE(square.size() == 1);
  CHECK(square.front().function_like);
  CHECK(square.front().text == "(x) ((x) * (x))");
  auto const empty = entries_of(index, "CONSTS_EMPTY");
  REQUIRE(empty.size() == 1);
  CHECK(empty.front().text.empty());
  auto const arch = entries_of(index, "__ARM_ARCH");
  REQUIRE(arch.size() == 1);
  CHECK(arch.front().units == std::vector<std::uint32_t>{0, 1});
  CHECK(std::ranges::is_sorted(index.entries, {}, [](macro_entry const& entry) {
    return std::tie(entry.name, entry.text, entry.function_like);
  }));
}

TEST_CASE("a file without .debug_macro gives an empty index")
{
  char const* const path = std::getenv("POPCORN_IMAGE_ELF");
  REQUIRE(path != nullptr);
  CHECK(popcorn::image::read_macros(path) == macro_index{});
}
