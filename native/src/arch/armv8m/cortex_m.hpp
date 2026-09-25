#pragma once

#include "profile.hpp"
#include "state.hpp"
#include "system_space.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>

#include <array>
#include <cstdint>
#include <exception>
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
   * @param core Core of the MCU description.
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
  void               set_irq_line(std::uint32_t source, bool active) override;
  auto               on_interrupt(std::uint32_t intno) -> trap_result override;
  auto               on_invalid_insn() -> trap_result override;
  auto               on_unmapped(address addr, memory_op op) -> trap_result override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto idle() const -> bool override;
  [[nodiscard]] auto pc() const -> address override;
  [[nodiscard]] auto resume_address() const -> address override;
  [[nodiscard]] auto registers() const -> std::span<cpu_register const> override;
  void               prepare_call(call_frame const& frame) override;
  [[nodiscard]] auto call_result(call_frame const& frame) const -> std::uint64_t override;
  void               read_args(call_frame& frame) const override;
  [[nodiscard]] auto return_address() const -> address override;
  void               return_to_caller(std::optional<call_arg> const& value, float_abi abi) override;
  void               save(std::vector<std::byte>& out) const override;
  void               load(std::span<std::byte const> in) override;

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

  struct window_user
  {
    cortex_m* self{};
    window    which{};
  };

  struct context_free
  {
    void operator()(uc_context* saved) const noexcept;
  };

  static auto mmio_read(uc_struct* engine, std::uint64_t offset, unsigned size, void* user) noexcept
      -> std::uint64_t;
  static void mmio_write(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      std::uint64_t value,
      void*         user) noexcept;

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
  std::array<window_user, 4>                windows_{};
  mutable std::exception_ptr                failure_{};
  system_space                              space_{*this};
};
}
