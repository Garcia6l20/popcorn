#pragma once

#include <popcorn/core/types.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace popcorn::arch::armv8m
{
/** @brief Special registers the engine drops or mixes up in a register-list write. */
struct special_registers
{
  /** @brief CONTROL. */
  std::uint32_t control{};
  /** @brief PRIMASK. */
  std::uint32_t primask{};
  /** @brief BASEPRI. */
  std::uint32_t basepri{};
  /** @brief FAULTMASK. */
  std::uint32_t faultmask{};
  /** @brief Main stack pointer. */
  std::uint32_t msp{};
  /** @brief Process stack pointer. */
  std::uint32_t psp{};
  /** @brief Exception number of the current mode, 0 in Thread mode. */
  std::uint32_t ipsr{};
  /** @brief APSR.GE, in xPSR bits 19:16. */
  std::uint32_t ge{};

  /** @brief Member-wise equality. */
  friend auto operator==(special_registers const&, special_registers const&) -> bool = default;
};

/** @brief Per exception number state of the NVIC and the system exceptions. */
struct exception_state
{
  /** @brief Priority byte per exception number, 16 + IRQ count. */
  std::vector<std::uint8_t> priority{};
  /** @brief Enabled bit per exception number. System exceptions are always enabled. */
  std::vector<std::uint32_t> enabled{};
  /** @brief Pending bit per exception number. */
  std::vector<std::uint32_t> pending{};
  /** @brief Active bit per exception number. */
  std::vector<std::uint32_t> active{};
  /** @brief IRQ line level per exception number. */
  std::vector<std::uint32_t> level{};

  /** @brief Member-wise equality. */
  friend auto operator==(exception_state const&, exception_state const&) -> bool = default;
};

/** @brief Stored registers of the system space. */
struct scs_state
{
  /** @brief VTOR. */
  address vtor{};
  /** @brief AIRCR.PRIGROUP. */
  std::uint32_t prigroup{};
  /** @brief SCR. */
  std::uint32_t scr{};
  /** @brief CCR. */
  std::uint32_t ccr{};
  /** @brief SHCSR fault enables, bits 16-18. */
  std::uint32_t shcsr_enables{};
  /** @brief CFSR. */
  std::uint32_t cfsr{};
  /** @brief HFSR. */
  std::uint32_t hfsr{};
  /** @brief DFSR. */
  std::uint32_t dfsr{};
  /** @brief MMFAR. */
  std::uint32_t mmfar{};
  /** @brief BFAR. */
  std::uint32_t bfar{};
  /** @brief AFSR. */
  std::uint32_t afsr{};
  /** @brief ACTLR. */
  std::uint32_t actlr{};
  /** @brief CPACR. */
  std::uint32_t cpacr{};
  /** @brief FPCCR. */
  std::uint32_t fpccr{};
  /** @brief FPCAR. */
  std::uint32_t fpcar{};
  /** @brief FPDSCR. */
  std::uint32_t fpdscr{};
  /** @brief DCRDR. */
  std::uint32_t dcrdr{};
  /** @brief DEMCR. */
  std::uint32_t demcr{};
  /** @brief DWT_CTRL. */
  std::uint32_t dwt_ctrl{};
  /** @brief DWT_COMPn and DWT_FUNCTIONn, alternating. */
  std::array<std::uint32_t, 8> dwt_comparators{};
  /** @brief MPU_CTRL. */
  std::uint32_t mpu_ctrl{};
  /** @brief MPU_RNR. */
  std::uint32_t mpu_rnr{};
  /** @brief MPU_MAIR0. */
  std::uint32_t mair0{};
  /** @brief MPU_MAIR1. */
  std::uint32_t mair1{};
  /** @brief MPU_RBAR per region. */
  std::vector<std::uint32_t> mpu_rbar{};
  /** @brief MPU_RLAR per region. */
  std::vector<std::uint32_t> mpu_rlar{};
  /** @brief ITM_TER. */
  std::uint32_t itm_ter{};
  /** @brief ITM_TPR. */
  std::uint32_t itm_tpr{};
  /** @brief ITM_TCR. */
  std::uint32_t itm_tcr{};

  /** @brief Member-wise equality. */
  friend auto operator==(scs_state const&, scs_state const&) -> bool = default;
};

/** @brief SysTick, counting from an anchor on the instruction count. */
struct systick_state
{
  /** @brief SYST_CSR ENABLE, TICKINT and CLKSOURCE, bits 0-2. */
  std::uint32_t csr{};
  /** @brief SYST_RVR, 24 bits. */
  std::uint32_t rvr{};
  /** @brief Instruction count of the anchor. */
  std::uint64_t anchor{};
  /** @brief Counter value at the anchor, and the frozen value while disabled. */
  std::uint32_t anchor_value{};
  /** @brief Tick count since the anchor of the last COUNTFLAG clear. */
  std::uint64_t flag_since{};
  /** @brief COUNTFLAG carried over from before the anchor. */
  bool flag{};
  /** @brief Crossings of the current anchor already dispatched, 0 for none. */
  std::uint64_t fired{};
  /** @brief Live event generation when saved, a floor for the generation after a restore. */
  std::uint64_t generation_floor{};

  /** @brief Member-wise equality. */
  friend auto operator==(systick_state const&, systick_state const&) -> bool = default;
};

/** @brief DWT CYCCNT, counting from an anchor on the instruction count. */
struct cyccnt_state
{
  /** @brief Counter value at the anchor. */
  std::uint32_t base{};
  /** @brief Instruction count of the anchor. */
  std::uint64_t anchor{};

  /** @brief Member-wise equality. */
  friend auto operator==(cyccnt_state const&, cyccnt_state const&) -> bool = default;
};

/** @brief Why the core sleeps. */
enum class sleep_kind : std::uint8_t
{
  wfi,
  wfe,
  sleep_on_exit,
};

/** @brief A sleeping core: the instruction it sleeps on. */
struct sleep_state
{
  /** @brief Address of the WFI or WFE, the return address for sleep-on-exit. */
  address at{};
  /** @brief Size of the instruction, 0 for sleep-on-exit. */
  std::uint32_t size{};
  /** @brief Why the core sleeps. */
  sleep_kind kind{};

  /** @brief Member-wise equality. */
  friend auto operator==(sleep_state const&, sleep_state const&) -> bool = default;
};

/** @brief Saved state of the module. */
struct state
{
  /** @brief NVIC and system exceptions. */
  exception_state exceptions{};
  /** @brief System space registers. */
  scs_state scs{};
  /** @brief Special registers, read from the engine by save() and re-applied by load(). */
  special_registers special{};
  /** @brief SysTick. */
  systick_state systick{};
  /** @brief DWT CYCCNT. */
  cyccnt_state cyccnt{};
  /** @brief Set while the core sleeps. */
  std::optional<sleep_state> sleep{};
  /** @brief Event register of WFE. */
  bool event_register{};
  /** @brief Set while a call masks asynchronous exceptions. */
  bool call_masked{};

  /** @brief Member-wise equality. */
  friend auto operator==(state const&, state const&) -> bool = default;
};
}
