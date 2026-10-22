#pragma once

#include "dwarf_units.hpp"

#include <popcorn/image/image.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace popcorn::image::detail
{
/** @brief Most imports nested in one another while a compile unit's macro program runs. */
inline constexpr std::size_t max_macro_import_depth = 64;

/** @brief Most operations applied for one compile unit, imports executed in place included. */
inline constexpr std::uint64_t max_macro_operations = std::uint64_t{1} << 24;

/** @brief Most operations applied for all the compile units of one file. */
inline constexpr std::uint64_t max_macro_file_operations = std::uint64_t{1} << 26;

/** @brief What read_macros() found, and how many times it decoded a macro unit. */
struct macro_scan
{
  macro_index index;
  std::size_t units_decoded{};
};

/**
 * @brief Runs the macro program of every compile unit with DW_AT_macros or DW_AT_GNU_macros:
 *        each define replaces the name's definition, each undef removes it, imports run in
 *        place. Then merges the units' end states into distinct entries. Each macro unit is
 *        decoded once, whatever its import count, and its operation count with imports
 *        expanded is computed once. When offset 0 holds a compile unit's own macro unit, an
 *        import of offset 0 is skipped and counted in macro_index::discarded_imports: GNU ld
 *        writes 0 for an import whose target section it discarded, such as a `wm4.*` group an
 *        LTO link drops, and GCC never imports a compile unit's own macro unit. When no compile
 *        unit's macros start at 0, an import of 0 runs.
 * @param sections The DWARF sections, for the unit heads and the strings.
 * @param macro The `.debug_macro` section.
 * @throws image_error on a truncated or malformed program, an unknown opcode without an
 *         operands table entry, a supplementary-file opcode, an import cycle, more than
 *         max_macro_import_depth nested imports, more than max_macro_operations operations
 *         for one compile unit, or more than max_macro_file_operations for the file. Every
 *         count is checked before any operation runs.
 */
[[nodiscard]] auto read_macros(debug_sections const& sections, std::span<std::byte const> macro)
    -> macro_scan;
}
