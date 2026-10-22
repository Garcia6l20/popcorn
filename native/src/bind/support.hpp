#pragma once

#include <popcorn/core/description.hpp>

#include <reflex/py.hpp>

#include <string>
#include <vector>

namespace popcorn::bind
{
/**
 * @brief Names of the registered arch modules, the latest registered last.
 */
[[= reflex::py::doc{
    "Names of the registered arch modules, the latest registered last."}]] [[nodiscard]] auto
    arch_modules() -> std::vector<std::string>;

/**
 * @brief Names of the peripherals of @p description that a built-in STM32 model serves. Builds
 *        one machine with the default board and no Python or plugin model, and keeps the names
 *        whose factory returned a model, so a factory that declines an instance counts as no
 *        model.
 * @param description Part description.
 * @param trustzone Whether the machine boots with TZEN = 1.
 * @throws config_error when no arch module implements the core, or @p trustzone is set for a
 *         part without a security extension or IDAU table.
 */
[[= reflex::py::doc{
    "Peripherals a built-in model serves in a machine of this description."}]] [[nodiscard]] auto
    builtin_models(part_description const& description, bool trustzone) -> std::vector<std::string>;
}
