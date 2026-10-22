#include "dwarf_macros.hpp"
#include "macro_sections.hpp"
#include "synthetic_elf.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>

#include <doctest/doctest.h>

#include <elf.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using popcorn::image::load_elf;
using popcorn::testing::synthetic_elf;

namespace
{
constexpr std::uint8_t attr_tag_file           = 1;
constexpr std::uint8_t attr_tag_abi_hardfp_use = 27;

template <typename T> void poke(std::vector<std::byte>& bytes, std::size_t offset, T value)
{
  REQUIRE(offset + sizeof(T) <= bytes.size());
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

auto peek_u32(std::vector<std::byte> const& bytes, std::size_t offset) -> Elf32_Word
{
  Elf32_Word value{};
  std::memcpy(&value, bytes.data() + offset, sizeof value);
  return value;
}

auto peek_u16(std::vector<std::byte> const& bytes, std::size_t offset) -> Elf32_Half
{
  Elf32_Half value{};
  std::memcpy(&value, bytes.data() + offset, sizeof value);
  return value;
}

auto build_symbol(
    Elf32_Word    name,
    Elf32_Addr    value,
    Elf32_Word    size,
    unsigned char info,
    Elf32_Half    shndx) -> std::vector<std::byte>
{
  Elf32_Sym sym{};
  sym.st_name  = name;
  sym.st_value = value;
  sym.st_size  = size;
  sym.st_info  = info;
  sym.st_shndx = shndx;
  std::vector<std::byte> out(sizeof(Elf32_Sym));
  std::memcpy(out.data(), &sym, sizeof(Elf32_Sym));
  return out;
}

void expect_image_error(
    std::filesystem::path const& path,
    std::string_view             segment,
    popcorn::address             addr,
    std::string_view             region_substr)
{
  try
  {
    std::ignore = load_elf(path);
    FAIL("expected image_error");
  }
  catch(popcorn::image_error const& e)
  {
    CHECK(e.segment() == segment);
    CHECK(e.addr() == addr);
    CHECK(e.region().find(region_substr) != std::string::npos);
  }
}

auto append_cstr(std::vector<std::byte>& out, std::string_view text) -> void
{
  for(char c : text)
  {
    out.push_back(static_cast<std::byte>(c));
  }
}

auto append_u32(std::vector<std::byte>& out, Elf32_Word value) -> void
{
  std::byte const* bytes = reinterpret_cast<std::byte const*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof value);
}

auto build_attrs_file_scope(std::vector<std::byte> const& file_scope_content)
    -> std::vector<std::byte>
{
  std::vector<std::byte> subsection;
  append_cstr(subsection, "aeabi");
  subsection.push_back(std::byte{0});
  subsection.push_back(static_cast<std::byte>(attr_tag_file));
  append_u32(subsection, static_cast<Elf32_Word>(5 + file_scope_content.size()));
  subsection.insert(subsection.end(), file_scope_content.begin(), file_scope_content.end());

  std::vector<std::byte> attrs{std::byte{'A'}};
  append_u32(attrs, static_cast<Elf32_Word>(4 + subsection.size()));
  attrs.insert(attrs.end(), subsection.begin(), subsection.end());
  return attrs;
}
}

TEST_CASE("load_elf rejects a PT_LOAD offset+size that wraps uint32")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(4, std::byte{0});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  auto              bytes    = builder.build();
  std::size_t const phdr_off = sizeof(Elf32_Ehdr);
  poke<Elf32_Word>(bytes, phdr_off + offsetof(Elf32_Phdr, p_offset), 0xFFFF'FFF0u);
  poke<Elf32_Word>(bytes, phdr_off + offsetof(Elf32_Phdr, p_filesz), 0x20u);
  poke<Elf32_Word>(bytes, phdr_off + offsetof(Elf32_Phdr, p_memsz), 0x20u);
  auto const path = synthetic_elf::write(bytes, "r1-ph-wrap.elf");
  expect_image_error(path, "header", 0, "truncated ELF: PT_LOAD segment");
}

TEST_CASE(
    "load_elf rejects an overlap the uint32 overlap check would miss at the top of "
    "the address space")
{
  synthetic_elf                builder;
  std::vector<std::byte> const first(72, std::byte{0});
  std::vector<std::byte> const second(4, std::byte{0});
  builder.add_segment(0xFFFF'FFB8u, 0xFFFF'FFB8u, first, 72);
  builder.add_segment(0xFFFF'FFC0u, 0xFFFF'FFC0u, second, 4);
  auto const path = synthetic_elf::write(builder.build(), "overlap-top.elf");
  expect_image_error(path, "1", 0xFFFF'FFC0u, "overlaps segment");
}

TEST_CASE("load_elf rejects a symtab section whose offset+size wraps uint32")
{
  synthetic_elf                builder;
  std::vector<std::byte> const strtab_data{std::byte{0}, std::byte{'a'}, std::byte{0}};
  auto const strtab_idx = builder.add_section("strtab", SHT_STRTAB, 0, strtab_data);
  auto const symtab_idx = builder.add_section(
      "symtab", SHT_SYMTAB, 0, build_symbol(1, 0x1000, 4, ELF32_ST_INFO(STB_GLOBAL, STT_OBJECT), 1),
      static_cast<Elf32_Word>(strtab_idx));
  auto       bytes  = builder.build();
  auto const sh_off = builder.shdr_file_offset(symtab_idx);
  poke<Elf32_Word>(bytes, sh_off + offsetof(Elf32_Shdr, sh_offset), 0xFFFF'FFF0u);
  poke<Elf32_Word>(bytes, sh_off + offsetof(Elf32_Shdr, sh_size), 0x20u);
  auto const path = synthetic_elf::write(bytes, "r2-symtab-wrap.elf");
  expect_image_error(path, "header", 0, "truncated ELF: section");
}

TEST_CASE("load_elf rejects a shstrtab offset+size that wraps uint32")
{
  synthetic_elf builder;
  auto const    dummy_idx = builder.add_section("dummy", SHT_PROGBITS, 0, {});
  auto          bytes     = builder.build();
  auto const    sh_off    = builder.shdr_file_offset(dummy_idx + 1);
  poke<Elf32_Word>(bytes, sh_off + offsetof(Elf32_Shdr, sh_offset), 0xFFFF'FFF0u);
  poke<Elf32_Word>(bytes, sh_off + offsetof(Elf32_Shdr, sh_size), 0x20u);
  auto const path = synthetic_elf::write(bytes, "r3a-shstrtab-wrap.elf");
  expect_image_error(path, "header", 0, "truncated ELF: section string table");
}

TEST_CASE("load_elf accepts a shstrtab that ends exactly at end of file")
{
  synthetic_elf builder;
  builder.add_section("dummy", SHT_PROGBITS, 0, {});
  auto const bytes = builder.build();
  auto const path  = synthetic_elf::write(bytes, "r3b-shstrtab-eof.elf");
  CHECK_NOTHROW(std::ignore = load_elf(path));
}

TEST_CASE("load_elf rejects a program header entry size that disagrees with Elf32_Phdr")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(4, std::byte{0});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  builder.header().e_phentsize = 1;
  auto const path              = synthetic_elf::write(builder.build(), "r4a-phentsize.elf");
  expect_image_error(path, "header", 0, "unexpected program header entry size");
}

TEST_CASE("load_elf rejects a section header entry size that disagrees with Elf32_Shdr")
{
  synthetic_elf builder;
  builder.add_section("dummy", SHT_PROGBITS, 0, {});
  auto bytes = builder.build();
  poke<Elf32_Half>(bytes, offsetof(Elf32_Ehdr, e_shentsize), 1);
  auto const path = synthetic_elf::write(bytes, "r4b-shentsize.elf");
  expect_image_error(path, "header", 0, "unexpected section header entry size");
}

TEST_CASE("load_elf rejects a misaligned program header table offset")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(4, std::byte{0});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  auto bytes = builder.build();
  bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(Elf32_Ehdr)), std::byte{0});
  poke<Elf32_Word>(
      bytes, offsetof(Elf32_Ehdr, e_phoff), static_cast<Elf32_Word>(sizeof(Elf32_Ehdr) + 1));
  auto const path = synthetic_elf::write(bytes, "r5a-phoff-misaligned.elf");
  expect_image_error(path, "header", 0, "misaligned program header table");
}

TEST_CASE("load_elf rejects a misaligned section header table offset")
{
  synthetic_elf builder;
  builder.add_section("dummy", SHT_PROGBITS, 0, {});
  auto       bytes         = builder.build();
  auto const misaligned_by = static_cast<Elf32_Word>(2 * sizeof(Elf32_Shdr)) - 1;
  poke<Elf32_Word>(
      bytes, offsetof(Elf32_Ehdr, e_shoff), static_cast<Elf32_Word>(bytes.size()) - misaligned_by);
  auto const path = synthetic_elf::write(bytes, "r5b-shoff-misaligned.elf");
  expect_image_error(path, "header", 0, "misaligned section header table");
}

TEST_CASE("load_elf rejects a symbol name with no NUL before the end of strtab")
{
  synthetic_elf                builder;
  std::vector<std::byte> const strtab_data{std::byte{0}, std::byte{'x'}};
  auto const strtab_idx = builder.add_section("strtab", SHT_STRTAB, 0, strtab_data);
  builder.add_section(
      "symtab", SHT_SYMTAB, 0, build_symbol(1, 0x1000, 4, ELF32_ST_INFO(STB_GLOBAL, STT_OBJECT), 1),
      static_cast<Elf32_Word>(strtab_idx));
  auto const path = synthetic_elf::write(builder.build(), "r6a-strtab-unterminated.elf");
  expect_image_error(path, "header", 0, "unterminated string");
}

TEST_CASE("load_elf rejects an ALLOC section name with no NUL before shstrtab ends")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(4, std::byte{0});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  auto const section_idx = builder.add_section(".x", SHT_PROGBITS, SHF_ALLOC, data);
  auto       bytes       = builder.build();

  auto const section_sh_off  = builder.shdr_file_offset(section_idx);
  auto const section_offset  = peek_u32(bytes, section_sh_off + offsetof(Elf32_Shdr, sh_offset));
  std::size_t const phdr_off = sizeof(Elf32_Ehdr);
  poke<Elf32_Word>(bytes, phdr_off + offsetof(Elf32_Phdr, p_offset), section_offset);

  auto const shstrtab_sh_off = builder.shdr_file_offset(section_idx + 1);
  auto const cur_size        = peek_u32(bytes, shstrtab_sh_off + offsetof(Elf32_Shdr, sh_size));
  poke<Elf32_Word>(bytes, shstrtab_sh_off + offsetof(Elf32_Shdr, sh_size), cur_size - 1);

  auto const path = synthetic_elf::write(bytes, "r6b-shstrtab-name-unterminated.elf");
  expect_image_error(path, "header", 0, "unterminated section name");
}

TEST_CASE("load_elf rejects .ARM.attributes with an unterminated vendor name")
{
  synthetic_elf          builder;
  std::vector<std::byte> attrs{std::byte{'A'}};
  append_u32(attrs, 4 + 5);
  append_cstr(attrs, "aeabi");
  builder.header().e_flags = EF_ARM_ABI_FLOAT_HARD;
  builder.add_section(".ARM.attributes", SHT_PROGBITS, 0, attrs);
  auto const path = synthetic_elf::write(builder.build(), "r6c-attrs-vendor-unterminated.elf");
  expect_image_error(path, "header", 0, "unterminated string");
}

TEST_CASE("load_elf does not hang or over-read on a truncated attribute scope")
{
  synthetic_elf          builder;
  std::vector<std::byte> subsection;
  append_cstr(subsection, "aeabi");
  subsection.push_back(std::byte{0});
  subsection.push_back(static_cast<std::byte>(attr_tag_file));
  subsection.push_back(std::byte{0});
  subsection.push_back(std::byte{0});

  std::vector<std::byte> attrs{std::byte{'A'}};
  append_u32(attrs, static_cast<Elf32_Word>(4 + subsection.size()));
  attrs.insert(attrs.end(), subsection.begin(), subsection.end());

  builder.header().e_flags = EF_ARM_ABI_FLOAT_HARD;
  builder.add_section(".ARM.attributes", SHT_PROGBITS, 0, attrs);
  auto const            path = synthetic_elf::write(builder.build(), "r7r8-scope-truncated.elf");
  popcorn::image::image built;
  CHECK_NOTHROW(built = load_elf(path));
  CHECK(built.abi == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("load_elf rejects a ULEB128 in .ARM.attributes whose shift reaches 64")
{
  synthetic_elf          builder;
  std::vector<std::byte> file_scope;
  file_scope.push_back(static_cast<std::byte>(attr_tag_abi_hardfp_use));
  for(int i = 0; i < 16; ++i)
  {
    file_scope.push_back(std::byte{0x80});
  }
  auto const attrs = build_attrs_file_scope(file_scope);

  builder.header().e_flags = EF_ARM_ABI_FLOAT_HARD;
  builder.add_section(".ARM.attributes", SHT_PROGBITS, 0, attrs);
  auto const path = synthetic_elf::write(builder.build(), "r9-uleb-shift.elf");
  expect_image_error(path, "header", 0, "malformed ULEB128");
}

TEST_CASE("load_elf rejects e_shstrndx == SHN_XINDEX")
{
  synthetic_elf builder;
  builder.add_section("dummy", SHT_PROGBITS, 0, {});
  auto bytes = builder.build();
  poke<Elf32_Half>(bytes, offsetof(Elf32_Ehdr, e_shstrndx), static_cast<Elf32_Half>(SHN_XINDEX));
  auto const path = synthetic_elf::write(bytes, "r10a-shstrndx-xindex.elf");
  expect_image_error(path, "header", 0, "invalid section header string table index");
}

TEST_CASE("load_elf rejects e_shstrndx >= e_shnum")
{
  synthetic_elf builder;
  builder.add_section("dummy", SHT_PROGBITS, 0, {});
  auto       bytes = builder.build();
  auto const shnum = peek_u16(bytes, offsetof(Elf32_Ehdr, e_shnum));
  poke<Elf32_Half>(bytes, offsetof(Elf32_Ehdr, e_shstrndx), shnum);
  auto const path = synthetic_elf::write(bytes, "r10b-shstrndx-oob.elf");
  expect_image_error(path, "header", 0, "invalid section header string table index");
}

namespace
{
auto macros_of(popcorn::testing::macro_section const& macros, std::uint32_t at)
    -> popcorn::image::macro_index
{
  popcorn::testing::synthetic_info const info{{{.name = "a.c", .macros = at}}};
  return popcorn::image::detail::read_macros(info.sections(), macros.bytes()).index;
}

struct chain
{
  popcorn::testing::macro_section macros;
  std::uint32_t                   top{};
};

auto chained(std::size_t levels, std::size_t imports_each) -> chain
{
  chain built;
  built.top = built.macros.unit();
  built.macros.define("LEAF 1");
  built.macros.end();
  for(std::size_t level = 0; level < levels; ++level)
  {
    auto const at = built.macros.unit();
    for(std::size_t i = 0; i < imports_each; ++i)
    {
      built.macros.import(built.top);
    }
    built.macros.end();
    built.top = at;
  }
  return built;
}
}

TEST_CASE("a hostile .debug_macro raises image_error")
{
  using popcorn::image_error;
  using popcorn::testing::macro_section;

  SUBCASE("truncated header")
  {
    macro_section macros;
    macros.raw(5);
    CHECK_THROWS_AS(macros_of(macros, 0), image_error);
  }
  SUBCASE("version 3")
  {
    macro_section macros;
    auto const    at = macros.unit(3);
    macros.end();
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
  SUBCASE("string past the end")
  {
    macro_section macros;
    auto const    at = macros.unit();
    macros.raw(0x01);
    macros.raw(1);
    macros.raw('X');
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
  SUBCASE("import past the end")
  {
    macro_section macros;
    auto const    at = macros.unit();
    macros.import(0x1000);
    macros.end();
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
  SUBCASE("strp past .debug_str")
  {
    macro_section macros;
    auto const    at = macros.unit();
    macros.strp(0x1000);
    macros.end();
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
  SUBCASE("import cycle")
  {
    macro_section macros;
    macros.unit();
    macros.end();
    auto const at = macros.unit();
    macros.import(at);
    macros.end();
    CHECK_THROWS_WITH_AS(macros_of(macros, at), doctest::Contains("cycle"), image_error);
  }
  SUBCASE("import cycle through two units below a root at offset 0")
  {
    macro_section macros;
    auto const    root = macros.unit();
    macros.import(0x10);
    macros.end();
    while(macros.bytes().size() < 0x10)
    {
      macros.end();
    }
    auto const first = macros.unit();
    REQUIRE(first == 0x10);
    macros.import(0x20);
    macros.end();
    while(macros.bytes().size() < 0x20)
    {
      macros.end();
    }
    auto const second = macros.unit();
    REQUIRE(second == 0x20);
    macros.import(first);
    macros.end();
    CHECK_THROWS_WITH_AS(macros_of(macros, root), doctest::Contains("cycle"), image_error);
  }
  SUBCASE("import cycle through offset 0 when no compile unit's macros start there")
  {
    macro_section macros;
    auto const    zero = macros.unit();
    REQUIRE(zero == 0);
    macros.import(0);
    macros.end();
    auto const root = macros.unit();
    macros.import(0);
    macros.end();
    CHECK_THROWS_WITH_AS(macros_of(macros, root), doctest::Contains("cycle"), image_error);
  }
  SUBCASE("unknown opcode without a table entry")
  {
    macro_section macros;
    auto const    at = macros.unit();
    macros.raw(0xe5);
    macros.end();
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
  SUBCASE("supplementary opcode")
  {
    macro_section macros;
    auto const    at = macros.unit();
    macros.raw(0x08);
    macros.raw(1);
    macros.raw(0);
    macros.raw(0);
    macros.raw(0);
    macros.raw(0);
    macros.end();
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
  SUBCASE("an operands table longer than the section")
  {
    popcorn::testing::dwarf_bytes raw;
    raw.u16(5);
    raw.u8(4);
    raw.u8(1);
    raw.u8(0xe5);
    raw.uleb(1000);
    raw.u8(0);
    macro_section macros;
    for(auto const byte : raw.bytes)
    {
      macros.raw(std::to_integer<std::uint8_t>(byte));
    }
    CHECK_THROWS_AS(macros_of(macros, 0), image_error);
  }
  SUBCASE("a bad form in the operands table")
  {
    macro_section macros;
    auto const    at = macros.unit_with_operands(0xe5, {0xff});
    macros.raw(0xe5);
    macros.end();
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
  SUBCASE("a define without a name")
  {
    macro_section macros;
    auto const    at = macros.unit();
    macros.define(" 1");
    macros.end();
    CHECK_THROWS_AS(macros_of(macros, at), image_error);
  }
}

TEST_CASE("units each under the operation cap raise image_error together, in under 1 s")
{
  auto const                                    shared = chained(22, 2);
  std::vector<popcorn::testing::synthetic_unit> units(9);
  for(auto& unit : units)
  {
    unit = {.name = "a.c", .macros = shared.top};
  }
  popcorn::testing::synthetic_info const info{units};
  CHECK(macros_of(shared.macros, shared.top).entries.size() == 1);
  auto const start = std::chrono::steady_clock::now();
  CHECK_THROWS_WITH_AS(
      std::ignore = popcorn::image::detail::read_macros(info.sections(), shared.macros.bytes()),
      doctest::Contains("for one file"), popcorn::image_error);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
}

TEST_CASE("64 nested macro imports run, 65 raise image_error")
{
  auto const fits = chained(64, 1);
  CHECK(macros_of(fits.macros, fits.top).entries.size() == 1);
  auto const deep = chained(65, 1);
  CHECK_THROWS_WITH_AS(
      macros_of(deep.macros, deep.top), doctest::Contains("nested"), popcorn::image_error);
}

TEST_CASE("a 40-level doubling import chain raises image_error in under 1 s")
{
  auto const doubling = chained(40, 2);
  auto const start    = std::chrono::steady_clock::now();
  CHECK_THROWS_WITH_AS(
      macros_of(doubling.macros, doubling.top), doctest::Contains("2^24"), popcorn::image_error);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
}
