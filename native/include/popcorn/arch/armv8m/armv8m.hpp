#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>

namespace popcorn::arch::armv8m
{
/**
 * @brief Whether the Cortex-M module implements a core: `cpu` CM33 in this release.
 * @param core Core of an MCU description.
 */
[[nodiscard]] auto implements(core_desc const& core) -> bool;

/**
 * @brief Factory of the Cortex-M arch module, for machine construction.
 *
 * The module maps the core's system space, ITM at 0xE0000000, DWT at 0xE0001000, FPB at
 * 0xE0002000 and the SCS at 0xE000E000, 4 KB each, and refuses a description with a peripheral
 * inside them. Unicorn reports WFE and YIELD with pc past the instruction, not at it.
 *
 * Current limitations:
 *
 * - A debugger `mem()` write to STIR is judged by the privilege of the paused core: while it is
 *   in unprivileged Thread mode with CCR.USERSETMPEND clear, the write pends nothing.
 * - A debugger `mem()` access to the system space that fails outside a slice returns 0, and the
 *   next `run_slice` rethrows the failure.
 *
 * @return A factory whose call throws config_error naming `core.cpu` for a core the module does
 *         not implement, or `core.revision` for a revision not spelled `rNpM`.
 */
[[nodiscard]] auto factory() -> arch_factory;
}
