#pragma once

#include "model_base.hpp"

#include <algorithm>
#include <array>
#include <contracts>
#include <cstddef>
#include <initializer_list>
#include <meta>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace popcorn::periph::stm32
{
/** @brief Register and field name of one role, as one family's description spells it. An
 * empty `reg` says the family has no such role. */
struct field_name
{
  /** @brief Register name or alias, empty when the family lacks the role. */
  std::string_view reg;
  /** @brief Field name. */
  std::string_view field;
  /** @brief The register or field may be absent from a part's description. */
  bool optional = false;
};

/** @brief One entry of a family's name table: the role and its name. */
template <typename Role> struct named_field
{
  /** @brief Role the entry names. */
  Role role;
  /** @brief Its name in the family. */
  field_name name;
};

/** @brief Number of enumerators of @p Role. */
template <typename Role>
  requires std::is_enum_v<Role>
inline constexpr std::size_t role_count = std::meta::enumerators_of(^^Role).size();

/** @brief Whether the enumerators of @p Role are 0 to N-1 in declaration order, so a role
 * indexes a table of role_count entries. */
template <typename Role>
  requires std::is_enum_v<Role>
consteval auto roles_in_order() -> bool
{
  std::size_t index = 0;
  for(auto const e : std::meta::enumerators_of(^^Role))
  {
    if(std::cmp_not_equal(std::to_underlying(std::meta::extract<Role>(e)), index))
    {
      return false;
    }
    ++index;
  }
  return true;
}

/**
 * @brief Whether @p table names every enumerator of @p Role exactly once, and nothing else.
 *        A role the family lacks is still listed, with an empty name.
 * @param table Name table of one family.
 */
template <typename Role>
  requires std::is_enum_v<Role>
consteval auto names_every_role(std::span<named_field<Role> const> table) -> bool
{
  if(table.size() != role_count<Role>)
  {
    return false;
  }
  for(auto const e : std::meta::enumerators_of(^^Role))
  {
    if(std::ranges::count(table, std::meta::extract<Role>(e), &named_field<Role>::role) != 1)
    {
      return false;
    }
  }
  return true;
}

/**
 * @brief names_every_role() over an array table, so @p Role is deduced.
 * @param table Name table of one family.
 */
template <typename Role, std::size_t N>
  requires std::is_enum_v<Role>
consteval auto names_every_role(std::array<named_field<Role>, N> const& table) -> bool
{
  return names_every_role<Role>(std::span<named_field<Role> const>{table});
}

/**
 * @brief Whether a capability flag and the roles it needs agree in @p table: the flag is set
 *        and each of @p roles has a name, or the flag is clear and none of them has one.
 * @param table Name table of one family.
 * @param flag Capability flag of the family.
 * @param roles Roles that exist exactly when the flag is set.
 */
template <typename Role>
  requires std::is_enum_v<Role>
consteval auto flag_matches_roles(
    std::span<named_field<Role> const> table,
    bool                               flag,
    std::initializer_list<Role>        roles) -> bool
{
  auto const named = [&](Role role) {
    auto const found = std::ranges::find(table, role, &named_field<Role>::role);
    return found != table.end() and not found->name.reg.empty();
  };
  return flag ? std::ranges::all_of(roles, named) : std::ranges::none_of(roles, named);
}

/**
 * @brief flag_matches_roles() over an array table, so @p Role is deduced.
 * @param table Name table of one family.
 * @param flag Capability flag of the family.
 * @param roles Roles that exist exactly when the flag is set.
 */
template <typename Role, std::size_t N>
  requires std::is_enum_v<Role>
consteval auto flag_matches_roles(
    std::array<named_field<Role>, N> const& table,
    bool                                    flag,
    std::initializer_list<Role>             roles) -> bool
{
  return flag_matches_roles<Role>(std::span<named_field<Role> const>{table}, flag, roles);
}

/**
 * @brief The fields of one family's name table, resolved against a peripheral description.
 *        A role with an empty name, or an optional one the description lacks, stays empty.
 */
template <typename Role>
  requires std::is_enum_v<Role>
class field_table
{
  static_assert(roles_in_order<Role>(), "the roles of a field table are 0 to N-1 in order");

public:
  /**
   * @brief Resolves every named role of @p table.
   * @param table Name table of one family, checked with names_every_role().
   * @param resolve Field of a register by name, such as stm32_model::field(), which throws
   *        config_error on a missing name.
   * @param present Whether the description has the register and the field.
   * @throws config_error from @p resolve for a required name the description lacks.
   */
  template <typename Resolve, typename Present>
  field_table(std::span<named_field<Role> const> table, Resolve resolve, Present present)
  {
    for(auto const& [role, name] : table)
    {
      auto const index = static_cast<std::size_t>(std::to_underlying(role));
      contract_assert(index < fields_.size());
      if(name.reg.empty() or (name.optional and not present(name.reg, name.field)))
      {
        continue;
      }
      fields_[index] = resolve(name.reg, name.field);
    }
  }

  /** @brief Field of @p role, which must be resolved. */
  [[nodiscard]] auto operator[](Role role) const -> field_ref pre(find(role).has_value())
  {
    return *fields_[static_cast<std::size_t>(std::to_underlying(role))];
  }

  /** @brief Field of @p role, std::nullopt when the family or the description lacks it. */
  [[nodiscard]] auto find(Role role) const -> std::optional<field_ref>
  {
    return fields_[static_cast<std::size_t>(std::to_underlying(role))];
  }

private:
  std::array<std::optional<field_ref>, role_count<Role>> fields_{};
};
}
