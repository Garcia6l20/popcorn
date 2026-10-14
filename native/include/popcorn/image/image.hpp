#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::image
{
/** @brief Loadable part of an image, an ELF PT_LOAD segment or a block of a bin or HEX file. */
struct segment
{
  /** @brief Name for messages: ELF segment index or section name, file name for bin or HEX. */
  std::string name{};
  /** @brief Guest address of the first byte, the ELF load address. */
  address start{};
  /** @brief Bytes to load. */
  std::vector<std::byte> bytes{};
  /** @brief Size in guest memory, at least the byte count, the rest zero-filled. */
  std::uint32_t memory_size{};
  /** @brief Whether the segment holds code. */
  bool executable{};
  /** @brief Whether the guest may write the segment. */
  bool writable{};

  /** @brief Member-wise equality. */
  friend auto operator==(segment const&, segment const&) -> bool = default;
};

/** @brief Kind of a symbol. */
enum class[[= reflex::derive{reflex::Format}]] symbol_kind
{
  function,
  object,
  other,
};

/** @brief Symbol of the image. */
struct symbol
{
  /** @brief Name. */
  std::string name{};
  /** @brief Value, the address, with the Arm Thumb bit cleared for a function. */
  address value{};
  /** @brief Size in bytes, 0 when unknown. */
  std::uint32_t size{};
  /** @brief Kind. */
  symbol_kind kind{symbol_kind::other};
  /** @brief Whether the symbol has global binding. lookup() prefers the global one of several
   * symbols sharing a name. */
  bool global{};

  /** @brief Member-wise equality. */
  friend auto operator==(symbol const&, symbol const&) -> bool = default;
};

/** @brief Row of a line table: the source position of the instructions from its address up to
 * the next row's address. An end-of-sequence row has line 0: it ends the previous sequence at its
 * address and covers no instruction itself. */
struct line_row
{
  /** @brief First address the row covers. */
  address addr{};
  /** @brief Index of the source file in `image::files`. */
  std::uint32_t file{};
  /** @brief Source line, 0 for code with no line. */
  std::uint32_t line{};
  /** @brief Whether the row starts a statement, the DWARF is_stmt flag. Rows without it are kept
   * too, they carry the line of inlined and scheduled code. */
  bool is_stmt{};

  /** @brief Member-wise equality. */
  friend auto operator==(line_row const&, line_row const&) -> bool = default;
};

/** @brief Function of the debug information with contiguous code, and where its source declares
 * it. */
struct debug_function
{
  /** @brief First address of its code, DW_AT_low_pc with the Arm Thumb bit cleared. */
  address low{};
  /** @brief Address past its code, DW_AT_high_pc with the Arm Thumb bit cleared. */
  address high{};
  /** @brief Index of the declaring source file in `image::files`, DW_AT_decl_file. */
  std::uint32_t file{};
  /** @brief Declaration line, DW_AT_decl_line, never 0. */
  std::uint32_t line{};

  /** @brief Member-wise equality. */
  friend auto operator==(debug_function const&, debug_function const&) -> bool = default;
};

/**
 * @brief Firmware image: segments to load, symbols, line table, entry and calling convention.
 *        Plain data, built by load_elf() and the other file loaders, or by the object linker. The
 * machine checks the segments against the description's regions when loading it.
 */
struct POPCORN_API image
{
  /** @brief Loadable segments, in address order. */
  std::vector<segment> segments{};
  /** @brief Symbols, in address order. */
  std::vector<symbol> symbols{};
  /** @brief Source file paths the line table indexes: the name, prefixed while relative by its
   * directory then by the compilation directory, lexically normalized with `/` separators. */
  std::vector<std::string> files{};
  /** @brief Line table, all rows, ordered by address. Empty without debug information. */
  std::vector<line_row> lines{};
  /** @brief Functions of the debug information, a DW_TAG_subprogram with DW_AT_low_pc,
   * DW_AT_high_pc and a declaration each, sorted by low, high, file then line. Empty without
   * debug information or when its DIEs cannot be read. A declaration file no row uses comes after
   * the rows' files in `files`. */
  std::vector<debug_function> functions{};
  /** @brief Entry point from the file header, empty when the file has none. */
  std::optional<address> entry{};
  /** @brief Floating-point calling convention: soft unless the ELF header flags set
   * EF_ARM_ABI_FLOAT_HARD, then hard_double for AAPCS-VFP whatever the FPU precision, or soft
   * when `.ARM.attributes` sets Tag_ABI_VFP_args to the base AAPCS. */
  arch::float_abi abi{arch::float_abi::soft};
  /** @brief SHA-256 of the loadable content, lower-case hex, what a saved snapshot checks. */
  std::string digest{};

  /**
   * @brief Looks a symbol up by exact name.
   * @param name Symbol name.
   * @return The symbol.
   * @throws symbol_error when no symbol has that name, with the closest names, or when
   * several symbols have it and none is the single global one, with every match.
   */
  [[nodiscard]] auto lookup(std::string_view name) const -> symbol const&;

  /**
   * @brief Locates an address: the function or object containing it, and the line of the row
   *        with the greatest address not above @p addr, as addr2line does.
   * @param addr Guest address.
   * @return The location, symbol and file empty when unknown. Its views point into this image.
   */
  [[nodiscard]] auto locate(address addr) const -> location;

  /** @brief Member-wise equality. */
  friend auto operator==(image const&, image const&) -> bool = default;
};

/**
 * @brief Loads an ELF32 file: PT_LOAD segments, symbols, line table, entry, calling convention
 *        and digest.
 * @param file Path of the ELF.
 * @return The image.
 * @throws image_error when the file is not a loadable ELF32 for a supported machine.
 */
POPCORN_API auto load_elf(std::filesystem::path const& file) -> image;

/** @brief One distinct macro definition and the compile units whose program ends with it. */
struct macro_entry
{
  /** @brief The name, without parameters. */
  std::string name{};
  /** @brief The text after the name: `(a, b) body` for a function-like macro, the body after one
   * space for an object-like one, empty for `#define X`. */
  std::string text{};
  /** @brief Whether the name is directly followed by `(` in the definition. */
  bool function_like{};
  /** @brief Indexes into macro_index::units, ascending, of the units that end with it. */
  std::vector<std::uint32_t> units{};

  /** @brief Member-wise equality. */
  friend auto operator==(macro_entry const&, macro_entry const&) -> bool = default;
};

/** @brief The macros of one file, deduplicated across its compile units. */
struct macro_index
{
  /** @brief DW_AT_name of each compile unit with macros, in .debug_info order. */
  std::vector<std::string> units{};
  /** @brief Distinct (name, text, function_like) entries, sorted by name, text, then
   * function_like. */
  std::vector<macro_entry> entries{};

  /** @brief Member-wise equality. */
  friend auto operator==(macro_index const&, macro_index const&) -> bool = default;
};

/**
 * @brief Reads the `.debug_macro` section of an ELF32 file, DWARF 5 or GNU version 4: the
 *        definitions in effect at the end of each compile unit's macro program, merged into
 *        distinct entries. Not part of image: macros do not belong to the loaded content.
 * @param file Path of the ELF.
 * @return The index, empty when the file has no `.debug_macro` or no DWARF.
 * @throws image_error when the file is not an ELF32 or its DWARF or macro program is malformed,
 *         holds an import cycle, more than 64 nested imports, more than 2^24 operations for one
 *         compile unit or 2^26 for the file, or an operation of a supplementary file.
 */
POPCORN_API auto read_macros(std::filesystem::path const& file) -> macro_index;
}
