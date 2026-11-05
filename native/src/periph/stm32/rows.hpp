#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief The STM32 families a built-in model row can serve. A new family adds an enumerator, a
 *         prefix in `family_of` and the rows that name it. */
enum class family : std::uint8_t
{
  u5,
  h5,
  l1,
  l4,
};

static_assert(
    std::meta::enumerators_of(^^family).size() < 8,
    "family_set holds one bit per family in 8 bits");

/**
 * @brief The family of a part, from the prefix of its name. A prefix that extends the prefix of
 *        another family is tested before it.
 * @return The family, or empty for a name with no family prefix.
 */
[[nodiscard]] constexpr auto family_of(std::string_view part) -> std::optional<family>
{
  if(part.starts_with("STM32U5"))
  {
    return family::u5;
  }
  if(part.starts_with("STM32H5"))
  {
    return family::h5;
  }
  if(part.starts_with("STM32L1"))
  {
    return family::l1;
  }
  if(part.starts_with("STM32L4"))
  {
    return family::l4;
  }
  return std::nullopt;
}

/** @brief The family of @p part. */
[[nodiscard]] inline auto family_of(part_description const& part) -> std::optional<family>
{
  return family_of(std::string_view{part.part});
}

/** @brief The families a row serves, written out for every row. `shared_families` serves every
 *         family and a part with no family. */
struct family_set
{
  /** @brief One bit per `family` enumerator, all set for the shared set. */
  std::uint8_t bits;

  constexpr family_set(family one) : bits{static_cast<std::uint8_t>(1u << std::to_underlying(one))}
  {}

  /** @brief Whether the set serves every family and a part with no family. */
  [[nodiscard]] constexpr auto shared() const -> bool
  {
    return bits == 0xFF;
  }

  /** @brief Whether a part of family @p of, empty for none, is served. */
  [[nodiscard]] constexpr auto serves(std::optional<family> of) const -> bool
  {
    return shared() or (of and ((bits >> std::to_underlying(*of)) & 1u) != 0);
  }

  [[nodiscard]] constexpr auto has(family one) const -> bool
  {
    return not shared() and ((bits >> std::to_underlying(one)) & 1u) != 0;
  }

  [[nodiscard]] static constexpr auto from_bits(std::uint8_t bits) -> family_set
  {
    family_set out{family::u5};
    out.bits = bits;
    return out;
  }
};

[[nodiscard]] constexpr auto operator|(family_set a, family_set b) -> family_set
{
  return family_set::from_bits(static_cast<std::uint8_t>(a.bits | b.bits));
}

/** @brief The set of a row that serves every family and a part with no family. */
inline constexpr auto shared_families = family_set::from_bits(0xFF);

[[nodiscard]] constexpr auto operator|(family a, family b) -> family_set
{
  return family_set{a} | family_set{b};
}

/** @brief Why a peripheral instance has the row it has, or none. */
enum class row_source : std::uint8_t
{
  /** @brief A row matches the instance. */
  row,
  /** @brief No row matches and none refuses the part, such as an IP no row knows or an
   *         instance no row lists. */
  none_no_row,
  /** @brief Some row knows the IP and version, none serves the part. */
  none_part_refused,
};

/** @brief The built-in model rows that match one peripheral instance. */
struct resolution
{
  /** @brief Instance name, such as `GPIOA`. */
  std::string instance;
  /** @brief IP name, as the description spells it. */
  std::string ip;
  /** @brief IP version, as the description spells it. */
  std::string ip_version;
  /** @brief Index of every row whose ip, version, part and instance globs match, in table order. */
  std::vector<std::size_t> matches;
  /** @brief The row that serves the instance, the first match. */
  std::optional<std::size_t> row;
  /** @brief Label of the row in `row`, empty when there is none. */
  std::string row_label;
  /** @brief Name of the row's factory function, `?` for a row not built by `row_of`, empty for
   *         no row. */
  std::string factory;
  /** @brief Name of the variant the row gives its factory, empty for none. */
  std::string variant;
  /** @brief Names of the board members the row reads, joined by `,`, empty for none. */
  std::string reads;
  /** @brief Why there is or is no row. */
  row_source source = row_source::none_no_row;
};

/** @brief One row of the built-in model table. */
struct row_info
{
  /** @brief Label, as `resolution::row_label` spells it. */
  std::string label;
  /** @brief Names of the families the row serves, `*` for a shared row. */
  std::vector<std::string> families;
  /** @brief Part globs that narrow the row inside its families, `*` for none. */
  std::vector<std::string> lines;
};

/**
 * @brief The `board` members read by the row that serves each non-alias peripheral of @p part,
 *        as a machine selects it.
 * @return Member names such as `spi` and `spi_controllers` by instance name. An instance with no
 *         row, or a row reading no member, is absent or maps to an empty list.
 */
[[nodiscard]] POPCORN_API auto board_inputs_of(part_description const& part)
    -> std::map<std::string, std::vector<std::string_view>, std::less<>>;

/** @brief Every row of the built-in model table, in table order. */
[[nodiscard]] POPCORN_API auto rows() -> std::vector<row_info>;

/**
 * @brief Resolves each non-alias peripheral of @p part against the built-in model rows, as a
 *        machine does, and reports instead of throwing when no row serves the part.
 * @return One entry per non-alias peripheral, in description order.
 */
[[nodiscard]] POPCORN_API auto resolve(part_description const& part) -> std::vector<resolution>;
}
