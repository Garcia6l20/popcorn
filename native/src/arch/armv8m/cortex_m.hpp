#pragma once

#include "exceptions.hpp"
#include "profile.hpp"
#include "state.hpp"
#include "system_space.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct uc_struct;
struct uc_context;

namespace popcorn::arch::armv8m
{
/**
 * @brief Cortex-M arch module over the Unicorn engine: system space register model, special
 *        register handling and the saved state of the core.
 */
class cortex_m : public arch
{
public:
  /**
   * @brief Creates the module for a core.
   * @param core Core of the part description.
   * @throws config_error naming `core.cpu` for a core the module does not implement, or
   *         `core.revision` for a revision not spelled `rNpM`.
   */
  explicit cortex_m(core_desc const& core);
  /** @brief Frees the saved engine context. */
  ~cortex_m() override;

  [[nodiscard]] auto engine() const -> engine_spec override;
  void               attach(arch_context const& ctx) override;
  void               reset(address reset_vector) override;
  auto               deliver_pending() -> trap_result override;
  [[nodiscard]] auto deliverable_now() const -> bool override;
  [[nodiscard]] auto would_preempt(std::uint32_t number) const -> bool override;
  [[nodiscard]] auto thread_mode() const -> bool override;
  void               set_irq_line(std::uint32_t source, bool active) override;
  void               set_nmi_line(bool active) override;
  auto               on_interrupt(std::uint32_t intno) -> trap_result override;
  [[nodiscard]] auto interrupt_returns(std::uint32_t intno, address frame_return) const
      -> bool override;
  auto               on_invalid_insn() -> trap_result override;
  auto               on_unmapped(address addr, memory_op op) -> trap_result override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto idle() const -> bool override;
  void               hold_core(bool held) override;
  [[nodiscard]] auto pc() const -> address override;
  [[nodiscard]] auto instructions_between(address from, address to) const
      -> std::optional<std::uint32_t> override;
  [[nodiscard]] auto ends_block_before(address from, address to) const
      -> std::optional<bool> override;
  [[nodiscard]] auto resume_address() const -> address override;
  [[nodiscard]] auto host_register_value(cpu_register const& reg, std::uint64_t value) const
      -> std::uint64_t override;
  [[nodiscard]] auto data_attrs(address at) const -> bus_attrs override;
  [[nodiscard]] auto registers() const -> std::span<cpu_register const> override;
  void               prepare_call(call_frame const& frame) override;
  void               check_call(call_frame const& frame) const override;
  [[nodiscard]] auto call_result(call_frame const& frame) const -> std::uint64_t override;
  void               read_args(call_frame& frame) const override;
  [[nodiscard]] auto return_address() const -> address override;
  void               return_to_caller(std::optional<call_arg> const& value, float_abi abi) override;
  [[nodiscard]] auto return_interrupt() const -> std::optional<std::uint32_t> override;
  void               save(std::vector<std::byte>& out) const override;
  void               load(std::span<std::byte const> in) override;

  /** @brief Bus attributes of the last access to the system space through the engine, by the
   * CPU or by the host. */
  [[nodiscard]] auto access_attrs() const noexcept -> uc_mem_attrs;

  /** @brief CPUID of the core. */
  [[nodiscard]] auto cpuid() const noexcept -> std::uint32_t;

  /** @brief Number of exceptions, 16 system ones plus the IRQs. */
  [[nodiscard]] auto exception_count() const noexcept -> std::uint32_t;

  /** @brief Whether exception @p number is pending. */
  [[nodiscard]] auto is_pending(std::uint32_t number) const noexcept -> bool;

  /** @brief Whether exception @p number is active. */
  [[nodiscard]] auto is_active(std::uint32_t number) const noexcept -> bool;

  /** @brief Whether exception @p number is enabled: IRQs by ISER, system ones always. */
  [[nodiscard]] auto is_enabled(std::uint32_t number) const noexcept -> bool;

  /** @brief Sets or clears the pending bit of exception @p number, then pending(). */
  void set_exception_pending(std::uint32_t number, bool value) pre(number < exception_count());

  /** @brief Sets or clears the enabled bit of IRQ exception @p number, then pending(). */
  void set_exception_enabled(std::uint32_t number, bool value)
      pre(number >= 16 and number < exception_count());

  /** @brief Keeps the first failure of an arch callback and ends the slice. */
  void keep_failure(std::exception_ptr failure) noexcept;

private:
  friend class system_space;

  [[nodiscard]] auto halfwords(address from, address to) const
      -> std::optional<std::span<std::uint16_t const>>;

  /** @brief SAU and IDAU attribution of an address, SecurityCheck() without the exemptions. */
  struct attribution
  {
    /** @brief Non-secure. */
    bool ns{};
    /** @brief Secure and Non-secure callable. */
    bool nsc{};
    /** @brief Exempt by the IDAU: an access there takes the security of its requester. */
    bool exempt{};
  };

  struct window_user
  {
    cortex_m* self{};
    window    which{};
  };

  struct mpu_bank
  {
    std::uint32_t&              ctrl;
    std::uint32_t&              rnr;
    std::uint32_t&              mair0;
    std::uint32_t&              mair1;
    std::vector<std::uint32_t>& rbar;
    std::vector<std::uint32_t>& rlar;
  };

  struct context_free
  {
    void operator()(uc_context* saved) const noexcept;
  };

  static auto mmio_read(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      uc_mem_attrs  attrs,
      void*         user) noexcept -> std::uint64_t;
  static void mmio_write(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      std::uint64_t value,
      uc_mem_attrs  attrs,
      void*         user) noexcept;

  static void idau_lookup(
      void*         user,
      std::uint32_t address,
      int*          region,
      bool*         exempt,
      bool*         ns,
      bool*         nsc) noexcept;

  [[nodiscard]] auto trustzone() const noexcept -> bool;
  [[nodiscard]] auto view_of(window which) const noexcept -> std::optional<security>;
  [[nodiscard]] auto mpu_of(security view) noexcept -> mpu_bank;
  [[nodiscard]] auto mpu_regions_of(security view) const noexcept -> std::uint8_t;
  [[nodiscard]] auto nonsecure_cpacr_mask() const noexcept -> std::uint32_t;
  void               attach_security();
  void               apply_security(bool restored);
  void               mirror_sau();
  void               mirror_cpacr() const;
  void               mirror_nsacr() const;
  void               mirror_fpccr(bool s_bit) const;
  [[nodiscard]] auto engine_fpccr_s() const -> bool;
  [[nodiscard]] auto read_fpccr(security view) const -> std::uint32_t;
  void               write_fpccr(std::uint32_t value, std::uint32_t mask, security view);
  void               sau_changed();
  void               set_nonsecure_pending(std::uint32_t number, bool value) noexcept;
  [[nodiscard]] auto priority_config() const noexcept -> security_config;
  [[nodiscard]] auto current_security() const -> security;
  void               pend(std::uint32_t number, security state);

  [[nodiscard]] auto timer_now() const -> std::uint64_t;
  [[nodiscard]] auto systick_of(security bank) noexcept -> systick_state&;
  [[nodiscard]] auto systick_ticks(systick_state const& s) const -> std::uint64_t;
  auto               read_systick(std::uint32_t offset, security bank) -> std::uint32_t;
  void write_systick(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, security bank);
  void arm_systick(security bank);
  void arm_systicks();
  [[nodiscard]] auto cyccnt_counting() const noexcept -> bool;
  [[nodiscard]] auto read_cyccnt() const -> std::uint32_t;
  void               rebase_cyccnt(std::uint32_t value);
  [[nodiscard]] auto stack_limits() const noexcept -> bool;
  [[nodiscard]] auto v7() const noexcept -> bool;
  [[nodiscard]] auto current_priority(bool ignore_primask = false) const -> int;
  [[nodiscard]] auto banked_priority(bool ignore_primask) const -> int;
  [[nodiscard]] auto one_bank_priority(bool ignore_primask) const -> int;
  auto               deliver_banked() -> trap_result;
  [[nodiscard]] auto banked_deliverable() const -> bool;
  static auto        wfi_hook(uc_struct* engine, void* user) noexcept -> int;
  void               wait_for_interrupt();
  [[nodiscard]] auto raw_pc() const -> address;
  [[nodiscard]] auto in_exit_list(address at) const -> bool;
  [[nodiscard]] auto wfi_size(address at) const -> std::uint32_t;
  [[nodiscard]] auto sleeping() const -> bool;
  [[nodiscard]] auto wake_event() const -> bool;
  void               wake_if_due();
  void               note_pending(std::uint32_t number) noexcept;
  void               drive_line(std::uint32_t number, bool active);
  auto               hint() -> bool;
  [[nodiscard]] auto halfword(address at) const -> std::uint32_t;
  [[nodiscard]] auto privileged() const -> bool;
  [[nodiscard]] auto preempts(std::uint32_t number) const -> bool;
  [[nodiscard]] auto preempts(std::uint32_t number, security state) const -> bool;
  auto               raise(std::uint32_t number, address at) -> trap_result;
  auto               raise_fault(fault_cause const& cause) -> trap_result;
  [[nodiscard]] auto nonsecure_fp_denied() const -> bool;
  auto create_exception(fault_cause const& cause) -> std::expected<pending_exception, trap_result>;
  auto derived_entry(
      pending_exception  original,
      fault_cause const& cause,
      std::uint32_t      exc_return,
      int                sp_id,
      std::uint32_t      sp) -> trap_result;
  auto return_fault(
      std::uint32_t      exc_return,
      std::uint32_t      number,
      security           exception,
      security           to,
      fault_cause const& cause) -> trap_result;
  auto enter_with_frame(pending_exception taken, std::uint32_t exc_return, bool tail_chain)
      -> trap_result;
  auto                           engine_fault(std::uint32_t intno) -> trap_result;
  template <bool extension> auto enter_as(std::uint32_t number, security state) -> trap_result;
  auto                           exception_return() -> trap_result;
  auto                           banked_return() -> trap_result;
  void                           repend_level(std::uint32_t number) noexcept;
  void                           trace_exit(core::machine_services& services, std::uint32_t number);
  auto                           returned(
      std::uint32_t number,
      std::uint32_t stacked_ipsr,
      address       resume_at,
      std::uint32_t scr) -> trap_result;
  auto               function_return() -> trap_result;
  auto               secure_entry(address at) -> trap_result;
  void               clear_caller_registers(uc_struct* engine, bool callee) const;
  [[nodiscard]] auto attribution_of(address at) const noexcept -> attribution;
  void               check_call_state(call_frame const& frame) const;
  [[nodiscard]] auto nonsecure_address(address at) const noexcept -> bool;
  [[nodiscard]] auto secure_fp_context() const noexcept -> bool;
  [[nodiscard]] auto first_secure_word(address from, address to) const noexcept
      -> std::optional<address>;
  [[nodiscard]] auto valid_return(std::uint32_t exc_return) const noexcept -> bool;

  [[nodiscard]] auto engine_handle() const -> uc_struct*;
  [[nodiscard]] auto read(int engine_id) const -> std::uint32_t;
  void               write(int engine_id, std::uint32_t value) const;
  void               refresh_pending() noexcept;
  void               rethrow_failure() const;
  [[nodiscard]] auto reset_state(address vector_table) const -> state;

  profile                                   profile_;
  core_desc                                 core_;
  std::uint32_t                             cpuid_;
  std::optional<arch_context>               context_{};
  state                                     state_{};
  std::unique_ptr<uc_context, context_free> pristine_{};
  std::array<window_user, 5>                windows_{};
  uc_mem_attrs                              access_{};
  mutable std::exception_ptr                failure_{};
  std::uint64_t                             generation_{};
  mutable std::vector<std::uint64_t>        exits_{};
  mutable std::vector<std::uint16_t>        halfwords_{};
  bool                                      in_call_{};
  bool                                      trustzone_{};
  system_space                              space_{*this};
};
}
