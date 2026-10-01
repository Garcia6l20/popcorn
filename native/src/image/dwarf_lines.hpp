#pragma once

#include "dwarf_units.hpp"

#include <popcorn/image/image.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::image::detail
{
/** @brief Entry of the file table of one line program. */
struct file_ref
{
  /** @brief `.debug_line` offset of the line program. */
  std::uint64_t line_offset{};
  /** @brief Index in its file table, as DW_AT_decl_file and the file register count. */
  std::uint32_t index{};
};

/** @brief Decoded `.debug_line` content for every compile unit passed in. */
struct line_table
{
  std::vector<std::string> files{};
  std::vector<line_row>    rows{};
  /** @brief Per file_ref asked for, its index in files, empty when no unit's table holds it. */
  std::vector<std::optional<std::uint32_t>> wanted{};
};

/**
 * @brief Decodes every compile unit's line program.
 * @param sections `.debug_line`, `.debug_str` and `.debug_line_str` used, `.debug_info` and
 *        `.debug_abbrev` not read again here.
 * @param units Compile units from compile_units(), each naming a `.debug_line` offset.
 * @param wanted File table entries to resolve beside the rows' files. A file no row uses is added
 *        after every file the rows use.
 * @throws image_error on a truncated or malformed line program.
 */
[[nodiscard]] auto decode_lines(
    debug_sections const&             sections,
    std::span<compile_unit_ref const> units,
    std::span<file_ref const>         wanted = {}) -> line_table;
}
