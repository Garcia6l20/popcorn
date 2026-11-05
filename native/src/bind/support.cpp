#include "support.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include "../periph/stm32/rows.hpp"

#include <reflex/enum.hpp>

#include <ranges>
#include <utility>

namespace popcorn::bind
{
auto arch_modules() -> std::vector<std::string>
{
  return arch::module_names();
}

auto builtin_models(part_description const& description, bool trustzone) -> std::vector<std::string>
{
  core::machine_options options;
  options.raz_wi = periph::stm32::raz_wi_map(description);
  if(trustzone)
  {
    auto const configured =
        periph::stm32::configure_option_bytes(description, {.tzen = true}, options);
    if(not configured)
    {
      throw config_error("option_bytes", 0, configured.error().key, configured.error().expected);
    }
  }
  auto builtin = periph::stm32::factories(periph::stm32::board{}, description);
  for(auto const& unit : description.peripherals)
  {
    if(unit.alias_of or options.models.contains(unit.name))
    {
      continue;
    }
    auto found = builtin.find(unit.name);
    options.models.emplace(
        unit.name, found != builtin.end() ? std::move(found->second) : periph::model_factory{});
  }
  core::machine target{description, arch::find_arch(description), std::move(options)};
  return description.peripherals
       | std::views::filter([&](peripheral_desc const& unit) {
           return not unit.alias_of and target.model(unit.name) != nullptr;
         })
       | std::views::transform(&peripheral_desc::name)
       | std::ranges::to<std::vector>();
}

auto resolve_rows(part_description const& description) -> std::vector<row_resolution>
{
  return periph::stm32::resolve(description)
       | std::views::transform([](periph::stm32::resolution const& found) {
           return row_resolution{
               .instance   = found.instance,
               .ip         = found.ip,
               .ip_version = found.ip_version,
               .matches    = found.matches,
               .row        = found.row,
               .row_label  = found.row_label,
               .source     = std::string{reflex::enum_value_name(found.source)}};
         })
       | std::ranges::to<std::vector>();
}
}
