#pragma once

/**
 * @file
 * @brief The entry queue of an i2c_controller_port that an STM32 I2C model keeps: the entry
 *        kinds, the saved queue, and the placement rules every I2C model with a controller port
 *        applies to it.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
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

inline auto
    phase_start(std::span<i2c_port_entry const> entries, std::uint64_t posted, std::uint64_t unit)
        -> std::uint64_t
{
  auto const earlier = [unit](i2c_port_entry const& entry) { return entry.unit < unit; };
  auto       ahead   = entries | std::views::filter(earlier);
  auto const below   = std::ranges::fold_left(
      ahead | std::views::filter([](auto const& entry) {
        return entry.kind == entry_stop;
      }) | std::views::transform([](auto const& entry) { return entry.unit; }),
      std::uint64_t{}, [](std::uint64_t one, std::uint64_t other) { return std::max(one, other); });
  auto start = posted;
  for(auto const& entry : ahead)
  {
    if(entry.kind == entry_stop)
    {
      start = std::max(start, entry.posted_ps);
    }
    else if(entry.unit < below)
    {
      continue;
    }
    else if(entry.kind == entry_phase)
    {
      start = std::max(start, entry.due_ps + entry.phase_ps);
    }
    else if(entry.kind == entry_ending or entry.kind == entry_held or entry.kind == entry_released)
    {
      start = std::max(start, entry.due_ps);
    }
    else if(entry.wire_end_ps)
    {
      start = std::max(start, *entry.wire_end_ps);
    }
  }
  return start;
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
