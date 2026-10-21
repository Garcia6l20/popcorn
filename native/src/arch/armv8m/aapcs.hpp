#pragma once

#include <popcorn/arch/arch.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace popcorn::arch::armv8m::aapcs
{
/** @brief Where an argument lives. */
enum class place : std::uint8_t
{
  core,
  vfp,
  stack,
};

/** @brief Location of one argument. */
struct slot
{
  /** @brief Register class or the stack. */
  place where{};
  /** @brief First core register, first single-precision register, or byte offset on the stack. */
  std::uint32_t index{};
  /** @brief Words the argument takes. */
  std::uint32_t words{};

  /** @brief Member-wise equality. */
  friend auto operator==(slot const&, slot const&) -> bool = default;
};

/** @brief Locations of every argument of a call. */
struct layout
{
  /** @brief One slot per argument, in declaration order. */
  std::vector<slot> args{};
  /** @brief Bytes of stack arguments. */
  std::uint32_t stack_bytes{};
};

/** @brief Whether a value kind takes two words. */
[[nodiscard]] constexpr auto wide(value_kind kind) noexcept -> bool
{
  return kind == value_kind::i64 or kind == value_kind::f64;
}

/**
 * @brief Allocates the arguments of a call under AAPCS32, with the VFP variant when @p vfp:
 *        core registers r0-r3 with even pairs for 64-bit values, s0-s15 with back-filling and
 *        even d0-d7 pairs for floating-point values, then the stack, 8-aligned for 64-bit values.
 *        No argument is split between registers and the stack.
 * @param args Arguments in declaration order.
 * @param vfp Whether floating-point arguments go in VFP registers.
 */
[[nodiscard]] constexpr auto allocate(std::span<call_arg const> args, bool vfp) -> layout
{
  layout               out;
  std::uint32_t        ncrn = 0;
  std::uint32_t        nsaa = 0;
  std::array<bool, 16> single_used{};
  bool                 vfp_exhausted = false;
  auto const           on_stack      = [&](std::uint32_t words) {
    nsaa = words == 2 ? (nsaa + 7u) & ~7u : nsaa;
    out.args.push_back({.where = place::stack, .index = nsaa, .words = words});
    nsaa += 4 * words;
  };
  for(auto const& arg : args)
  {
    auto const words    = wide(arg.kind) ? 2u : 1u;
    auto const floating = arg.kind == value_kind::f32 or arg.kind == value_kind::f64;
    if(vfp and floating)
    {
      if(not vfp_exhausted)
      {
        auto found = false;
        for(std::uint32_t at = 0; at + words <= 16 and not found; at += words)
        {
          if(not single_used[at] and (words == 1 or not single_used[at + 1]))
          {
            single_used[at] = true;
            if(words == 2)
            {
              single_used[at + 1] = true;
            }
            out.args.push_back({.where = place::vfp, .index = at, .words = words});
            found = true;
          }
        }
        if(found)
        {
          continue;
        }
        vfp_exhausted = true;
        single_used.fill(true);
      }
      on_stack(words);
      continue;
    }
    if(words == 2)
    {
      ncrn = (ncrn + 1u) & ~1u;
    }
    if(ncrn + words <= 4)
    {
      out.args.push_back({.where = place::core, .index = ncrn, .words = words});
      ncrn += words;
      continue;
    }
    ncrn = 4;
    on_stack(words);
  }
  out.stack_bytes = nsaa;
  return out;
}
}
