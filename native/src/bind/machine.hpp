#pragma once

#include "image.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>

#include <reflex/py.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
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
   * @throws config_error naming the address when a byte is not writable memory or the range
   *         leaves the 32-bit address space.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Write bytes into guest memory as the debugger."}]] void
      write_memory(address at, reflex::py::nb::bytes const& bytes);

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
  std::unique_ptr<core::machine> machine_;
};
}
