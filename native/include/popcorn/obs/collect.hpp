#pragma once

#include <popcorn/core/machine.hpp>
#include <popcorn/obs/coverage.hpp>

#include <span>
#include <string>

namespace popcorn::obs
{
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
 * instruction cannot be read are counted in coverage_group::dropped, and so are all hits when no
 * registered arch module implements the machine's core with a code analysis,
 * arch::find_code_analysis. Exceptions that depend on CPU state end blocks the decoder cannot
 * predict. Once a u32 counter saturates, a start's hits over its sizes are the sum of its edges
 * into the block's middle when that is larger.
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
}
