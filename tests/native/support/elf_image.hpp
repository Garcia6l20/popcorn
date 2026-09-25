#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/image/image.hpp>

#include <filesystem>
#include <string_view>

namespace popcorn::testing
{
/**
 * @brief Reads an Arm ELF32 into an image, for core tests before the image loader exists:
 *        PT_LOAD segments at their load address, `.symtab` functions and objects, entry and
 *        digest. No line table, soft float ABI. Test-only.
 * @param file Path of the ELF.
 * @return The image. A segment whose load and run addresses differ keeps only its file bytes.
 * @throws image_error when the file is not an Arm ELF32 or is truncated.
 */
auto read_elf(std::filesystem::path const& file) -> image::image;

/**
 * @brief Address of a symbol, what `image::lookup` gives once implemented.
 * @param loaded Image to search.
 * @param name Exact symbol name.
 * @return The symbol value, global binding preferred.
 * @throws symbol_error when no symbol has that name.
 */
auto find_symbol(image::image const& loaded, std::string_view name) -> address;
}
