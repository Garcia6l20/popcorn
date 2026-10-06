#pragma once

#include "board.hpp"
#include "gil_locks.hpp"
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
#include <type_traits>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

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
};

namespace detail
{
/** @brief Takes the mutex of @p ptys, releasing the GIL while it blocks when the thread holds it.
 */
[[nodiscard]] auto lock_ptys(pty_bridges& ptys) -> std::unique_lock<std::mutex>;
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

  /** @brief Stops pumping and closes both ends of the pty. Idempotent. */
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
  machine_handle(mcu_description const& description, machine_config const& config);

  /** @brief Not copyable, models hold its lifetime flag. */
  machine_handle(machine_handle const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(machine_handle const&) -> machine_handle& = delete;
  /**
   * @brief Marks the machine gone for its views, halts, closes and joins the host with the GIL
   *        released, then destroys the machine. A free run's end never collected by halt() goes
   *        to sys.unraisablehook when the destructor holds the GIL, and is dropped otherwise. On
   *        a host thread or inside a parked section it takes the host's orphan path. While the
   *        interpreter finalizes with the host thread alive it leaves both alive: the thread
   *        needs the GIL to exit.
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
   * @param stop_when Predicate evaluated at event boundaries and slice ends, on the host thread
   *        in a copy of the caller's context, None for none.
   * @return Resolves to the StopInfo.
   * @throws machine_busy while a goal or a free run is active.
   * @throws machine_running from a callback or a parked section.
   */
  [[= py::doc{"Start a goal, return a HostFuture resolving to its StopInfo."}]] auto run_goal(
      std::optional<address>                until,
      std::optional<std::uint64_t>          budget,
      std::optional<std::uint64_t>          exact,
      std::optional<std::uint64_t>          for_ps,
      std::optional<reflex::py::nb::object> stop_when) -> host_future_handle;

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
   * @return Resolves to the CallOutcome.
   * @throws machine_busy while a goal or a free run is active, or a call is in progress.
   * @throws machine_running from a callback or a parked section.
   */
  [[= py::doc{
      "Start a guest call as one goal, return a HostFuture resolving to its CallOutcome."}]] auto
      call_goal(
          arch::call_frame const&               frame,
          std::optional<std::uint64_t>          budget,
          std::optional<reflex::py::nb::object> prepare) -> host_future_handle;

  /**
   * @brief Closes the free run, waiting with the GIL released until the machine parks. On a
   *        host thread it only requests the end. The check that no goal runs instead and the
   *        halt take the host mutex separately: a goal started by another thread in between is
   *        halted too, so one thread at a time starts and halts a machine.
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
   *         raised in it.
   * @throws machine_running inside a parked section.
   */
  [[= py::doc{"Cancel a goal or halt the free run, return what halt() returns."}]] auto park()
      -> reflex::py::nb::object;

  /**
   * @brief Waits on the free run.
   * @param ready Predicate called on the host thread at event boundaries and slice ends, in a
   *        copy of the caller's context, None for a deadline only.
   * @param for_ps Picoseconds of virtual time after the first boundary that evaluates the wait,
   *        at which it ends with reason time, None for no deadline.
   * @return Resolves to the StopInfo of the boundary that ended the wait, or to the free run's
   *         own end.
   * @throws std::runtime_error without an open free run.
   */
  [[= py::doc{"Wait on the free run, return a HostFuture resolving to a StopInfo."}]] auto
      wait(std::optional<reflex::py::nb::object> ready, std::optional<std::uint64_t> for_ps)
          -> host_future_handle;

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
   * @param body Called with no argument, as a callback of the machine, in a copy of the
   *        caller's context.
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
      open_uart_pty(std::string name, uart_link const& link) -> pty_handle;

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
      attach_uart(std::string const& peripheral) -> uart_link;

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
   * @brief Calls a Python callable with each machine log record, the GIL taken.
   * @param sink Called with a LogRecord.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Call sink(LogRecord) for each machine log record."}]] auto
      add_log_sink(reflex::py::nb::object sink) -> hook;

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
   * @brief Address of the next instruction.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Address of the next instruction."}]] [[nodiscard]] auto pc() const -> address;

  /** @brief The native machine, for its fixed data only: the host owns it. */
  [[= py::skip]] [[nodiscard]] auto native() const noexcept -> core::machine&;

private:
  [[nodiscard]] auto gpio(std::string const& peripheral, std::uint8_t pin) const
      -> periph::stm32::gpio_port&;
  [[nodiscard]] auto analog(std::string const& peripheral) const -> periph::stm32::analog_port&;
  [[nodiscard]] auto lock_for_request() const -> core::held_lock;
  [[nodiscard]] auto lock_releasing() const -> core::held_lock;
  auto open_pty(std::string name, std::unique_ptr<detail::pty_link> link) -> pty_handle;

  template <typename Body>
  auto access(Body&& body) const -> std::invoke_result_t<Body, core::machine&>
  {
    return detail::host_access(*host_, std::forward<Body>(body));
  }

  std::shared_ptr<std::atomic<bool>>      alive_;
  std::shared_ptr<detail::host_slot>      slot_;
  std::shared_ptr<detail::pty_bridges>    ptys_;
  std::shared_ptr<std::atomic<bool>>      thread_live_;
  std::map<std::string, detail::usb_hubs> hubs_;
  core::machine*                          engine_{};
  std::uint64_t                           id_{};
  std::shared_ptr<core::host>             host_;
};
}
