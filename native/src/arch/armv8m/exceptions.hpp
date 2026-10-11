#pragma once

#include "state.hpp"

#include <popcorn/core/types.hpp>

#include <cstdint>
#include <optional>

struct uc_struct;

namespace popcorn::arch::armv8m
{
/** @brief Exception number of NMI. */
inline constexpr std::uint32_t exc_nmi = 2;
/** @brief Exception number of HardFault. */
inline constexpr std::uint32_t exc_hardfault = 3;
/** @brief Exception number of MemManage. */
inline constexpr std::uint32_t exc_memmanage = 4;
/** @brief Exception number of BusFault. */
inline constexpr std::uint32_t exc_busfault = 5;
/** @brief Exception number of UsageFault. */
inline constexpr std::uint32_t exc_usagefault = 6;
/** @brief Exception number of SVCall. */
inline constexpr std::uint32_t exc_svcall = 11;
/** @brief Exception number of DebugMonitor. */
inline constexpr std::uint32_t exc_debugmon = 12;
/** @brief Exception number of PendSV. */
inline constexpr std::uint32_t exc_pendsv = 14;
/** @brief Exception number of SysTick. */
inline constexpr std::uint32_t exc_systick = 15;
/** @brief Exception number of IRQ 0. */
inline constexpr std::uint32_t exc_irq0 = 16;

/** @brief SCR.SLEEPONEXIT. */
inline constexpr std::uint32_t scr_sleeponexit = 1u << 1;
/** @brief SCR.SEVONPEND. */
inline constexpr std::uint32_t scr_sevonpend = 1u << 4;

/** @brief CCR.STKALIGN, 8-byte aligned exception frames. */
inline constexpr std::uint32_t ccr_stkalign = 1u << 9;

/** @brief HFSR.VECTTBL, a vector read failed on exception entry. */
inline constexpr std::uint32_t hfsr_vecttbl = 1u << 1;

/** @brief CFSR bits of the configurable faults. */
namespace cfsr
{
/** @brief MMFSR.IACCVIOL. */
inline constexpr std::uint32_t iaccviol = 1u << 0;
/** @brief MMFSR.MMARVALID. */
inline constexpr std::uint32_t mmarvalid = 1u << 7;
/** @brief BFSR.IBUSERR. */
inline constexpr std::uint32_t ibuserr = 1u << 8;
/** @brief BFSR.PRECISERR. */
inline constexpr std::uint32_t preciserr = 1u << 9;
/** @brief BFSR.IMPRECISERR. */
inline constexpr std::uint32_t impreciserr = 1u << 10;
/** @brief BFSR.UNSTKERR. */
inline constexpr std::uint32_t unstkerr = 1u << 11;
/** @brief BFSR.STKERR. */
inline constexpr std::uint32_t stkerr = 1u << 12;
/** @brief BFSR.BFARVALID. */
inline constexpr std::uint32_t bfarvalid = 1u << 15;
/** @brief UFSR.UNDEFINSTR. */
inline constexpr std::uint32_t undefinstr = 1u << 16;
/** @brief UFSR.INVSTATE. */
inline constexpr std::uint32_t invstate = 1u << 17;
/** @brief UFSR.INVPC. */
inline constexpr std::uint32_t invpc = 1u << 18;
/** @brief UFSR.NOCP. */
inline constexpr std::uint32_t nocp = 1u << 19;
/** @brief UFSR.STKOF. */
inline constexpr std::uint32_t stkof = 1u << 20;
/** @brief UFSR.UNALIGNED. */
inline constexpr std::uint32_t unaligned = 1u << 24;
/** @brief UFSR.DIVBYZERO. */
inline constexpr std::uint32_t divbyzero = 1u << 25;
}

/** @brief A configurable fault to raise: its class, target handler and status. */
struct fault_cause
{
  /** @brief Class reported when the fault stops the run. */
  fault_kind kind{};
  /** @brief MemManage, BusFault or UsageFault. */
  std::uint32_t target{};
  /** @brief CFSR bits the fault sets. */
  std::uint32_t cfsr_bits{};
  /** @brief Address reported when the fault stops the run. */
  address reported{};
  /** @brief Faulting data address, recorded in BFAR or MMFAR by target. */
  std::optional<address> data_address{};
  /** @brief Whether the fault is synchronous: it escalates when it cannot preempt. */
  bool synchronous{true};
};

/** @brief HFSR.FORCED, a configurable fault or SVC escalated to HardFault. */
inline constexpr std::uint32_t hfsr_forced = 1u << 30;

/** @brief Mask of the group priority bits for an AIRCR.PRIGROUP value. */
constexpr auto group_mask(std::uint32_t prigroup) noexcept -> std::uint32_t
{
  return (0xffu << (prigroup + 1)) & 0xffu;
}

/** @brief Group priority of a priority, fixed negative priorities kept. */
constexpr auto group_priority(int priority, std::uint32_t prigroup) noexcept -> int
{
  return priority < 0
           ? priority
           : static_cast<int>(static_cast<std::uint32_t>(priority) & group_mask(prigroup));
}

/** @brief Priority masking registers. */
struct masks
{
  /** @brief PRIMASK.PM. */
  bool primask{};
  /** @brief BASEPRI, 0 for no masking. */
  std::uint8_t basepri{};
  /** @brief FAULTMASK.FM. */
  bool faultmask{};
};

/** @brief Whether bit @p number of a bit vector is set. */
[[nodiscard]] auto test_bit(std::vector<std::uint32_t> const& bits, std::uint32_t number) noexcept
    -> bool;

/** @brief Sets or clears bit @p number of a bit vector. */
void put_bit(std::vector<std::uint32_t>& bits, std::uint32_t number, bool value) noexcept;

/** @brief Priority of exception @p number: NMI -2, HardFault -1, otherwise its priority byte. */
[[nodiscard]] auto priority_of(exception_state const& exceptions, std::uint32_t number) noexcept
    -> int;

/**
 * @brief ExecutionPriority() of ARMv7-M, the same in ARMv8-M Mainline without security: 256,
 *        lowered to the group priority of every active exception and of a nonzero BASEPRI, to 0
 *        by PRIMASK and to -1 by FAULTMASK.
 */
[[nodiscard]] auto execution_priority(
    exception_state const& exceptions,
    std::uint32_t          prigroup,
    masks                  held) noexcept -> int;

/** @brief Whether exception @p number is synchronous: HardFault, the configurable faults, SVCall.
 */
[[nodiscard]] constexpr auto synchronous(std::uint32_t number) noexcept -> bool
{
  return (number >= exc_hardfault and number <= exc_usagefault) or number == exc_svcall;
}

/**
 * @brief Pending and enabled exception taken first: lowest priority value, subpriority
 *        included, then lowest number. System exceptions 2-15 are always enabled, IRQs by ISER.
 * @param exceptions Exception state.
 * @param synchronous_only Whether asynchronous exceptions are left out, as in a masked call.
 */
[[nodiscard]] auto
    best_pending(exception_state const& exceptions, bool synchronous_only = false) noexcept
    -> std::optional<std::uint32_t>;

/**
 * @brief Whether any exception from 2 below @p count is pending and enabled, as best_pending
 *        counts them.
 * @param exceptions Exception state.
 * @param count Exception count, 16 + IRQ count.
 * @param synchronous_only Whether asynchronous exceptions are left out, as in a masked call.
 */
[[nodiscard]] auto any_pending(
    exception_state const& exceptions,
    std::uint32_t          count,
    bool                   synchronous_only = false) noexcept -> bool;

/**
 * @brief Writes the special registers in the one order the engine accepts from any privilege:
 *        Handler mode, nPRIV cleared, privileged Thread, target SPSEL and FPCA, both stack
 *        pointers and the masks, target IPSR, target CONTROL with nPRIV last, then GE.
 * @param engine Engine to write.
 * @param target Values to apply.
 * @param stack_limits Whether the core has MSPLIM and PSPLIM, written last.
 * @throws emulation_error when the engine refuses a write.
 */
void set_context(uc_struct* engine, special_registers const& target, bool stack_limits);

/**
 * @brief Reads the special registers from the engine, also in unprivileged Thread mode, where
 *        the engine reads the stack pointers and masks as zero: the read goes through Handler
 *        mode for its duration.
 * @param engine Engine to read.
 * @param stack_limits Whether the core has MSPLIM and PSPLIM to read, else they read 0.
 * @throws emulation_error when the engine refuses a read or write.
 */
[[nodiscard]] auto read_special(uc_struct* engine, bool stack_limits) -> special_registers;
}
