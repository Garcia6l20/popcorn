#include "machine_impl.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::core
{
namespace
{
void fill_trace_ids(auto& slot, part_description const& description, std::uint32_t peripheral)
{
  auto const&                  unit = description.peripherals[peripheral];
  std::map<std::uint32_t, int> per_word;
  for(auto const& desc : unit.registers)
  {
    if(not desc.alternate_of)
    {
      ++per_word[desc.offset / 4];
    }
  }
  slot.trace_ids.assign(slot.layout.rule_count(), trace_id_overflow);
  for(auto const& [index, desc] : std::views::enumerate(unit.registers))
  {
    if(desc.alternate_of)
    {
      continue;
    }
    auto const id   = trace_id(flat_register_id(
        description, {.peripheral = peripheral, .reg = static_cast<std::uint32_t>(index)}));
    auto const word = desc.offset / 4;
    if(word >= slot.layout.words())
    {
      continue;
    }
    if(per_word[word] == 1)
    {
      if(auto const rule = slot.layout.rule_index(word); rule != 0)
      {
        slot.trace_ids[rule - 1] = id;
      }
      continue;
    }
    for(std::uint32_t byte = 0; byte < (desc.width + 7u) / 8u; ++byte)
    {
      slot.byte_trace_ids.emplace_back(desc.offset + byte, id);
    }
  }
  std::ranges::sort(slot.byte_trace_ids);
}
}

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
    fill_trace_ids(*slot, owner.description_, at);
    auto const sources = irq_sources(owner.description_, unit, owner.options_.trustzone);
    slot->irqs.reserve(sources.size());
    for(auto const source : sources)
    {
      slot->irqs.push_back(periph::irq_line{lines, source});
    }
    slot->nmi = periph::irq_line{lines, periph::nmi_source};
    slot->context.emplace(
        periph::model_context{
            .regs               = slot->regs,
            .irqs               = slot->irqs,
            .nmi                = slot->nmi,
            .dma                = slot->dma,
            .events             = slot->events,
            .clock              = owner.clock_,
            .services           = static_cast<machine_services&>(owner),
            .peers              = peers,
            .part               = owner.description_,
            .strict_peripherals = owner.options_.strict_peripherals,
            .trustzone          = owner.options_.trustzone,
        });
    auto const configured = owner.options_.models.find(unit.name);
    auto       factory =
        configured != owner.options_.models.end() ? configured->second : periph::find_model(unit);
    if(factory)
    {
      auto* const installed = firewall;
      building              = &unit;
      slot->model           = factory(*slot->context, unit);
      building              = nullptr;
      if(not slot->model and firewall != installed)
      {
        firewall = installed;
        throw model_error(unit.name, "firewall", 0);
      }
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

  std::vector<std::pair<std::string_view, periph::model*>> by_name;
  for(auto const& [name, serving] : peripheral_index)
  {
    if(auto* const found = owner.peripherals_[serving]->model.get())
    {
      by_name.emplace_back(name, found);
    }
  }
  std::vector<periph::model*> models;
  for(auto const& slot : owner.peripherals_)
  {
    if(slot and slot->model)
    {
      models.push_back(slot->model.get());
    }
  }
  peers.fill(std::move(by_name), std::move(models));
  for(auto const& region : owner.memory_)
  {
    if(region->desc->controller)
    {
      region->controller = peers.find(*region->desc->controller);
    }
  }
}

void machine_peers::fill(
    std::vector<std::pair<std::string_view, periph::model*>> by_name,
    std::vector<periph::model*>                              models) noexcept
{
  by_name_ = std::move(by_name);
  models_  = std::move(models);
}

auto machine_peers::find(std::string_view peripheral) const noexcept -> periph::model*
{
  auto const found = std::ranges::lower_bound(
      by_name_, peripheral, {}, &std::pair<std::string_view, periph::model*>::first);
  return found == by_name_.end() or found->first != peripheral ? nullptr : found->second;
}

auto machine_peers::all() const noexcept -> std::span<periph::model* const>
{
  return models_;
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
