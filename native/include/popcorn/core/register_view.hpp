#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace popcorn::core
{
/**
 * @brief Whether a value fits a register of a given width.
 * @param value Register value, right-aligned.
 * @param bits Register width in bits, 1 to 64.
 */
constexpr auto fits_register(std::uint64_t value, std::uint8_t bits) noexcept -> bool
{
  return bits >= 64 or value < (std::uint64_t{1} << bits);
}

/**
 * @brief CPU registers of a stopped machine, those `arch::arch::registers()` lists, for test code
 *        and the Python facade. Reads and writes the engine directly: no
 *        exception entry, no masking side effect beyond what the engine itself applies.
 *
 * Valid while its machine lives. Every function but list() and find() throws machine_running
 * while a slice runs, except from a handler of that slice: handlers run while the core is paused
 * at an instruction boundary.
 */
class POPCORN_API register_view
{
public:
  /**
   * @brief Creates the view of a machine, as `machine::regs()` does.
   * @param target Machine whose registers the view reads and writes.
   */
  explicit register_view(machine& target) noexcept : machine_{&target}
  {}

  /** @brief Registers of the core, in snapshot order. */
  [[nodiscard]] auto list() const -> std::span<arch::cpu_register const>;

  /**
   * @brief Looks a register up by name.
   * @param name Name as `arch::cpu_register::name` spells it, such as `r0` or `basepri`.
   * @return The register, empty when the core has none of that name.
   */
  [[nodiscard]] auto find(std::string_view name) const -> std::optional<arch::cpu_register>;

  /**
   * @brief Reads a register.
   * @param reg Register from list() or find().
   * @return Value right-aligned to `reg.bits`.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[nodiscard]] auto read(arch::cpu_register const& reg) const -> std::uint64_t;

  /**
   * @brief Writes a register as a debugger does, through arch::arch::host_register_value: on
   *        Cortex-M a pc write keeps the Thumb state whatever bit 0 of @p value.
   * @param reg Register from list() or find().
   * @param value Value right-aligned to `reg.bits`.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  void write(arch::cpu_register const& reg, std::uint64_t value) const
      pre(fits_register(value, reg.bits));

  /**
   * @brief Program counter of the next instruction, `arch::arch::pc()`.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[nodiscard]] auto pc() const -> address;

private:
  machine* machine_;
};
}
