#include "synthetic_elf.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/image/image.hpp>

#include <doctest/doctest.h>

#include <elf.h>

#include <cstddef>
#include <vector>

using popcorn::image::load_elf;
using popcorn::testing::synthetic_elf;

namespace
{
constexpr std::uint8_t attr_tag_file           = 1;
constexpr std::uint8_t attr_tag_fp_arch        = 10;
constexpr std::uint8_t attr_tag_abi_hardfp_use = 27;
constexpr std::uint8_t attr_tag_abi_vfp_args   = 28;

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

auto build_attrs(std::vector<std::byte> const& file_scope_content) -> std::vector<std::byte>
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

auto load_with_attrs(std::vector<std::byte> const& file_scope_content, std::string const& name)
    -> popcorn::arch::float_abi
{
  synthetic_elf builder;
  builder.header().e_flags = EF_ARM_ABI_FLOAT_HARD;
  builder.add_section(".ARM.attributes", SHT_PROGBITS, 0, build_attrs(file_scope_content));
  auto const path = synthetic_elf::write(builder.build(), name);
  return load_elf(path).abi;
}
}

TEST_CASE("A single-precision FPU with VFP argument passing gives hard_double")
{
  std::vector<std::byte> const file_scope{
      static_cast<std::byte>(attr_tag_abi_hardfp_use), std::byte{1},
      static_cast<std::byte>(attr_tag_abi_vfp_args), std::byte{1}};
  CHECK(load_with_attrs(file_scope, "abi-hardfp-1.elf") == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("Tag_ABI_HardFP_use == 1 alone gives hard_double")
{
  std::vector<std::byte> const file_scope{
      static_cast<std::byte>(attr_tag_abi_hardfp_use), std::byte{1}};
  CHECK(
      load_with_attrs(file_scope, "abi-hardfp-1-only.elf")
      == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("Tag_ABI_VFP_args == 0, the base AAPCS, gives soft")
{
  std::vector<std::byte> const file_scope{
      static_cast<std::byte>(attr_tag_abi_vfp_args), std::byte{0}};
  CHECK(load_with_attrs(file_scope, "abi-vfp-args-0.elf") == popcorn::arch::float_abi::soft);
}

TEST_CASE("Tag_ABI_HardFP_use absent with Tag_FP_arch == 4 gives hard_double")
{
  std::vector<std::byte> const file_scope{static_cast<std::byte>(attr_tag_fp_arch), std::byte{4}};
  CHECK(
      load_with_attrs(file_scope, "abi-absent-fparch4.elf")
      == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("Tag_ABI_HardFP_use == 0 with Tag_FP_arch == 8 gives hard_double")
{
  std::vector<std::byte> const file_scope{
      static_cast<std::byte>(attr_tag_abi_hardfp_use), std::byte{0},
      static_cast<std::byte>(attr_tag_fp_arch), std::byte{8}};
  CHECK(
      load_with_attrs(file_scope, "abi-hardfp0-fparch8.elf")
      == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("An unknown odd tag above 32 is skipped as a string, not misparsed as a ULEB")
{
  constexpr std::uint8_t unknown_odd_tag = 69;
  std::vector<std::byte> file_scope{static_cast<std::byte>(unknown_odd_tag)};
  append_cstr(file_scope, "unknown");
  file_scope.push_back(std::byte{0});
  file_scope.push_back(static_cast<std::byte>(attr_tag_abi_vfp_args));
  file_scope.push_back(std::byte{0});
  CHECK(load_with_attrs(file_scope, "abi-unknown-odd-tag.elf") == popcorn::arch::float_abi::soft);
}

TEST_CASE("Tag_ABI_VFP_args == 2, toolchain-specific, gives hard_double")
{
  std::vector<std::byte> const file_scope{
      static_cast<std::byte>(attr_tag_abi_vfp_args), std::byte{2}};
  CHECK(load_with_attrs(file_scope, "abi-vfp-args-2.elf") == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("Tag_ABI_VFP_args == 3, no floating-point arguments, gives hard_double")
{
  std::vector<std::byte> const file_scope{
      static_cast<std::byte>(attr_tag_abi_vfp_args), std::byte{3}};
  CHECK(load_with_attrs(file_scope, "abi-vfp-args-3.elf") == popcorn::arch::float_abi::hard_double);
}

TEST_CASE("A file scope with only other tags gives hard_double")
{
  std::vector<std::byte> const file_scope{
      static_cast<std::byte>(attr_tag_fp_arch), std::byte{8},
      static_cast<std::byte>(attr_tag_abi_hardfp_use), std::byte{3}};
  CHECK(load_with_attrs(file_scope, "abi-other-tags.elf") == popcorn::arch::float_abi::hard_double);
}
