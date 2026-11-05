#pragma once

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include "rows.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <meta>
#include <span>
#include <string>
#include <string_view>

namespace popcorn::periph::stm32
{
inline constexpr auto every = std::to_array<std::string_view>({"*"});

/** @brief One row of the built-in model table: the model made for the instances it matches. */
struct builtin
{
  std::string_view ip;
  std::string_view ip_version_glob;
  auto (*make)(board const& b) -> model_factory;
  std::span<std::string_view const> instance_globs = every;
  /** @brief Families the row serves. */
  family_set families;
  /** @brief Part globs that narrow the row inside its families, for a line of a family that no
   *         other line shares the model of. */
  std::span<std::string_view const> lines = every;
  /** @brief Name of the factory function, `?` for a row not built by `row_of`. */
  std::string_view factory = "?";
  /** @brief Name of the variant the factory is given, empty for none. */
  std::string_view variant{};
  /** @brief Names of the board members the factory reads, joined by `,`, empty for none. */
  std::string_view reads{};
};

/** @brief The keys a row matches on. */
struct row_key
{
  std::string_view                  ip;
  std::string_view                  ip_version_glob;
  std::span<std::string_view const> instance_globs = every;
  family_set                        families;
  std::span<std::string_view const> lines = every;
};

[[nodiscard]] inline auto any_glob(std::span<std::string_view const> globs, std::string_view text)
    -> bool
{
  return std::ranges::any_of(globs, [&](std::string_view g) { return glob_match(g, text); });
}

/** @brief Whether @p entry matches an instance of an IP and version on a part. */
[[nodiscard]] inline auto row_matches(
    builtin const&   entry,
    std::string_view ip,
    std::string_view version,
    std::string_view part,
    std::string_view instance) -> bool
{
  return entry.ip == ip
     and glob_match(entry.ip_version_glob, version)
     and entry.families.serves(family_of(part))
     and any_glob(entry.lines, part)
     and any_glob(entry.instance_globs, instance);
}

/** @brief Whether some row of @p rows knows the ip and version and none serves @p part. */
[[nodiscard]] inline auto part_refused(
    std::span<builtin const> rows,
    std::string_view         ip,
    std::string_view         version,
    std::string_view         part) -> bool
{
  auto const same_version = [&](builtin const& entry) {
    return entry.ip == ip and glob_match(entry.ip_version_glob, version);
  };
  return std::ranges::any_of(rows, same_version)
     and not std::ranges::any_of(rows, [&](builtin const& entry) {
           return same_version(entry)
              and entry.families.serves(family_of(part))
              and any_glob(entry.lines, part);
         });
}

/**
 * @brief The first row of @p rows matching an instance.
 * @return The row, or null when none matches.
 * @throws config_error when some row knows the ip and version and none serves @p part.
 */
[[nodiscard]] inline auto select_in(
    std::span<builtin const> rows,
    std::string_view         ip,
    std::string_view         version,
    std::string_view         part,
    std::string_view         instance) -> builtin const*
{
  auto const found = std::ranges::find_if(
      rows, [&](builtin const& entry) { return row_matches(entry, ip, version, part, instance); });
  if(found != rows.end())
  {
    return &*found;
  }
  if(part_refused(rows, ip, version, part))
  {
    throw config_error(
        std::string{part}, 0, std::format("peripherals.{}", instance),
        std::format("no model row of the {} version {} serves the part {}", ip, version, part));
  }
  return nullptr;
}

namespace detail
{
template <std::meta::info... Inputs> consteval auto names_of() -> std::string_view
{
  std::string joined;
  for(auto const name : {std::meta::identifier_of(Inputs)...})
  {
    joined += joined.empty() ? "" : ",";
    joined += name;
  }
  return std::define_static_string(joined);
}
}

/**
 * @brief A row whose make function calls the reflected factory with the reflected board members
 *        and the reflected variant, in that order.
 * @tparam Factory The factory function.
 * @tparam Variant The variant variable the factory takes last, or `^^void` for none.
 * @tparam Inputs The `board` members the factory takes first, in order.
 * @param key The keys the row matches on.
 */
template <std::meta::info Factory, std::meta::info Variant = ^^void, std::meta::info... Inputs>
consteval auto row_of(row_key key) -> builtin
{
  constexpr bool has_variant = Variant != ^^void;
  builtin        row{
      .ip              = key.ip,
      .ip_version_glob = key.ip_version_glob,
      .make            = nullptr,
      .instance_globs  = key.instance_globs,
      .families        = key.families,
      .lines           = key.lines,
      .factory         = std::meta::identifier_of(Factory)};
  if constexpr(has_variant)
  {
    row.variant = std::meta::identifier_of(Variant);
  }
  if constexpr(sizeof...(Inputs) > 0)
  {
    row.reads = detail::names_of<Inputs...>();
  }
  row.make = [](board const& b) -> model_factory {
    if constexpr(has_variant)
    {
      return [:Factory:](b.[:Inputs:]..., [:Variant:]);
    }
    else
    {
      return [:Factory:](b.[:Inputs:]...);
    }
  };
  return row;
}
}
