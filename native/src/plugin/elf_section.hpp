#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace popcorn::plugin
{
/**
 * @brief Reads the `.popcorn_abi` section of an ELF64 file without loading it: the bytes up to
 *        the first NUL. Reads at most the section headers, the section name table, capped at
 *        1 MiB, and 4 KiB of the section.
 * @return The tag, empty when the file is no ELF64 of this host's byte order, has no such
 *         section, or the section holds no NUL within 4 KiB.
 */
[[nodiscard]] auto read_abi_section(std::filesystem::path const& path)
    -> std::optional<std::string>;
}
