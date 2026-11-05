#pragma once

#include <popcorn/core/description.hpp>

#include <reflex/py.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::bind
{
/**
 * @brief Names of the registered arch modules, the latest registered last.
 */
[[= reflex::py::doc{
    "Names of the registered arch modules, the latest registered last."}]] [[nodiscard]] auto
    arch_modules() -> std::vector<std::string>;

/**
 * @brief Names of the peripherals of @p description that a built-in STM32 model serves. Builds
 *        one machine with the default board and no Python or plugin model, and keeps the names
 *        whose factory returned a model, so a factory that declines an instance counts as no
 *        model.
 * @param description Part description.
 * @param trustzone Whether the machine boots with TZEN = 1.
 * @throws config_error when no arch module implements the core, or @p trustzone is set for a
 *         part without a security extension or IDAU table.
 */
[[= reflex::py::doc{
    "Peripherals a built-in model serves in a machine of this description."}]] [[nodiscard]] auto
    builtin_models(part_description const& description, bool trustzone) -> std::vector<std::string>;

/** @brief The built-in model rows that match one peripheral instance of a part. */
struct[[= reflex::py::rename{"RowResolution"}]] row_resolution
{
  /** @brief Instance name. */
  [[= reflex::py::readonly]] std::string instance{};
  /** @brief IP name. */
  [[= reflex::py::readonly]] std::string ip{};
  /** @brief IP version. */
  [[= reflex::py::readonly]] std::string ip_version{};
  /** @brief Index of every matching row, in table order. */
  [[= reflex::py::readonly]] std::vector<std::size_t> matches{};
  /** @brief Index of the row that serves the instance, None for no row. */
  [[= reflex::py::readonly]] std::optional<std::size_t> row{};
  /** @brief Label of that row, empty for no row. */
  [[= reflex::py::readonly]] std::string row_label{};
  /** @brief `row`, `none_no_row` or `none_part_refused`. */
  [[= reflex::py::readonly]] std::string source{};
};

/**
 * @brief The built-in model rows that match each non-alias peripheral of @p description.
 * @param description Part description.
 */
[[= reflex::py::doc{
    "The built-in model rows matching each peripheral instance of a part."}]] [[nodiscard]] auto
    resolve_rows(part_description const& description) -> std::vector<row_resolution>;
}
