#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/services.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct uc_struct;

namespace popcorn::testing
{
/**
 * @brief Minimal M-profile arch module over the real engine, for core tests before the armv8m
 *        module exists. Maps a 4 KB system page at 0xE000E000 whose reads return a fixed
 *        pattern, resets from a vector table, exposes the core registers. Latches IRQ lines,
 *        enters one handler at a time with a basic frame while PRIMASK is clear, returns through
 *        EXC_RETURN, stops on BKPT past the instruction, faults on invalid instructions and
 *        refused accesses. Test-only, tests derive from it to change one behaviour.
 */
class test_arch : public popcorn::arch::arch
{
public:
  /** @brief Base of the system page attach() maps. */
  static constexpr address system_base = 0xE000'E000;
  /** @brief Size of the system page. */
  static constexpr std::uint32_t system_size = 0x1000;
  /** @brief Word every read of the system page returns, cut to the access size. */
  static constexpr std::uint32_t system_pattern = 0x5ca1'ab1e;

  /**
   * @brief Creates the module for @p core.
   * @throws config_error naming `core.cpu` when Unicorn has no Cortex model of that name.
   */
  explicit test_arch(core_desc const& core);

  [[nodiscard]] auto engine() const -> popcorn::arch::engine_spec override;
  void               attach(popcorn::arch::arch_context const& ctx) override;
  void               reset(address reset_vector) override;
  auto               deliver_pending() -> popcorn::arch::trap_result override;
  [[nodiscard]] auto deliverable_now() const -> bool override;
  void               set_irq_line(std::uint32_t source, bool active) override;
  auto               on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override;
  auto               on_invalid_insn() -> popcorn::arch::trap_result override;
  auto               on_unmapped(address addr, popcorn::arch::memory_op op)
      -> popcorn::arch::trap_result override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto idle() const -> bool override;
  [[nodiscard]] auto pc() const -> address override;
  [[nodiscard]] auto resume_address() const -> address override;
  [[nodiscard]] auto registers() const -> std::span<popcorn::arch::cpu_register const> override;
  void               prepare_call(popcorn::arch::call_frame const& frame) override;
  [[nodiscard]] auto call_result(popcorn::arch::call_frame const& frame) const
      -> std::uint64_t override;
  void               read_args(popcorn::arch::call_frame& frame) const override;
  [[nodiscard]] auto return_address() const -> address override;
  void               return_to_caller(
      std::optional<popcorn::arch::call_arg> const& value,
      popcorn::arch::float_abi                      abi) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  /** @brief Engine the machine attached the module to, null before attach(). */
  [[nodiscard]] auto attached_engine() const noexcept -> uc_struct*;

  /** @brief Services of the machine the module is attached to, which attach() must have bound. */
  [[nodiscard]] auto attached_services() const noexcept
      -> core::machine_services& pre(context_.has_value());

  /** @brief Event port of the module, which attach() must have bound. */
  [[nodiscard]] auto attached_events() const noexcept
      -> core::event_port& pre(context_.has_value());

  /** @brief Kind of the last access on_unmapped() handled, empty before any. */
  [[nodiscard]] auto last_refused() const noexcept -> std::optional<popcorn::arch::memory_op>;

  /** @brief State save() writes and load() reads. */
  struct state
  {
    /** @brief Whether the core sleeps after WFI. */
    bool idle{};
    /** @brief Latched IRQ lines, bit per source. */
    std::uint32_t pending_lines{};
    /** @brief Vector table the last reset read. */
    address vector_table{};
  };

protected:
  /** @brief Reads a 32-bit engine register. */
  [[nodiscard]] auto read(int engine_id) const -> std::uint32_t;
  /** @brief Writes a 32-bit engine register. */
  void write(int engine_id, std::uint32_t value) const;
  /** @brief Immediate of the BKPT instruction at pc(). */
  [[nodiscard]] auto bkpt_immediate() const -> std::uint8_t;

private:
  void enter(std::uint32_t source);
  void exception_return();

  int                                        cpu_model_;
  std::optional<popcorn::arch::arch_context> context_{};
  state                                      state_{};
  std::optional<popcorn::arch::memory_op>    last_refused_{};
};

/** @brief Factory creating a test_arch for any core Unicorn models. */
auto test_arch_factory() -> popcorn::arch::arch_factory;
}
