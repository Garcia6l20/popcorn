#pragma once

#include <popcorn/periph/stm32/caches.hpp>

#include "row_of.hpp"

#include <span>

namespace popcorn::periph::stm32
{
/**
 * @brief Factory of the ICACHE model, RM0456 section 8.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto icache_factory(cache_options options) -> model_factory;

/**
 * @brief Factory of the DCACHE model, RM0456 section 9.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto dcache_factory(cache_options options) -> model_factory;

/** @brief The ICACHE and DCACHE model rows. */
[[nodiscard]] auto caches_rows() -> std::span<builtin const>;
}
