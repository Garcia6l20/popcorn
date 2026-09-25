#pragma once

#include "dwarf_units.hpp"

#include <popcorn/image/image.hpp>

#include <span>
#include <string>
#include <vector>

namespace popcorn::image::detail
{
/** @brief Decoded `.debug_line` content for every compile unit passed in. */
struct line_table
{
  std::vector<std::string> files{};
  std::vector<line_row>    rows{};
};

/**
 * @brief Decodes every compile unit's line program.
 * @param sections `.debug_line`, `.debug_str` and `.debug_line_str` used, `.debug_info` and
 *        `.debug_abbrev` not read again here.
 * @param units Compile units from compile_units(), each naming a `.debug_line` offset.
 * @throws image_error on a truncated or malformed line program.
 */
[[nodiscard]] auto
    decode_lines(debug_sections const& sections, std::span<compile_unit_ref const> units)
        -> line_table;
}
