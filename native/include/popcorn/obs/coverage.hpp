#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/image/image.hpp>

#include <reflex/enum.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace popcorn::obs
{
/** @brief What a loaded image was loaded from. */
enum class[[= reflex::derive{reflex::Format}]] image_kind : std::uint32_t
{
  elf = 0,
  raw = 1,
};

/** @brief Image a coverage group was collected against. */
struct image_ref
{
  /** @brief What the image was loaded from. */
  image_kind kind{};
  /** @brief Absolute path of the ELF, empty for a raw image. */
  std::string path{};
  /** @brief SHA-256 of the loadable content, image::image::digest. */
  std::string digest{};
  /** @brief Lowest start of the image's executable segments, 0 without any. */
  address base{};
  /** @brief Highest end of the image's executable segments, 0 without any. */
  address end{};

  /** @brief Member-wise equality. */
  friend auto operator==(image_ref const&, image_ref const&) -> bool = default;
};

/** @brief Executions of one translated block, with its byte size fixed at collect time. */
struct block_hit
{
  /** @brief Start address. */
  address start{};
  /** @brief Size in bytes. */
  std::uint32_t size{};
  /** @brief Times it ran. */
  std::uint64_t hits{};

  /** @brief Member-wise equality. */
  friend auto operator==(block_hit const&, block_hit const&) -> bool = default;
};

/** @brief Executions of one edge between block starts. */
struct edge_hit
{
  /** @brief Start of the block the edge leaves. */
  address from{};
  /** @brief Start of the block the edge enters. */
  address to{};
  /** @brief Times it was taken. */
  std::uint64_t hits{};

  /** @brief Member-wise equality. */
  friend auto operator==(edge_hit const&, edge_hit const&) -> bool = default;
};

/** @brief Coverage collected against one list of images, the unit merges sum. */
struct coverage_group
{
  /** @brief Images, in load order. */
  std::vector<image_ref> images{};
  /** @brief Code ranges coverage was enabled over, sorted by start, size and name. */
  std::vector<address_range> ranges{};
  /** @brief Blocks with hits, sorted by start then size. */
  std::vector<block_hit> blocks{};
  /** @brief Edges with hits, sorted by from then to. */
  std::vector<edge_hit> edges{};
  /** @brief Block starts whose hits were split over several sizes. */
  std::uint64_t split{};
  /** @brief Hits of block starts whose first instruction could not be read, in no block. */
  std::uint64_t dropped{};

  /** @brief Member-wise equality. */
  friend auto operator==(coverage_group const&, coverage_group const&) -> bool = default;
};

/** @brief Coverage of one or more runs, possibly against different images. */
struct coverage_set
{
  /** @brief Groups, one per list of image digests. */
  std::vector<coverage_group> groups{};

  /** @brief Member-wise equality. */
  friend auto operator==(coverage_set const&, coverage_set const&) -> bool = default;
};

/**
 * @brief Code ranges of functions by name, for scoping coverage and trace sinks.
 * @param images Images to search, in load order. The first image holding a name wins.
 * @param names Function names.
 * @return One range per name, in the order of @p names: the symbol's value and size, named after
 *         it. A symbol of size 0 reaches the next function symbol inside the executable segment
 *         holding it, or that segment's end. Held by no executable segment, it reaches the next
 *         function symbol of its image.
 * @throws symbol_error when no image has a name, as image::image::lookup() reports it for the
 *         last image searched, when the first image having it holds it ambiguously, as lookup()
 *         reports it, or for a size-0 symbol held by no executable segment and followed by no
 *         function symbol.
 */
auto function_ranges(std::span<image::image const> images, std::span<std::string const> names)
    -> std::vector<address_range>;

/** @brief What lcov() writes beyond line counts. */
struct lcov_options
{
  /** @brief Whether to write BRDA, BRF and BRH records. */
  bool branches = true;
  /** @brief Test name of the TN record, empty allowed. */
  std::string test_name{};

  /** @brief Member-wise equality. */
  friend auto operator==(lcov_options const&, lcov_options const&) -> bool = default;
};

/**
 * @brief lcov tracefile text of a coverage set, the records TN, SF, FN, FNDA, FNF, FNH, BRDA, BRF,
 *        BRH, DA, LF, LH, end_of_record, files sorted by path.
 *
 * Inside a group an instruction's count is the sum of the hits of the blocks holding it, and a
 * line's count is the largest count among its instructions, over the group's ranges, row starts
 * and block starts. Across groups, which are different images, the counts of one file and line
 * are summed: the only place a line count is a sum. Branches are Thumb B<cond>, CBZ and CBNZ
 * outside IT blocks, found by following control flow through each function in range, and counted
 * from the edges out of the blocks that end with them. A branch whose block never ran is `-`.
 * Edges the machine did not record, at an exception entry or a stop right after a branch, are
 * missing from the counts. BRDA block numbers are the branch's index among its line's branches.
 * Each file record starts with its own TN record.
 *
 * Rules and limits:
 * - Rows of zero length, followed by a row at the same address, cover no instruction: their line
 *   gets no DA record from them, as addr2line resolves the address to the last row.
 * - Branch discovery starts from each function's entry, row starts and block starts. A start other
 *   than the entry, with an IT instruction among the 8 halfwords before it, may sit inside an IT
 *   block: its first 4 instructions are not reported as branches.
 * - A call that never returns, followed by a literal pool, lets discovery decode the pool's words
 *   as instructions: a word that encodes B<cond>, CBZ or CBNZ is reported as a branch. The images
 *   carry no mapping symbols to tell code from data.
 *
 * @param set Coverage, as collect() or a `.pcov` load gives it.
 * @param images Per group, its images in the order of coverage_group::images. A raw image
 *        contributes nothing.
 * @param options Branch records and test name.
 * @return The tracefile text.
 */
auto lcov(
    coverage_set const&                        set,
    std::span<std::vector<image::image> const> images,
    lcov_options const& options) -> std::string pre(images.size() == set.groups.size());
}
