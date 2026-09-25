#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/image/image.hpp>

#include <filesystem>

namespace popcorn::image
{
/**
 * @brief Loads a raw binary file as one segment.
 * @param file Path of the binary.
 * @param start Guest address of its first byte.
 * @return The image, one read-only executable segment, no symbols, no lines, no entry.
 * @throws image_error when the file cannot be read, is empty, or does not fit below 4 GiB.
 */
auto load_bin(std::filesystem::path const& file, address start) -> image;

/**
 * @brief Loads an Intel HEX file: one segment per contiguous run of data records.
 * @param file Path of the HEX file.
 * @return The image, entry from a start linear address record when present.
 * @throws image_error naming the file and line of a malformed record.
 */
auto load_hex(std::filesystem::path const& file) -> image;
}
