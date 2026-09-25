#pragma once

#include "memory_map.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/machine.hpp>

#include <cstdint>
#include <optional>

namespace popcorn::core
{
/** @brief Table behind every hook_handle of a machine. */
class hook_registry
{
public:
  /** @brief Whether a slice runs, so a handle refuses removal. */
  bool running = false;

  /** @brief Removes a hook by id. */
  void remove(std::uint64_t) noexcept
  {}
};

/** @brief Per peripheral state of a machine. */
struct machine::peripheral_slot
{};

/** @brief Run state of a machine, and what it keeps for the arch module. */
struct machine::run_state
{
  /** @brief Whether a slice runs. */
  bool running = false;
  /** @brief Whether a handler of the running slice is on the stack. */
  bool in_handler = false;
  /** @brief Whether a call begun by begin_call() is not ended. */
  bool in_call = false;
  /** @brief Address the arch module's reset reads the vector table at. */
  address reset_vector{};
  /** @brief Context handed to arch::attach(), which the module may keep. */
  std::optional<arch::arch_context> arch_context{};
};

/**
 * @brief Throws unless the machine is stopped.
 * @throws machine_running while a slice runs, handlers included.
 */
void require_stopped(machine const& target);

/**
 * @brief Power-on core clock of a machine.
 * @throws config_error when neither the options nor the description give one.
 */
auto core_clock_hz(mcu_description const& description, machine_options const& options)
    -> std::uint64_t;
}
