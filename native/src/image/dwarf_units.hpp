#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace popcorn::image::detail
{
/** @brief The DWARF sections a line-table decode and a function scan need. */
struct debug_sections
{
  std::span<std::byte const> info, abbrev, line, str, line_str, str_offsets;
  std::span<std::byte const> addr{};
};

/** @brief What a compile unit's line table needs: where it is, and its compilation directory. */
struct compile_unit_ref
{
  std::uint64_t line_offset{};
  std::string   comp_dir{};
};

/** @brief Compile units found by scan_compile_units(), and how many abbrev tables it parsed. */
struct compile_unit_scan
{
  std::vector<compile_unit_ref> units;
  std::size_t                   abbrev_tables_parsed{};
};

/**
 * @brief Scans every compile unit's first DIE for DW_AT_stmt_list and DW_AT_comp_dir, parsing
 *        each distinct abbrev table once.
 * @return The units compile_units() returns, and the number of abbrev tables parsed.
 * @throws image_error on a truncated or malformed .debug_info/.debug_abbrev.
 */
[[nodiscard]] auto scan_compile_units(debug_sections const& sections) -> compile_unit_scan;

/** @brief A subprogram DIE with code, and where its source declares it. */
struct function_die
{
  /** @brief DW_AT_low_pc. */
  std::uint64_t low{};
  /** @brief End of the code, DW_AT_high_pc made absolute. */
  std::uint64_t high{};
  /** @brief `.debug_line` offset of the line program whose file table file indexes. */
  std::uint64_t line_offset{};
  /** @brief DW_AT_decl_file, an index of that file table. */
  std::uint32_t file{};
  /** @brief DW_AT_decl_line, not 0. */
  std::uint32_t line{};
};

/**
 * @brief Finds every DW_TAG_subprogram with DW_AT_low_pc and DW_AT_high_pc and a declaration
 *        line, its own DW_AT_decl_file and DW_AT_decl_line or, for each missing, those of the
 *        DIEs its DW_AT_abstract_origin or DW_AT_specification leads to, up to 8 DIEs.
 * @return The functions sorted by low, high, line offset, file and line, without repeats.
 * @throws image_error on a truncated or malformed .debug_info/.debug_abbrev, or a form it cannot
 *         read.
 */
[[nodiscard]] auto scan_functions(debug_sections const& sections) -> std::vector<function_die>;

/**
 * @brief Scans every compile unit's first DIE for DW_AT_stmt_list and DW_AT_comp_dir.
 * @return One entry per compile unit that has a DW_AT_stmt_list; units without one are skipped.
 * @throws image_error on a truncated or malformed .debug_info/.debug_abbrev.
 */
[[nodiscard]] auto compile_units(debug_sections const& sections) -> std::vector<compile_unit_ref>;
}
