#include "fixture_files.hpp"

#include "elf_section.hpp"

#include <doctest/doctest.h>

#include <elf.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using bytes = std::vector<char>;

auto good_bytes() -> bytes
{
  std::ifstream in{plugin_test::fixture("good"), std::ios::binary};
  REQUIRE(in);
  return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

auto header_of(bytes const& file) -> Elf64_Ehdr
{
  Elf64_Ehdr header{};
  std::memcpy(&header, file.data(), sizeof header);
  return header;
}

auto section_at(bytes const& file, std::size_t index) -> Elf64_Shdr
{
  Elf64_Shdr section{};
  std::memcpy(
      &section, file.data() + header_of(file).e_shoff + index * sizeof(Elf64_Shdr), sizeof section);
  return section;
}

template <typename T> void put(bytes& file, std::uint64_t offset, T const& value)
{
  std::memcpy(file.data() + offset, &value, sizeof value);
}

auto shdr_field(bytes const& file, std::size_t index, std::size_t field) -> std::uint64_t
{
  return header_of(file).e_shoff + index * sizeof(Elf64_Shdr) + field;
}

auto scan(plugin_test::copies& files, bytes const& file) -> std::optional<std::string>
{
  auto const path = files.dir() / "scanned.so";
  {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
  }
  return popcorn::plugin::read_abi_section(path);
}

auto index_of(bytes const& file, std::uint32_t type) -> std::size_t
{
  auto const header = header_of(file);
  for(std::size_t index = 0; index < header.e_shnum; ++index)
  {
    if(section_at(file, index).sh_type == type)
    {
      return index;
    }
  }
  FAIL("no section of type " << type);
  return 0;
}

void point_at(bytes& file, std::size_t index, bytes const& contents)
{
  while(file.size() % 8 != 0)
  {
    file.push_back('\0');
  }
  put(file, shdr_field(file, index, offsetof(Elf64_Shdr, sh_offset)), Elf64_Off{file.size()});
  put(file, shdr_field(file, index, offsetof(Elf64_Shdr, sh_size)), Elf64_Xword{contents.size()});
  file.insert(file.end(), contents.begin(), contents.end());
}

auto as_bytes_of(auto const& entries) -> bytes
{
  bytes out(entries.size() * sizeof(entries[0]));
  std::memcpy(out.data(), entries.data(), out.size());
  return out;
}

struct long_names
{
  bytes         file;
  std::uint32_t at;
  std::uint32_t registry;
};

auto with_long_name(std::uint64_t run) -> long_names
{
  auto             file    = good_bytes();
  auto const       strtab  = section_at(file, section_at(file, index_of(file, SHT_DYNSYM)).sh_link);
  auto const       strings = file.begin() + static_cast<long>(strtab.sh_offset);
  bytes            table(strings, strings + static_cast<long>(strtab.sh_size));
  auto const       registry = static_cast<std::uint32_t>(table.size());
  std::string_view mangled{"_ZN7popcorn6periph14register_modelESt17basic_string_view"};
  table.insert(table.end(), mangled.begin(), mangled.end());
  table.push_back('\0');
  auto const at = static_cast<std::uint32_t>(table.size());
  table.insert(table.end(), run, 'x');
  table.push_back('\0');
  point_at(file, section_at(file, index_of(file, SHT_DYNSYM)).sh_link, table);
  return {std::move(file), at, registry};
}

auto links_of(plugin_test::copies& files, bytes const& file)
    -> std::optional<popcorn::plugin::dynamic_links>
{
  auto const path = files.dir() / "linked.so";
  {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
  }
  return popcorn::plugin::read_dynamic_links(path);
}
}

TEST_CASE("the scan reads the tag of a well-formed plugin")
{
  plugin_test::copies files;
  auto const          tag = scan(files, good_bytes());
  REQUIRE(tag);
  CHECK(tag->starts_with("popcorn "));
}

TEST_CASE("a truncated plugin carries no tag and the scan returns cleanly")
{
  plugin_test::copies files;
  auto const          file  = good_bytes();
  auto const          shoff = header_of(file).e_shoff;
  for(auto const size :
      {std::size_t{0}, std::size_t{63}, std::size_t{64}, std::size_t{shoff},
       std::size_t{shoff + 10}, file.size() - 1})
  {
    CAPTURE(size);
    CHECK_FALSE(scan(files, bytes(file.begin(), file.begin() + static_cast<long>(size))));
  }
}

TEST_CASE("corrupted section header offsets, counts and sizes carry no tag")
{
  plugin_test::copies files;
  auto const          good    = good_bytes();
  auto const          header  = header_of(good);
  auto const          strndx  = header.e_shstrndx;
  auto const          corrupt = [&](auto edit) {
    auto file = good;
    edit(file);
    return scan(files, file);
  };
  CHECK_FALSE(
      corrupt([&](bytes& f) { put(f, offsetof(Elf64_Ehdr, e_shoff), Elf64_Off{f.size()}); }));
  CHECK_FALSE(
      corrupt([](bytes& f) { put(f, offsetof(Elf64_Ehdr, e_shoff), Elf64_Off{UINT64_MAX - 10}); }));
  CHECK_FALSE(corrupt([](bytes& f) { put(f, offsetof(Elf64_Ehdr, e_shnum), Elf64_Half{0xffff}); }));
  CHECK_FALSE(corrupt([](bytes& f) { put(f, offsetof(Elf64_Ehdr, e_shnum), Elf64_Half{0}); }));
  CHECK_FALSE(corrupt(
      [&](bytes& f) { put(f, offsetof(Elf64_Ehdr, e_shstrndx), Elf64_Half{header.e_shnum}); }));
  CHECK_FALSE(corrupt([](bytes& f) {
    put(f, offsetof(Elf64_Ehdr, e_shentsize), Elf64_Half{sizeof(Elf64_Shdr) / 2});
  }));
  CHECK_FALSE(corrupt([&](bytes& f) {
    put(f, shdr_field(f, strndx, offsetof(Elf64_Shdr, sh_size)), Elf64_Xword{2u << 20});
  }));
  CHECK_FALSE(corrupt([&](bytes& f) {
    put(f, shdr_field(f, strndx, offsetof(Elf64_Shdr, sh_size)), Elf64_Xword{UINT64_MAX});
  }));
  CHECK_FALSE(corrupt([&](bytes& f) {
    put(f, shdr_field(f, strndx, offsetof(Elf64_Shdr, sh_offset)), Elf64_Off{UINT64_MAX - 4});
  }));
  for(std::size_t index = 0; index < header.e_shnum; ++index)
  {
    if(index == strndx)
    {
      continue;
    }
    auto const name_offset = section_at(good, index).sh_name;
    auto const names       = section_at(good, strndx);
    if(std::string_view{good.data() + names.sh_offset + name_offset} != ".popcorn_abi")
    {
      continue;
    }
    CHECK_FALSE(corrupt([&](bytes& f) {
      put(f, shdr_field(f, index, offsetof(Elf64_Shdr, sh_offset)), Elf64_Off{f.size()});
    }));
    CHECK_FALSE(corrupt([&](bytes& f) {
      put(f, shdr_field(f, index, offsetof(Elf64_Shdr, sh_name)), Elf64_Word{UINT32_MAX});
    }));
    CHECK_FALSE(corrupt([&](bytes& f) {
      put(f, shdr_field(f, index, offsetof(Elf64_Shdr, sh_type)), Elf64_Word{SHT_NOBITS});
    }));
  }
}

TEST_CASE(
    "the scan reads no program header, ld.so's handling of a corrupted one is out of its "
    "contract")
{
  plugin_test::copies files;
  auto                file = good_bytes();
  put(file, offsetof(Elf64_Ehdr, e_phoff), Elf64_Off{UINT64_MAX - 4});
  put(file, offsetof(Elf64_Ehdr, e_phnum), Elf64_Half{0xffff});
  CHECK(scan(files, file));
}

TEST_CASE("random flips in the ELF header and section table end the scan cleanly")
{
  plugin_test::copies files;
  auto const          good   = good_bytes();
  auto const          header = header_of(good);
  std::mt19937_64     random{20261015};
  auto const          table = header.e_shoff;
  auto const          span  = std::uint64_t{header.e_shnum} * sizeof(Elf64_Shdr);
  for(int round = 0; round < 500; ++round)
  {
    auto file  = good;
    auto flips = 1 + random() % 6;
    for(std::uint64_t flip = 0; flip < flips; ++flip)
    {
      auto const in_header = random() % 2 == 0;
      auto const at        = in_header ? random() % sizeof(Elf64_Ehdr) : table + random() % span;
      file[at]             = static_cast<char>(file[at] ^ static_cast<char>(1u << (random() % 8)));
    }
    static_cast<void>(scan(files, file));
  }
  CHECK(scan(files, good));
}

TEST_CASE("the links of a plugin name libpopcorn_core.so and no registry definition")
{
  for(auto const* const name : {"good", "good-stripped"})
  {
    CAPTURE(name);
    auto const links = popcorn::plugin::read_dynamic_links(plugin_test::fixture(name));
    REQUIRE(links);
    CHECK(std::ranges::contains(links->needed, std::string_view{"libpopcorn_core.so"}));
    CHECK(links->registry_definitions.empty());
  }
}

TEST_CASE("the links of popcorn core list both registry functions it defines")
{
  auto const links = popcorn::plugin::read_dynamic_links(
      std::filesystem::path{std::getenv("POPCORN_PLUGIN_CORE")});
  REQUIRE(links);
  CHECK_FALSE(std::ranges::contains(links->needed, std::string_view{"libpopcorn_core.so"}));
  CHECK(
      std::ranges::contains(
          links->registry_definitions, std::string_view{"popcorn::periph::registered_glob"}));
  CHECK(
      std::ranges::contains(
          links->registry_definitions, std::string_view{"popcorn::periph::register_model"}));
}

TEST_CASE("a truncated or flipped plugin ends the links scan cleanly")
{
  plugin_test::copies files;
  auto const          good  = good_bytes();
  auto const          shoff = header_of(good).e_shoff;
  for(auto const size : {std::size_t{0}, std::size_t{64}, std::size_t{shoff}, good.size() - 1})
  {
    CAPTURE(size);
    CHECK_FALSE(links_of(files, bytes(good.begin(), good.begin() + static_cast<long>(size))));
  }
  auto const      header = header_of(good);
  std::mt19937_64 random{20261016};
  auto const      span = std::uint64_t{header.e_shnum} * sizeof(Elf64_Shdr);
  for(int round = 0; round < 500; ++round)
  {
    auto file  = good;
    auto flips = 1 + random() % 6;
    for(std::uint64_t flip = 0; flip < flips; ++flip)
    {
      auto const at = shoff + random() % span;
      file[at]      = static_cast<char>(file[at] ^ static_cast<char>(1u << (random() % 8)));
    }
    static_cast<void>(links_of(files, file));
  }
  CHECK(links_of(files, good));
}

TEST_CASE("many symbols naming one long string are scanned once each, not to its end")
{
  plugin_test::copies files;
  auto [file, at, registry] = with_long_name(std::uint64_t{12} << 20);
  std::vector<Elf64_Sym> symbols(
      600'000,
      Elf64_Sym{
          .st_name = at, .st_info = 0, .st_other = 0, .st_shndx = 1, .st_value = 0, .st_size = 0});
  symbols.back().st_name = registry;
  point_at(file, index_of(file, SHT_DYNSYM), as_bytes_of(symbols));
  auto const start = std::chrono::steady_clock::now();
  auto const links = links_of(files, file);
  auto const spent = std::chrono::steady_clock::now() - start;
  REQUIRE(links);
  CHECK(
      links->registry_definitions
      == std::vector<std::string>{std::string{"popcorn::periph::register_model"}});
  CHECK(spent < std::chrono::seconds{5});
}

TEST_CASE("DT_NEEDED names longer together than their string table are refused")
{
  plugin_test::copies files;
  auto [file, at, registry] = with_long_name(std::uint64_t{1} << 20);
  std::vector<Elf64_Dyn> dynamic(1000, Elf64_Dyn{.d_tag = DT_NEEDED, .d_un = {.d_val = at}});
  dynamic.push_back(Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}});
  point_at(file, index_of(file, SHT_DYNAMIC), as_bytes_of(dynamic));
  CHECK_FALSE(links_of(files, file));
  dynamic.resize(1);
  dynamic.push_back(Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}});
  point_at(file, index_of(file, SHT_DYNAMIC), as_bytes_of(dynamic));
  auto const links = links_of(files, file);
  REQUIRE(links);
  REQUIRE(links->needed.size() == 1);
  CHECK(links->needed[0].size() == std::size_t{1} << 20);
}
