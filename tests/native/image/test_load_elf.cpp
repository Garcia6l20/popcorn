#include "synthetic_elf.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <vector>

using popcorn::image::image;
using popcorn::image::load_elf;
using popcorn::image::symbol_kind;
using popcorn::testing::synthetic_elf;

namespace
{
auto image_elf_path() -> std::string
{
  char const* const path = std::getenv("POPCORN_IMAGE_ELF");
  REQUIRE(path != nullptr);
  return path;
}

auto image_bin_path() -> std::string
{
  char const* const path = std::getenv("POPCORN_IMAGE_BIN");
  REQUIRE(path != nullptr);
  return path;
}

auto read_whole_file(std::string const& path) -> std::vector<std::byte>
{
  std::ifstream          in{path, std::ios::binary};
  std::vector<char>      raw{std::istreambuf_iterator<char>{in}, {}};
  std::vector<std::byte> out(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}
}

TEST_CASE("load_elf reads the committed image firmware")
{
  auto const built = load_elf(image_elf_path());

  REQUIRE(built.segments.size() == 2);
  CHECK(built.segments[0].start == 0x0800'0000);
  CHECK(built.segments[0].memory_size == 0x48);
  CHECK(built.segments[0].executable);
  CHECK_FALSE(built.segments[0].writable);
  CHECK(built.segments[1].start == 0x0800'0048);
  CHECK(built.segments[1].memory_size == 4);
  CHECK(built.segments[1].writable);

  auto const bin = read_whole_file(image_bin_path());
  for(auto const& seg : built.segments)
  {
    auto const offset = seg.start - built.segments.front().start;
    REQUIRE(offset + seg.bytes.size() <= bin.size());
    CHECK(std::ranges::equal(seg.bytes, std::span{bin}.subspan(offset, seg.bytes.size())));
  }

  std::set<std::string> names;
  for(auto const& sym : built.symbols)
  {
    names.insert(sym.name);
  }
  CHECK(names.contains("Reset_Handler"));
  CHECK(names.contains("global_counter"));
  CHECK(names.contains("zero_buffer"));
  CHECK(names.contains("_estack"));
  CHECK(names.contains("shapes_perimeter"));
  CHECK(
      std::ranges::count(built.symbols, std::string_view{"scale"}, &popcorn::image::symbol::name)
      == 2);
  for(auto const& sym : built.symbols)
  {
    CHECK(sym.name.find('$') == std::string::npos);
    CHECK((sym.name.empty() || sym.name[0] != '?'));
  }

  auto const& reset = built.lookup("Reset_Handler");
  CHECK(reset.kind == symbol_kind::function);
  CHECK(reset.global);
  CHECK((reset.value & 1) == 0);

  auto const& counter = built.lookup("global_counter");
  CHECK(counter.kind == symbol_kind::object);
  CHECK(counter.global);

  CHECK_THROWS_AS(std::ignore = built.lookup("scale"), popcorn::symbol_error);

  REQUIRE(built.entry.has_value());
  CHECK((*built.entry & 1) != 0);
  CHECK(built.abi == popcorn::arch::float_abi::hard_double);
  CHECK(built.digest.size() == 64);
}

TEST_CASE("load_elf digest is stable across reloads of the same file")
{
  auto const first  = load_elf(image_elf_path());
  auto const second = load_elf(image_elf_path());
  CHECK(first.digest == second.digest);
  CHECK(first == second);
}

TEST_CASE("load_elf on a missing file throws image_error naming the file")
{
  CHECK_THROWS_AS(load_elf("does-not-exist.elf"), popcorn::image_error);
}

TEST_CASE("load_elf with no PT_LOAD segment gives an empty image, not an error")
{
  synthetic_elf builder;
  auto const    path  = synthetic_elf::write(builder.build(), "no-load.elf");
  auto const    built = load_elf(path);
  CHECK(built.segments.empty());
  CHECK(built.entry == std::nullopt);
}

TEST_CASE("load_elf drops PT_LOAD segments with zero file size")
{
  synthetic_elf builder;
  builder.add_segment(0x0800'0000, 0x0800'0000, {}, 0x100);
  auto const path = synthetic_elf::write(builder.build(), "zero-filesz.elf");
  CHECK(load_elf(path).segments.empty());
}

TEST_CASE("load_elf rejects a segment whose memory size is below its file size")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(8, std::byte{0x11});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  auto const path = synthetic_elf::write(builder.build(), "memsz-below-filesz.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects overlapping segments")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(0x10, std::byte{0x22});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 0x10);
  builder.add_segment(0x0800'0008, 0x0800'0008, data, 0x10);
  auto const path = synthetic_elf::write(builder.build(), "overlap.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf entry is empty when e_entry is 0")
{
  synthetic_elf builder;
  builder.header().e_entry = 0;
  std::vector<std::byte> const data(4, std::byte{0x00});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  auto const path = synthetic_elf::write(builder.build(), "entry-zero.elf");
  CHECK(load_elf(path).entry == std::nullopt);
}

TEST_CASE("load_elf without EF_ARM_ABI_FLOAT_HARD gives a soft float abi")
{
  synthetic_elf builder;
  builder.header().e_flags = 0;
  auto const path          = synthetic_elf::write(builder.build(), "soft-abi.elf");
  CHECK(load_elf(path).abi == popcorn::arch::float_abi::soft);
}

TEST_CASE("load_elf with the hard flag but no .ARM.attributes gives hard_double")
{
  synthetic_elf builder;
  builder.header().e_flags = EF_ARM_ABI_FLOAT_HARD;
  auto const path          = synthetic_elf::write(builder.build(), "hard-no-attrs.elf");
  CHECK(load_elf(path).abi == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("load_elf with no section table has no symbols, not an error")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(4, std::byte{0x00});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  auto const path = synthetic_elf::write(builder.build(), "no-shdrs.elf");
  CHECK(load_elf(path).symbols.empty());
}

TEST_CASE("load_elf rejects an empty file")
{
  auto const path = synthetic_elf::write({}, "empty.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects a file truncated at 10 bytes")
{
  synthetic_elf builder;
  auto          bytes = builder.build();
  bytes.resize(10);
  auto const path = synthetic_elf::write(bytes, "truncated-header.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects a file truncated at the program header table")
{
  synthetic_elf                builder;
  std::vector<std::byte> const data(4, std::byte{0x00});
  builder.add_segment(0x0800'0000, 0x0800'0000, data, 4);
  auto bytes = builder.build();
  bytes.resize(sizeof(Elf32_Ehdr) + 4);
  auto const path = synthetic_elf::write(bytes, "truncated-phdrs.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects a file truncated at the section header table")
{
  synthetic_elf builder;
  auto          bytes = builder.build();
  Elf32_Ehdr    eh;
  std::memcpy(&eh, bytes.data(), sizeof eh);
  eh.e_shoff     = static_cast<Elf32_Word>(bytes.size());
  eh.e_shnum     = 4;
  eh.e_shentsize = sizeof(Elf32_Shdr);
  std::memcpy(bytes.data(), &eh, sizeof eh);
  auto const path = synthetic_elf::write(bytes, "truncated-shdrs.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects a 64-bit ELF")
{
  synthetic_elf builder;
  builder.header().e_ident[EI_CLASS] = ELFCLASS64;
  auto const path                    = synthetic_elf::write(builder.build(), "elfclass64.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects a big-endian ELF")
{
  synthetic_elf builder;
  builder.header().e_ident[EI_DATA] = ELFDATA2MSB;
  auto const path                   = synthetic_elf::write(builder.build(), "elfdata2msb.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects a non-ARM machine")
{
  synthetic_elf builder;
  builder.header().e_machine = EM_X86_64;
  auto const path            = synthetic_elf::write(builder.build(), "em-x86-64.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}

TEST_CASE("load_elf rejects a relocatable ELF")
{
  synthetic_elf builder;
  builder.header().e_type = ET_REL;
  auto const path         = synthetic_elf::write(builder.build(), "et-rel.elf");
  CHECK_THROWS_AS(load_elf(path), popcorn::image_error);
}
