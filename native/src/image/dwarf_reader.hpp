#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::image::detail
{
/** @brief Bounded cursor over a DWARF section, throwing image_error on any out-of-range read. */
class dwarf_cursor
{
public:
  dwarf_cursor(std::span<std::byte const> data, std::string_view section_name);

  [[nodiscard]] auto u8() -> std::uint8_t;
  [[nodiscard]] auto u16() -> std::uint16_t;
  [[nodiscard]] auto u32() -> std::uint32_t;
  [[nodiscard]] auto u64() -> std::uint64_t;
  [[nodiscard]] auto uleb() -> std::uint64_t;
  [[nodiscard]] auto sleb() -> std::int64_t;
  /** @brief NUL-terminated string starting at the cursor, advanced past the terminator. */
  [[nodiscard]] auto cstr() -> std::string_view;
  void               skip(std::uint64_t count);
  void               seek(std::size_t offset);

  [[nodiscard]] auto pos() const -> std::size_t;
  [[nodiscard]] auto size() const -> std::size_t;
  [[nodiscard]] auto remaining() const -> std::size_t;
  [[nodiscard]] auto at_end() const -> bool;
  [[nodiscard]] auto data() const -> std::span<std::byte const>;

private:
  void check(std::size_t count) const;

  std::span<std::byte const> data_;
  std::size_t                pos_{};
  std::string_view           section_;
};

/**
 * @brief Reads a DWARF initial length, distinguishing 32-bit from 64-bit DWARF format.
 * @param is64 Set to true when the section uses the 64-bit DWARF format (escape 0xffffffff).
 * @return The unit length, not counting the initial length field itself.
 */
[[nodiscard]] auto read_initial_length(dwarf_cursor& cursor, bool& is64) -> std::uint64_t;

/** @brief Reads an offset, 4 bytes for 32-bit DWARF, 8 bytes for 64-bit DWARF. */
[[nodiscard]] auto read_offset(dwarf_cursor& cursor, bool is64) -> std::uint64_t;

/** @brief Bounded null-terminated string starting at @p offset in @p data. */
[[nodiscard]] auto read_cstr_at(
    std::span<std::byte const> data,
    std::uint64_t              offset,
    std::string_view           section_name) -> std::string_view;

/**
 * @brief @p start + @p length, checked so it cannot wrap and stays within @p bound.
 * @throws image_error naming @p section when @p length runs past @p bound.
 */
[[nodiscard]] auto checked_offset(
    std::size_t      start,
    std::uint64_t    length,
    std::size_t      bound,
    std::string_view section) -> std::size_t;

/**
 * @brief A unit's end offset: checked_offset() into @p size, plus a progress guarantee.
 * @throws image_error naming @p section when @p length runs past @p size or does not advance
 *         past @p start (which would loop a caller that uses the result as a while-condition).
 */
[[nodiscard]] auto checked_unit_end(
    std::size_t      start,
    std::uint64_t    length,
    std::size_t      size,
    std::string_view section) -> std::size_t;

/** @brief A form value: either a raw scalar, a resolved string, or a deferred strx index. */
struct form_value
{
  std::uint64_t    u{};
  std::string_view s{};
  bool             is_string{};
  bool             is_strx{};
};

/**
 * @brief Reads one DWARF attribute or line-header-entry value in @p form_code's encoding.
 * @param implicit_const The abbrev-declared value for DW_FORM_implicit_const, unused otherwise.
 * @throws image_error on an unhandled or malformed form, or a chained DW_FORM_indirect.
 */
[[nodiscard]] auto read_form(
    dwarf_cursor&              cursor,
    std::uint64_t              form_code,
    std::span<std::byte const> str,
    std::span<std::byte const> line_str,
    bool                       is64,
    std::int64_t               implicit_const) -> form_value;
}
