#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct uc_struct;

namespace popcorn::arch
{
/**
 * @brief Owner index of the arch module's own events, such as a system timer. The machine
 *        dispatches events of this owner to arch::on_event() and never gives a model this index.
 */
inline constexpr std::uint32_t arch_event_owner = std::numeric_limits<std::uint32_t>::max();

/** @brief Kind of guest memory access. */
enum class[[= reflex::derive{reflex::Format}]] memory_op
{
  read,
  write,
  fetch,
};

/** @brief Type of a call argument or return value, which the calling convention places. */
enum class[[= reflex::derive{reflex::Format}]] value_kind
{
  i32,
  i64,
  f32,
  f64,
};

/**
 * @brief Floating-point calling convention of the image, taken from its ELF header.
 *
 * - soft: every floating-point value in integer registers or on the stack. Arm soft and softfp,
 *   RISC-V ilp32 and lp64.
 * - hard_single: f32 in floating-point registers, f64 as soft. RISC-V ilp32f and lp64f.
 * - hard_double: f32 and f64 in floating-point registers. Arm AAPCS-VFP, RISC-V ilp32d and lp64d.
 */
enum class[[= reflex::derive{reflex::Format}]] float_abi
{
  soft,
  hard_single,
  hard_double,
};

/**
 * @brief What a processor fault does: stop the run and report it, or enter the firmware's fault
 *        handler, HardFault, MemManage, BusFault or UsageFault on Arm, the trap vector on RISC-V.
 */
enum class[[= reflex::derive{reflex::Format}]] fault_policy
{
  stop,
  enter_handler,
};

/**
 * @brief Stack pointer a call runs on. Arm: main selects MSP, process selects PSP through
 *        CONTROL.SPSEL in thread mode. An architecture with one stack pointer accepts only
 *        current.
 */
enum class[[= reflex::derive{reflex::Format}]] call_stack
{
  current,
  main,
  process,
};

/** @brief One call argument. */
struct call_arg
{
  /** @brief Type, which decides the register class and alignment. */
  value_kind kind{value_kind::i32};
  /** @brief Value bits, right-aligned, a float as its IEEE 754 bit pattern. */
  std::uint64_t bits{};

  /** @brief Member-wise equality. */
  friend auto operator==(call_arg, call_arg) -> bool = default;
};

/**
 * @brief Guest function call, set up by arch::prepare_call() under the arch's calling
 *        convention. Around the call the machine saves the engine registers listed by
 *        arch::registers() and the bytes of arch::save(), and restores both after it returns or
 *        stops, so the interrupted context is left as it was.
 */
struct call_frame
{
  /** @brief Address of the function, as its symbol gives it. */
  address entry{};
  /** @brief Arguments in declaration order. Those past the argument registers go on the stack. */
  std::vector<call_arg> args{};
  /** @brief Type of the return value, read back by arch::call_result(). */
  value_kind returns{value_kind::i32};
  /** @brief Floating-point calling convention of the image. */
  float_abi abi{float_abi::soft};
  /** @brief Return address the function returns to, where the machine stops the call. */
  address return_sentinel{};
  /** @brief Initial stack pointer, the current one when empty. */
  std::optional<address> stack{};
  /** @brief Stack pointer the call runs on and @p stack initialises. Restored with the rest of
   * the interrupted context after the call. */
  call_stack stack_select{call_stack::current};
  /** @brief Run privileged: Arm privileged thread mode, RISC-V machine mode. Unprivileged is
   * Arm unprivileged thread mode, RISC-V user mode. */
  bool privileged{true};
  /** @brief Take interrupts during the call. False masks delivery until the call returns: no
   * handler runs, pending interrupts stay pending. */
  bool interrupts{false};

  /** @brief Member-wise equality. */
  friend auto operator==(call_frame const&, call_frame const&) -> bool = default;
};

/**
 * @brief What the run loop does after the arch handled a trap: resume at the current pc, which
 *        the arch may have moved to a handler, stop with a reason, or stop on a fault. Built only
 *        through resume(), stop() and fault(), so a fault always carries its decoded detail and
 *        no other stop does.
 */
class trap_result
{
public:
  /** @brief Resume at the current pc. */
  [[nodiscard]] static constexpr auto resume() noexcept -> trap_result
  {
    return trap_result{std::nullopt, std::nullopt};
  }

  /**
   * @brief Stop the slice at once, not on a fault.
   * @param reason Reason the run reports, never stop_reason::fault.
   */
  [[nodiscard]] static constexpr auto stop(stop_reason reason) noexcept -> trap_result
      pre(is_enumerator(reason)) pre(reason != stop_reason::fault)
  {
    return trap_result{reason, std::nullopt};
  }

  /**
   * @brief Stop the slice at once on a fault.
   * @param detail Decoded fault.
   */
  [[nodiscard]] static constexpr auto fault(fault_info const& detail) noexcept -> trap_result
  {
    return trap_result{stop_reason::fault, detail};
  }

  /** @brief Whether the run loop resumes. */
  [[nodiscard]] constexpr auto resumes() const noexcept -> bool
  {
    return not reason_.has_value();
  }

  /** @brief Reason the run stops, empty when it resumes. */
  [[nodiscard]] constexpr auto reason() const noexcept -> std::optional<stop_reason>
  {
    return reason_;
  }

  /** @brief Decoded fault, set exactly when reason() is stop_reason::fault. */
  [[nodiscard]] constexpr auto fault_detail() const noexcept -> std::optional<fault_info> const&
  {
    return fault_;
  }

private:
  constexpr trap_result(
      std::optional<stop_reason> reason,
      std::optional<fault_info>  detail) noexcept
      : reason_{reason}, fault_{detail}
  {}

  std::optional<stop_reason> reason_;
  std::optional<fault_info>  fault_;
};

/** @brief CPU register as the engine exposes it, for register views and serialized snapshots. */
struct cpu_register
{
  /** @brief Name as the architecture manual spells it, lower case, such as `r0` or `mstatus`. */
  std::string_view name;
  /** @brief Engine register id, a `uc_arm_reg` or `uc_riscv_reg` value. */
  int engine_id{};
  /** @brief Width in bits. */
  std::uint8_t bits{};

  /** @brief Member-wise equality. */
  friend auto operator==(cpu_register, cpu_register) -> bool = default;
};

/** @brief Unicorn engine parameters of a core, as the Unicorn constants' integer values. */
struct engine_spec
{
  /** @brief `uc_arch` value passed to `uc_open`. */
  int arch{};
  /** @brief `uc_mode` bit set passed to `uc_open`. */
  int mode{};
  /** @brief CPU model set with `UC_CTL_CPU_MODEL` before any other engine call, empty for the
   * engine default. */
  std::optional<int> cpu_model{};

  /** @brief Member-wise equality. */
  friend auto operator==(engine_spec const&, engine_spec const&) -> bool = default;
};

/**
 * @brief What the machine hands the arch module on attach. The context and everything it
 *        references outlive the arch module, so the module may keep the references.
 */
struct arch_context
{
  /** @brief Engine the module drives, open and with memory mapped. */
  uc_struct* engine;
  /** @brief Description of the MCU: core features, IRQ count, regions. */
  mcu_description const& description;
  /** @brief Virtual clock of the machine, read by system timers. */
  core::clock& clock;
  /** @brief The module's own access to the event queue, owner arch_event_owner. */
  core::event_port& events;
  /** @brief Deferred stop, guest bus access, trace ring. */
  core::machine_services& services;
  /** @brief Whether a fault stops the run, trap_result::fault(), or enters the firmware
   * handler, trap_result::resume() after entry. */
  fault_policy faults{fault_policy::stop};
};

/**
 * @brief Architecture module: interrupt controller, system timer, exception entry and return,
 *        calling convention and CPU state of one core. The run loop calls it at fixed points:
 *        before each slice, from the block hook while an interrupt is pending, and from the
 *        engine's trap hooks. The contract fits armv6m, armv7m, armv8m and RISC-V.
 *
 * Unicorn hooks: the machine installs the interrupt, invalid-instruction and unmapped-access
 * hooks and forwards them to on_interrupt(), on_invalid_insn() and on_unmapped(). The module may
 * add hooks private to its architecture in attach(), such as an idle-instruction hook, and maps
 * its system space as MMIO there. Those hooks never call `uc_emu_stop`: they call
 * `core::machine_services::end_slice`, `request_stop` or `request_reset`, which the next block
 * hook honours. No member function unwinds through Unicorn: the machine catches at the hook
 * boundary. Neither copyable nor movable.
 */
class arch
{
public:
  /** @brief Creates the module, detached. */
  arch() = default;
  /** @brief Not copyable. */
  arch(arch const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(arch const&) -> arch& = delete;
  /** @brief Not movable. */
  arch(arch&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(arch&&) -> arch& = delete;
  /** @brief Destroys the module. */
  virtual ~arch();

  /**
   * @brief Engine the machine opens for this core. Called once, after the factory and before
   *        attach().
   * @return Architecture, mode and CPU model.
   */
  [[nodiscard]] virtual auto engine() const -> engine_spec = 0;

  /**
   * @brief Binds the module to its engine and machine, once, before any other call. Maps the
   *        system space, such as the Arm SCS or the RISC-V CLINT and PLIC, as MMIO.
   * @param ctx Engine, description, clock, event port and services.
   * @throws config_error when the description names a core the module does not implement.
   */
  virtual void attach(arch_context const& ctx) = 0;

  /**
   * @brief Brings the core and the module to their reset state and sets the first instruction.
   * @param reset_vector Architecture reset vector: the Arm vector table, whose first two words
   *        give the stack pointer and entry point, or the RISC-V first instruction.
   */
  virtual void reset(address reset_vector) = 0;

  /**
   * @brief Takes the highest-priority deliverable interrupt or exception, called before every
   *        slice. Wakes an idle core when a pending interrupt is a wake-up event, even when
   *        masked for delivery.
   * @return Resume, or a stop when entry itself failed, such as an Arm lockup.
   */
  virtual auto deliver_pending() -> trap_result = 0;

  /**
   * @brief Whether an interrupt is pending and enabled, masking aside. Read by the block hook
   *        every translated block without a virtual call, so the hook reads no engine register
   *        while nothing is pending.
   */
  [[nodiscard]] auto pending() const noexcept -> bool
  {
    return pending_;
  }

  /**
   * @brief Whether a pending interrupt would be taken at the current instruction boundary under
   *        the current masks and execution priority. Called by the block hook only while
   *        pending() holds, it stops the slice when true.
   */
  [[nodiscard]] virtual auto deliverable_now() const -> bool = 0;

  /**
   * @brief Whether the core executes in thread mode: Arm IPSR 0, whatever other exceptions are
   *        still active.
   */
  [[nodiscard]] virtual auto thread_mode() const -> bool = 0;

  /**
   * @brief Drives an interrupt source from a peripheral IRQ line. Level-sensitive: the source is
   *        pending while active, and an active level latches it pending, so a raise then lower
   *        pends it once.
   * @param source Interrupt source as the description numbers it, `irq_desc::number`.
   * @param active Line level.
   */
  virtual void set_irq_line(std::uint32_t source, bool active) = 0;

  /**
   * @brief Handles an exception the engine reports through its interrupt hook, such as Arm
   *        EXC_RETURN or SVC, or RISC-V `ecall` and `ebreak`.
   * @param intno Engine exception number, architecture-specific.
   * @return Resume, or a stop.
   */
  virtual auto on_interrupt(std::uint32_t intno) -> trap_result = 0;

  /**
   * @brief Whether an engine exception completes the return of a function entered with
   *        @p frame_return as its return_address(), when on_interrupt() would handle it as a
   *        return from an exception handler. On Arm: an exception exit in handler mode through
   *        a valid EXC_RETURN, with @p frame_return a valid EXC_RETURN too, not necessarily the
   *        same, since a handler such as an RTOS context switch may return another one. The
   *        machine tells the frames apart by stack pointer. Called before on_interrupt(), with
   *        the core registers still as the function left them, and only while a return hook
   *        waits.
   * @param intno Engine exception number, as on_interrupt() gets it.
   * @param frame_return Return address a waiting frame got at entry.
   * @return Whether the frame returns here. The default reports no such return.
   */
  [[nodiscard]] virtual auto interrupt_returns(
      [[maybe_unused]] std::uint32_t intno,
      [[maybe_unused]] address       frame_return) const -> bool
  {
    return false;
  }

  /**
   * @brief Handles an instruction the engine cannot execute, pc at the instruction: an
   *        architectural fault entry, or an instruction the module emulates, such as Arm WFE.
   * @return Resume, or a stop. The machine tells the engine to resume when no stop is set.
   */
  virtual auto on_invalid_insn() -> trap_result = 0;

  /**
   * @brief Handles a slice that ended on an unmapped or protected access, pc at the faulting
   *        instruction. Also called for a bus error a peripheral model returned on a CPU access:
   *        that access completed, the call comes at the next block boundary with the pc there, an
   *        imprecise bus fault, and @p addr lies in a described peripheral. Called between
   *        slices, never from the hook.
   * @param addr Address of the access.
   * @param op Kind of the access.
   * @return Resume after fault entry, or a stop.
   */
  virtual auto on_unmapped(address addr, memory_op op) -> trap_result = 0;

  /**
   * @brief Handles an event the module scheduled through its event port.
   * @param kind Event kind the module chose.
   * @param payload Payload the module passed.
   */
  virtual void on_event(std::uint32_t kind, std::uint64_t payload) = 0;

  /**
   * @brief Whether the core sleeps waiting for an interrupt, after Arm WFI or WFE, or RISC-V
   *        `wfi`. The idle hook enters this state and calls `machine_services::end_slice`. A
   *        slice that ends with idle() true and no stop request moves the clock to the next event,
   *        or stops as idle when none is pending.
   */
  [[nodiscard]] virtual auto idle() const -> bool = 0;

  /** @brief Program counter of the next instruction. */
  [[nodiscard]] virtual auto pc() const -> address = 0;

  /**
   * @brief Counts the instructions that start in [@p from, @p to), decoding code memory.
   * @param from Address of the first instruction.
   * @param to Address of an instruction after it.
   * @return The count, empty when the arch cannot count or @p to is not on an instruction
   *         boundary from @p from. The default cannot count.
   */
  [[nodiscard]] virtual auto
      instructions_between([[maybe_unused]] address from, [[maybe_unused]] address to) const
      -> std::optional<std::uint32_t>
  {
    return std::nullopt;
  }

  /**
   * @brief Whether the instruction that ends at @p to ends the translated block holding it, by
   *        its encoding: a branch, a write to the program counter, an exception or a system
   *        instruction the engine ends a block after. A block also ends where the engine's page
   *        or instruction limit falls, which this does not tell.
   * @param from Address of an instruction at or before it, decoding starts there.
   * @param to Address right after it.
   * @return Whether it ends its block, empty when the arch cannot decode, @p to equals @p from or
   *         is not on an instruction boundary from @p from. The default cannot decode.
   */
  [[nodiscard]] virtual auto
      ends_block_before([[maybe_unused]] address from, [[maybe_unused]] address to) const
      -> std::optional<bool>
  {
    return std::nullopt;
  }

  /**
   * @brief Address to start the engine at to resume at pc(), with the instruction set state the
   *        engine needs, such as the Arm Thumb bit.
   */
  [[nodiscard]] virtual auto resume_address() const -> address = 0;

  /** @brief CPU registers of this core, in snapshot order, only those the core implements. */
  [[nodiscard]] virtual auto registers() const -> std::span<cpu_register const> = 0;

  /**
   * @brief Sets up a function call: arguments, stack, return address, privilege, pc at the
   *        entry, and interrupt delivery masked unless `frame.interrupts`. The machine has saved
   *        registers() and save() bytes, and restores both afterwards.
   * @param frame Call to set up.
   * @throws config_error when the core has no such floating-point convention, privilege or stack
   *         pointer.
   */
  virtual void prepare_call(call_frame const& frame) = 0;

  /**
   * @brief Refuses a call frame for what prepare_call() checks before touching any state: the
   *        floating-point convention and the return sentinel. The default refuses nothing.
   * @param frame Call to check.
   * @throws config_error when prepare_call() would refuse the frame for those.
   */
  virtual void check_call([[maybe_unused]] call_frame const& frame) const
  {}

  /**
   * @brief Holds or releases the core, see core::machine::hold_core(): freezes what stops while
   *        a core is held in Debug state, such as core timers. Pending exceptions and the sleep
   *        state stay as they are. The default freezes nothing.
   * @param held True on a hold, false on the release, true again after a reset of a held
   *        machine.
   */
  virtual void hold_core([[maybe_unused]] bool held)
  {}

  /**
   * @brief Reads the return value of a call that reached its sentinel.
   * @param frame Frame given to prepare_call().
   * @return Value bits of type `frame.returns`, a float as its IEEE 754 bit pattern.
   */
  [[nodiscard]] virtual auto call_result(call_frame const& frame) const -> std::uint64_t = 0;

  /**
   * @brief Reads the arguments of a function just entered, the callee side of prepare_call(),
   *        for intercepts. The core is at the first instruction of the function.
   * @param frame Signature in: each `args[i].kind` and `abi`. Values out: each `args[i].bits`.
   */
  virtual void read_args(call_frame& frame) const = 0;

  /**
   * @brief Return address of a function just entered, without instruction set state: Arm LR
   *        with the Thumb bit cleared, RISC-V ra.
   */
  [[nodiscard]] virtual auto return_address() const -> address = 0;

  /**
   * @brief Returns from a function just entered without running its body, the callee side of
   *        call_result(), for intercepts: places the return value, then moves pc to
   *        return_address() with the instruction set state the return address carries.
   * @param value Return value, empty for a void function.
   * @param abi Floating-point calling convention of the image.
   */
  virtual void return_to_caller(std::optional<call_arg> const& value, float_abi abi) = 0;

  /**
   * @brief Appends the module state to a snapshot: interrupt controller, system timer, nesting,
   *        idle state. The engine registers are saved by the machine.
   * @param out Snapshot bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores the module state from bytes save() wrote, pending() included.
   * @param in Bytes of one save() call.
   */
  virtual void load(std::span<std::byte const> in) = 0;

protected:
  /**
   * @brief Updates pending(). A module calls it whenever its set of pending and enabled
   *        interrupts changes, including in reset() and load().
   * @param any Whether any interrupt is pending and enabled.
   */
  void set_pending(bool any) noexcept
  {
    pending_ = any;
  }

private:
  bool pending_ = false;
};

/**
 * @brief Creates the arch module for a core, chosen by the core name and features.
 */
using arch_factory = std::function<std::unique_ptr<arch>(core_desc const& core)>;
}
