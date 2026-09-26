#pragma once

#include "image.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>

#include <reflex/py.hpp>

#include <cstddef>
#include <cstdint>
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

  /** @brief The native machine. */
  [[= py::skip]] [[nodiscard]] auto native() const noexcept -> core::machine&;

private:
  std::unique_ptr<core::machine> machine_;
};
}
