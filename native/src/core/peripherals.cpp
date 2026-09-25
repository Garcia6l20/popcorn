#include "machine_impl.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>

namespace popcorn::core
{
void machine::run_state::attach_peripherals()
{
  lines.arch        = owner.arch_.get();
  auto const& units = owner.description_.peripherals;
  owner.peripherals_.resize(units.size());
  for(auto const& [index, unit] : std::views::enumerate(units))
  {
    auto const at = static_cast<std::uint32_t>(index);
    if(unit.alias_of)
    {
      continue;
    }
    auto slot = std::make_unique<peripheral_slot>(unit, owner.scheduler_, at);
    slot->irqs.reserve(unit.irqs.size());
    for(auto const source : unit.irqs)
    {
      slot->irqs.push_back(periph::irq_line{lines, source});
    }
    slot->context.emplace(
        periph::model_context{
            .regs     = slot->regs,
            .irqs     = slot->irqs,
            .dma      = slot->dma,
            .events   = slot->events,
            .clock    = owner.clock_,
            .services = static_cast<machine_services&>(owner),
        });
    auto const configured = owner.options_.models.find(unit.name);
    auto const factory =
        configured != owner.options_.models.end() ? configured->second : periph::find_model(unit);
    if(factory)
    {
      slot->model = factory(*slot->context, unit);
    }
    if(slot->model)
    {
      for(auto const& name : slot->model->claims())
      {
        auto const word = slot->layout.word_of(name);
        if(not word)
        {
          throw config_error(
              unit.name, 0, std::format("peripherals.{}.registers", unit.name),
              std::format("a register of the peripheral for the model's claim of {}", name));
        }
        slot->claimed[*word] = 1;
      }
      for(auto const& rule : unit.cross_rules)
      {
        auto const source = slot->layout.word_of(rule.reg);
        auto const target = slot->layout.word_of(rule.target_reg);
        if(target and slot->claimed[*target] != 0 and source and slot->claimed[*source] == 0)
        {
          throw config_error(
              unit.name, 0,
              std::format("peripherals.{}.cross_rules.{}.{}", unit.name, rule.reg, rule.field),
              std::format(
                  "a cross rule into a register no model claims, or from one the model claims: "
                  "the model of {} claims {} but not {}",
                  unit.name, rule.target_reg, rule.reg));
        }
      }
    }
    owner.peripherals_[index] = std::move(slot);
  }

  for(auto const& [index, unit] : std::views::enumerate(units))
  {
    auto serving = static_cast<std::uint32_t>(index);
    if(unit.alias_of)
    {
      auto const target = std::ranges::find(units, *unit.alias_of, &peripheral_desc::name);
      if(target == units.end() or target->alias_of)
      {
        throw config_error(
            unit.name, 0, std::format("peripherals.{}.alias_of", unit.name),
            "the name of a peripheral that is not itself an alias");
      }
      serving = static_cast<std::uint32_t>(target - units.begin());
    }
    peripheral_index.emplace_back(unit.name, serving);
  }
  std::ranges::sort(peripheral_index);
}

auto machine::run_state::slot(std::string_view name) const -> peripheral_slot&
{
  auto const found = std::ranges::lower_bound(
      peripheral_index, name, {}, &std::pair<std::string_view, std::uint32_t>::first);
  if(found == peripheral_index.end() or found->first != name)
  {
    throw config_error(
        owner.description_.part, 0, "peripherals",
        std::format("a peripheral of the description, not {}", name));
  }
  return *owner.peripherals_[found->second];
}

auto machine::peripheral(std::string_view name) -> periph::register_file&
{
  return run_->slot(name).regs;
}

auto machine::model(std::string_view name) -> periph::model*
{
  return run_->slot(name).model.get();
}
}
