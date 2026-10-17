#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/export.hpp>

namespace popcorn::arch::armv8m
{
/**
 * @brief Whether the Cortex-M module implements a core: `cpu` CM33, CM4, CM7 or CM3 in this
 *        release. CM0 and CM0PLUS are refused.
 * @param core Core of a part description.
 */
[[nodiscard]] POPCORN_API auto implements(core_desc const& core) -> bool;

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
 * CM33 runs as ARMv8-M Mainline, in the Secure state, with MSPLIM and PSPLIM carried in
 * snapshots and calls. CM4, CM7 and CM3 run as ARMv7-M: EXC_RETURN takes only its six defined
 * values, CCR.STKALIGN resets set and decides the frame alignment, the MPU is PMSAv7 with RBAR
 * and RASR and their aliases, SHCSR has no HARDFAULTACT or NMIACT, and no SFPA is stacked. CM4
 * has FPv4-SP, CM7 FPv5 single or double precision by `core.fpu_double`, CM3 no FPU: no
 * floating-point registers, FPCCR, FPCAR and FPDSCR read 0.
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
 * - MSPLIM and PSPLIM survive a register-list restore and a call, only `reset()` clears them.
 *   Exception entry stacking below the limit of the stack it pushes to, MSPLIM in Handler mode or
 *   Thread mode on MSP, PSPLIM in Thread mode on PSP, stops with a UsageFault STKOF and leaves
 *   the exception pending, where hardware stacks down to the limit and takes the UsageFault.
 * - Lockup, and the INVPC, UNSTKERR, STKERR, STKOF on entry, VECTTBL and INVSTATE faults, stop
 *   the run under every fault policy.
 * - With TrustZone and fault_policy::enter_handler, the integrity faults of an exception return,
 *   SFSR.INVER, SFSR.INVIS, UFSR.INVPC and SFSR.AUVIOL on the frame, are taken as exceptions on the
 *   frame left in place, the returning exception deactivated first. The entry faults SFSR.AUVIOL
 *   and UFSR.STKOF of the stacked frame set the stack pointer to the frame or to the limit, store
 *   nothing, and are taken as derived exceptions, the higher of the derived and the original
 *   exception first, the other pended. A failed vector fetch takes HFSR.VECTTBL HardFault on the
 *   stacked frame, Secure for a Non-secure slot in Secure memory, else in the state of the
 *   exception being fetched. Lockup, a STKOF of the additional state context alone, a fault while
 *   taking the derived exception, UNSTKERR, STKERR and the NOCP of a Non-secure entry still stop.
 *   The registers keep their values from before the faulting return.
 * - The floating-point context is stacked at exception entry, never lazily: FPCCR and FPCAR are
 *   storage. FPDSCR is storage too, the engine builds new floating-point contexts from its own
 *   defaults.
 * - Spin detection tracks one loop per exception level. It counts one level too few after an SVC,
 *   or a fault the engine raises as an exception, taken while already in Handler mode, until the
 *   level returns to Thread mode. After such an SVC returns, the handler's loop is tracked on the
 *   level of the loop it interrupted, whose tracking restarts once per such SVC. A hung handler
 *   loop that issues SVCs is still reported. The count errs low: a spin may go undetected, never
 *   falsely reported.
 * - CM7: CCR.IC, CCR.DC and CCR.BP are storage. The cache maintenance registers, CCSIDR and
 *   CSSELR read 0 and ignore writes. The DWT lock access register is ignored and CYCCNT is always
 *   writable.
 * - ARMv7-M: a return to Thread mode with other exceptions active and CCR.NONBASETHRDENA clear
 *   is not refused with INVPC. `msr` to MSPLIM or PSPLIM is ignored, not an undefined
 *   instruction.
 * - A debugger `mem()` write to STIR is judged by the privilege of the paused core: while it is
 *   in unprivileged Thread mode with CCR.USERSETMPEND clear, the write pends nothing.
 * - A debugger `mem()` access to the system space that fails outside a slice returns 0, and the
 *   next `run_slice` rethrows the failure.
 *
 * @return A factory whose call throws config_error naming `core.cpu` for a core the module does
 *         not implement, or `core.revision` for a revision not spelled `rNpM`.
 */
[[nodiscard]] POPCORN_API auto factory() -> arch_factory;

/**
 * @brief The Thumb code analysis for coverage reports: the engine's 1 KiB page and 512
 *        instruction block limits, IT predication, UDF as a trap.
 * @return A static object, valid for the whole process.
 */
[[nodiscard]] POPCORN_API auto analysis() noexcept -> code_analysis const&;

/**
 * @brief Registers this module with arch::register_arch under the name "armv8m", with
 *        implements(), factory() and analysis() for ELF machine EM_ARM. Only the first call
 *        registers.
 */
POPCORN_API void register_builtin();
}
