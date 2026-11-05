#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
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
  /** @brief Part globs of the row. */
  std::vector<std::string> part_globs;
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
