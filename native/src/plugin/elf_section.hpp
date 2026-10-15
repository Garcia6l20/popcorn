#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

/** @brief What a shared library links and which of popcorn core's registry functions it defines. */
struct dynamic_links
{
  /** @brief The `DT_NEEDED` entries of `.dynamic`, in order. */
  std::vector<std::string> needed;
  /** @brief `popcorn::periph::registered_glob` and `popcorn::periph::register_model` when
   * `.dynsym` defines them, in table order: a copy of core's registry. */
  std::vector<std::string> registry_definitions;
};

/**
 * @brief Reads the dynamic links of an ELF64 file without loading it. Reads at most the section
 *        headers, the first `SHT_DYNAMIC` and `SHT_DYNSYM` sections and their string tables,
 *        each capped at 16 MiB. The `DT_NEEDED` names together read at most their string
 *        table's size, and a symbol name only as far as the registry prefixes reach, so the work
 *        is linear in the bytes read.
 * @return Empty when the file is no ELF64 of this host's byte order, has no dynamic section, a
 *         table it needs is over the cap or outside the file, or the `DT_NEEDED` names are
 *         longer together than their string table.
 */
[[nodiscard]] auto read_dynamic_links(std::filesystem::path const& path)
    -> std::optional<dynamic_links>;
}
