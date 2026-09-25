#pragma once

#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace popcorn::obs::thumb
{
/**
 * @brief Size of a Thumb instruction from its first halfword.
 * @param first First halfword, as fetched.
 * @return 4 for a 32-bit encoding, 2 otherwise.
 */
constexpr auto size(std::uint16_t first) noexcept -> std::uint8_t
{
  return (first >> 11) >= 0b11101 ? 4 : 2;
}

/** @brief One Thumb instruction. */
struct insn
{
  /** @brief Address of its first halfword. */
  address at{};
  /** @brief The halfword for a 16-bit instruction, first halfword << 16 | second otherwise. */
  std::uint32_t bits{};
  /** @brief Size in bytes, 2 or 4. */
  std::uint8_t size{};

  /** @brief Member-wise equality. */
  friend auto operator==(insn const&, insn const&) -> bool = default;
};

/**
 * @brief Reads the instruction at the start of little-endian code bytes.
 * @param at Guest address of the first byte.
 * @param bytes Code from @p at on.
 * @return The instruction, empty when @p bytes ends inside it.
 */
auto read(address at, std::span<std::byte const> bytes) noexcept -> std::optional<insn>;

/** @brief How an instruction ends the translated block holding it. */
enum class[[= reflex::derive{reflex::Format}]] ending
{
  none,
  direct,
  indirect,
  exception,
  system,
};

/** @brief Control flow of one instruction, as far as translated block boundaries go. */
struct flow
{
  /** @brief Whether and how the instruction ends its translated block. */
  ending kind{ending::none};
  /** @brief Target of a direct branch. */
  std::optional<address> target{};
  /** @brief Whether the branch is conditional by its encoding: B T1, B T3, CBZ, CBNZ. */
  bool conditional{};

  /** @brief Member-wise equality. */
  friend auto operator==(flow const&, flow const&) -> bool = default;
};

/** @brief How a direct branch encodes its target. */
enum class target_form
{
  none,
  b_t1,
  b_t2,
  cbz,
  b_t3,
  b_t4,
};

/**
 * @brief One row of the block-end table: an instruction is the first row whose masked bits equal
 *        its value. Rows of kind none shadow later rows, such as B T3 with a condition of 111x,
 *        which encodes hints, MSR, MRS and SMC.
 */
struct block_end_row
{
  /** @brief Instructions the row matches, for diagnostics and tests. */
  std::string_view name{};
  /** @brief Instruction size in bytes, 2 or 4. */
  std::uint8_t size{};
  /** @brief Bits compared, in the layout of insn::bits. */
  std::uint32_t mask{};
  /** @brief Their value. */
  std::uint32_t value{};
  /** @brief How a matching instruction ends its block. */
  ending kind{ending::none};
  /** @brief How its target is encoded, none for no direct target. */
  target_form form{target_form::none};
  /** @brief Whether the encoding carries a condition. */
  bool conditional{};
};

/** @brief The block-end table classify() searches, in match order. */
auto block_end_rows() noexcept -> std::span<block_end_row const>;

/**
 * @brief Classifies an instruction by whether the engine ends a translated block after it on an
 *        M-profile core. Exceptions that depend on CPU state, such as NOCP with the FPU disabled,
 *        also end a block and are not predicted. BLX immediate is not valid on M-profile: it
 *        raises an exception, so it ends the block with no predicted target.
 * @param i Instruction.
 * @return Its ending kind, the target of a direct branch, and whether it is conditional.
 */
auto classify(insn const& i) noexcept -> flow;

/**
 * @brief Whether an instruction is an IT instruction.
 * @param i Instruction.
 */
auto is_it(insn const& i) noexcept -> bool;

/** @brief A conditional branch by encoding: B T1, B T3, CBZ or CBNZ. */
struct cond_branch
{
  /** @brief Address of the branch. */
  address at{};
  /** @brief Its size in bytes. */
  std::uint8_t size{};
  /** @brief Target when taken. */
  address taken{};
  /** @brief Next instruction, reached when not taken. */
  address fallthrough{};

  /** @brief Member-wise equality. */
  friend auto operator==(cond_branch const&, cond_branch const&) -> bool = default;
};

/**
 * @brief The conditional branch an instruction is by its encoding. IT-predicated instructions
 *        are not, the caller tracks IT state.
 * @param i Instruction.
 * @return The branch, empty for any other instruction.
 */
auto conditional_branch(insn const& i) noexcept -> std::optional<cond_branch>;

/**
 * @brief Whether an instruction is a call, BL or BLX register, after which execution resumes at
 *        the next instruction.
 * @param i Instruction.
 */
auto is_call(insn const& i) noexcept -> bool;

/**
 * @brief Instructions an IT instruction makes conditional, from its mask.
 * @param i An IT instruction, see is_it().
 * @return 1 to 4.
 */
auto it_length(insn const& i) noexcept -> std::uint8_t;
}
