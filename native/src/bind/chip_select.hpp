#pragma once

#include "../periph/stm32/pin_range.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace popcorn::bind::detail
{
/**
 * @brief Refuses a chip select pin that is an unbonded pad of the package.
 * @param description The MCU the chip select is on.
 * @param port GPIO port name or alias, such as `GPIOA`. A port the description lacks passes.
 * @param pin Pin number, empty for the hardware SS output.
 * @throws config_error with key `pin` when the port's bonded mask lacks @p pin or @p pin is
 *         above 15.
 */
inline void check_bonded_chip_select(
    part_description const&     description,
    std::string const&          port,
    std::optional<std::uint8_t> pin)
{
  auto const named = std::ranges::find(description.peripherals, port, &peripheral_desc::name);
  if(not pin or named == description.peripherals.end())
  {
    return;
  }
  auto const target = named->alias_of.value_or(port);
  auto const unit   = std::ranges::find_if(description.peripherals, [&](peripheral_desc const& p) {
    return p.name == target and not p.alias_of;
  });
  if(unit == description.peripherals.end() or not unit->bonded)
  {
    return;
  }
  if(*pin >= 16 or (*unit->bonded >> *pin & 1U) == 0)
  {
    throw config_error(
        port, 0, "pin",
        periph::stm32::unbonded_pin_text(description.part, port, *pin, *unit->bonded));
  }
}
}
