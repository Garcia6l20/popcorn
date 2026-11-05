#pragma once

#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include "variants.hpp"

#include <array>
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
  std::span<std::string_view const> part_globs     = every;
  flash_variant                     flash          = {};
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
  std::span<std::string_view const> part_globs     = every;
};

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
      .part_globs      = key.part_globs,
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
