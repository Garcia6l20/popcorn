#pragma once

/**
 * @file
 * @brief The entry queue of an i2c_controller_port that an STM32 I2C model keeps: the entry
 *        kinds, the saved queue, and the placement rules every I2C model with a controller port
 *        applies to it.
 */

#include "target_port.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{

constexpr std::uint8_t entry_phase    = 0;
constexpr std::uint8_t entry_ending   = 1;
constexpr std::uint8_t entry_stop     = 2;
constexpr std::uint8_t entry_dropped  = 3;
constexpr std::uint8_t entry_held     = 4;
constexpr std::uint8_t entry_released = 5;

struct i2c_port_entry
{
  std::uint64_t                due_ps{};
  std::uint64_t                unit{};
  std::uint64_t                phase_ps{};
  std::uint64_t                posted_ps{};
  std::optional<std::uint64_t> wire_end_ps{};
  std::uint32_t                lane{};
  std::uint8_t                 kind{};
  std::uint8_t                 phase{};
  std::uint8_t                 data{};
  std::uint8_t                 ack{};

  friend auto operator==(i2c_port_entry const&, i2c_port_entry const&) -> bool = default;
};

struct i2c_port
{
  std::vector<i2c_port_entry>  entries;
  std::uint64_t                last_unit{};
  std::uint32_t                generation{};
  std::optional<std::uint64_t> reset_ps;

  friend auto operator==(i2c_port const&, i2c_port const&) -> bool = default;
};

template <> struct port_traits<i2c_port_entry>
{
  static constexpr auto         span    = &i2c_port_entry::phase_ps;
  static constexpr std::uint8_t stop    = entry_stop;
  static constexpr std::uint8_t running = entry_phase;
  static constexpr auto         ends_at_due(std::uint8_t kind) -> bool
  {
    return kind == entry_ending or kind == entry_held or kind == entry_released;
  }
  static constexpr std::string_view protocol = "I2C";
};

inline auto
    phase_start(std::span<i2c_port_entry const> entries, std::uint64_t posted, std::uint64_t unit)
        -> std::uint64_t
{
  return entry_start(entries, posted, unit);
}

/**
 * @brief Places @p entry, a phase with every field but its due time, kind and wire end set,
 *        behind the entries of @p port.
 * @param port Port the phase joins.
 * @param entry Phase to place.
 * @param linked Whether a link controller drives the port, which leaves the wire end empty.
 * @return @p entry with its due time and kind, and its wire end when the port's last reset drops
 *         it and no link controller drives the port.
 * @throws std::invalid_argument for a phase of 0 ps or one ending past the 64-bit range.
 */
inline auto placed_phase(i2c_port const& port, i2c_port_entry entry, bool linked) -> i2c_port_entry
{
  if(entry.phase_ps == 0)
  {
    throw std::invalid_argument("an I2C port phase lasts at least 1 ps");
  }
  auto const start = phase_start(port.entries, entry.posted_ps, entry.unit);
  if(entry.phase_ps > std::numeric_limits<std::uint64_t>::max() - start)
  {
    throw std::invalid_argument("an I2C port phase ends within the 64-bit picosecond range");
  }
  auto const end        = start + entry.phase_ps;
  auto const last_reset = port.reset_ps;
  auto const dropped    = last_reset and start <= *last_reset;
  entry.due_ps          = dropped ? std::max(*last_reset, end) : start;
  entry.wire_end_ps     = dropped and not linked ? std::optional{end} : std::nullopt;
  entry.kind            = dropped ? entry_dropped : entry_phase;
  return entry;
}

inline auto
    place_after(std::vector<i2c_port_entry>& entries, std::uint64_t unit, std::uint64_t floor)
        -> std::vector<std::size_t>
{
  auto later =
      std::views::iota(std::size_t{}, entries.size())
      | std::views::filter([&](std::size_t index) {
          auto const& entry = entries[index];
          return entry.unit > unit and (entry.kind == entry_phase or entry.kind == entry_stop);
        })
      | std::ranges::to<std::vector>();
  std::ranges::sort(later, {}, [&](std::size_t index) { return entries[index].unit; });
  std::vector<std::size_t> moved;
  for(auto const index : later)
  {
    auto&      entry = entries[index];
    auto const due =
        entry.kind == entry_phase
            ? std::max({entry.due_ps, floor, phase_start(entries, entry.posted_ps, entry.unit)})
            : std::max(entry.due_ps, floor);
    if(due != entry.due_ps or due <= floor)
    {
      entry.due_ps = due;
      moved.push_back(index);
    }
  }
  return moved;
}

inline auto held_unit(std::span<i2c_port_entry const> entries) -> std::optional<std::uint64_t>
{
  std::optional<std::uint64_t> out;
  for(auto const& entry : entries)
  {
    if(entry.kind == entry_held and (not out or entry.unit < *out))
    {
      out = entry.unit;
    }
  }
  return out;
}
}
