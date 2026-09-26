#pragma once

#include "state.hpp"

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

/**
 * @brief Pending and enabled exception taken first: lowest priority value, subpriority
 *        included, then lowest number. System exceptions 2-15 are always enabled, IRQs by ISER.
 */
[[nodiscard]] auto best_pending(exception_state const& exceptions) noexcept
    -> std::optional<std::uint32_t>;

/**
 * @brief Writes the special registers in the one order the engine accepts from any privilege:
 *        Handler mode, nPRIV cleared, privileged Thread, target SPSEL and FPCA, both stack
 *        pointers and the masks, target IPSR, target CONTROL with nPRIV last, then GE.
 * @param engine Engine to write.
 * @param target Values to apply.
 * @throws emulation_error when the engine refuses a write.
 */
void set_context(uc_struct* engine, special_registers const& target);

/**
 * @brief Reads the special registers from the engine, also in unprivileged Thread mode, where
 *        the engine reads the stack pointers and masks as zero: the read goes through Handler
 *        mode for its duration.
 * @throws emulation_error when the engine refuses a read or write.
 */
[[nodiscard]] auto read_special(uc_struct* engine) -> special_registers;
}
