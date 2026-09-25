#pragma once

#include <popcorn/core/machine.hpp>
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

/** @brief Where a loaded image came from, supplied by the caller of collect(). */
struct image_source
{
  /** @brief ELF file, or a raw image from a bin or HEX file or the object linker. */
  image_kind kind{image_kind::elf};
  /** @brief Absolute path of the ELF, empty for a raw image. */
  std::string path{};

  /** @brief Member-wise equality. */
  friend auto operator==(image_source const&, image_source const&) -> bool = default;
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
 * @brief Collects a machine's coverage counters as byte-ranged blocks. Each counted block start
 *        is decoded to the end of its translated block, from the loaded images' bytes or, for
 *        code outside them, from memory as it is now. An edge from a block into its own decoded
 *        range, other than to its branch target, proves the engine ended the block there: the
 *        block's hits are split over the sizes the edges show.
 *
 * Known limits: while stepping every stepped instruction counts as a block start and decodes to
 * a full block, so stepped code over-counts. A block that ended early without a recorded edge,
 * at a stop or an exception entry, counts at full size. The hits of a block start whose first
 * instruction cannot be read are counted in coverage_group::dropped. Exceptions that depend on
 * CPU state end blocks the decoder cannot predict. Once a u32 counter saturates, a start's hits
 * over its sizes are the sum of its edges into the block's middle when that is larger.
 *
 * @param target Machine, stopped or inside one of its handlers.
 * @param sources One source per loaded image, in load order. An ELF source's path is stored
 *        made absolute.
 * @return One group.
 * @throws config_error for an ELF source with an empty path or a raw source with a path.
 * @throws machine_running while a slice runs, outside its handlers.
 */
auto collect(core::machine& target, std::span<image_source const> sources) -> coverage_set
    pre(sources.size() == target.loaded_images().size());

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
}
