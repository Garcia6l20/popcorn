#include "fixture_files.hpp"

#include "elf_section.hpp"

#include <doctest/doctest.h>

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
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
