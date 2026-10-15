#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/coverage.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/coverage.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace popcorn::obs
{
/** @brief Byte range of one decoded translated block. */
struct tb_extent
{
  /** @brief Start address. */
  address start{};
  /** @brief Size in bytes. */
  std::uint32_t size{};
  /** @brief Instructions. */
  std::uint32_t insns{};
  /** @brief The last instruction. */
  arch::decoded_insn last{};

  /** @brief Member-wise equality. */
  friend auto operator==(tb_extent const&, tb_extent const&) -> bool = default;
};

/**
 * @brief Reads guest bytes for code outside every image segment.
 * @return How many bytes from the start of the span were read.
 */
using memory_reader = std::function<std::size_t(address at, std::span<std::byte> out)>;

/** @brief Code bytes a translated block is decoded from, one page at a time. */
class code_bytes
{
public:
  /**
   * @brief Creates the source.
   * @param images Loaded images, in load order. The last one holding an address wins.
   * @param fallback Reader for addresses no image segment holds.
   * @param analysis Decoder of the code, null when the arch has none: every block then decodes
   *        to size 0.
   */
  code_bytes(
      std::span<image::image const> images,
      memory_reader                 fallback,
      arch::code_analysis const*    analysis);

  /** @brief The decoder, null for none. */
  [[nodiscard]] auto analysis() const noexcept -> arch::code_analysis const*;

  /**
   * @brief Bytes from an address on, as many as are readable up to @p count, never past the end
   *        of the address space.
   * @param start First address.
   * @param count Bytes wanted.
   * @return The bytes, shorter where the source ends. Valid until the next call.
   */
  auto read(address start, std::uint32_t count) -> std::span<std::byte const>;

  /**
   * @brief Bytes from an address to the end of its page.
   * @param start First address.
   * @return The bytes, shorter where the source ends. Valid until the next call.
   */
  auto page(address start) -> std::span<std::byte const> pre(analysis() != nullptr);

private:
  std::span<image::image const> images_;
  memory_reader                 fallback_;
  arch::code_analysis const*    analysis_;
  std::vector<std::byte>        scratch_;
};

/**
 * @brief Decodes the translated block the engine builds at an address.
 * @param start Block start.
 * @param code Code source.
 * @return The block, size 0 when its first instruction cannot be read or @p code has no
 *         decoder.
 */
auto decode_tb(address start, code_bytes& code) -> tb_extent;

/** @brief Blocks with byte sizes, from raw counters. */
struct sized_blocks
{
  /** @brief Blocks, sorted by start then size. */
  std::vector<block_hit> blocks{};
  /** @brief Starts whose hits were split over several sizes. */
  std::uint64_t split{};
  /** @brief Hits of starts whose first instruction could not be read, left out of blocks. */
  std::uint64_t dropped{};
};

/**
 * @brief Turns raw counters into sized blocks: each start decoded, its hits split where edges
 *        prove the engine ended the block early or cuts show it was left early. A start counted
 *        in several overlapping ranges keeps its largest count. Each edge into the block's middle
 *        and each cut there gives a block of that size with their hits, a cut at the start
 *        removes its hits, since none of the block's instructions ran, and the full size keeps
 *        what is left of the count. The hits over all sizes of a start therefore sum to the
 *        larger of its count less its cuts at the start and its early edges and cuts: once a u32
 *        counter saturates, the edges and cuts, which are u64, win.
 * @param data Raw counters.
 * @param code Code source.
 */
auto size_blocks(core::coverage_data const& data, code_bytes& code) -> sized_blocks;
}
