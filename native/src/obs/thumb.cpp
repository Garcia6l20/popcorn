#include "thumb.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace popcorn::obs::thumb
{
namespace
{
constexpr std::array rows{
    block_end_row{
                  .name  = "udf t1",
                  .size  = 2,
                  .mask  = 0xff00,
                  .value = 0xde00,
                  .kind  = ending::exception},
    block_end_row{
                  .name  = "svc",
                  .size  = 2,
                  .mask  = 0xff00,
                  .value = 0xdf00,
                  .kind  = ending::exception},
    block_end_row{
                  .name        = "b t1",
                  .size        = 2,
                  .mask        = 0xf000,
                  .value       = 0xd000,
                  .kind        = ending::direct,
                  .form        = target_form::b_t1,
                  .conditional = true},
    block_end_row{
                  .name  = "b t2",
                  .size  = 2,
                  .mask  = 0xf800,
                  .value = 0xe000,
                  .kind  = ending::direct,
                  .form  = target_form::b_t2},
    block_end_row{
                  .name        = "cbz cbnz",
                  .size        = 2,
                  .mask        = 0xf500,
                  .value       = 0xb100,
                  .kind        = ending::direct,
                  .form        = target_form::cbz,
                  .conditional = true},
    block_end_row{
                  .name  = "bx blx bxns blxns",
                  .size  = 2,
                  .mask  = 0xff00,
                  .value = 0x4700,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "add pc",
                  .size  = 2,
                  .mask  = 0xff87,
                  .value = 0x4487,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "mov pc",
                  .size  = 2,
                  .mask  = 0xff87,
                  .value = 0x4687,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "pop pc",
                  .size  = 2,
                  .mask  = 0xff00,
                  .value = 0xbd00,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "bkpt",
                  .size  = 2,
                  .mask  = 0xff00,
                  .value = 0xbe00,
                  .kind  = ending::exception},
    block_end_row{
                  .name  = "yield",
                  .size  = 2,
                  .mask  = 0xffff,
                  .value = 0xbf10,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "wfe",
                  .size  = 2,
                  .mask  = 0xffff,
                  .value = 0xbf20,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "wfi",
                  .size  = 2,
                  .mask  = 0xffff,
                  .value = 0xbf30,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "cps",
                  .size  = 2,
                  .mask  = 0xffe8,
                  .value = 0xb660,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "msr",
                  .size  = 4,
                  .mask  = 0xfff0'd000,
                  .value = 0xf380'8000,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "yield.w",
                  .size  = 4,
                  .mask  = 0xffff'ffff,
                  .value = 0xf3af'8001,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "wfe.w",
                  .size  = 4,
                  .mask  = 0xffff'ffff,
                  .value = 0xf3af'8002,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "wfi.w",
                  .size  = 4,
                  .mask  = 0xffff'ffff,
                  .value = 0xf3af'8003,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "isb",
                  .size  = 4,
                  .mask  = 0xffff'fff0,
                  .value = 0xf3bf'8f60,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "udf t2",
                  .size  = 4,
                  .mask  = 0xfff0'f000,
                  .value = 0xf7f0'a000,
                  .kind  = ending::exception},
    block_end_row{
                  .name  = "b t3 cond 111x, hints msr mrs smc",
                  .size  = 4,
                  .mask  = 0xfb80'd000,
                  .value = 0xf380'8000,
                  .kind  = ending::none},
    block_end_row{
                  .name        = "b t3",
                  .size        = 4,
                  .mask        = 0xf800'd000,
                  .value       = 0xf000'8000,
                  .kind        = ending::direct,
                  .form        = target_form::b_t3,
                  .conditional = true},
    block_end_row{
                  .name  = "b t4",
                  .size  = 4,
                  .mask  = 0xf800'd000,
                  .value = 0xf000'9000,
                  .kind  = ending::direct,
                  .form  = target_form::b_t4},
    block_end_row{
                  .name  = "bl",
                  .size  = 4,
                  .mask  = 0xf800'd000,
                  .value = 0xf000'd000,
                  .kind  = ending::direct,
                  .form  = target_form::b_t4},
    block_end_row{
                  .name  = "blx t2",
                  .size  = 4,
                  .mask  = 0xf800'd000,
                  .value = 0xf000'c000,
                  .kind  = ending::exception},
    block_end_row{
                  .name  = "ldm pc",
                  .size  = 4,
                  .mask  = 0xffd0'8000,
                  .value = 0xe890'8000,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "ldmdb pc",
                  .size  = 4,
                  .mask  = 0xffd0'8000,
                  .value = 0xe910'8000,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "ldr pc",
                  .size  = 4,
                  .mask  = 0xff70'f000,
                  .value = 0xf850'f000,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "tbb tbh",
                  .size  = 4,
                  .mask  = 0xfff0'ffe0,
                  .value = 0xe8d0'f000,
                  .kind  = ending::indirect},
    block_end_row{
                  .name  = "vmsr fpscr",
                  .size  = 4,
                  .mask  = 0xffff'0fff,
                  .value = 0xeee1'0a10,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "vmsr fpexc",
                  .size  = 4,
                  .mask  = 0xffff'0fff,
                  .value = 0xeee8'0a10,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "vlstm",
                  .size  = 4,
                  .mask  = 0xfff0'ffff,
                  .value = 0xec20'0a00,
                  .kind  = ending::system},
    block_end_row{
                  .name  = "vlldm",
                  .size  = 4,
                  .mask  = 0xfff0'ffff,
                  .value = 0xec30'0a00,
                  .kind  = ending::system},
};

constexpr auto sign_extend(std::uint32_t value, unsigned bits) noexcept -> std::uint32_t
{
  auto const shift = 32 - bits;
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(value << shift) >> shift);
}

constexpr auto offset(target_form shape, std::uint32_t bits) noexcept -> std::uint32_t
{
  auto const first  = bits >> 16;
  auto const second = bits & 0xffff;
  switch(shape)
  {
    case target_form::none:
      return 0;
    case target_form::b_t1:
      return sign_extend((bits & 0xff) << 1, 9);
    case target_form::b_t2:
      return sign_extend((bits & 0x7ff) << 1, 12);
    case target_form::cbz:
      return ((bits >> 9) & 1) << 6 | ((bits >> 3) & 0x1f) << 1;
    case target_form::b_t3:
    {
      auto const s  = (first >> 10) & 1;
      auto const j1 = (second >> 13) & 1;
      auto const j2 = (second >> 11) & 1;
      return sign_extend(
          s << 20 | j2 << 19 | j1 << 18 | (first & 0x3f) << 12 | (second & 0x7ff) << 1, 21);
    }
    case target_form::b_t4:
    {
      auto const s  = (first >> 10) & 1;
      auto const i1 = ~(((second >> 13) & 1) ^ s) & 1;
      auto const i2 = ~(((second >> 11) & 1) ^ s) & 1;
      return sign_extend(
          s << 24 | i1 << 23 | i2 << 22 | (first & 0x3ff) << 12 | (second & 0x7ff) << 1, 25);
    }
  }
  std::unreachable();
}

auto halfword(std::span<std::byte const> bytes, std::size_t at) noexcept -> std::uint16_t
{
  return static_cast<std::uint16_t>(
      std::to_integer<unsigned>(bytes[at]) | std::to_integer<unsigned>(bytes[at + 1]) << 8);
}
}

auto block_end_rows() noexcept -> std::span<block_end_row const>
{
  return rows;
}

auto read(address at, std::span<std::byte const> bytes) noexcept -> std::optional<insn>
{
  if(bytes.size() < 2)
  {
    return std::nullopt;
  }
  auto const first = halfword(bytes, 0);
  if(size(first) == 2)
  {
    return insn{.at = at, .bits = first, .size = 2};
  }
  if(bytes.size() < 4)
  {
    return std::nullopt;
  }
  return insn{.at = at, .bits = std::uint32_t{first} << 16 | halfword(bytes, 2), .size = 4};
}

auto classify(insn const& i) noexcept -> flow
{
  auto const found = std::ranges::find_if(rows, [&](block_end_row const& candidate) {
    return candidate.size == i.size and (i.bits & candidate.mask) == candidate.value;
  });
  if(found == rows.end() or found->kind == ending::none)
  {
    return {};
  }
  flow result{.kind = found->kind, .conditional = found->conditional};
  if(found->form != target_form::none)
  {
    result.target = i.at + 4 + offset(found->form, i.bits);
  }
  return result;
}

auto is_it(insn const& i) noexcept -> bool
{
  return i.size == 2 and (i.bits & 0xff00) == 0xbf00 and (i.bits & 0x000f) != 0;
}

auto conditional_branch(insn const& i) noexcept -> std::optional<cond_branch>
{
  auto const control = classify(i);
  if(control.kind != ending::direct or not control.conditional)
  {
    return std::nullopt;
  }
  return cond_branch{
      .at          = i.at,
      .size        = i.size,
      .taken       = *control.target,
      .fallthrough = i.at + i.size,
  };
}

auto is_call(insn const& i) noexcept -> bool
{
  auto const bl  = i.size == 4 and (i.bits & 0xf800'd000) == 0xf000'd000;
  auto const blx = i.size == 2 and (i.bits & 0xff80) == 0x4780;
  return bl or blx;
}

auto it_length(insn const& i) noexcept -> std::uint8_t
{
  return static_cast<std::uint8_t>(4 - std::countr_zero(i.bits & 0xf));
}
}
