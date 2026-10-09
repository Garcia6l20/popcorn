#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/services.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
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
 *        refused accesses. A timer at system_base + 0x10, enable, and + 0x14, period in
 *        instructions, pends line 0 on each expiry through the event port. It keeps no mirror of
 *        the queue: each next expiry is derived from the clock, the enable count and the period,
 *        and events carry a generation that is never saved, so load() re-arms the timer and
 *        orphans the events of the state it replaces. A write of reset_key at system_base + 0x0C
 *        requests a software reset. WFI idles the core until a line is pending or pc leaves the
 *        WFI and its successor. Calls follow the AAPCS in r0-r3, integer values under any float
 *        ABI, floating-point values under the soft one only. Events of other kinds are logged.
 *        A kind 2 event also schedules a kind 1 event due at once. Test-only, tests derive from
 *        it to change one behaviour.
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
  /** @brief Offset of the reset request register in the system page. */
  static constexpr std::uint32_t reset_offset = 0x0c;
  /** @brief Value whose write to the reset request register requests a reset. */
  static constexpr std::uint32_t reset_key = 0x05fa'0004;
  /** @brief Offset of the timer enable register in the system page. */
  static constexpr std::uint32_t timer_control_offset = 0x10;
  /** @brief Offset of the timer period register in the system page. */
  static constexpr std::uint32_t timer_period_offset = 0x14;
  /** @brief Event kind of the timer expiry. */
  static constexpr std::uint32_t timer_event = 0;
  /** @brief Event kind that also schedules a kind 1 event due at once. */
  static constexpr std::uint32_t chain_event = 2;

  /** @brief One dispatched event other than the timer. */
  struct logged_event
  {
    /** @brief Event kind. */
    std::uint32_t kind{};
    /** @brief Event payload. */
    std::uint64_t payload{};
    /** @brief Instruction count at dispatch. */
    icount at{};

    /** @brief Member-wise equality. */
    friend auto operator==(logged_event const&, logged_event const&) -> bool = default;
  };

  /** @brief Timer expiries dispatched so far. */
  std::uint64_t timer_expiries{};

  /** @brief Events dispatched to the module, timer expiries excepted, in dispatch order. */
  std::vector<logged_event> event_log{};
  /** @brief What thread_mode() returns, for a test that drives the run loop's thread check. */
  bool in_thread_mode = true;

  /** @brief Enters the idle state and ends the slice, what the WFI hook does, then lets the WFI
   * halt the engine. The hook also runs for an engine exit, which Unicorn implements as a WFI: a
   * pc in the engine's exit list, or no WFI or WFI.W at pc or just before it, is an exit and the
   * core does not idle. The engine leaves pc on the WFI or past it, depending on the translated
   * block. When on it, pc() reports the successor and deliver_pending() moves pc there, between
   * engine runs. */
  void wait_for_interrupt();

  /**
   * @brief Handles a write to the system page, what its MMIO callback does.
   * @param offset Offset in the page.
   * @param value Value written.
   */
  void write_system(std::uint32_t offset, std::uint32_t value);

  /**
   * @brief Reads the system page, what its MMIO callback does.
   * @param offset Offset in the page.
   * @return The timer registers, the pattern elsewhere.
   */
  [[nodiscard]] auto read_system(std::uint32_t offset) const -> std::uint32_t;

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
  [[nodiscard]] auto thread_mode() const -> bool override
  {
    return in_thread_mode;
  }
  void set_irq_line(std::uint32_t source, bool active) override;
  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override;
  auto on_invalid_insn() -> popcorn::arch::trap_result override;
  auto on_unmapped(address addr, popcorn::arch::memory_op op)
      -> popcorn::arch::trap_result override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto idle() const -> bool override;
  [[nodiscard]] auto pc() const -> address override;
  [[nodiscard]] auto instructions_between(address from, address to) const
      -> std::optional<std::uint32_t> override;
  [[nodiscard]] auto ends_block_before(address from, address to) const
      -> std::optional<bool> override;
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

  /** @brief Clock of the machine the module is attached to, which attach() must have bound. */
  [[nodiscard]] auto attached_clock() const noexcept -> core::clock& pre(context_.has_value());

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
    /** @brief Address of the WFI that entered the idle state. The core sleeps while pc is this
     * address or its successor, any other pc wakes it, as a debug halt on a Cortex-M does. */
    std::optional<address> wfi_at{};
    /** @brief Timer period in instructions, 0 while unset. */
    std::uint32_t timer_period{};
    /** @brief Whether the timer runs. */
    bool timer_enabled{};
    /** @brief Instruction count at which the timer was enabled, expiries fall at whole periods
     * after it. */
    std::uint64_t timer_start{};
  };

protected:
  /** @brief Reads a 32-bit engine register. */
  [[nodiscard]] auto read(int engine_id) const -> std::uint32_t;
  /** @brief Writes a 32-bit engine register. */
  void write(int engine_id, std::uint32_t value) const;
  /** @brief Immediate of the BKPT instruction at pc(). */
  [[nodiscard]] auto bkpt_immediate() const -> std::uint8_t;

private:
  void               enter(std::uint32_t source);
  [[nodiscard]] auto wfi_size(address at) const -> std::uint32_t;
  [[nodiscard]] auto sleeping() const -> bool;
  void               arm_timer();
  [[nodiscard]] auto is_exit(address at) const -> bool;
  void               place_args(popcorn::arch::call_frame const& frame);
  [[nodiscard]] auto arg_registers(popcorn::arch::call_frame const& frame) const
      -> std::vector<std::pair<std::size_t, bool>>;
  void exception_return();

  int                                        cpu_model_;
  std::optional<popcorn::arch::arch_context> context_{};
  state                                      state_{};
  std::optional<popcorn::arch::memory_op>    last_refused_{};
  std::uint64_t                              timer_generation_{};
};

/** @brief Factory creating a test_arch for any core Unicorn models. */
auto test_arch_factory() -> popcorn::arch::arch_factory;
}
