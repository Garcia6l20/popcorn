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
 * - While the core is in unprivileged Thread mode, the register view and the machine's register
 *   list read MSP, PSP, PRIMASK, BASEPRI and FAULTMASK as 0, and drop writes to them and to
 *   CONTROL: the engine gates them by privilege. Snapshots and calls restore them correctly
 *   through the module's own state.
 * - A debugger `mem()` access to the system space that fails outside a slice returns 0, and the
 *   next `run_slice` rethrows the failure.
 *
 * @return A factory whose call throws config_error naming `core.cpu` for a core the module does
 *         not implement, or `core.revision` for a revision not spelled `rNpM`.
 */
[[nodiscard]] auto factory() -> arch_factory;
}
