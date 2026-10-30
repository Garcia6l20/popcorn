#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/export.hpp>

#include <optional>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief The FLASH unit of a part and the program memory it controls. */
struct flash_program
{
  /** @brief The unit, never an alias. */
  peripheral_desc const* unit{};
  /** @brief Program memory regions the unit controls, without aliases, by start address. The
   *         first holds the reset flash base. */
  std::vector<region_desc const*> regions;
};

/**
 * @brief Finds the FLASH unit of @p part: the controller of the executable region with a
 *        controller and no `physical` at the lowest address, the program memory. Data EEPROM
 *        and option byte regions the same unit controls are not executable.
 * @param part Description the machine is built from.
 * @return The unit and its program memory regions, std::nullopt when no executable region names
 *         a controller or the controller is no unit of @p part.
 */
[[nodiscard]] POPCORN_API auto flash_unit(part_description const& part)
    -> std::optional<flash_program>;
}
