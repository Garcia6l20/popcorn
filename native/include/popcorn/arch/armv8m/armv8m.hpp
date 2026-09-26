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
 * Calls follow AAPCS32: `float_abi::soft` passes floating-point values in core registers,
 * `float_abi::hard_double` in s0-s15 and d0-d7 with back-filling, the AAPCS-VFP variant.
 * `float_abi::hard_single` is a RISC-V convention and refused. `call_stack::main` runs the call
 * on MSP, `call_stack::process` on PSP, `call_stack::current` on the stack Thread mode had
 * selected, MSP when paused in a handler. The return sentinel must lie below 0xE0000000, the
 * execute-never system region. A call never changes PRIMASK: a masked call holds back
 * asynchronous exceptions only, SVC and faults stay live. After the call the machine restores the
 * paused registers and module state. SysTick keeps its live phase unless the call reprogrammed
 * it, ticks missed during a masked call are taken as one SysTick, and an IRQ line raised during
 * the call is taken after it.
 *
 * Current limitations:
 *
 * - SysTick and DWT CYCCNT count processor cycles derived from executed instructions and the
 *   machine's cycles per instruction. SYST_CSR.CLKSOURCE 0 divides by 8, the STM32 HCLK/8
 *   convention. CYCCNT keeps counting through WFI and WFE sleep, where a real core gates it.
 * - A guest read of SYST_CVR, SYST_CSR.COUNTFLAG or CYCCNT resolves to the end of the translated
 *   block holding the read, which also ends at a 1 KB page boundary.
 * - A debugger `mem()` read of SYST_CSR clears COUNTFLAG, as a guest read does.
 * - WFE sleeps unless the event register is set or an SEV sits right before it: Unicorn runs SEV
 *   as a NOP, so an SEV elsewhere never wakes a WFE. The event register is set by exception
 *   entry and return, and by an exception becoming pending while SCR.SEVONPEND is set. WFE also
 *   wakes on the WFI condition with PRIMASK, BASEPRI and FAULTMASK respected, where WFI ignores
 *   PRIMASK. YIELD runs as a NOP.
 * - A fault the engine raises as an exception, such as a fetch from the system region or a
 *   stack-limit violation, stops the engine run when it is pended, and the handler is entered at
 *   once in the same slice. An unprivileged access to the system space is served, never a
 *   BusFault.
 * - A bus error from a peripheral model, on a read or a write, is reported as IMPRECISERR with no
 *   BFAR, where hardware reports a failed read as a precise error.
 * - MSPLIM and PSPLIM have no engine register id: a reset clears them, an in-memory snapshot
 *   restores them, a serialized snapshot does not. Exception entry stacking below MSPLIM is not
 *   detected, where hardware raises STKOF.
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
