#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/rcc.hpp>

#include <functional>
#include <map>
#include <string>

namespace popcorn::periph::stm32
{
/**
 * @brief Board configuration of the built-in STM32 models: clock sources, and the devices and
 *        endpoints wired to the MCU. Owned by the caller, it outlives every machine built from
 *        factories() over it.
 */
struct board
{
  /** @brief Clock sources and timing of the RCC model. */
  rcc_options rcc{};
};

/**
 * @brief Factory per peripheral instance of @p part that a built-in model serves, for
 *        core::machine_options::models.
 * @param b Board the models read their configuration from, kept by reference.
 * @param part Description the machine is built from.
 * @return Factories keyed by instance name, aliases excluded.
 */
[[nodiscard]] auto factories(board const& b, mcu_description const& part)
    -> std::map<std::string, model_factory, std::less<>>;
}
