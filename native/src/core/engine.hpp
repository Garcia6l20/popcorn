#pragma once

#include <popcorn/arch/arch.hpp>

#include <unicorn/unicorn.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::core
{
/** @brief Closes a Unicorn engine. */
struct engine_close
{
  /** @brief Closes @p engine. */
  void operator()(uc_struct* engine) const noexcept;
};

/** @brief Owner of an open Unicorn engine. */
using engine_handle = std::unique_ptr<uc_struct, engine_close>;

/**
 * @brief Turns a Unicorn error into an exception, so no `uc_err` escapes the core.
 * @param err Result of a Unicorn call.
 * @param what What the call did, for the message.
 * @throws emulation_error naming @p what and `uc_strerror(err)` when @p err is not UC_ERR_OK.
 */
void check(uc_err err, std::string_view what);

/**
 * @brief Opens the engine an arch module asks for and sets its CPU model.
 * @param spec Architecture, mode and CPU model.
 * @return The open engine.
 * @throws emulation_error naming @p spec when Unicorn refuses it.
 */
auto open_engine(arch::engine_spec const& spec) -> engine_handle;

/**
 * @brief On-demand memory hook that makes the pc an MMIO callback reads exact. Unicorn syncs the
 *        pc before a memory hook runs, and only then: without a hook the callback reads the start
 *        of the translated block. Every guest load and store pays for the hook while it is
 *        installed, wherever it lands, so consumers that need an exact pc hold a reference and
 *        the hook exists only while one does. Add and remove only between slices: a hook change
 *        flushes translated code.
 */
class exact_pc_hook
{
public:
  /**
   * @brief Sets the engine and the guest ranges, inclusive, the hook covers once installed.
   * @param engine Engine to hook.
   * @param ranges First and last address of each MMIO range.
   */
  void configure(uc_struct* engine, std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges);

  /**
   * @brief Takes a reference, installing the hook on the first one.
   * @throws emulation_error when the engine refuses the hook.
   */
  void acquire();

  /** @brief Drops a reference, removing the hook with the last one. */
  void release() noexcept;

  /** @brief Whether the hook is installed. */
  [[nodiscard]] auto installed() const noexcept -> bool;

  /** @brief References held. */
  [[nodiscard]] auto users() const noexcept -> std::uint32_t;

private:
  void release_all() noexcept;

  uc_struct*                                           engine_{};
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges_{};
  std::vector<uc_hook>                                 hooks_{};
  std::uint32_t                                        users_{};
};
}
