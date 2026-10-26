#pragma once

#include <popcorn/core/coverage.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/image/image.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
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
  /** @brief CPU accesses to unmodelled registers with hits, sorted by pc, peripheral, register,
   * offset, write and reason. */
  std::vector<core::unmodelled_access> unmodelled{};
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
POPCORN_API auto
    function_ranges(std::span<image::image const> images, std::span<std::string const> names)
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
 * and block starts. A block left before its end, by a stop, a fault or an intercept, holds only
 * the instructions that ran, and none when it was left at its start. Across groups, which are
 * different images, the counts of one file and line are summed: the only place a line count is a
 * sum. Branches are Thumb B<cond>, CBZ and CBNZ outside IT blocks, found by following control
 * flow through each function in range, and counted from the edges out of the blocks that end with
 * them. A branch whose block never ran to it is `-`. Edges the machine did not record, at an
 * exception entry or a stop right after a branch, are missing from the counts. BRDA block numbers
 * are the branch's index among its line's branches. A line that has a BRDA record has a DA
 * record of at least the executed count of the branch instruction. Each file record starts with
 * its own TN record.
 *
 * A function is `FN:<start>,<end>,<name>`. With a debug function at its address, image::image
 * functions, it is filed under the declaring file at the declaration line. Without one, it is
 * filed under the file of the row that holds its entry, the one addr2line reports, at the
 * smallest non-zero line among the rows at its entry in that file. Its end is the largest line of
 * that file among the rows of its code, at least its start and before the start of the next
 * function of the file. The start line has a DA record of at least the function's entry count, so
 * that genhtml finds it a line. A function with no counted entry whose `FN` range holds a
 * line with a DA count above 0 has FNDA 1, and FNH counts it. Such a line comes from a body
 * entered before counting began, or from an inlined helper or split part whose rows are filed
 * inside the range. A function with counted entries keeps their sum.
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
 * - A line can be hit while its branch never ran: another path ran an instruction the line table
 *   gives that line, such as a shared exit, or a run stopped or faulted after the line's first
 *   instruction and before its branch. genhtml 2 with branch coverage reports it as `line is hit
 *   but no branches on line have been evaluated`.
 *
 * @param set Coverage, as collect() or a `.pcov` load gives it.
 * @param images Per group, its images in the order of coverage_group::images. A raw image
 *        contributes nothing. An ELF image whose `elf_machine` has no code analysis,
 *        arch::find_code_analysis, contributes no branches.
 * @param options Branch records and test name.
 * @return The tracefile text.
 */
POPCORN_API auto lcov(
    coverage_set const&                        set,
    std::span<std::vector<image::image> const> images,
    lcov_options const& options) -> std::string pre(images.size() == set.groups.size());

/** @brief CPU accesses of one source line to one unmodelled register, summed over the line's
 * instructions. */
struct unmodelled_use
{
  /** @brief Peripheral as the window names it, or for a raz_wi range `raz_wi[<index>]` when
   * unnamed, `<name>[<index>]` when several ranges share the name, the bare name otherwise. */
  std::string peripheral{};
  /** @brief Register name, empty where the description has none. */
  std::string reg{};
  /** @brief Byte offset of the accessed word in the peripheral or range. */
  std::uint32_t offset{};
  /** @brief Whether the access was a write. */
  bool write{};
  /** @brief Why the register is unmodelled. */
  core::unmodelled_reason reason{};
  /** @brief Times it was accessed. */
  std::uint64_t hits{};

  /** @brief Member-wise equality. */
  friend auto operator==(unmodelled_use const&, unmodelled_use const&) -> bool = default;
};

/** @brief A source line whose instructions accessed unmodelled registers. */
struct unmodelled_line
{
  /** @brief Source path as the lcov `SF` record names it. */
  std::string file{};
  /** @brief Line number, one that lcov() writes a `DA` record for. */
  std::uint32_t line{};
  /** @brief Accesses, sorted by peripheral, register, offset, write and reason. */
  std::vector<unmodelled_use> uses{};

  /** @brief Member-wise equality. */
  friend auto operator==(unmodelled_line const&, unmodelled_line const&) -> bool = default;
};

/**
 * @brief Source lines that accessed unmodelled registers, the accesses of
 *        coverage_group::unmodelled mapped to lines with the rules lcov() uses.
 *
 * An access goes to the line of the line-table row holding its pc, in the first ELF image of its
 * group whose executable segment holds the pc, when lcov() writes a `DA` record for that file and
 * line. Accesses of other pcs, such as code outside the group's ranges, are left out. Accesses
 * of one register from several instructions of a line, and of several groups, sum.
 *
 * @param set Coverage, as collect() or a `.pcov` load gives it.
 * @param images Per group, its images as lcov() takes them.
 * @return Lines sorted by file then line.
 */
POPCORN_API auto
    unmodelled_lines(coverage_set const& set, std::span<std::vector<image::image> const> images)
        -> std::vector<unmodelled_line> pre(images.size() == set.groups.size());

/**
 * @brief Encodes a coverage set as a `.pcov` version 2 file, little endian, fields 4-byte aligned,
 *        strings padded with zeros: `PCOV`, u16 version 2, u16 flags, u32 group count, then per
 *        group u64 split, u64 dropped, the images `{u32 kind, u32 base, u32 end, u32 length, path,
 *        u32 length, digest}`, the ranges `{u32 start, u32 size, u32 length, name}`, the blocks
 *        `{u32 start, u32 size, u64 hits}`, the edges `{u32 from, u32 to, u64 hits}` and, when
 *        flags bit 0 is set, the unmodelled accesses `{u32 pc, u32 offset, u32 access, u64 hits,
 *        u32 length, peripheral, u32 length, register}`, access holding the write flag in bit 0
 *        and the core::unmodelled_reason in bits 8 to 15. Each list comes after its u32 count.
 *        Flags bit 0 is set when some group has an unmodelled access, every other flag bit is
 *        0. The set is made canonical first, as merge() does.
 * @param set Coverage.
 * @return The file bytes.
 */
POPCORN_API auto encode_pcov(coverage_set const& set) -> std::vector<std::byte>;

/**
 * @brief Decodes a `.pcov` file of version 2, or of version 1, the layout of version 2 with flags
 *        0, whose groups have no unmodelled access.
 * @param bytes File bytes.
 * @param file Name for errors.
 * @return The set, canonical.
 * @throws format_error when the bytes are not a canonical `.pcov` version 1 or 2 file: bad magic,
 *         version or flags, a count past the end, an unsorted or repeated key, a zero hit, a
 *         block size outside 1 to 1024, an unknown image kind, a raw image with a path, an
 *         unmodelled access whose access word sets bits other than write and reason or holds a
 *         reason outside no_register to raz_wi, or flags bit 0 with no unmodelled access.
 */
POPCORN_API auto decode_pcov(std::span<std::byte const> bytes, std::string_view file = {})
    -> coverage_set;

/**
 * @brief Writes a `.pcov` file, through a temporary file `<path>.tmp.<pid>` in the same directory
 *        renamed over @p path, so that a reader never sees half a file.
 * @param path Destination.
 * @param set Coverage.
 * @throws format_error when the file cannot be written.
 */
POPCORN_API void save_pcov(std::filesystem::path const& path, coverage_set const& set);

/**
 * @brief Reads a `.pcov` file.
 * @param path File.
 * @return The set.
 * @throws format_error when the file cannot be read or is not a `.pcov` file.
 */
POPCORN_API auto load_pcov(std::filesystem::path const& path) -> coverage_set;

/**
 * @brief Sums coverage sets, canonical: groups with the same image digests add their block,
 *        edge, unmodelled access, split and dropped counts, saturating, and take the union of their
 *        ranges, the smallest name for one start and size. Each image keeps the smallest of its
 *        paths, so the result does not depend on the order of @p sets. Other groups are kept.
 * @param sets Sets to sum, possibly none.
 * @return The sum, groups sorted by their digest lists.
 */
POPCORN_API auto merge(std::span<coverage_set const> sets) -> coverage_set;

/**
 * @brief Adds a coverage set to a `.pcov` file, creating it when missing: what the machines of
 *        one worker do with the worker's file. One writer per file: two processes adding to the
 *        same file at once can lose one's coverage.
 * @param path File.
 * @param set Coverage to add.
 * @return The merged set now in the file.
 * @throws format_error when the existing file is not a `.pcov` file or cannot be written.
 */
POPCORN_API auto merge_into(std::filesystem::path const& path, coverage_set const& set)
    -> coverage_set;

/**
 * @brief Loads the images of every group, for lcov().
 * @param set Coverage.
 * @return Per group, its images: an ELF loaded from its path, a raw image as an empty image.
 * @throws format_error when an ELF is missing or its digest differs from the one recorded, the
 *         ELF changed since collect.
 * @throws image_error when an ELF cannot be loaded.
 */
POPCORN_API auto load_images(coverage_set const& set) -> std::vector<std::vector<image::image>>;

/** @brief A drcov file and what it left out. */
struct drcov_file
{
  /** @brief The file bytes. */
  std::vector<std::byte> bytes{};
  /** @brief Blocks with hits outside every module's executable extent, not in the file. */
  std::uint64_t dropped{};

  /** @brief Member-wise equality. */
  friend auto operator==(drcov_file const&, drcov_file const&) -> bool = default;
};

/**
 * @brief drcov version 2 file of a coverage set, for Lighthouse and other drcov readers.
 *
 * One module per image of every group, in group order then load order, ids from 0, its extent
 * the image's executable segments: the base is the lowest executable segment's start, not the
 * start of the whole mapping, so offsets count from the first executable byte. The path column is
 * the ELF path, `raw-` and the first 12 hex digits of the digest for a raw image. Entry, checksum
 * and timestamp are 0. A block goes to the last image of its group whose extent holds its start,
 * and one BB entry `{u32 offset from the module base, u16 size, u16 module id}` is written per
 * module, start and size, sorted, whatever its hits: drcov has no counts. A block in no module,
 * such as code run from RAM, is left out and counted.
 *
 * @param set Coverage, canonical: every block sized 1 to 1024 bytes with hits.
 * @return The file and the count of blocks left out.
 * @throws format_error when there are more than 65536 modules.
 */
POPCORN_API auto drcov(coverage_set const& set) -> drcov_file
    pre(std::ranges::all_of(set.groups, [](coverage_group const& group) {
      return std::ranges::all_of(group.blocks, [](block_hit const& block) {
        return block.size > 0 and block.size <= 1024 and block.hits > 0;
      });
    }));
}
