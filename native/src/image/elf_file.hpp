#pragma once

#include <popcorn/core/errors.hpp>

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::image::detail
{
/**
 * @brief Bounded slice of @p bytes at [@p offset, @p offset + @p size).
 * @throws image_error{"header", 0, what} when the slice runs past the end of @p bytes. The
 *         bound is computed in std::size_t so a 32-bit offset and size cannot wrap around.
 */
inline auto checked_span(
    std::span<std::byte const> bytes,
    std::size_t                offset,
    std::size_t                size,
    std::string_view           what) -> std::span<std::byte const>
{
  if(std::size_t{offset} + size > bytes.size())
  {
    throw image_error{"header", 0, std::string{"truncated ELF: "} + std::string{what}};
  }
  return bytes.subspan(offset, size);
}

/**
 * @brief Reads a trivially-copyable value at @p offset in @p bytes.
 * @throws image_error{"header", 0, what} when the value runs past the end of @p bytes.
 */
template <typename T>
auto read(std::span<std::byte const> bytes, std::size_t offset, std::string_view what) -> T
{
  auto const slice = checked_span(bytes, offset, sizeof(T), what);
  T          value;
  std::memcpy(&value, slice.data(), sizeof(T));
  return value;
}

/**
 * @brief Bounded null-terminated byte string starting at @p pos in @p data.
 * @param pos Advanced past the terminating NUL.
 * @throws image_error when no NUL terminator is found before the end of @p data.
 */
inline auto read_ntbs(std::span<std::byte const> data, std::size_t& pos) -> std::string_view
{
  std::string_view const rest{reinterpret_cast<char const*>(data.data() + pos), data.size() - pos};
  auto const             nul = rest.find('\0');
  if(nul == std::string_view::npos)
  {
    throw image_error{"header", 0, "unterminated string"};
  }
  pos += nul + 1;
  return rest.substr(0, nul);
}

/** @brief Whole ELF32 file, read once, with checked header and table access. */
class elf_file
{
public:
  /**
   * @brief Reads and header-checks @p file.
   * @throws image_error when the file cannot be opened, is not an ARM ELF32 executable, its
   *         program or section table runs past the end of the file, its table entry sizes
   *         disagree with the actual struct layout, its table offsets are misaligned, or its
   *         section header string table index is invalid.
   */
  explicit elf_file(std::filesystem::path const& file);

  /** @brief File header. */
  [[nodiscard]] auto header() const -> Elf32_Ehdr const&;
  /** @brief Program headers, copied out of the file into natural alignment. */
  [[nodiscard]] auto program_headers() const -> std::span<Elf32_Phdr const>;
  /** @brief Section headers, copied out of the file into natural alignment. */
  [[nodiscard]] auto sections() const -> std::span<Elf32_Shdr const>;
  /** @brief Name of a section from the section header string table. */
  [[nodiscard]] auto section_name(Elf32_Shdr const& section) const -> std::string_view;
  /** @brief Bytes of @p section, empty when it has no file content (SHT_NOBITS or size 0). */
  [[nodiscard]] auto section_bytes(Elf32_Shdr const& section) const -> std::span<std::byte const>;
  /** @brief Bytes of a section by name, empty when the section is not present or has no bytes. */
  [[nodiscard]] auto section(std::string_view name) const
      -> std::optional<std::span<std::byte const>>;
  /** @brief Bytes of the whole file. */
  [[nodiscard]] auto bytes() const -> std::span<std::byte const>;

private:
  std::vector<std::byte>  bytes_;
  Elf32_Ehdr              header_{};
  std::vector<Elf32_Phdr> program_headers_;
  std::vector<Elf32_Shdr> sections_;
  std::string_view        shstrtab_{};
};
}
