#pragma once

#include "hooks.hpp"
#include "image.hpp"
#include "snapshot.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>

#include <reflex/py.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
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
};

/** @brief Bounds of one slice, core::slice_request without the predicate. */
struct[[= py::rename{"SliceArgs"}]] slice_args
{
  /** @brief Instructions after which the slice yields. */
  std::uint64_t max_instructions{1'000'000};
  /** @brief Wall time after which the slice yields, in nanoseconds. */
  std::int64_t wall_budget_ns{1'000'000};
  /** @brief Stop with reason address before executing this address. */
  std::optional<address> until{};
  /** @brief Instructions left in the run's budget, stop with reason budget past them. */
  std::optional<std::uint64_t> budget_left{};
  /** @brief Stop with reason budget after exactly this many instructions. */
  std::optional<std::uint64_t> exact_instructions{};
  /** @brief Stop with reason time on reaching this virtual time since reset, in picoseconds. */
  std::optional<std::uint64_t> until_time_ps{};
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

/** @brief Result of one slice. */
struct[[= py::rename{"SliceOutcome"}]] slice_outcome
{
  /** @brief The stop, empty when the slice only yielded. */
  [[= py::readonly]] std::optional<stop_record> stopped{};
  /** @brief Instructions executed in the slice. */
  [[= py::readonly]] std::uint64_t executed{};
};

/** @brief Owner of one native machine, the object `popcorn.Machine` wraps. */
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
  /** @brief Marks the machine gone for its Python models, then destroys it. */
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
   * @brief Runs one slice with the GIL released.
   * @param args Bounds.
   * @param stop_when Predicate evaluated at event boundaries and at the slice end, called with
   *        the GIL held, empty for none.
   * @throws machine_busy from inside a slice of this machine.
   * @throws Any exception the predicate raised, the machine stopped at an instruction boundary.
   */
  [[= py::doc{"Run one slice, the GIL released. stop_when runs at event boundaries."}]] auto
      run_slice(slice_args const& args, std::optional<std::function<bool()>> const& stop_when)
          -> slice_outcome;

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
      in_call() const noexcept -> bool;

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

  /** @brief The native machine. */
  [[= py::skip]] [[nodiscard]] auto native() const noexcept -> core::machine&;

private:
  std::shared_ptr<bool>          alive_;
  std::unique_ptr<core::machine> machine_;
};
}
