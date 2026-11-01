#pragma once

/**
 * @file
 * @brief The skeleton every STM32 model with a target port applies to its port entries: the
 *        entry checks, the scheduling, the pick of the next due entry and the start of a new
 *        entry behind the ones ahead of it. A model keys it by its entry type through
 *        port_traits.
 */

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/model.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>

namespace popcorn::periph::stm32
{

/**
 * @brief What the port skeleton reads of an entry type, specialized next to the entry type.
 *
 * A specialization holds `span`, the member pointer of the entry's own duration, `stop`, the kind
 * that ends a transaction, `running`, the kind of an entry that lasts its span, `ends_at_due`, the
 * kinds that end at their due time, and `protocol`, the name the texts of the checks carry.
 */
template <typename Entry> struct port_traits;

/**
 * @brief Instruction count of a port entry due at @p due_ps, clamped to the epoch and to now.
 * @param time Clock of the machine.
 * @param due_ps Due time of the entry.
 */
[[nodiscard]] inline auto port_count_of(core::clock const& time, std::uint64_t due_ps) -> icount
{
  auto const count = time.to_icount(vtime{std::max(due_ps, time.epoch_time().ps)});
  return count.value < time.now().value ? count : time.now();
}

/**
 * @brief Schedules @p entry at its due time on its lane, with the port generation and the lane
 *        as payload.
 * @param ctx Context of the model.
 * @param kind Event kind of the model's port events.
 * @param port Port of the entry.
 * @param entry Entry to schedule.
 */
template <typename Port, typename Entry>
void schedule_port(model_context& ctx, std::uint32_t kind, Port const& port, Entry const& entry)
{
  ctx.events.schedule_at(
      vtime{entry.due_ps}, kind, std::uint64_t{port.generation} << 32 | entry.lane, entry.lane,
      ctx.clock);
}

/**
 * @brief Appends @p entry to @p port, records its unit as the last one and schedules it.
 * @param ctx Context of the model.
 * @param kind Event kind of the model's port events.
 * @param port Port of the entry.
 * @param entry Entry to queue.
 */
template <typename Port, typename Entry>
void queue_port(model_context& ctx, std::uint32_t kind, Port& port, Entry entry)
{
  port.last_unit = entry.unit;
  port.entries.push_back(entry);
  schedule_port(ctx, kind, port, entry);
}

/**
 * @brief Refuses a port entry before anything is queued.
 * @param ctx Context of the model.
 * @param instance Name of the instance, such as `I2C1`.
 * @param attached Whether a controller is attached to the instance.
 * @param port Port the entry would join.
 * @param max_entries Most entries the port holds.
 * @param lane Lane of the entry.
 * @param unit Unit of the entry.
 * @throws config_error with no controller attached or a full port.
 * @throws std::invalid_argument for lane 0 or a unit not above the last one.
 */
template <typename Port>
void check_port_entry(
    model_context const& ctx,
    std::string_view     instance,
    bool                 attached,
    Port const&          port,
    std::size_t          max_entries,
    std::uint32_t        lane,
    std::uint64_t        unit)
{
  using traits = port_traits<typename decltype(Port::entries)::value_type>;
  if(not attached)
  {
    throw config_error(
        ctx.part.part, 0, std::format("devices.{}", instance),
        std::format("an {} controller attached to the instance", traits::protocol));
  }
  if(port.entries.size() >= max_entries)
  {
    throw config_error(
        ctx.part.part, 0, std::format("devices.{}", instance),
        std::format("at most {} {} port entries queued", max_entries, traits::protocol));
  }
  if(lane == 0)
  {
    throw std::invalid_argument(
        std::format("an {} port entry goes on a lane of 1 and up", traits::protocol));
  }
  if(unit <= port.last_unit)
  {
    throw std::invalid_argument(
        std::format(
            "an {} port unit {} is not above the last unit {}", traits::protocol, unit,
            port.last_unit));
  }
}

/**
 * @brief Index of the entry to run next: on a lane up to @p lane, due by @p now, not skipped, the
 *        lowest instruction count first, then the lowest unit, then the first in the queue.
 * @param entries Entries of the port.
 * @param lane Highest lane the event runs.
 * @param now Current time.
 * @param time Clock of the machine.
 * @param skip Whether an entry waits, called on each entry on time.
 */
template <typename Entry, typename Skip>
[[nodiscard]] auto next_due(
    std::span<Entry const> entries,
    std::uint32_t          lane,
    std::uint64_t          now,
    core::clock const&     time,
    Skip                   skip) -> std::optional<std::size_t>
{
  std::optional<std::size_t> pick;
  auto                       pick_count = icount{};
  for(std::size_t index = 0; index < entries.size(); ++index)
  {
    auto const& entry = entries[index];
    if(entry.lane > lane or entry.due_ps > now or skip(entry))
    {
      continue;
    }
    auto const count = port_count_of(time, entry.due_ps);
    if(not pick
       or count.value < pick_count.value
       or (count.value == pick_count.value and entry.unit < entries[*pick].unit))
    {
      pick       = index;
      pick_count = count;
    }
  }
  return pick;
}

/**
 * @brief Start time of an entry of @p unit posted at @p posted, behind the entries of lower
 *        units: every stop's post time, and the end of each entry since the last stop.
 * @param entries Entries of the port.
 * @param posted Time the entry was posted.
 * @param unit Unit of the entry.
 */
template <typename Entry>
[[nodiscard]] auto
    entry_start(std::span<Entry const> entries, std::uint64_t posted, std::uint64_t unit)
        -> std::uint64_t
{
  using traits       = port_traits<Entry>;
  auto const earlier = [unit](Entry const& entry) { return entry.unit < unit; };
  auto       ahead   = entries | std::views::filter(earlier);
  auto const below   = std::ranges::fold_left(
      ahead | std::views::filter([](auto const& entry) {
        return entry.kind == traits::stop;
      }) | std::views::transform([](auto const& entry) { return entry.unit; }),
      std::uint64_t{}, [](std::uint64_t one, std::uint64_t other) { return std::max(one, other); });
  auto start = posted;
  for(auto const& entry : ahead)
  {
    if(entry.kind == traits::stop)
    {
      start = std::max(start, entry.posted_ps);
    }
    else if(entry.unit < below)
    {
      continue;
    }
    else if(entry.kind == traits::running)
    {
      start = std::max(start, entry.due_ps + entry.*traits::span);
    }
    else if(traits::ends_at_due(entry.kind))
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
}
