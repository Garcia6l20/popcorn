#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace popcorn::image::detail
{
/** @brief The four DWARF sections a line-table decode needs. */
struct debug_sections
{
  std::span<std::byte const> info, abbrev, line, str, line_str, str_offsets;
};

/** @brief What a compile unit's line table needs: where it is, and its compilation directory. */
struct compile_unit_ref
{
  std::uint64_t line_offset{};
  std::string   comp_dir{};
};

/**
 * @brief Scans every compile unit's first DIE for DW_AT_stmt_list and DW_AT_comp_dir.
 * @return One entry per compile unit that has a DW_AT_stmt_list; units without one are skipped.
 * @throws image_error on a truncated or malformed .debug_info/.debug_abbrev.
 */
[[nodiscard]] auto compile_units(debug_sections const& sections) -> std::vector<compile_unit_ref>;
}
