#pragma once

#include "access_op.hpp"
#include "board.hpp"
#include "gil_locks.hpp"
#include "held_python_end.hpp"
#include "hooks.hpp"
#include "host_future.hpp"
#include "image.hpp"
#include "observability.hpp"
#include "pty.hpp"
#include "snapshot.hpp"
#include "usb.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <reflex/py.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief Bit set in every log token, so it never equals a request token. */
inline constexpr std::uint64_t log_token_bit = std::uint64_t{1} << 63;

/** @brief Machine options the facade exposes, the subset of core::machine_options Python sets. */
struct[[= py::rename{"MachineConfig"}]] machine_config
{
  /** @brief Core clock at power-on in Hz, 0 for the description's reset clock. */
  std::uint64_t core_hz{};
  /** @brief Core cycles per instruction, above 0. */
  double cpi{1.0};
  /** @brief Records kept by the always-on trace ring, above 0. */
  std::size_t trace_capacity{4096};
  /** @brief Iterations of a loop making no progress before a spin stop, 0 for off. */
  std::uint32_t spin_threshold{};
  /** @brief Whether an unmodelled register access fails the run. */
  bool strict_peripherals{};
  /** @brief Address of the vector table reset reads, empty for the description's default. */
  std::optional<address> vector_table{};
  /** @brief Digest of the configuration, recorded in snapshots. */
  std::string config_digest{};
  /** @brief First address of the call arena, a RAM region outside the description, empty for
   * none. */
  std::optional<address> arena_start{};
  /** @brief Size of the call arena in bytes, a multiple of the page size. */
  std::uint32_t arena_size{};
  /** @brief Python model maker per peripheral name, called with the ModelContext, returning a
   * popcorn.model.Model. Overrides the registered models of that peripheral. */
  std::map<std::string, reflex::py::nb::object> python_models{};
  /** @brief Plugin model per peripheral name: the loaded plugin and the name of the model its
   * init records. Overrides the registered models of that peripheral, as python_models does. */
  std::map<std::string, std::pair<plugin::loaded const*, std::string>> plugin_models{};
  /** @brief HSE frequency in Hz the board gives the RCC model, from 4 to 50 MHz, empty for the
   * model's default. */
  std::optional<std::uint64_t> rcc_hse_hz{};
  /** @brief LSE frequency in Hz the board gives the RCC model, above 0 and at most 1 MHz, empty for
   * the model's default. */
  std::optional<std::uint64_t> rcc_lse_hz{};
  /** @brief VDDA in millivolts the board gives the ADC model, from 1620 to 3600, empty for the
   * model's default of 3000. */
  std::optional<std::uint32_t> vdda_mv{};
  /** @brief Python bus targets per peripheral name, `popcorn.board.I2CTarget` or `SpiTarget`
   * instances in attachment order, served by the built-in bus model of that peripheral. */
  std::map<std::string, std::vector<reflex::py::nb::object>> python_devices{};
  /** @brief Virtual USB host per USB device controller name, served by the built-in model of
   * that controller. */
  std::map<std::string, usb_host_config> usb_hosts{};
  /** @brief Object whose `context` attribute every Python model and bus device runs in, held
   * weakly by them, empty to run them in the calling thread's context. */
  std::optional<reflex::py::nb::object> models_context{};
  /** @brief Instructions after which a slice of the machine thread serves its requests, above
   * 0. */
  std::uint64_t slice_instructions{1'000'000};
  /** @brief Wall time after which a slice of the machine thread serves its requests, in
   * nanoseconds, above 0. */
  std::int64_t slice_wall_ns{1'000'000};
};

/** @brief A decoded processor fault. */
struct[[= py::rename{"FaultInfo"}]] fault_record
{
  /** @brief Class of the fault. */
  [[= py::readonly]] fault_kind kind{};
  /** @brief Faulting address, 0 when none. */
  [[= py::readonly]] address fault_address{};
  /** @brief Primary fault status register, Arm CFSR. */
  [[= py::readonly]] std::uint32_t status{};
  /** @brief Secondary fault register, Arm HFSR. */
  [[= py::readonly]] std::uint32_t detail{};
};

/** @brief A stop, owning its text. */
struct[[= py::rename{"StopInfo"}]] stop_record
{
  /** @brief Why the run stopped. */
  [[= py::readonly]] stop_reason reason{};
  /** @brief Where the core stopped, the next instruction to execute. */
  [[= py::readonly]] location_info where{};
  /** @brief Instructions executed since reset. */
  [[= py::readonly]] std::uint64_t instructions{};
  /** @brief Virtual time since reset, in picoseconds. */
  [[= py::readonly]] std::uint64_t time_ps{};
  /** @brief Decoded fault, set exactly when reason is fault. */
  [[= py::readonly]] std::optional<fault_record> fault{};
  /** @brief What caused the stop beyond its reason. */
  [[= py::readonly]] std::string detail{};
};

/** @brief How a guest call goal ended. */
struct[[= py::rename{"CallOutcome"}]] call_outcome
{
  /** @brief The stop that ended the call. */
  [[= py::readonly]] stop_record stopped{};
  /** @brief Return value bits end_call gave, 0 when the call did not return. */
  [[= py::readonly]] std::uint64_t bits{};
  /** @brief Every core register in register_names() order at a FAULT stop, read before the
   * caller's context came back, empty for any other stop. */
  [[= py::readonly]] std::vector<std::uint64_t> registers{};
  /** @brief The exception number active when the call began, 0 in thread mode. For a call that
   * never began, the one at its stop. */
  [[= py::readonly]] std::uint32_t ipsr{};
  /** @brief Whether the call began, false when a from_thread wait stopped first. */
  [[= py::readonly]] bool began{};
  /** @brief Exceptions pending and enabled when a BUDGET stop ended a call that masked
   * interrupts, NMI, PendSV, SysTick and external interrupts as 16 + n, ascending, that the
   * firmware's masks and active exceptions let preempt: a call with interrupts takes them. Read
   * on the machine's thread at the stop. Empty otherwise. */
  [[= py::readonly]] std::vector<std::uint32_t> pending_exceptions{};
  /** @brief The other exceptions pending and enabled at that stop, which the firmware's
   * PRIMASK, BASEPRI, FAULTMASK or an active exception holds back too. */
  [[= py::readonly]] std::vector<std::uint32_t> held_exceptions{};
};

namespace detail
{
/**
 * @brief Gives a machine's host up for a Python caller interrupted while a handler blocks the
 *        host thread: the borrowed handles of the machine's Python models are dropped first, so
 *        no collection visits a model the abandoned thread frees, the machine is marked gone for
 *        its views and links, then core::host::abandon_thread() runs with the GIL released.
 */
struct host_abandon
{
  /** @brief The machine's borrowed Python model handles. */
  std::shared_ptr<std::vector<reflex::py::nb::handle>> owned;
  /** @brief The machine's lifetime flag, cleared by the abandon. */
  std::shared_ptr<std::atomic<bool>> alive;
  /** @brief The host. */
  std::weak_ptr<core::host> host;

  /** @brief Abandons the host, the GIL held. */
  void operator()() const;
};

/** @brief Takes the mutex of @p ptys, releasing the GIL while it blocks when the thread holds it.
 */
[[nodiscard]] auto lock_ptys(pty_bridges& ptys) -> std::unique_lock<std::mutex>;

/** @brief The StopInfo of @p stopped, owning its text. */
[[nodiscard]] auto stop_info(stop const& stopped) -> stop_record;
}

/**
 * @brief A host pty bridged to a UART or a CDC function, pumped by the machine's thread. Holds
 *        its bridge weakly: the machine owns it.
 */
class[[= py::rename{"PtyBridge"}]] pty_handle
{
public:
  /** @brief A handle on @p bridge, owned by @p owner, polled by @p host under @p watched. */
  [[= py::skip]] pty_handle(
      std::weak_ptr<detail::pty_bridges>         owner,
      std::shared_ptr<detail::pty_bridge> const& bridge,
      std::weak_ptr<core::host>                  host,
      core::watch_id                             watched);

  /** @brief Path of the pty, /dev/pts/N, also after close(). */
  [[= py::doc{"Path of the pty, /dev/pts/N."}]] [[nodiscard]] auto path() const -> std::string;

  /** @brief Stops pumping and closes both ends of the pty. Idempotent. Does nothing in a process
   * forked from the one that opened it. */
  [[= py::doc{"Stop pumping and close the pty."}]] void close();

private:
  std::weak_ptr<detail::pty_bridges> owner_;
  std::weak_ptr<detail::pty_bridge>  bridge_;
  std::weak_ptr<core::host>          host_;
  core::watch_id                     watched_;
  std::string                        path_;
};

/**
 * @brief Owner of one native machine, the object `popcorn.Machine` wraps. The machine is built
 *        on the caller thread, then owned by a core::host whose thread runs every goal. Sync
 *        methods run inline on the thread that owns the engine and directly under the host
 *        mutex while the machine is parked, and raise MachineRunning while it runs.
 */
class[[= py::rename{"MachineHandle"}]] machine_handle
{
public:
  /**
   * @brief Builds a machine, powered off, with the arch module that implements the core.
   * @param description Description of the MCU.
   * @param config Options.
   * @throws config_error naming `core.cpu` when no arch module implements the core, naming
   *         `cpi` or `trace_capacity` when not above 0, or as core::machine's constructor.
   */
  machine_handle(part_description const& description, machine_config const& config);

  /** @brief Not copyable, models hold its lifetime flag. */
  machine_handle(machine_handle const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(machine_handle const&) -> machine_handle& = delete;
  /**
   * @brief Marks the machine gone for its views, halts, closes and joins the host with the GIL
   *        released, then destroys the machine. A free run's end never collected by halt() goes
   *        to sys.unraisablehook. Runs with the GIL: nanobind destroys the handle in instance
   *        deallocation, and no native code owns one. On a host thread or inside a parked
   *        section it takes the host's orphan path. While the
   *        interpreter finalizes with the host thread alive it leaves both alive: the thread
   *        needs the GIL to exit, and late command failures still queued are not reported. An
   *        abandoned host is left to its thread, which frees the machine when the blocked handler
   *        returns. It never checks signals: on a live host whose handler blocks it waits for
   *        the handler.
   */
  ~machine_handle();

  /**
   * @brief Loads one more image.
   * @param loaded Image.
   * @throws image_error when a segment falls outside the description's regions.
   */
  [[= py::doc{"Load one more image, ImageError when a segment falls outside memory."}]] void
      load(image const& loaded);

  /**
   * @brief Resets the machine.
   * @param kind Reset kind.
   */
  [[= py::doc{"Reset the machine as kind."}]] void reset(reset_kind kind);

  /** @brief Digest of each loaded image, in load order. */
  [[= py::doc{"Digest of each loaded image, in load order."}]] [[nodiscard]] auto
      image_digests() const -> std::vector<std::string>;

  /**
   * @brief Locates an address the way a stop's location does.
   * @param pc Guest address.
   */
  [[= py::doc{
      "Symbol, offset, file and line of an address across the loaded images."}]] [[nodiscard]] auto
      locate(address pc) const -> location_info;

  /**
   * @brief Starts a goal over as many slices as it needs and returns at once.
   * @param until Stop before executing this address, left first when the goal starts on it.
   * @param budget Stop with reason budget past this many instructions.
   * @param exact Stop after exactly this many instructions.
   * @param for_ps Stop with reason time this many picoseconds of virtual time from now.
   * @param deadline_ps Stop with reason time at this virtual time since reset in picoseconds, at
   *        once when already reached. Exclusive with @p for_ps.
   * @param stop_when Predicate evaluated at event boundaries and slice ends, on the host thread
   *        in a copy of the caller's context, None for none.
   * @param until_return Stop with reason returned at this return address, or EXC_RETURN, and
   *        stack pointer, core::frame_return. None for none.
   * @return Resolves to the StopInfo.
   * @throws machine_busy while a goal or a free run is active.
   * @throws machine_running from a callback or a parked section.
   * @throws std::invalid_argument for both @p for_ps and @p deadline_ps.
   */
  [[= py::doc{"Start a goal, return a HostFuture resolving to its StopInfo. until_return is "
              "(return address or EXC_RETURN, stack pointer): stop with RETURNED there."}]] auto
      run_goal(
          std::optional<address>                            until,
          std::optional<std::uint64_t>                      budget,
          std::optional<std::uint64_t>                      exact,
          std::optional<std::uint64_t>                      for_ps,
          std::optional<std::uint64_t>                      deadline_ps,
          std::optional<reflex::py::nb::object>             stop_when,
          std::optional<std::tuple<address, std::uint64_t>> until_return) -> host_future_handle;

  /**
   * @brief Opens a free run on the host thread, until halt().
   * @throws machine_busy while a goal or a free run is active.
   * @throws machine_running from a callback or a parked section.
   */
  [[= py::doc{"Start a free run on the machine thread, until halt()."}]] void start();

  /**
   * @brief Starts a guest call as one goal: under the host mutex, @p prepare, then begin_call,
   *        then slices to a stop. The machine thread ends the call before the future completes,
   *        also when the goal fails or is cancelled.
   * @param frame The call.
   * @param budget Stop with reason budget past this many instructions.
   * @param prepare Called with no argument before begin_call, as a callback of the parked
   *        machine, None for none.
   * @param from_thread Run first until machine::thread_ready(), then begin the call, all in the
   *        budget. The frame's ABI and sentinel are checked before. A machine already ready
   *        begins at once.
   * @return Resolves to the CallOutcome.
   * @throws machine_busy while a goal or a free run is active, or a call is in progress.
   * @throws machine_running from a callback or a parked section.
   */
  [[= py::doc{
      "Start a guest call as one goal, return a HostFuture resolving to its CallOutcome."}]] auto
      call_goal(
          arch::call_frame const&               frame,
          std::optional<std::uint64_t>          budget,
          std::optional<reflex::py::nb::object> prepare,
          bool                                  from_thread) -> host_future_handle;

  /**
   * @brief A goal whose finish throws popcorn::error, for tests of the chained end of a failed
   *        or cancelled goal.
   * @param budget Stop with reason budget past this many instructions.
   * @return Resolves to None, fails with what the goal ended by.
   */
  [[= py::doc{"Run a goal whose finish raises, for tests."}]] auto
      failing_finish_goal(std::uint64_t budget) -> host_future_handle;

  /**
   * @brief Closes the free run, waiting with the GIL released until the machine parks. On a
   *        host thread it only requests the end. A goal running instead is refused under the
   *        host mutex and goes on.
   * @return None, the StopInfo that ended the free run by itself, or the exception a handler
   *         raised in it.
   * @throws machine_busy while a goal is active instead.
   * @throws machine_running inside a parked section.
   */
  [[= py::doc{"Halt the free run, return None, the StopInfo or exception that ended it."}]] auto
      halt() -> reflex::py::nb::object;

  /**
   * @brief Parks the machine whatever runs: cancels an active goal, its future failing with
   *        operation_cancelled, or closes the free run as halt() does, waiting with the GIL
   *        released until the machine parks. A no-op on a parked machine. On a host thread it
   *        only requests the end, as halt() does.
   * @return None, the StopInfo that ended the free run by itself, or the exception a handler
   *         raised in it. None as well for an end a wait of the free run already returned or
   *         raised.
   * @throws machine_running inside a parked section.
   */
  [[= py::doc{"Cancel a goal or halt the free run, return the end no wait delivered."}]] auto park()
      -> reflex::py::nb::object;

  /**
   * @brief Parks the machine as park() does, for a collection: returns the end to report.
   * @return None when nothing ended the free run by itself, when a wait already returned or
   *         raised that end, or when the collection caused it: a stop_reason::user stop with
   *         detail::collected_detail, or `popcorn._hooks.HookGone`. Otherwise EmulationError
   *         naming the stop, or the exception a handler raised.
   * @throws machine_running inside a parked section.
   */
  [[= py::doc{"Park for a collection, return the end to report or None."}]] auto unreported_end()
      -> reflex::py::nb::object;

  /**
   * @brief Marks the `Machine` garbage: every later call of its Python models and bus devices
   *        is refused and stops the run with stop_reason::user and detail::collected_detail,
   *        on a machine a finalizer resurrected too.
   */
  [[= py::doc{"Refuse every later model and device call, the Machine is garbage."}]] void
      mark_collected() noexcept;

  /**
   * @brief Waits on the free run.
   * @param ready Predicate called on the host thread at every slice end after the boundary that
   *        registers the wait, in a copy of the caller's context, None for a deadline only.
   * @param for_ps Picoseconds of virtual time after the boundary that registers the wait, at
   *        which it ends with reason time, None for no deadline. With it, posting ends the
   *        running slice at its next block boundary, which registers the wait. Without it, the
   *        next slice boundary registers it.
   * @param deadline_ps Virtual time since reset in picoseconds at which it ends with reason
   *        time, None for none. It ends the running slice as @p for_ps does. Already reached,
   *        the wait ends at the next boundary. Exclusive with @p for_ps.
   * @return Resolves to the StopInfo of the boundary that ended the wait, or to the free run's
   *         own end.
   * @throws std::runtime_error without an open free run.
   */
  [[= py::doc{"Wait on the free run, return a HostFuture resolving to a StopInfo."}]] auto wait(
      std::optional<reflex::py::nb::object> ready,
      std::optional<std::uint64_t>          for_ps,
      std::optional<std::uint64_t>          deadline_ps) -> host_future_handle;

  /**
   * @brief Waits on the free run for native conditions, evaluated with no GIL after every
   *        dispatched event and at slice ends.
   * @param when Conditions, each on plain memory, at most 64.
   * @param for_ps Picoseconds of virtual time after the boundary that registers the wait, which
   *        posting creates at the next block boundary, at which it ends with reason time, None
   *        for no deadline.
   * @param deadline_ps As wait().
   * @return Resolves to a tuple of the StopInfo of the boundary that ended the wait and the
   *         mask of the members that held there, bit i for member i, or to the free run's own
   *         end.
   * @throws std::invalid_argument for no member or more than 64.
   * @throws std::runtime_error without an open free run.
   */
  [[= py::doc{
      "Wait on the free run for conditions, return a HostFuture of (StopInfo, mask)."}]] auto
      wait_when(
          core::condition_set          when,
          std::optional<std::uint64_t> for_ps,
          std::optional<std::uint64_t> deadline_ps) -> host_future_handle;

  /**
   * @brief Waits on the free run for a native predicate, at every slice end after the boundary
   *        that registers the wait, as wait() does for a Python one, with no GIL.
   * @param ready The predicate.
   * @param for_ps As wait().
   * @param deadline_ps As wait().
   * @return As wait().
   * @throws std::runtime_error without an open free run.
   */
  [[= py::doc{
      "Wait on the free run for a native predicate, return a HostFuture of a StopInfo."}]] auto
      wait_predicate(
          native_predicate const&      ready,
          std::optional<std::uint64_t> for_ps,
          std::optional<std::uint64_t> deadline_ps) -> host_future_handle;

  /**
   * @brief wait_predicate() for popcorn's own waits, such as a stream read, which must tell the
   *        free run's end from their predicate.
   * @param ready The predicate.
   * @param for_ps As wait().
   * @param deadline_ps As wait().
   * @return A HostFuture of a (StopInfo, ended) tuple, ended true when the free run's own end
   *         completed the wait.
   * @throws std::runtime_error without an open free run.
   */
  [[= py::doc{
      "wait_predicate() returning (StopInfo, whether the free run's end completed it)."}]] auto
      wait_predicate_ended(
          native_predicate const&      ready,
          std::optional<std::uint64_t> for_ps,
          std::optional<std::uint64_t> deadline_ps) -> host_future_handle;

  /**
   * @brief wait_predicate_ended() also returning when the wait would have ended on time.
   * @param ready The predicate.
   * @param for_ps As wait().
   * @return A HostFuture of a (StopInfo, ended, deadline_ps) tuple, deadline_ps the virtual time
   *         in picoseconds @p for_ps past the boundary that registered the wait, None when the
   *         free run ended before one did.
   * @throws std::runtime_error without an open free run.
   */
  [[= py::doc{"wait_predicate_ended() returning (StopInfo, ended, deadline_ps)."}]] auto
      wait_predicate_timed(native_predicate const& ready, std::uint64_t for_ps)
          -> host_future_handle;

  /**
   * @brief Starts a goal that stops when a native predicate holds, evaluated after every event
   *        and at slice ends with no GIL, or after @p for_ps.
   * @param ready The predicate.
   * @param budget Stop with reason budget past this many instructions, None for none.
   * @param for_ps Picoseconds of virtual time from now, None for none.
   * @param deadline_ps As run_goal().
   * @return Resolves to the StopInfo.
   * @throws machine_busy while a goal or a free run is active.
   * @throws std::invalid_argument for both @p for_ps and @p deadline_ps.
   */
  [[= py::doc{"Run until a native predicate holds, return a HostFuture of a StopInfo."}]] auto
      run_predicate(
          native_predicate const&      ready,
          std::optional<std::uint64_t> budget,
          std::optional<std::uint64_t> for_ps,
          std::optional<std::uint64_t> deadline_ps) -> host_future_handle;

  /**
   * @brief wait() for a Python predicate called only at the boundaries where a native gate
   *        holds, so a boundary where it does not takes no GIL.
   * @param gate Evaluated first, with no GIL.
   * @param ready Called on the host thread in a copy of the caller's context when @p gate holds.
   * @param for_ps As wait().
   * @param deadline_ps As wait().
   * @return As wait().
   * @throws std::runtime_error without an open free run.
   */
  [[= py::doc{"wait() calling the Python predicate only where the native gate holds."}]] auto
      wait_gated(
          native_predicate const&      gate,
          reflex::py::nb::object       ready,
          std::optional<std::uint64_t> for_ps,
          std::optional<std::uint64_t> deadline_ps) -> host_future_handle;

  /**
   * @brief run_predicate() for a Python predicate called only where a native gate holds.
   * @param gate Evaluated first, with no GIL.
   * @param ready Called on the host thread in a copy of the caller's context when @p gate holds.
   * @param budget As run_predicate().
   * @param for_ps As run_predicate().
   * @param deadline_ps As run_goal().
   * @return Resolves to the StopInfo.
   * @throws machine_busy while a goal or a free run is active.
   * @throws std::invalid_argument for both @p for_ps and @p deadline_ps.
   */
  [[= py::doc{
      "run_predicate() calling the Python predicate only where the native gate holds."}]] auto
      run_gated(
          native_predicate const&      gate,
          reflex::py::nb::object       ready,
          std::optional<std::uint64_t> budget,
          std::optional<std::uint64_t> for_ps,
          std::optional<std::uint64_t> deadline_ps) -> host_future_handle;

  /** @brief A predicate true while the next instruction is at @p at. */
  [[= py::doc{"Predicate: pc is at an address."}]] [[nodiscard]] auto
      predicate_pc_at(address at) const -> native_predicate;

  /**
   * @brief Whether an access lies inside one mapped memory region, not a peripheral window.
   *        Regions are fixed at construction, so it never locks.
   * @param addr First byte.
   * @param size Access size in bytes, 1 to 8.
   * @return True for plain memory.
   * @throws std::invalid_argument for a size outside 1 to 8.
   */
  [[= py::doc{"Whether [addr, addr + size) is plain memory, not a peripheral or unmapped."}]]
      [[nodiscard]] auto is_plain_memory(address addr, std::uint8_t size) const -> bool;

  /** @brief Whether a goal is active or a free run is open, even one that ended by itself.
   * True on the machine thread, false inside a parked section of this machine. */
  [[= py::doc{"Whether a goal is active or a free run is open, until halt()."}]] [[nodiscard]] auto
      busy() const -> bool;

  /** @brief Whether start() was called and halt() not yet. */
  [[= py::doc{"Whether start() was called and halt() not yet."}]] [[nodiscard]] auto
      session_open() const -> bool;

  /**
   * @brief Runs @p body as one command on the machine: at once on this thread while parked, at
   *        the next block boundary on the machine thread while running.
   * @param body Called with no argument, as a callback of the machine, in the context of the
   *        thread that runs it: a body that needs the caller's context enters it itself, as a
   *        `Relay` of the machine's callback table does, where the cycle collector sees it. An
   *        exception it raises is detached from its frames on every thread, parked too: the
   *        future keeps it natively, out of the collector's sight.
   * @param at_ps Virtual time since reset the command waits for, run at the first block
   *        boundary at or after it, None for the next boundary.
   * @return Resolves to the virtual time it ran at in picoseconds, or fails with what @p body
   *         raised. Cancelling it withdraws a command not yet run.
   * @throws std::invalid_argument for @p at_ps before the time of a parked machine.
   * @throws machine_running from a callback of another machine holding its lock.
   */
  [[= py::doc{
      "Run body() as one command at a block boundary, return a HostFuture of its time."}]] auto
      submit(reflex::py::nb::object body, std::optional<std::uint64_t> at_ps) -> host_future_handle;

  /**
   * @brief Calls @p fn synchronously with the machine parked, or inline from a callback of it.
   * @throws machine_running while the machine runs.
   */
  [[= py::doc{"Call fn() with the machine parked or from its callback, MachineRunning else."}]] auto
      parked(reflex::py::nb::object fn) -> reflex::py::nb::object;

  /**
   * @brief Bridges @p link to a new host pty, pumped on the machine thread around every slice,
   *        and polled while a free run idles. A parked machine leaves host input unread.
   * @param name Key of the bridge in Board.ptys, named in its overflow warning.
   * @param link The UART.
   * @throws config_error once the machine is destroyed.
   */
  [[= py::doc{"Bridge a UART to a new host pty pumped by the machine thread."}]] auto
      open_uart_pty(std::string name, uart_queue const& link) -> pty_handle;

  /**
   * @brief As open_uart_pty(), for a CDC ACM function of a virtual USB host.
   * @param name Key of the bridge in Board.ptys, named in its overflow warning.
   * @param link The function.
   * @throws config_error once the machine is destroyed.
   */
  [[= py::doc{"Bridge a CDC ACM function to a new host pty pumped by the machine thread."}]] auto
      open_cdc_pty(std::string name, cdc_link const& link) -> pty_handle;

  /** @brief Whether the calling thread owns the engine: the host thread in a slice, a caller in
   * its parked section. */
  [[= py::doc{"Whether the calling thread owns the engine: a callback of this machine."}]]
      [[nodiscard]] auto owns_engine() const noexcept -> bool;

  /**
   * @brief Writes bytes into guest memory, as the debugger.
   * @param at First guest address.
   * @param bytes Bytes to write.
   * @throws config_error naming the address when the range leaves the 32-bit address space.
   * @throws emulation_error naming the failing address on a bus error.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Write bytes into guest memory as the debugger."}]] void
      write_memory(address at, reflex::py::nb::bytes const& bytes);

  /**
   * @brief Reads bytes of guest memory, as the debugger.
   * @param at First guest address.
   * @param size Byte count.
   * @throws config_error naming the address when the range leaves the 32-bit address space.
   * @throws emulation_error naming the failing address on a bus error.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Read bytes of guest memory as the debugger."}]] [[nodiscard]] auto
      read_memory(address at, std::uint32_t size) const -> reflex::py::nb::bytes;

  /**
   * @brief Writes the bits of @p value that @p mask selects into guest memory, reading and
   *        writing in one access, as the debugger. Meant for RAM: on a peripheral the read runs
   *        the model's read effects.
   * @param at First guest address.
   * @param mask Bits written, one byte per byte of memory.
   * @param value Bytes whose masked bits are written, as long as @p mask.
   * @throws config_error for lengths that differ or a range outside the 32-bit address space.
   * @throws emulation_error naming the failing address on a bus error.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Write the masked bits of value into guest memory as the debugger."}]] void
      write_memory_masked(
          address                      at,
          reflex::py::nb::bytes const& mask,
          reflex::py::nb::bytes const& value);

  /**
   * @brief The bytes of the NUL-terminated string at @p at, without the NUL, at most @p limit
   *        bytes, read 64-byte block by block and byte by byte where a block meets a bus error.
   * @throws emulation_error when the bytes before a NUL meet a bus error.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{
      "Bytes of the NUL-terminated string at an address, at most limit."}]] [[nodiscard]] auto
      cstring(address at, std::uint32_t limit) const -> reflex::py::nb::bytes;

  /** @brief Names of the core registers, in the arch module's order. */
  [[= py::doc{"Names of the core registers, in the arch module's order."}]] [[nodiscard]] auto
      register_names() const -> std::vector<std::string>;

  /**
   * @brief Reads a core register by its index in register_names().
   * @param index Register index.
   * @throws config_error when the index is out of range.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Read a core register by its index in register_names()."}]] [[nodiscard]] auto
      read_register(std::uint32_t index) const -> std::uint64_t;

  /**
   * @brief Writes a core register by its index in register_names().
   * @param index Register index.
   * @param value Value, at most the register's width.
   * @throws config_error when the index is out of range or the value is too wide.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Write a core register by its index in register_names()."}]] void
      write_register(std::uint32_t index, std::uint64_t value);

  /**
   * @brief Stored value of a peripheral register, without read effects.
   * @param peripheral Peripheral name.
   * @param offset Byte offset of the register.
   * @param width Register width in bits, 8, 16 or 32.
   * @throws config_error when the peripheral is unknown or the register falls outside it.
   */
  [[= py::doc{"Stored value of a peripheral register, without read effects."}]] [[nodiscard]] auto
      peek(std::string const& peripheral, std::uint32_t offset, std::uint8_t width)
          -> std::uint32_t;

  /**
   * @brief Stores a peripheral register value, without write effects.
   * @param peripheral Peripheral name.
   * @param offset Byte offset of the register.
   * @param width Register width in bits, 8, 16 or 32.
   * @param value Value, at most @p width bits.
   * @throws config_error when the peripheral is unknown, the register falls outside it or the
   *         value is too wide.
   */
  [[= py::doc{"Store a peripheral register value, without write effects."}]] void poke(
      std::string const& peripheral,
      std::uint32_t      offset,
      std::uint8_t       width,
      std::uint32_t      value);

  /**
   * @brief Stores the bits of a peripheral register value that @p mask selects, without write
   *        effects, reading and writing the stored word in one access.
   * @param peripheral Peripheral name.
   * @param offset Byte offset of the register.
   * @param width Register width in bits, 8, 16 or 32.
   * @param mask Bits stored, at most @p width bits.
   * @param value Value whose masked bits are stored, at most @p width bits.
   * @throws config_error as poke().
   */
  [[= py::doc{"Store the masked bits of a peripheral register, without write effects."}]] void
      poke_masked(
          std::string const& peripheral,
          std::uint32_t      offset,
          std::uint8_t       width,
          std::uint32_t      mask,
          std::uint32_t      value);

  /**
   * @brief Writes one field of a peripheral register over the bus as the debugger, so the model
   *        and the description's write effects act. The other bits come from the stored word,
   *        read without read effects, each sibling field set to the value its write effect
   *        ignores: the stored value under modify, 0 under the one-to effects, all ones under
   *        the zero-to effects, 0 for a write-only field, as a read returns. A sibling whose
   *        every write acts, clear or set, gets its stored value, and the first such write to
   *        the register that the machine applies logs a model warning.
   * @param peripheral Peripheral name.
   * @param offset Byte offset of the register.
   * @param width Register width in bits, 8, 16 or 32.
   * @param mask Bits of the field, at most @p width bits.
   * @param value Value whose masked bits are written, at most @p width bits.
   * @throws config_error as poke().
   * @throws BusAccessError when the bus refuses the write.
   */
  [[= py::doc{"Write a peripheral register field over the bus, siblings left neutral."}]] void
      write_field(
          std::string const& peripheral,
          std::uint32_t      offset,
          std::uint8_t       width,
          std::uint32_t      mask,
          std::uint32_t      value);

  /** @brief read_memory() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of read_memory."}]] [[nodiscard]] auto
      op_read_memory(address at, std::uint32_t size) const -> access_op;
  /** @brief write_memory() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of write_memory."}]] [[nodiscard]] auto
      op_write_memory(address at, reflex::py::nb::bytes const& bytes) const -> access_op;
  /** @brief write_memory_masked() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of write_memory_masked."}]] [[nodiscard]] auto op_memory_masked(
      address                      at,
      reflex::py::nb::bytes const& mask,
      reflex::py::nb::bytes const& value) const -> access_op;
  /** @brief cstring() as an op of submit_ops(). */
  [[= py::doc{"Op of cstring."}]] [[nodiscard]] auto
      op_cstring(address at, std::uint32_t limit) const -> access_op;
  /** @brief read_register() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of read_register."}]] [[nodiscard]] auto
      op_read_register(std::uint32_t index) const -> access_op;
  /** @brief write_register() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of write_register."}]] [[nodiscard]] auto
      op_write_register(std::uint32_t index, std::uint64_t value) const -> access_op;
  /** @brief peek() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of peek."}]] [[nodiscard]] auto
      op_peek(std::string const& peripheral, std::uint32_t offset, std::uint8_t width) const
      -> access_op;
  /** @brief poke_masked() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of poke_masked."}]] [[nodiscard]] auto op_poke_masked(
      std::string const& peripheral,
      std::uint32_t      offset,
      std::uint8_t       width,
      std::uint32_t      mask,
      std::uint32_t      value) const -> access_op;
  /** @brief write_field() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of write_field."}]] [[nodiscard]] auto op_write_field(
      std::string const& peripheral,
      std::uint32_t      offset,
      std::uint8_t       width,
      std::uint32_t      mask,
      std::uint32_t      value) const -> access_op;
  /** @brief pc() as an op of submit_ops(). */
  [[= py::doc{"Op of pc."}]] [[nodiscard]] auto op_pc() const -> access_op;
  /** @brief instructions() as an op of submit_ops(). */
  [[= py::doc{"Op of instructions."}]] [[nodiscard]] auto op_instructions() const -> access_op;
  /** @brief pending_events() as an op of submit_ops(). */
  [[= py::doc{"Op of pending_events."}]] [[nodiscard]] auto op_pending_events() const -> access_op;
  /** @brief gpio_level() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of gpio_level."}]] [[nodiscard]] auto
      op_gpio_level(std::string const& peripheral, std::uint8_t pin) const -> access_op;
  /** @brief gpio_drive() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of gpio_drive."}]] [[nodiscard]] auto op_gpio_drive(
      std::string const&  peripheral,
      std::uint8_t        pin,
      std::optional<bool> level) const -> access_op;
  /** @brief gpio_mode() as an op of submit_ops(), its value the mode's integer, checked now. */
  [[= py::doc{"Op of gpio_mode, its integer value."}]] [[nodiscard]] auto
      op_gpio_mode(std::string const& peripheral, std::uint8_t pin) const -> access_op;
  /** @brief gpio_alternate() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of gpio_alternate."}]] [[nodiscard]] auto
      op_gpio_alternate(std::string const& peripheral, std::uint8_t pin) const -> access_op;
  /** @brief adc_input() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of adc_input."}]] [[nodiscard]] auto
      op_adc_input(std::string const& peripheral, std::uint8_t channel) const -> access_op;
  /**
   * @brief A command holding an I2C bus as another controller would, i2c_bus::hold, checked
   *        now. Its value is the hold's start in picoseconds.
   * @param peripheral I2C instance name.
   * @param at_ps Start, machine time since reset, empty for the time the command runs at.
   * @param for_ps Length, at least 1.
   * @throws config_error now, when the peripheral's model is no built-in I2C.
   * @throws std::invalid_argument now for a length of 0 or an end past the time range, when
   *         the command runs for a start already past.
   */
  [[= py::doc{"Command holding an I2C bus, its value the start."}]] [[nodiscard]] auto
      command_i2c_hold(
          std::string const&           peripheral,
          std::optional<std::uint64_t> at_ps,
          std::uint64_t                for_ps) const -> access_op;
  /**
   * @brief A command queuing an SPI transfer of the instance's controller,
   *        spi_controller_port::transfer, checked when it runs. Its value is the transfer's
   *        start in picoseconds.
   * @param peripheral SPI instance name.
   * @param at_ps Earliest start, machine time since reset, empty for the time the command runs at.
   * @param frame_ps Length of one frame.
   * @param bits Frame size, 4 to 32.
   * @param frames The frames.
   * @throws config_error now, when the peripheral's model is no built-in SPI, when the command
   *         runs with no controller attached or the queue full.
   * @throws std::invalid_argument when the command runs, for a start already past or an
   *         argument out of range.
   */
  [[= py::doc{
      "Command queuing an SPI controller transfer, its value the start."}]] [[nodiscard]] auto
      command_spi_transfer(
          std::string const&           peripheral,
          std::optional<std::uint64_t> at_ps,
          std::uint64_t                frame_ps,
          std::uint8_t                 bits,
          std::vector<std::uint32_t>   frames) const -> access_op;
  /**
   * @brief A command scheduling edges of a GPIO pin, gpio_port::drive_at for each, checked now
   *        and when it runs: it schedules every edge or none.
   * @param peripheral GPIO port name.
   * @param pin Pin number, 0 to 15.
   * @param at_ps Edge times in picoseconds, machine time since reset, sorted, 1 to
   *        periph::stm32::max_pending_edges of them.
   * @param levels Level of each edge, 0 or 1, as many as @p at_ps.
   * @throws config_error now, when the port is unknown or the pin out of range, when the
   *         command runs and the port would keep more than periph::stm32::max_pending_edges.
   * @throws model_error now, when the peripheral's model is not a GPIO port.
   * @throws std::invalid_argument now for lists of another length, unsorted times or a level
   *         above 1, when the command runs for a first edge already past.
   */
  [[= py::doc{"Command scheduling edges of a GPIO pin."}]] [[nodiscard]] auto command_gpio_edges(
      std::string const&         peripheral,
      std::uint8_t               pin,
      std::vector<std::uint64_t> at_ps,
      std::vector<std::uint8_t>  levels) const -> access_op;
  /** @brief adc_drive() as an op of submit_ops(), checked now. */
  [[= py::doc{"Op of adc_drive."}]] [[nodiscard]] auto op_adc_drive(
      std::string const& peripheral,
      std::uint8_t       channel,
      std::uint32_t      microvolts) const -> access_op;

  /**
   * @brief Runs @p ops in order as one command: at once on this thread while parked, at the
   *        next block boundary on the machine thread while running, with no GIL. Stops at the
   *        first op that fails.
   * @param ops The ops.
   * @param notes Note added to the error of the op at the same index.
   * @param at_ps Virtual time since reset the command waits for, None for the next boundary.
   * @return Resolves to (time in picoseconds, list of raw values), or fails with the error of the
   *         failing op, its note added. Cancelling it withdraws a command not yet run.
   * @throws std::invalid_argument for @p at_ps before the time of a parked machine, or notes
   *         not one per op.
   * @throws machine_running from a callback of another machine holding its lock.
   */
  [[= py::doc{
      "Run ops as one command at a block boundary, return a HostFuture of (time, values)."}]] auto
      submit_ops(
          std::vector<access_op>       ops,
          std::vector<std::string>     notes,
          std::optional<std::uint64_t> at_ps) -> host_future_handle;

  /**
   * @brief Takes a snapshot.
   * @throws machine_running while a slice runs, handlers included.
   * @throws machine_busy during a call.
   */
  [[= py::doc{"Take a snapshot of the whole machine state."}]] [[nodiscard]] auto snapshot() const
      -> snapshot_handle;

  /**
   * @brief Restores a snapshot of this machine, or one read back from bytes.
   * @param saved The snapshot.
   * @throws config_error when a read-back snapshot names another description, images or config.
   * @throws emulation_error when @p saved was taken by another machine.
   */
  [[= py::doc{"Restore a snapshot this machine took, or one read back from bytes."}]] void
      restore(snapshot_handle const& saved);

  /**
   * @brief Begins a guest function call, saving the interrupted context.
   * @param frame The call.
   * @throws machine_busy when a call is already in progress.
   * @throws config_error when the arch module refuses the frame.
   */
  [[= py::doc{"Begin a guest function call, saving the interrupted context."}]] void
      begin_call(arch::call_frame const& frame);

  /**
   * @brief Ends the call, restoring the interrupted context.
   * @return Return value bits, 0 when the call did not return.
   * @throws config_error when no call is in progress.
   */
  [[= py::doc{"End the call, restore the interrupted context, return the value bits."}]] auto
      end_call() -> std::uint64_t;

  /** @brief Whether a call is in progress. */
  [[= py::doc{"Whether a call begun by begin_call has not been ended."}]] [[nodiscard]] auto
      in_call() const -> bool;

  /** @brief Events waiting in the machine's event queue. */
  [[= py::doc{
      "Events waiting in the event queue, model and core events alike."}]] [[nodiscard]] auto
      pending_events() const -> std::size_t;

  /**
   * @brief Intercepts calls to an address.
   * @param at Function entry.
   * @param handler Called with an InterceptCall, returns an InterceptResult.
   * @param once Only the first arrival.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Intercept calls to an address, handler(InterceptCall) -> InterceptResult."}]] auto
      add_intercept(address at, reflex::py::nb::object handler, bool once) -> hook;

  /**
   * @brief Counts arrivals at an address with no handler and no GIL, as core::machine::add_probe.
   *        Parked or from a callback of this machine between slices, it is added at once. While
   *        the machine runs, it is added on the machine's thread at the next block boundary and
   *        this call waits for it, without the GIL.
   * @param at Code address.
   * @throws machine_running from a handler of this machine during a slice.
   * @throws std::runtime_error from a callback of another machine.
   */
  [[= py::doc{"Count arrivals at an address, added at the next boundary while running."}]] auto
      add_probe(address at) -> probe_hook;

  /**
   * @brief Removes a probe as add_probe() adds one: at once while parked, at the next block
   *        boundary while the machine runs, this call waiting for it. Its counts stay readable.
   *        While the machine runs the probe turns inactive at the call, and a signal that
   *        interrupts the wait still removes it at the next boundary.
   * @param probe The probe.
   * @throws machine_running from a handler of this machine during a slice.
   * @throws std::runtime_error from a callback of another machine.
   */
  [[= py::doc{"Remove a probe, at the next boundary while running."}]] void
      remove_probe(probe_hook const& probe);

  /** @brief A predicate true once a probe counted an arrival. */
  [[= py::doc{"Predicate: the probe counted an arrival."}]] [[nodiscard]] auto
      predicate_probe_hit(probe_hook const& probe) const -> native_predicate;

  /**
   * @brief Calls a handler when a function returns.
   * @param at Function entry.
   * @param args Kind of each argument captured at entry.
   * @param returns Kind of the return value.
   * @param handler Called with a ReturnedCall.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Call handler(ReturnedCall) when the function at an address returns."}]] auto
      add_return_hook(
          address                              at,
          std::vector<arch::value_kind> const& args,
          arch::value_kind                     returns,
          reflex::py::nb::object               handler) -> hook;

  /**
   * @brief Appends each return of a function to a log, on the machine thread without the GIL.
   * @param at Function entry.
   * @param args Kind of each argument captured at entry.
   * @param returns Kind of the return value.
   * @param log The log, shared with the caller.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Append each return of the function at an address to log, with no GIL."}]] auto
      add_return_log(
          address                              at,
          std::vector<arch::value_kind> const& args,
          arch::value_kind                     returns,
          return_log const&                    log) -> hook;

  /**
   * @brief Watches accesses to an address range.
   * @param at First address.
   * @param size Bytes, at least 1.
   * @param kind Accesses reported.
   * @param handler Called with a WatchHit, returns a WatchAction.
   * @throws config_error when the range is empty, leaves the address space or reaches nothing.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Watch an address range, handler(WatchHit) -> WatchAction."}]] auto add_watch(
      address                at,
      std::uint32_t          size,
      core::watch_kind       kind,
      reflex::py::nb::object handler) -> hook;

  /**
   * @brief Python model of a peripheral.
   * @param peripheral Peripheral name.
   * @return The model object, None when the peripheral has no model or a native one.
   * @throws config_error when the description has no such peripheral.
   */
  [[= py::doc{"Python model of a peripheral, None when it has none."}]] [[nodiscard]] auto
      model(std::string const& peripheral) const -> reflex::py::nb::object;

  /**
   * @brief Names of the peripherals whose model is a built-in UART, in description order.
   */
  [[= py::doc{"Names of the peripherals whose model is a built-in UART."}]] [[nodiscard]] auto
      uart_names() const -> std::vector<std::string>;

  /**
   * @brief Connects a UART of the machine to Python, replacing its far end.
   * @param peripheral UART instance name.
   * @throws config_error when the description has no such peripheral.
   * @throws model_error when the peripheral's model is not a UART.
   */
  [[= py::doc{"Connect a UART to Python, replacing its far end."}]] [[nodiscard]] auto
      attach_uart(std::string const& peripheral) -> uart_queue;

  /**
   * @brief Names of the peripherals whose model is a built-in SAI, in description order.
   */
  [[= py::doc{"Names of the peripherals whose model is a built-in SAI."}]] [[nodiscard]] auto
      audio_names() const -> std::vector<std::string>;

  /**
   * @brief Connects an SAI audio block of the machine to Python, replacing its attachment.
   * @param peripheral SAI instance name.
   * @param block Audio block, `A` or `B`.
   * @param decimation Frame time multiplier, 1 for the configured rate.
   * @param external_frame_hz Frame rate of an external clock master, 0 for none.
   * @param clock_wired A bus device drives the external clock at run time.
   * @throws config_error when the description has no such peripheral or @p block is neither
   *         `A` nor `B`.
   * @throws model_error when the peripheral's model is not a built-in SAI.
   */
  [[= py::doc{
      "Connect an SAI audio block to Python, replacing its attachment."}]] [[nodiscard]] auto
      attach_audio(
          std::string const& peripheral,
          std::string const& block,
          std::uint32_t      decimation,
          std::uint32_t      external_frame_hz,
          bool               clock_wired) -> audio_link;

  /**
   * @brief Names of the peripherals whose built-in model has a virtual USB host, in description
   *        order.
   */
  [[= py::doc{
      "Names of the peripherals whose built-in model has a virtual USB host."}]] [[nodiscard]] auto
      usb_names() const -> std::vector<std::string>;

  /**
   * @brief Connects the virtual USB host of a device controller to Python.
   * @param peripheral Controller instance name.
   * @throws config_error when the description has no such peripheral.
   * @throws model_error when the peripheral's model has no virtual USB host.
   */
  [[= py::doc{"Connect the virtual USB host of a device controller to Python."}]] [[nodiscard]] auto
      attach_usb(std::string const& peripheral) -> usb_link;

  /**
   * @brief Drives a GPIO pin from outside the MCU, or releases it.
   * @param peripheral GPIO port name.
   * @param pin Pin number, 0 to 15.
   * @param level Level driven, empty to release the pin.
   * @throws config_error when the port is unknown or the pin out of range.
   * @throws model_error when the peripheral's model is not a GPIO port.
   */
  [[= py::doc{"Drive a GPIO pin from outside, None to release it."}]] void
      gpio_drive(std::string const& peripheral, std::uint8_t pin, std::optional<bool> level);

  /**
   * @brief Level of a GPIO pin as its input data register reads it.
   * @param peripheral GPIO port name.
   * @param pin Pin number, 0 to 15.
   * @throws config_error when the port is unknown or the pin out of range.
   * @throws model_error when the peripheral's model is not a GPIO port.
   */
  [[= py::doc{"Level of a GPIO pin as the input data register reads it."}]] [[nodiscard]] auto
      gpio_level(std::string const& peripheral, std::uint8_t pin) const -> bool;

  /**
   * @brief Mode of a GPIO pin.
   * @param peripheral GPIO port name.
   * @param pin Pin number, 0 to 15.
   * @throws config_error when the port is unknown or the pin out of range.
   * @throws model_error when the peripheral's model is not a GPIO port.
   */
  [[= py::doc{"Mode of a GPIO pin."}]] [[nodiscard]] auto
      gpio_mode(std::string const& peripheral, std::uint8_t pin) const -> periph::stm32::pin_mode;

  /**
   * @brief Alternate function number of a GPIO pin.
   * @param peripheral GPIO port name.
   * @param pin Pin number, 0 to 15.
   * @throws config_error when the port is unknown or the pin out of range.
   * @throws model_error when the peripheral's model is not a GPIO port.
   */
  [[= py::doc{"Alternate function number of a GPIO pin."}]] [[nodiscard]] auto
      gpio_alternate(std::string const& peripheral, std::uint8_t pin) const -> std::uint8_t;

  /**
   * @brief Drives an external ADC channel from outside the MCU.
   * @param peripheral ADC name.
   * @param channel External channel, 1 to 17 on ADC1.
   * @param microvolts Input voltage in microvolts.
   * @throws config_error when the ADC is unknown or the channel is not external.
   * @throws model_error when the peripheral's model is not the built-in ADC.
   */
  [[= py::doc{"Drive an external ADC channel from outside, in microvolts."}]] void
      adc_drive(std::string const& peripheral, std::uint8_t channel, std::uint32_t microvolts);

  /**
   * @brief Voltage driven on an external ADC channel.
   * @param peripheral ADC name.
   * @param channel External channel, 1 to 17 on ADC1.
   * @return The input in microvolts, 0 when never driven.
   * @throws config_error when the ADC is unknown or the channel is not external.
   * @throws model_error when the peripheral's model is not the built-in ADC.
   */
  [[= py::doc{"Voltage driven on an external ADC channel, in microvolts."}]] [[nodiscard]] auto
      adc_input(std::string const& peripheral, std::uint8_t channel) const -> std::uint32_t;

  /**
   * @brief Code ranges of functions, for coverage and traces.
   * @param names Function names.
   * @throws symbol_error when no loaded image has a name.
   */
  [[= py::doc{"Code range (name, start, size) of each function, in order."}]] [[nodiscard]] auto
      function_ranges(std::vector<std::string> const& names) const
      -> std::vector<detail::range_row>;

  /**
   * @brief Starts collecting coverage counters, adding ranges to those already counted.
   * @param ranges Code ranges, empty for every executable segment of the loaded images.
   * @throws config_error when a range is empty or leaves the 32-bit address space.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Start collecting coverage over ranges, all code when empty."}]] void
      enable_coverage(std::vector<detail::range_row> const& ranges);

  /**
   * @brief Stops collecting, keeping the counters.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Stop collecting coverage, keeping the counters."}]] void disable_coverage();

  /** @brief Whether coverage is being collected. */
  [[= py::doc{"Whether coverage is being collected."}]] [[nodiscard]] auto coverage_enabled() const
      -> bool;

  /**
   * @brief Zeroes every counter.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Zero every coverage counter, ranges kept."}]] void clear_coverage();

  /**
   * @brief The coverage collected so far as `.pcov` bytes, with the GIL released.
   * @param elf_paths Absolute ELF path of each loaded image in load order, empty for a raw one.
   * @throws config_error when the count differs from the loaded images.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Coverage so far as .pcov bytes, one ELF path or '' per loaded image."}]] auto
      coverage_pcov(std::vector<std::string> const& elf_paths) -> reflex::py::nb::bytes;

  /**
   * @brief Streams trace records of some kinds into a `.pctr` file until the handle closes.
   * @param path File.
   * @param kinds Trace kind values, neither str nor time.
   * @param ranges Code ranges traced, empty for everywhere.
   * @throws config_error for an unknown kind or an invalid range.
   * @throws format_error when the file cannot be created.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Stream trace records of kinds into a .pctr file until closed."}]] auto open_trace(
      std::string const&                    path,
      std::vector<std::uint8_t> const&      kinds,
      std::vector<detail::range_row> const& ranges) -> trace_handle;

  /**
   * @brief Writes the always-on trace ring to a `.pctr` file, oldest record first.
   * @param path File.
   * @throws format_error when the file cannot be written.
   */
  [[= py::doc{"Write the trace ring to a .pctr file."}]] void
      write_ring(std::string const& path) const;

  /** @brief Drops every record of the trace ring, the next ones counting from now. */
  [[= py::doc{"Drop every record of the trace ring."}]] void clear_ring();

  /**
   * @brief Chooses whether executed blocks append block records to the trace ring.
   * @param on Whether the ring keeps block records.
   * @throws machine_running while the machine runs, handlers included.
   */
  [[= py::doc{"Choose whether executed blocks append BLOCK records to the trace ring."}]] void
      set_ring_blocks(bool on);

  /**
   * @brief Whether executed blocks append block records to the trace ring.
   * @throws machine_running while the machine runs, outside its handlers.
   */
  [[= py::doc{"Whether the trace ring keeps BLOCK records."}]] [[nodiscard]] auto
      ring_blocks() const -> bool;

  /**
   * @brief Forwards the machine's log records to Python. The machine's thread queues each record
   *        with its virtual time and no GIL, at most 4096 of them, later ones counted. A thread
   *        holding the GIL and no host mutex forwards the queue in order, through @p emit: the
   *        loop a request of this machine was last awaited on, woken through its LoopWaker with
   *        the log token, and every caller at its next completed request, halt, park, parked
   *        access or parked section. Past the bound one record reports the dropped count.
   * @param emit Called with a LogRecord and its virtual time in picoseconds. An exception it
   *        raises leaves the flush, the records after it kept for the next one. Raised from an
   *        await, halt() or park() with no end, sent to sys.unraisablehook after a parked access,
   *        kept for the next raising flush by flush_logs(). Each flush forwards the records
   *        queued when it began, a flush nested in a handler forwards nothing. Called once, at
   *        construction.
   */
  [[= py::doc{"Forward each queued log record to emit(LogRecord, time_ps), in order."}]] void
      forward_logs(reflex::py::nb::object emit);

  /**
   * @brief Forwards the queued log records now, as forward_logs() describes, unless the thread
   *        is a host thread or holds a host mutex. A log filter or handler error is kept and
   *        raised by the machine's next await, halt() or park().
   */
  [[= py::doc{"Forward the queued log records now, keeping an error for the next await."}]] void
      flush_logs();

  /** @brief Token the machine's log queue wakes a loop with: log_token_bit | id(). */
  [[= py::doc{"Token the machine's log queue wakes a loop with."}]] [[nodiscard]] auto
      log_token() const noexcept -> std::uint64_t;

  /**
   * @brief Times the machine's thread entered the GIL: the first creates its Python thread
   *        state, each later one runs a Python callback or releases a Python reference.
   */
  [[= py::doc{"Times the machine thread took the GIL, for tests."}]] [[nodiscard]] auto
      gil_entries() const noexcept -> std::uint64_t;

  /** @brief Identity of the machine, never 0. */
  [[= py::doc{"Identity of the machine, never 0."}]] [[nodiscard]] auto id() const noexcept
      -> std::uint64_t;

  /** @brief Instructions executed since reset. */
  [[= py::doc{"Instructions executed since reset."}]] [[nodiscard]] auto instructions() const
      -> std::uint64_t;

  /** @brief Virtual time since reset, in picoseconds. */
  [[= py::doc{"Virtual time since reset, in picoseconds."}]] [[nodiscard]] auto time_ps() const
      -> std::uint64_t;

  /**
   * @brief The value a function returning now leaves in its result registers,
   *        core::machine::returned_value().
   * @param returns Kind of the value.
   * @param abi Float ABI of the function.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{
      "Bits of the value a function returning now leaves, of kind returns."}]] [[nodiscard]] auto
      result_bits(arch::value_kind returns, arch::float_abi abi) const -> std::uint64_t;

  /**
   * @brief Address of the next instruction.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Address of the next instruction."}]] [[nodiscard]] auto pc() const -> address;

  /** @brief The native machine, for its fixed data only: the host owns it. */
  [[= py::skip]] [[nodiscard]] auto native() const noexcept -> core::machine&;

  /** @brief The host that runs the machine. */
  [[= py::skip]] [[nodiscard]] auto host() const noexcept -> std::shared_ptr<core::host> const&;

  /** @brief The slot its views and links reach the host through. */
  [[= py::skip]] [[nodiscard]] auto slot() const noexcept -> detail::host_ref;

private:
  enum class wait_shape : std::uint8_t
  {
    stop,
    fired,
    ended,
    timed,
  };

  /**
   * @brief Refuses a caller at_boundary() cannot serve once it does not own the engine.
   * @throws std::runtime_error from a callback of another machine.
   * @throws core::machine_closed when the host was abandoned.
   */
  void require_boundary_caller() const;

  void at_boundary(std::move_only_function<void(core::machine&)> body);

  auto run_test(
      std::function<bool()>        test,
      std::optional<std::uint64_t> budget,
      std::optional<std::uint64_t> for_ps,
      std::optional<std::uint64_t> deadline_ps,
      std::shared_ptr<void const>  keep = {}) -> host_future_handle;

  auto waiting_on(
      core::waiter_spec            spec,
      std::optional<std::uint64_t> for_ps,
      std::optional<std::uint64_t> deadline_ps,
      wait_shape                   shape,
      std::shared_ptr<void const>  keep = {}) -> host_future_handle;

  [[nodiscard]] auto gpio(std::string const& peripheral, std::uint8_t pin) const
      -> periph::stm32::gpio_port&;
  [[nodiscard]] auto analog(std::string const& peripheral) const -> periph::stm32::analog_port&;
  [[nodiscard]] auto lock_for_request() const -> core::held_lock;
  [[nodiscard]] auto lock_releasing() const -> core::held_lock;
  [[nodiscard]] auto engine() const -> core::machine&;
  [[nodiscard]] auto abandoner() const -> detail::host_abandon;
  void               flush_logs_unraisable() const noexcept;
  auto open_pty(std::string name, std::unique_ptr<detail::pty_link> link) -> pty_handle;

  template <typename Body>
  auto access(Body&& body) const -> std::invoke_result_t<Body, core::machine&>
  {
    detail::logs_flushed const flushed{*slot_};
    return detail::host_access(*host_, *slot_, std::forward<Body>(body));
  }

  auto halt_or_park(bool refuse_goal) -> reflex::py::nb::object;

  [[= py::traverse]] void report_python(reflex::py::gc_visitor& visit) const noexcept
  {
    for(auto const owned : *python_owned_)
    {
      visit(owned);
    }
  }

  std::shared_ptr<std::vector<reflex::py::nb::handle>> python_owned_ =
      std::make_shared<std::vector<reflex::py::nb::handle>>();
  std::shared_ptr<std::atomic<bool>>          alive_;
  std::shared_ptr<detail::host_slot>          slot_;
  std::shared_ptr<detail::pty_bridges>        ptys_;
  std::shared_ptr<std::atomic<bool>>          thread_live_;
  std::shared_ptr<detail::delivered_end>      delivered_;
  std::shared_ptr<std::atomic<bool>>          collected_;
  std::shared_ptr<std::atomic<std::uint64_t>> gil_entries_;
  std::shared_ptr<core::machine>              engine_owner_;
  std::optional<hook>                         log_hook_;
  std::map<std::string, detail::usb_hubs>     hubs_;
  core::machine*                              engine_{};
  std::uint64_t                               id_{};
  std::shared_ptr<core::host>                 host_;
  mutable std::map<std::pair<std::string, std::uint32_t>, std::shared_ptr<std::atomic<bool>>>
      field_merge_logged_;
};
}
