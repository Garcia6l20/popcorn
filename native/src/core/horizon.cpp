#include "machine_impl.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace popcorn::core
{
namespace
{
void lower(std::optional<vtime>& bound, std::optional<vtime> value)
{
  if(value and (not bound or *value < *bound))
  {
    bound = value;
  }
}
}

auto machine::horizon(std::optional<icount> instructions_left) const -> core::horizon
{
  require_stopped(*this);
  class horizon_reader final : public periph::quiet_query
  {
  public:
    horizon_reader(machine const& owner, vtime now, run_state::horizon_tables& tables)
        : owner_{owner}, now_{now}, tables_{tables}, first_{tables_.first},
          first_at_{tables_.first_at}, wake_memo_{tables_.wakes}
    {
      wake_memo_.clear();
      first_.assign(owner_.peripherals_.size(), std::nullopt);
      first_at_.assign(owner_.peripherals_.size(), 0);
      auto const pending = owner_.scheduler_.pending();
      for(auto const& [position, due] : std::views::enumerate(pending))
      {
        auto const at = time_of(due);
        if(due.id.owner == arch::arch_event_owner)
        {
          if(not arch_first_ and owner_.arch_->event_pends(due.id.kind, due.payload))
          {
            arch_first_ = at;
          }
          continue;
        }
        lower(any_first_, at);
        if(due.id.owner < first_.size())
        {
          if(not first_[due.id.owner])
          {
            first_at_[due.id.owner] = static_cast<std::uint32_t>(position);
          }
          lower(first_[due.id.owner], at);
        }
      }
    }

    [[nodiscard]] auto now() const -> vtime override
    {
      return now_;
    }

    [[nodiscard]] auto next_event(
        periph::model const&                                        of,
        std::function_ref<bool(std::uint32_t, std::uint64_t) const> counts) const
        -> std::optional<vtime> override
    {
      auto const index = index_of(of);
      if(not index or not first_[*index])
      {
        return std::nullopt;
      }
      for(auto const& due : owner_.scheduler_.pending().subspan(first_at_[*index]))
      {
        if(due.id.owner == *index and counts(due.id.kind, due.payload))
        {
          return time_of(due);
        }
      }
      return std::nullopt;
    }

    [[nodiscard]] auto wakes(periph::irq_line const& line) const -> bool override
    {
      auto const& lines = tables_.lines;
      auto const  after = std::ranges::upper_bound(
          lines, &line, std::less{}, &run_state::horizon_tables::line_span::first);
      if(after == lines.begin())
      {
        return true;
      }
      auto const& span = *std::prev(after);
      if(not std::less{}(&line, span.last))
      {
        return true;
      }
      if(span.nmi)
      {
        return true;
      }
      auto const& slot = *owner_.peripherals_[span.slot];
      return wakes_source(slot.irq_numbers[static_cast<std::size_t>(&line - span.first)]);
    }

    [[nodiscard]] auto wakes(periph::model const& of) const -> bool override
    {
      auto const index = index_of(of);
      return index and wakes_slot(*index);
    }

    [[nodiscard]] auto opaque_at(address at) const -> bool override
    {
      auto const index = unit_at(at);
      return index and opaque(*index);
    }

    [[nodiscard]] auto read_watched(address at, std::uint64_t size) const -> bool override
    {
      auto const& ranges = owner_.run_->controller_ranges;
      if(ranges.empty())
      {
        return false;
      }
      auto const* const region = size == 0 or size > std::numeric_limits<std::uint32_t>::max()
                                   ? nullptr
                                   : mapped_region::find(owner_.memory_, at, size);
      if(region == nullptr)
      {
        return not peripheral_holds(at, size);
      }
      auto const* const buffer = region->buffer.get();
      auto const        first  = region->offset + (at - region->desc->start);
      auto const        last   = first + size - 1;
      return std::ranges::any_of(ranges, [&](auto const& entry) {
        auto const& [key, controller]   = entry;
        auto const& [watched, from, to] = key;
        return watched == buffer and from <= last and first <= to;
      });
    }

    [[nodiscard]] auto peripheral_at(address at) const -> bool override
    {
      return unit_at(at).has_value();
    }

    [[nodiscard]] auto firewall_faults_transfers() const -> bool override
    {
      auto const* const firewall = owner_.run_->firewall;
      return firewall != nullptr and firewall->faults_transfers();
    }

    [[nodiscard]] auto wakes_slot(std::size_t index) const -> bool
    {
      auto const& slot = *owner_.peripherals_[index];
      return std::ranges::any_of(
          slot.irq_numbers, [this](std::uint32_t source) { return wakes_source(source); });
    }

    [[nodiscard]] auto opaque(std::size_t index) const -> bool
    {
      auto const& slot = owner_.peripherals_[index];
      return slot
         and (slot->python_backed
              or (slot->model and slot->model->role_in_horizon() == periph::wake_role::opaque));
    }

    [[nodiscard]] auto first(std::size_t index) const -> std::optional<vtime>
    {
      return first_[index];
    }

    [[nodiscard]] auto arch_first() const -> std::optional<vtime>
    {
      return arch_first_;
    }

    [[nodiscard]] auto any_first() const -> std::optional<vtime>
    {
      return any_first_;
    }

    [[nodiscard]] auto raised(std::optional<vtime> value) const -> std::optional<vtime>
    {
      return value ? std::optional{std::max(*value, now_)} : std::nullopt;
    }

  private:
    [[nodiscard]] auto time_of(event const& due) const -> vtime
    {
      auto const at =
          due.kind == deadline::time ? vtime{due.due_ps} : owner_.clock_driver_.time_at(due.at);
      return std::max(at, now_);
    }

    [[nodiscard]] auto index_of(periph::model const& of) const -> std::optional<std::uint32_t>
    {
      auto const& models = tables_.models;
      auto const  found  = std::ranges::lower_bound(
          models, &of, std::less{}, &std::pair<periph::model const*, std::uint32_t>::first);
      if(found == models.end() or found->first != &of)
      {
        return std::nullopt;
      }
      return found->second;
    }

    [[nodiscard]] auto unit_at(address at) const -> std::optional<std::uint32_t>
    {
      auto const& units = tables_.units;
      auto const  after = std::ranges::upper_bound(
          units, std::uint64_t{at}, std::less{}, &run_state::horizon_tables::unit_window::base);
      if(after == units.begin() or std::uint64_t{at} >= std::prev(after)->end)
      {
        return std::nullopt;
      }
      return std::prev(after)->serving;
    }

    [[nodiscard]] auto peripheral_holds(address at, std::uint64_t size) const -> bool
    {
      return size != 0
         and std::ranges::any_of(owner_.description_.peripherals, [&](auto const& unit) {
               return at >= unit.base
                  and std::uint64_t{at} + size <= std::uint64_t{unit.base} + unit.size;
             });
    }

    [[nodiscard]] auto wakes_source(std::uint32_t source) const -> bool
    {
      if(source >= wake_memo_.size())
      {
        wake_memo_.resize(source + 1, std::int8_t{-1});
      }
      auto& known = wake_memo_[source];
      if(known < 0)
      {
        known = owner_.arch_->may_wake(source) ? 1 : 0;
      }
      return known != 0;
    }

    machine const&                     owner_;
    vtime                              now_;
    run_state::horizon_tables&         tables_;
    std::vector<std::optional<vtime>>& first_;
    std::vector<std::uint32_t>&        first_at_;
    std::optional<vtime>               arch_first_{};
    std::optional<vtime>               any_first_{};
    std::vector<std::int8_t>&          wake_memo_;
  };

  if(not run_->horizon_index)
  {
    run_state::horizon_tables tables;
    for(auto const& unit : description_.peripherals)
    {
      if(unit.size == 0)
      {
        continue;
      }
      tables.units.push_back(
          {.base    = unit.base,
           .end     = std::uint64_t{unit.base} + unit.size,
           .serving = run_->serving(unit.name)});
    }
    std::ranges::sort(tables.units, std::less{}, &run_state::horizon_tables::unit_window::base);
    for(auto const& [index, slot] : std::views::enumerate(peripherals_))
    {
      if(not slot)
      {
        continue;
      }
      auto const at = static_cast<std::uint32_t>(index);
      if(slot->model)
      {
        tables.models.emplace_back(slot->model.get(), at);
      }
      tables.lines.push_back(
          {.first = &slot->nmi, .last = &slot->nmi + 1, .slot = at, .nmi = true});
      if(not slot->irqs.empty())
      {
        tables.lines.push_back(
            {.first = slot->irqs.data(),
             .last  = slot->irqs.data() + slot->irqs.size(),
             .slot  = at,
             .nmi   = false});
      }
    }
    std::ranges::sort(tables.models);
    std::ranges::sort(tables.lines, std::less{}, &run_state::horizon_tables::line_span::first);
    run_->horizon_index.emplace(std::move(tables));
  }
  auto const           now = clock_driver_.exact_time();
  horizon_reader const reader{*this, now, *run_->horizon_index};
  core::horizon        out;
  auto const&          wires = run_->transmit_wires;
  auto const           held  = run_->core_held;
  auto const           idle  = not held and arch_->idle();
  auto const           pc    = arch_->pc();
  auto const           looping =
      not held and not idle and run_->self_branch_skippable(pc) and arch_->self_branch(pc);
  out.asleep = held or ((idle or looping) and not arch_->wake_pending());

  std::optional<vtime>              stop;
  std::optional<vtime>              wake;
  std::optional<vtime>              transmit_all;
  std::vector<std::optional<vtime>> transmit(wires.size());
  if(not run_->watches.empty())
  {
    lower(stop, reader.any_first());
  }
  if(out.asleep and looping and instructions_left)
  {
    auto const last = icount{clock_driver_.exact_now().value + instructions_left->value};
    lower(stop, std::max(clock_driver_.time_at(last), now));
  }
  if(out.asleep and not held)
  {
    lower(wake, reader.arch_first());
  }
  for(auto const& [at, slot] : std::views::enumerate(peripherals_))
  {
    if(not slot or not slot->model)
    {
      continue;
    }
    auto const  index = static_cast<std::size_t>(at);
    auto const& model = *slot->model;
    auto const  role  = slot->python_backed ? periph::wake_role::opaque : model.role_in_horizon();
    auto const  first = reader.first(index);
    if(role == periph::wake_role::opaque
       or role == periph::wake_role::reset_source
       or role == periph::wake_role::trigger)
    {
      lower(stop, first);
    }
    if(not out.asleep)
    {
      continue;
    }
    switch(role)
    {
      case periph::wake_role::opaque:
      case periph::wake_role::trigger:
        lower(wake, first);
        lower(transmit_all, first);
        break;
      case periph::wake_role::reset_source:
        lower(wake, first);
        break;
      case periph::wake_role::bus_master:
      {
        auto const quiet = reader.raised(model.quiet_until(reader));
        lower(wake, quiet);
        lower(transmit_all, quiet);
        break;
      }
      case periph::wake_role::lines:
      {
        auto const waking = not held and reader.wakes_slot(index);
        auto const sends  = std::ranges::any_of(wires, [&](std::vector<std::uint32_t> const& wire) {
          return std::ranges::contains(wire, index);
        });
        if(not waking and not sends)
        {
          break;
        }
        auto const quiet = reader.raised(model.quiet_until(reader));
        if(waking)
        {
          lower(wake, quiet);
        }
        for(auto const& [w, wire] : std::views::enumerate(wires))
        {
          if(std::ranges::contains(wire, index))
          {
            lower(transmit[static_cast<std::size_t>(w)], quiet);
          }
        }
        break;
      }
    }
  }
  out.stop = stop;
  if(out.asleep)
  {
    out.wake = held ? std::nullopt : wake;
    for(auto& one : transmit)
    {
      lower(one, transmit_all);
    }
    out.transmit = std::move(transmit);
  }
  return out;
}

void machine::set_transmit_sources(std::vector<std::vector<std::string>> const& wires)
{
  require_stopped(*this);
  std::vector<std::vector<std::uint32_t>> resolved;
  resolved.reserve(wires.size());
  for(auto const& wire : wires)
  {
    auto& indexes = resolved.emplace_back();
    for(auto const& name : wire)
    {
      auto const index = run_->serving(name);
      if(not index)
      {
        throw config_error(
            description_.part, 0, "transmit_sources",
            std::format("a peripheral of the description, not {}", name));
      }
      indexes.push_back(*index);
    }
  }
  run_->transmit_wires = std::move(resolved);
}
}
