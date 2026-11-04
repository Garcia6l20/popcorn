#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/firewall.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief A memory region that aliases another, as a filter sees it. */
struct memory_alias
{
  /** @brief First address of the alias. */
  address start{};
  /** @brief Size in bytes. */
  std::uint32_t size{};
  /** @brief Address the alias's first byte maps. */
  address physical{};
};

/**
 * @brief Every region of @p part that aliases another, bit-band windows excepted.
 * @param part Description the machine is built from.
 */
[[nodiscard]] inline auto memory_aliases(part_description const& part) -> std::vector<memory_alias>
{
  return part.regions
       | std::views::filter([](region_desc const& region) {
           return region.physical and region.kind != region_kind::bit_band;
         })
       | std::views::transform([](region_desc const& region) {
           return memory_alias{
               .start = region.start, .size = region.size, .physical = *region.physical};
         })
       | std::ranges::to<std::vector>();
}

/**
 * @brief The alias of @p aliases holding @p at, null when none does.
 * @param aliases Aliases of the part.
 * @param at Guest address.
 */
[[nodiscard]] inline auto alias_at(std::span<memory_alias const> aliases, address at)
    -> memory_alias const*
{
  auto const found = std::ranges::find_if(aliases, [at](memory_alias const& alias) {
    return at >= alias.start and at - alias.start < alias.size;
  });
  return found == aliases.end() ? nullptr : &*found;
}

/**
 * @brief The address @p at reaches: its target when an alias of @p aliases holds it, @p at
 *        itself otherwise.
 * @param aliases Aliases of the part.
 * @param at Guest address.
 */
[[nodiscard]] inline auto physical_of(std::span<memory_alias const> aliases, address at) -> address
{
  auto const* const alias = alias_at(aliases, at);
  return alias == nullptr ? at : alias->physical + (at - alias->start);
}

/**
 * @brief @p extent, a range of target addresses, seen through @p alias: shifted to the alias
 *        addresses and clipped to the alias.
 * @param alias Alias whose target holds the extent.
 * @param extent Range of target addresses that holds a byte of the alias's target.
 */
[[nodiscard]] inline auto through(memory_alias const& alias, memory_extent extent) -> memory_extent
{
  auto const last  = std::uint64_t{alias.physical} + alias.size - 1u;
  auto const first = std::max<std::uint64_t>(extent.first, alias.physical);
  auto const end   = std::min<std::uint64_t>(extent.last, last);
  return {
      .first = static_cast<address>(alias.start + (first - alias.physical)),
      .last  = static_cast<address>(alias.start + (end - alias.physical))};
}
}
