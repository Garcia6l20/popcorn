#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/clock.hpp>
#include <popcorn/core/coverage.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/memory_view.hpp>
#include <popcorn/core/register_view.hpp>
#include <popcorn/core/reset_counter.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include <reflex/enum.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct uc_struct;

namespace popcorn::core
{
/** @brief What a reset request from firmware, such as Arm AIRCR.SYSRESETREQ, does to a run. */
enum class[[= reflex::derive{reflex::Format}]] reset_request_policy
{
  stop,
  reset_and_continue,
};

/** @brief Construction options of a machine beyond its description. */
struct machine_options
{
  /** @brief Core clock at power-on in Hz, 0 for the description's `core.reset_clock_hz`. */
  std::uint64_t core_hz{};
  /** @brief Core cycles per instruction. */
  double cpi{1.0};
  /** @brief Records kept by the always-on trace ring. */
  std::size_t trace_capacity{4096};
  /** @brief Model factory per peripheral name, set explicitly. Overrides the
   * `periph::register_model` registry for that peripheral. */
  std::map<std::string, periph::model_factory, std::less<>> models{};
  /** @brief Whether a processor fault stops the run, the default, or enters the firmware's
   * handler. Passed to the arch module in `arch::arch_context::faults`. */
  arch::fault_policy faults{arch::fault_policy::stop};
  /** @brief Whether a firmware reset request stops the run with stop_reason::reset_request, the
   * default, or resets the machine as reset_kind::software and continues. A reset request during
   * a call begun by begin_call() always stops with stop_reason::reset_request, whatever the
   * policy: a reset would lose the call frame and its return sentinel. end_call() then restores
   * the caller's context. */
  reset_request_policy on_reset_request{reset_request_policy::stop};
  /** @brief Iterations of a loop that makes no progress, the same pc window and the same machine
   * state, after which the run stops with stop_reason::spin, naming the loop and the polled
   * register when it is MMIO. 0 turns spin detection off. A loop waiting on RAM that an
   * interrupt handler updates, such as a tick counter, repeats identically between two
   * interrupts, so the threshold should stay well above the iterations between interrupts,
   * 1e5 to 1e6: a stuck loop repeats forever, so a large threshold only delays the stop.
   * Independently of spin_threshold, the run also stops with stop_reason::spin when 65536 events
   * are dispatched at one instruction count, detail 'events repeat without time passing: <owner>
   * kind <n>'. */
  std::uint32_t spin_threshold{};
  /** @brief Whether every CPU or DMA access to a register of an unmodelled peripheral, or to a
   * raz_wi range, fails the run with model_error. The arch module's and the models' own debugger
   * accesses are neither refused nor logged. A memory_view access to either throws model_error
   * whatever this flag says. The log keeps one record per register and per range. */
  bool strict_peripherals{};
  /** @brief Address of the vector table the arch module's reset reads. Empty: the start of the
   * region containing address 0, otherwise of the first region in description order with execute
   * access. */
  std::optional<address> vector_table{};
  /** @brief Whether the part boots with its security extension enabled, TZEN = 1 on an STM32.
   * Passed to the arch module in `arch::arch_context::trustzone`. The machine refuses it with
   * config_error keyed `trustzone` on a core without `core_desc::trustzone`. */
  bool trustzone{};
  /** @brief IDAU of the part, used while trustzone is set. Passed to the arch module in
   * `arch::arch_context::idau`. The machine refuses with config_error keyed `idau` ranges
   * without trustzone, and keyed `idau[i]` a range whose start is not below its end or below
   * the end of the range before it. */
  std::vector<idau_region> idau{};
  /** @brief RAM region mapped read-write and not executable outside the description, such as a
   * call argument arena. Part of snapshot memory, so a restore resets it. Named by its `name`,
   * `scratch` when empty. Must stay clear of the description's regions and peripherals, which
   * the machine refuses with config_error, and of the arch module's system space, which the arch
   * module's attach refuses. A description alias never targets it. */
  std::optional<address_range> scratch{};
  /** @brief Ranges outside the description that read as zero and ignore writes, such as debug
   * blocks a part has and no model serves. No storage, nothing in snapshots. The first access to
   * each range is logged as log_kind::model_warning, once per machine life. Under
   * strict_peripherals every CPU or DMA access to a range raises model_error naming the range,
   * with an empty register, and an arch or model debugger access reads zero unlogged. A
   * memory_view access raises model_error, strict or not. Each range must be
   * non-empty, start and size multiples of the engine page, and stay clear of the description's
   * regions and peripherals, of scratch and of the other ranges, which the machine refuses with
   * config_error keyed `raz_wi[i]`. The engine refuses a range over the arch module's system
   * space with emulation_error. */
  std::vector<address_range> raz_wi{};
  /** @brief Digest of the configuration the machine was built from, such as a project digest,
   * stored in every snapshot as `snapshot_data::config_digest`. Opaque to the core. */
  std::string config_digest{};
};

/**
 * @brief A return a run stops at: the first block at ret entered with the stack pointer equal
 *        to sp, or an exception return through an EXC_RETURN ret with the stack pointer equal to
 *        sp before the core unstacks. Compared at each candidate, no state kept, so slicing and
 *        a switch to another stack do not change where it stops.
 */
struct frame_return
{
  /** @brief Return address, Thumb bit clear, or an EXC_RETURN. */
  address ret{};
  /** @brief Stack pointer at the function's entry, the same at its return under AAPCS. */
  std::uint64_t sp{};

  friend auto operator==(frame_return const&, frame_return const&) -> bool = default;
};

/**
 * @brief Bounds of one native slice. The slice ends at the first block boundary at or past any
 *        bound, except exact_instructions. Only until, budget_left, exact_instructions,
 *        until_time and stop_when stop the run, the other bounds only end the slice so the
 *        caller can yield.
 */
struct slice_request
{
  /** @brief Instructions after which the slice ends, the caller then yields and calls again. */
  icount max_instructions{1'000'000};
  /** @brief Wall time after which the slice ends, read every 256 blocks. */
  std::chrono::nanoseconds wall_budget{std::chrono::milliseconds{1}};
  /** @brief Stop with stop_reason::address before executing this address. A slice that starts at
   * this address stops at once, 0 instructions executed, unless leave_until is set. */
  std::optional<address> until{};
  /** @brief Further addresses that stop the slice as until does, leave_until included. */
  std::vector<address> until_also{};
  /** @brief Addresses of until and until_also that stop a slice starting at them although
   * leave_until is set: the run arrives there, it does not leave. */
  std::vector<address> arrive_at{};
  /** @brief Instructions left in the run's budget, stop with stop_reason::budget at the first
   * block boundary at or past them. The caller subtracts `slice_result::executed` between
   * slices. */
  std::optional<icount> budget_left{};
  /** @brief Stop with stop_reason::budget after exactly this many instructions, for stepping.
   * Whole blocks run up to the last block boundary before the count, then the remaining
   * instructions are single-stepped, which costs a per-instruction hook for those only. */
  std::optional<icount> exact_instructions{};
  /** @brief Stop with stop_reason::time on reaching this virtual time since reset, also while
   * the core sleeps with no event pending: the clock jumps there. */
  std::optional<vtime> until_time{};
  /** @brief Once until_time is reached, dispatch only the events due before it, whatever stop
   * ends the slice. The others stay pending and the next slice dispatches them first, in key
   * order with any lane event posted in between. For a bound a scheduler of several machines
   * sets, never for a stamp bound. */
  bool hold_from_until{};
  /** @brief Stop predicate, evaluated after every dispatched event and at the slice end, never
   * per instruction. Stops with stop_reason::user when it returns true. */
  std::function<bool()> stop_when{};
  /** @brief Stop with stop_reason::returned at this return. A slice that starts at it stops at
   * once, 0 instructions executed. */
  std::optional<frame_return> until_return{};
  /** @brief A slice that starts at until, or at an address of until_also, executes at least one
   * instruction before that address may stop it, so a run resuming from its own until address
   * goes on to the next hit, unless the address is in arrive_at. The caller sets it until the run
   * has executed an instruction. */
  bool leave_until{};
  /** @brief A stop_reason::time stop is the caller's bookkeeping, such as a host landing a
   * stamped command at until_time: the stop is returned with its pc unlocated and no trace stop
   * record. Other stops are reported as usual. */
  bool internal_stop{};
  /** @brief End the slice, without a stop, at the first loop turn where thread_ready() holds,
   * after any deliverable exception was entered, so a tail-chained handler runs first. A slice
   * that starts ready ends with no instruction executed. An exception return to thread mode ends
   * the engine run, so the check sees it. */
  bool until_thread{};
  /** @brief A core asleep with nothing deliverable and no event due before until_time stops where
   * it is with stop_reason::idle, instead of the clock jumping to until_time. With internal_stop
   * that stop is the caller's bookkeeping too: pc unlocated, no trace stop record. For a scheduler
   * of several machines that keeps a quiescent member where it fell asleep. */
  bool stop_quiescent{};
  /** @brief Evaluate stop_when only after a dispatched event, never at the slice end, so a slice
   * end set by max_instructions, the wall budget or a bound never decides the user stop. */
  bool stop_when_events_only{};
};

/** @brief Outcome of one native slice. */
struct slice_result
{
  /** @brief Stop record when the run must stop, empty when the slice only ended a bound that
   * lets the caller continue. */
  std::optional<stop> stopped{};
  /** @brief Instructions executed in the slice, read from the block hook, never assumed from the
   * bounds: a slice overshoots by up to one block. A block the engine leaves before its end, for
   * any reason, an intercept that returns or stops, a code hook stop, an exception or a fault,
   * counts its full length: deterministic, not exact. Instructions single-stepped for
   * exact_instructions count one by one. */
  icount executed{};
  /** @brief Whether stopped is the stop_reason::user stop of the request's stop_when, not one
   * a model or handler requested. A requested stop wins over stop_when at the same boundary. */
  bool asked = false;
  /** @brief Whether the slice ended without a stop for slice_request::until_thread. */
  bool thread_ready = false;
};

/** @brief Outcome of machine::call(). */
struct call_result
{
  /** @brief Return value bits of the frame's `returns` kind, 0 unless the call returned. */
  std::uint64_t value{};
  /** @brief Why the call ended, stop_reason::returned when it reached its sentinel. */
  stop stopped{};
};

/**
 * @brief One emulated MCU: engine, memory map, clock, event queue, arch module, peripherals,
 *        intercepts, watches, coverage and trace. Built from a description, never from ad-hoc
 *        arguments. No globals, not thread-safe, one thread drives it.
 *
 * Ownership. The machine owns, and outlives, everything it hands out references to:
 *
 * - the Unicorn engine and one host buffer per RAM and flash region, mapped with
 *   `uc_mem_map_ptr`,
 * - the clock, its clock_driver, which it never gives away, and the scheduler,
 * - the arch module from the factory, with an event_port of owner `arch::arch_event_owner`,
 * - per peripheral of the description, in description order: a `periph::register_file`, the
 *   `periph::irq_line` and `periph::dma_request` arrays, an event_port whose owner is the
 *   peripheral index, a `periph::model_context` referencing them, the clock and the machine's
 *   services, and the model the factory returned for that context,
 * - the trace ring, the hook table behind every hook_handle, coverage counters, the loaded
 *   images.
 *
 * Services. The machine is the machine_services implementation of its arch module and models:
 * it derives from it privately, so only the contexts it builds reach it. Stop, reset and slice
 * end requests set a native flag the next block hook reads, bus accesses go through memory, MMIO
 * dispatch and watches, trace appends to the ring stamped with the current instruction count.
 *
 * Construction order: the arch module from the factory, the engine it asks for with
 * `arch::engine()`, memory map, `arch_context{engine, description, clock, arch events, *this,
 * options.faults, options.trustzone, options.idau}` then `arch::attach`, then one model_context,
 * with options.strict_peripherals and options.trustzone, and model per peripheral, the bus
 * firewall a model installs included, then the engine hooks. Events of owner
 * `arch::arch_event_owner` go to `arch::on_event`, other owners to the model of that peripheral
 * index.
 *
 * Running. Only run_slice(), call() and the slices of a call begun by begin_call() execute guest
 * code. While a slice runs, running() is true and a second run_slice() throws machine_busy. The
 * mem() and regs() views and coverage() stay usable from handlers, which run while the core is
 * paused at an instruction boundary. Every other state change, load(), reset(), calls, snapshot(),
 * restore(), adding hooks, hook_handle::reset() and changing coverage, throws machine_running for
 * the whole slice, handlers included. For hooks this deliberately departs from the Python API
 * contract, which lets handlers edit a paused machine: adding or removing a hook flushes translated
 * code of the running slice. Between the slices of a call or of a run, running() is false: the
 * Python facade keeps its own guard for a run in progress. interrupt() is the one member callable
 * from another thread.
 *
 * Neither copyable nor movable: contexts and views hold references to it.
 */
class POPCORN_API machine final : private machine_services
{
public:
  /**
   * @brief Builds a machine, powered off: nothing loaded, no reset done. The call arena, scratch
   *        memory for call arguments, and the trampoline page of linked objects are regions the
   *        caller adds to @p description before construction, like any other region.
   * @param description Description of the MCU, copied, description() returns the copy.
   * @param factory Creates the arch module for `description.core`.
   * @param options Clock, trace capacity, model wiring and run policies.
   * @throws config_error when neither `options.core_hz` nor `description.core.reset_clock_hz`
   *         gives a clock, when the arch module refuses the core or a model factory refuses its
   *         peripheral.
   * @throws config_error when options.vector_table is empty and the description has no region
   *         with execute access.
   * @throws config_error when options.trustzone or options.idau is refused, as their
   *         documentation states, before the engine opens.
   * @throws emulation_error when the engine cannot be opened, or is not the pinned dev build.
   */
  machine(part_description description, arch::arch_factory factory, machine_options options)
      pre(static_cast<bool>(factory)) pre(options.cpi > 0.0) pre(options.trace_capacity > 0)
          pre(is_enumerator(options.faults)) pre(is_enumerator(options.on_reset_request));

  /** @brief Not copyable. */
  machine(machine const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(machine const&) -> machine& = delete;
  /** @brief Not movable. */
  machine(machine&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(machine&&) -> machine& = delete;
  /** @brief Destroys the machine. Hook handles still alive become inactive. */
  ~machine() override;

  /** @brief Description the machine was built from. */
  [[nodiscard]] auto description() const noexcept -> part_description const&;

  /** @brief Identity of this machine, unique within the process, never 0. */
  [[nodiscard]] auto id() const noexcept -> std::uint64_t;

  /** @brief Generation, bumped by every restore(), so state from before it is recognised. */
  [[nodiscard]] auto generation() const noexcept -> std::uint64_t;

  /** @brief Resets done since construction, every kind, whether reset() or a firmware request
   * under reset_request_policy::reset_and_continue, so a new timeline is recognised. */
  [[nodiscard]] auto resets() const noexcept -> std::uint64_t;

  /**
   * @brief resets() as a counter any thread may read while the machine runs, raised once each
   *        reset's models have reset. It outlives the machine: a host-side queue tags what it
   *        keeps with it, so a record from before a reset is told from a later one.
   */
  [[nodiscard]] auto reset_counter() const -> core::reset_counter;

  /** @brief Whether a slice is running, true inside handlers too. */
  [[nodiscard]] auto running() const noexcept -> bool;

  /**
   * @brief Events waiting in the event queue, model and core events alike. An event a model
   *        superseded without cancelling it counts until it is dispatched.
   */
  [[nodiscard]] auto pending_events() const -> std::size_t;

  /** @brief Instruction count of the earliest pending event, empty when none is pending. An event
   * a model superseded without cancelling it counts until it is dispatched. */
  [[nodiscard]] auto next_event() const -> std::optional<icount>;

  /**
   * @brief Count up to which the last slice_request::hold_from_until slice that reached its
   *        until_time left due events held, empty once a slice dispatched without a hold. A
   *        host defers stamped commands while an event at or before it is pending. Saved in
   *        snapshots.
   */
  [[nodiscard]] auto held_through() const noexcept -> std::optional<icount>;

  /**
   * @brief Instructions the core executed since reset, counted as virtual_clock().now() counts
   *        them, without the time the core slept in WFI or WFE or was held: virtual_clock().now()
   *        minus this is that time in instructions. Zeroed by every reset kind, saved in
   *        snapshots and restored with them. A save from before it existed restores it as 0,
   *        and the difference is then not that time.
   */
  [[nodiscard]] auto executed() const noexcept -> icount;

  /** @brief Virtual clock, for reading instruction count, time and domain rates. */
  [[nodiscard]] auto virtual_clock() const noexcept -> core::clock const&;

  /**
   * @brief Loads one more image, such as an application after a bootloader: checks each segment
   *        against the description's regions and copies it into flash or RAM, over earlier
   *        content where they overlap. Its symbols follow those of earlier images in the
   *        machine's symbol table.
   * @param loaded Image to load, copied for symbols, lines and digest.
   * @throws image_error naming the segment and region when a segment falls outside every
   *         region.
   * @throws machine_busy while a trace sink selecting call records is attached.
   * @throws machine_running while a slice runs, handlers included.
   */
  void load(popcorn::image::image const& loaded);

  /**
   * @brief Adds a symbols-only image, such as the ELF of a secure part embedded as raw bytes in
   *        a loaded image: nothing is written to memory and it adds no image digest. Coverage
   *        and trace see it as a loaded image, after every loaded one: its executable segments
   *        are default coverage ranges and its symbols take trace symbol ids after the loaded
   *        images' ones, see observed_images(). locate() falls back to its symbols where the
   *        loaded images give no symbol, and to its lines where they give no line, the last
   *        added first.
   * @param symbols Image whose symbols and lines are kept, copied.
   * @throws machine_busy while a trace sink selecting call records is attached.
   * @throws machine_running while a slice runs, handlers included.
   */
  void add_symbols(popcorn::image::image const& symbols);

  /** @brief Images loaded so far, in load order, the symbols-only images left out. */
  [[nodiscard]] auto loaded_images() const noexcept -> std::span<popcorn::image::image const>;

  /** @brief The loaded images in load order, then the symbols-only images in the order
   * add_symbols() added them: the images coverage and trace see. Their symbols concatenated in
   * this order are the symbol table call trace records index, so a later load moves the ids of
   * symbols-only images, never those of loaded images. */
  [[nodiscard]] auto observed_images() const noexcept -> std::span<popcorn::image::image const>;

  /**
   * @brief Locates an address the way a stop's `where` does: the function or object containing
   *        it across every loaded image, then the file and line from the last loaded image whose
   *        segments cover it.
   * @param pc Guest address.
   * @return The location, symbol and file empty when unknown. Its views stay valid until the
   *         next load().
   */
  [[nodiscard]] auto locate(address pc) const -> location;

  /**
   * @brief Resets the machine as @p kind, in this order: every periph::reset_listener's
   *        `before_reset` in description order, with the clock at the reset's time, where a
   *        throw leaves the machine unreset, then event queue cleared, clock restarted at
   *        instruction 0 and time 0 at the power-on core clock, for every kind, the arch module's
   *        `reset` at the reset vector, see machine_options::vector_table, then per peripheral in
   *        description order its register storage to reset values and the model's `reset`, then
   *        `trace_ring::rebase` at instruction 0. The arch resets first so that a line a model
   *        raises in its own reset reaches it. Memory is not touched. Also what a firmware reset
   *        request leads to under reset_request_policy::reset_and_continue: the request takes
   *        effect at the next block boundary, the reset runs there, the trace rebases at 0, and
   *        the slice continues.
   * @param kind Cause of the reset, passed to every model.
   * @throws machine_running while a slice runs, handlers included.
   * @throws machine_busy while a call begun by begin_call() is in progress.
   */
  void reset(reset_kind kind = reset_kind::power_on) pre(is_enumerator(kind));

  /**
   * @brief Cuts the supply and powers the machine again: every periph::reset_listener's
   *        `before_reset(reset_kind::power_on)` first, where a throw refuses the cycle before
   *        any change, then every model's `power_cut()` in description order, at the time of
   *        the cut, then every RAM region without `physical` zeroed with its translated code
   *        dropped, then reset(reset_kind::power_on) without a second `before_reset`. Flash,
   *        OTP and every other rom region keep their bytes, the scratch region of
   *        machine_options::scratch too. Hooks, probes, watches and host bridges stay, as across
   *        a reset. A cut cannot be refused once a `power_cut()` began: when one throws, every
   *        later model's still runs, RAM is zeroed, the reset runs, then the first exception is
   *        rethrown. An exception from the zeroing or from a model's reset is kept the same way:
   *        the reset still runs after a zeroing failure, and the first exception of the whole
   *        cycle is the one rethrown.
   * @throws machine_running while a slice runs, handlers included.
   * @throws machine_busy while a call begun by begin_call() is in progress.
   * @throws Any exception a `before_reset` threw, with nothing cut, zeroed or reset.
   * @throws Any exception the first failing `power_cut()`, zeroing or model reset threw, such
   *         as model_error under machine_options::strict_peripherals, after the cycle ran to
   *         its end.
   */
  void power_cycle();

  /**
   * @brief Holds or releases the core. While held, the run loop never executes guest code and
   *        never delivers an exception: every slice takes the idle path, so the clock, events and
   *        DMA keep following virtual time, and the arch and the models freeze what a core held
   *        in Debug state freezes, see arch::arch::hold_core() and periph::core_hold_listener.
   *        Pending exceptions and the sleep state are kept, so a released core takes them first,
   *        as after a debugger resume. A held machine stays held across reset() and a firmware
   *        reset, and refuses calls, see begin_call(). Not core::host::halt(), which ends a free
   *        run. Called by the engine owner only, as reset() and restore().
   * @param held True to hold, false to release. A request for the current state does nothing.
   * @throws machine_running while a slice runs, handlers included.
   * @throws machine_busy while a call begun by begin_call() is in progress.
   * @throws Any exception the arch or a listener throws, such as model_error under
   *         machine_options::strict_peripherals: the hold is undone and the machine behaves as
   *         before the call.
   */
  void hold_core(bool held);

  /** @brief Whether the core is held, see hold_core(). */
  [[nodiscard]] auto core_held() const noexcept -> bool;

  /**
   * @brief Runs one native slice, synchronously: delivers a pending interrupt, runs the engine
   *        until a bound, a stop request or a trap, dispatches due events, and when the core
   *        idles moves the clock to the next event or to until_time, whichever comes first, and
   *        stops with stop_reason::idle only when neither exists. Loop-agnostic: an asyncio
   *        driver and a fuzz driver both call it in a loop.
   * @param request Bounds and stop conditions.
   * @return The stop, or an empty stop when only a yield bound was reached.
   * @throws machine_busy when a slice is already running, from a handler of it.
   * @throws emulation_error when the machine was left invalid by a failed restore().
   * @throws model_error on an unmodelled access under `machine_options::strict_peripherals`.
   * @throws Any exception a handler or Python model raised during the slice, rethrown after the
   *         engine stopped at an instruction boundary, the machine consistent and resumable,
   *         before any fault of the same slice is reported. Only the first exception of a slice
   *         is rethrown, later ones of the same slice are dropped.
   */
  auto run_slice(slice_request const& request) -> slice_result;

  /**
   * @brief Ends the running slice at the next block boundary, from any thread. A slice that is
   *        not running ends at its first boundary. The slice returns with an empty stop, the
   *        machine resumable. Safe to call concurrently with run_slice().
   */
  void interrupt() noexcept;

  /**
   * @brief Begins a guest function call: saves the engine registers of `arch::arch::registers()`
   *        and the `arch::arch::save()` bytes, then `arch::arch::prepare_call()`. The slices that
   *        follow run the call. Reaching `frame.return_sentinel` stops with
   *        stop_reason::returned, also when the engine faults the fetch there, as a security
   *        lookup does for a sentinel of the other security state.
   * @param frame Call to make, its entry and sentinel set.
   * @throws machine_busy when a call is already in progress, or the core is held, see
   *         hold_core().
   * @throws machine_running while a slice runs, handlers included.
   * @throws config_error when the arch module refuses the frame.
   */
  void begin_call(arch::call_frame const& frame);

  /**
   * @brief Ends the call begun by begin_call(), after it returned or stopped: reads the return
   *        value when it returned, then restores the saved registers and arch state, so the
   *        interrupted context continues as if no call happened.
   * @return Return value bits of the frame's `returns` kind, 0 when the call did not return.
   * @throws machine_running while a slice runs, handlers included.
   */
  auto end_call() -> std::uint64_t pre(in_call());

  /**
   * @brief The value a function returning now leaves in its result registers,
   *        `arch::arch::call_result()`: for a stop at its return address.
   * @param returns Kind of the value.
   * @param abi Float ABI of the function.
   * @return Value bits of @p returns.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[nodiscard]] auto returned_value(arch::value_kind returns, arch::float_abi abi) const
      -> std::uint64_t;

  /** @brief Whether a call begun by begin_call() has not been ended yet. */
  [[nodiscard]] auto in_call() const noexcept -> bool;

  /**
   * @brief Whether pending exception @p number would preempt the core now, the firmware's
   *        masks and active exceptions counted, a call's masking not, `arch::arch::would_preempt`.
   * @param number Exception number, an external interrupt n as 16 + n on Arm.
   */
  [[nodiscard]] auto would_preempt(std::uint32_t number) const -> bool;

  /**
   * @brief Whether the core is in thread mode, `arch::arch::thread_mode()`, and no exception is
   *        deliverable now, so its next instruction is a thread-mode one. A thread asleep with
   *        nothing deliverable is ready, also after a return with sleep-on-exit.
   */
  [[nodiscard]] auto thread_ready() const -> bool;

  /**
   * @brief Refuses a call frame before any state changes, `arch::arch::check_call()`.
   * @param frame Call to check.
   * @throws config_error when the arch module refuses the frame.
   */
  void check_call(arch::call_frame const& frame) const;

  /**
   * @brief Makes a guest function call synchronously: begin_call(), slices until the call stops,
   *        end_call(). For native callers without an event loop.
   * @param frame Call to make, its entry and sentinel set.
   * @param budget Instructions the call may execute before it stops with stop_reason::budget.
   * @return Return value and stop.
   * @throws As begin_call() and run_slice().
   */
  auto call(arch::call_frame const& frame, icount budget) -> call_result;

  /** @brief Guest memory view, valid while the machine lives. */
  [[nodiscard]] auto mem() noexcept -> memory_view;

  /**
   * @brief Guest memory view filtered as DAP accesses of one security state, see memory_view.
   * @param secure Whether the view requests Secure accesses.
   * @return The view, valid while the machine lives.
   */
  [[nodiscard]] auto mem(bool secure) noexcept -> memory_view;

  /** @brief CPU register view, valid while the machine lives. */
  [[nodiscard]] auto regs() noexcept -> register_view;

  /**
   * @brief Takes a full copy of the machine state: every RAM and flash region, the engine CPU
   *        context and register list, the arch bytes, register storage and line levels, model
   *        bytes, event queue, clock, intercept and return hook state. Every
   *        periph::snapshot_listener's `before_snapshot` runs first, so models that compute
   *        lazily save their state at the machine's time. Cost is a copy of the mapped memory,
   *        about 15 us for 784 KB.
   * @return The snapshot, restorable any number of times.
   * @throws machine_running while a slice runs, handlers included.
   * @throws machine_busy during a call begun by begin_call() and not ended.
   * @throws emulation_error when the machine was left invalid by a failed restore().
   * @throws Any exception a snapshot listener threw, with nothing captured.
   */
  [[nodiscard]] auto snapshot() const -> core::snapshot;

  /**
   * @brief Restores a snapshot, whatever changed since.
   *
   * Order, each step complete before the next starts:
   *
   * 1. memory: `memcpy` of each region into its host buffer, then `uc_ctl_remove_cache` over
   *    every page of every host buffer mapped executable whose bytes the copy changed, at every
   *    address the buffer is mapped. The translated block cache is otherwise kept, which
   *    spares retranslating the code the next slice runs.
   * 2. CPU: `uc_context_restore` when @p saved carries this machine's engine context, otherwise
   *    the register list through `uc_reg_write_batch`. A `uc_context` never moves between
   *    engines: on Arm M-profile it carries pointers owned by the engine that saved it, Unicorn
   *    issue #2195.
   * 3. scheduler: `scheduler::restore` with the pending events and sequence counter, so seqs and
   *    tie order replay identically.
   * 4. clock: `clock_driver::restore`, domain handles stay valid, then `trace_ring::rebase` at
   *    the restored instruction count.
   * 5. arch: `arch::arch::load` of the arch part, which also restores `pending()` and may
   *    schedule against the restored queue and clock.
   * 6. peripherals and models: register storage and line levels of every peripheral, then
   *    `periph::model::load` of every model, native and Python alike, in description order.
   *
   * Then intercept and return hook state, and generation() bumps.
   *
   * Rules:
   *
   * - Only between slices, never from a handler, never during a call.
   * - Any number of times on the same snapshot, which stays valid after further snapshots and
   *   restores.
   * - A snapshot taken by another machine is refused. A snapshot read back from bytes has no
   *   engine context: it restores through the register list, after its description and image
   *   digests are checked against this machine's.
   * - The snapshot is checked whole before the machine changes. A failure after that, such as a
   *   model load that throws, leaves the machine invalid: run_slice() and snapshot() throw until
   *   a restore succeeds.
   * - Calls in flight at the snapshot are not reported by return hooks after a restore, and an
   *   intercept stop is not resumed past.
   * - Stop, reset and slice-end requests pending at the restore are dropped. A snapshot does not
   *   carry them.
   * - restore() does not undo load(): images loaded since the snapshot stay listed in
   *   loaded_images() and in the image digests of later snapshots, while their bytes revert.
   * - Not restored: the intercept, return hook, probe, watch and sink tables, which are
   *   configuration, the trace ring, coverage counters and probe counts, which are cumulative,
   *   and host bridges, which are outside the machine.
   *
   * @param saved Snapshot to restore.
   * @throws machine_running while a slice runs, handlers included.
   * @throws machine_busy during a call begun by begin_call() and not ended.
   * @throws emulation_error when @p saved was taken by another machine.
   * @throws config_error when a snapshot read back from bytes names another description, other
   *         images or another `machine_options::config_digest`, or has another format, or when its
   * memory regions, the scratch region included, are not this machine's, and keyed
   *         `trustzone` when its `machine_options::trustzone` or `machine_options::idau` differs.
   */
  void restore(core::snapshot const& saved);

  /**
   * @brief Intercepts calls to a code address: a code hook on exactly that address, with the
   *        translated block cache invalidated for it on add and on removal so the hook takes
   *        effect at once.
   * @param at Code address, the function entry as its symbol gives it.
   * @param handler Called on arrival at @p at.
   * @param mode Every arrival, or only the first.
   * @return Handle removing the intercept.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[nodiscard]] auto add_intercept(
      address           at,
      intercept_handler handler,
      intercept_mode    mode = intercept_mode::every) -> intercept_handle pre(is_enumerator(mode));

  /**
   * @brief Counts arrivals at a code address without stopping or calling anything: the same
   *        code hook as add_intercept(), sharing its site, with the time of the first arrival.
   *        An arrival counts once, before the instruction executes and before every intercept
   *        of the address, whatever their add order, also when an intercept then skips the
   *        function or stops the run: resuming from that stop does not count it again.
   *        With @p secure, an arrival counts only when the core runs in that security state,
   *        read from the engine at the arrival. Without it no state is read.
   * @param at Code address, any instruction, not only a block start.
   * @param secure Security state an arrival must run in, empty for any.
   * @return The handle removing the probe, and the counts, which outlive it.
   * @throws machine_running while a slice runs, handlers included.
   * @throws config_error for @p secure on a machine without TrustZone enabled.
   */
  [[nodiscard]] auto add_probe(address at, std::optional<bool> secure = std::nullopt)
      -> probe_handle;

  /**
   * @brief Calls a handler when a function returns: captures the arguments at entry and the
   *        return value at the return address, counting the instructions in between. An
   *        exception return that arch::interrupt_returns() accepts, such as Arm EXC_RETURN from a
   *        handler or from a function it tail-called, counts before the core unstacks. When the
   *        handler throws, the other frames returning at that point are dropped unreported and
   *        the exception exit still completes.
   * @param at Function entry.
   * @param args Kind of each argument captured at entry.
   * @param returns Kind of the return value.
   * @param handler Called on every return.
   * @return Handle removing the hook.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[nodiscard]] auto add_return_hook(
      address                       at,
      std::vector<arch::value_kind> args,
      arch::value_kind              returns,
      return_handler                handler) -> intercept_handle;

  /**
   * @brief Watches guest accesses to an address range, by the CPU and by DMA. A watch covers
   *        every address that maps the watched bytes: the memory region and its aliases, the
   *        peripheral window and its alias windows. An access that overlaps the range hits it,
   *        whichever address it starts at. watch_hit::addr is the address accessed. While any
   *        watch exists, every guest load and store pays for a memory hook.
   * @param at First watched address.
   * @param size Watched bytes, at least 1.
   * @param kind Accesses reported.
   * @param handler Called on every matching access.
   * @return Handle removing the watch.
   * @throws machine_running while a slice runs, handlers included.
   * @throws config_error when the range reaches no memory and no peripheral window.
   */
  [[nodiscard]] auto
      add_watch(address at, std::uint32_t size, watch_kind kind, watch_handler handler)
          -> watch_handle pre(size > 0) pre(fits_address_space(at, size)) pre(is_enumerator(kind));

  /**
   * @brief Diagnostics: ranges watched through machine_services::watch_reads, one per distinct
   *        range of bytes, whichever alias named it. Models own the table: reset() and
   *        restore() keep it, a model rebuilds what it needs in its own load().
   */
  [[nodiscard]] auto controller_watches() const noexcept -> std::size_t;

  /**
   * @brief Diagnostics: engine read hooks behind controller_watches(), one per alias of each
   *        engine page a watched range or the 7 bytes before it touches, the region's own
   *        mapping included.
   */
  [[nodiscard]] auto controller_watch_hooks() const noexcept -> std::size_t;

  /** @brief Always-on trace ring. */
  [[nodiscard]] auto trace() noexcept -> trace_ring&;

  /**
   * @brief Chooses whether executed blocks append block records to the trace ring, off by
   *        default. Trace sinks select block records on their own. Machine configuration: not
   *        in snapshots, kept across restore().
   * @param on Whether the ring keeps block records.
   * @throws machine_running while a slice runs, handlers included.
   */
  void set_ring_blocks(bool on);

  /** @brief Whether executed blocks append block records to the trace ring. */
  [[nodiscard]] auto ring_blocks() const noexcept -> bool;

  /**
   * @brief Streams trace records of the selected kinds to a sink, the full trace a `.pctr` file
   *        writer consumes. Kinds that cost a hook, such as block and mem, are hooked only while
   *        a sink selects them, and only over its ranges. dt counts per sink, see trace_sink.
   * @param kinds Kinds the sink receives.
   * @param sink Receives batches of dt-stamped records, in order.
   * @param ranges Code ranges whose execution is traced, empty for everywhere. A record belongs
   *        to a range when the pc that caused it does.
   * @return Handle removing the sink.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[nodiscard]] auto
      add_trace_sink(trace_kinds kinds, trace_sink sink, std::vector<address_range> ranges = {})
          -> hook_handle;

  /**
   * @brief Receives the machine's log records, such as unmodelled peripheral accesses.
   * @param sink Receives each record.
   * @return Handle removing the sink.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[nodiscard]] auto add_log_sink(log_sink sink) -> hook_handle;

  /**
   * @brief Sends a record to the log sinks, as a model does through machine_services::log, for
   *        an embedder reporting on the machine's behalf. Called by the thread that runs the
   *        machine, between slices.
   */
  using machine_services::log;

  /**
   * @brief Starts collecting block and edge counters over code ranges. A native hook limited to
   *        the ranges counts, nothing crosses to Python. Counters already held for a range are
   *        kept, so enabling again only adds ranges.
   * @param ranges Code ranges, empty for every executable segment of the loaded images.
   * @throws machine_running while a slice runs, handlers included.
   */
  void enable_coverage(std::vector<address_range> ranges);

  /**
   * @brief Stops collecting, keeping the counters.
   * @throws machine_running while a slice runs, handlers included.
   */
  void disable_coverage();

  /** @brief Whether coverage is being collected. */
  [[nodiscard]] auto coverage_enabled() const noexcept -> bool;

  /**
   * @brief Copy of the counters, cumulative across restores.
   * @throws machine_running while a slice runs, outside its handlers, which read the counts so far.
   */
  [[nodiscard]] auto coverage() const -> coverage_data;

  /**
   * @brief Zeroes every counter, ranges kept.
   * @throws machine_running while a slice runs, handlers included.
   */
  void clear_coverage();

  /**
   * @brief Register storage of a peripheral.
   * @param name Peripheral name, as the description spells it.
   * @return The register file, valid while the machine lives.
   * @throws config_error when the description has no such peripheral.
   */
  [[nodiscard]] auto peripheral(std::string_view name) -> periph::register_file&;

  /**
   * @brief Behaviour model of a peripheral, for model-specific APIs such as a UART endpoint.
   * @param name Peripheral name, as the description spells it.
   * @return The model, valid while the machine lives, null when no model serves the peripheral.
   * @throws config_error when the description has no such peripheral.
   */
  [[nodiscard]] auto model(std::string_view name) -> periph::model*;

private:
  struct engine_closer
  {
    void operator()(uc_struct* engine) const noexcept;
  };
  struct POPCORN_HIDDEN mapped_region;
  struct POPCORN_HIDDEN peripheral_slot;
  struct POPCORN_HIDDEN run_state;

  void require_view_access() const;

  void do_request_stop(stop_reason reason, std::string detail) override;
  void do_request_reset(reset_kind kind) override;
  void do_end_slice() override;
  void do_end_after_instruction() override;
  auto do_read(address addr, std::uint8_t size, bus_attrs attrs)
      -> std::expected<std::uint32_t, bus_error> override;
  auto do_write(address addr, std::uint8_t size, std::uint32_t value, bus_attrs attrs)
      -> std::expected<void, bus_error> override;
  auto do_read_granted(address addr, std::uint8_t size, bus_attrs attrs, memory_grant& grant)
      -> std::expected<std::uint32_t, bus_error> override;
  auto do_write_granted(
      address       addr,
      std::uint8_t  size,
      std::uint32_t value,
      bus_attrs     attrs,
      memory_grant& grant) -> std::expected<void, bus_error> override;
  auto read_memory(mapped_region const& region, address addr, std::uint8_t size, bus_attrs attrs)
      -> std::uint32_t;
  auto write_memory(
      mapped_region const& region,
      address              addr,
      std::uint8_t         size,
      std::uint32_t        value,
      bus_attrs            attrs) -> std::expected<void, bus_error>;
  auto do_read_block(address addr, std::span<std::byte> out, bus_attrs attrs)
      -> std::expected<void, block_error> override;
  auto do_write_block(address addr, std::span<std::byte const> in, bus_attrs attrs)
      -> std::expected<void, block_error> override;
  void do_trace(trace_record const& record) override;
  void do_log(log_record record) override;
  void do_watch_reads(address at, std::uint32_t size, bool on) override;
  void do_install_firewall(periph::bus_firewall& firewall) override;
  void do_memory_policy_changed() override;
  auto do_emulated_instruction(address at) -> emulated_arrival override;

  friend class memory_view;
  friend class register_view;
  friend class intercept_call;
  friend struct machine_internals;

  std::uint64_t                                 id_;
  part_description                              description_;
  machine_options                               options_;
  std::unique_ptr<uc_struct, engine_closer>     engine_;
  std::vector<std::unique_ptr<mapped_region>>   memory_{};
  core::clock                                   clock_;
  clock_driver                                  clock_driver_;
  scheduler                                     scheduler_;
  event_port                                    arch_events_;
  std::unique_ptr<arch::arch>                   arch_;
  std::vector<std::unique_ptr<peripheral_slot>> peripherals_{};
  std::vector<popcorn::image::image>            images_{};
  std::size_t                                   loaded_count_{};
  trace_ring                                    trace_;
  std::shared_ptr<hook_registry>                hooks_;
  std::unique_ptr<run_state>                    run_;
  std::uint64_t                                 generation_ = 0;
};
}
