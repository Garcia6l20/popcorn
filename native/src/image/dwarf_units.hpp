#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/** @brief The attributes of a compile unit's first DIE that its macros need. */
struct unit_head
{
  /** @brief DW_AT_name, the primary source file. */
  std::string name{};
  /** @brief DW_AT_macros or DW_AT_GNU_macros, a .debug_macro offset. Empty when neither. */
  std::optional<std::uint64_t> macros{};
  /** @brief DW_AT_str_offsets_base, empty when absent. */
  std::optional<std::uint64_t> str_offsets_base{};
  /** @brief Whether the unit uses the 64-bit DWARF format. */
  bool is64{};
};

/**
 * @brief Every DW_TAG_compile_unit's head, in .debug_info order, units without macros included.
 *        Partial units are left out.
 * @throws image_error on a truncated or malformed .debug_info/.debug_abbrev.
 */
[[nodiscard]] auto unit_heads(debug_sections const& sections) -> std::vector<unit_head>;

/**
 * @brief The string of .debug_str that entry @p index of .debug_str_offsets names, from
 *        @p str_offsets_base, or past the section header when absent.
 * @throws image_error when the index or the string runs past its section.
 */
[[nodiscard]] auto resolve_strx(
    std::uint64_t                index,
    std::optional<std::uint64_t> str_offsets_base,
    std::span<std::byte const>   str_offsets,
    std::span<std::byte const>   str,
    bool                         is64) -> std::string_view;

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
