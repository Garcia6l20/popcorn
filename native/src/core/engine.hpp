#pragma once

#include <popcorn/arch/arch.hpp>

#include <unicorn/unicorn.h>

#include <memory>
#include <string_view>

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
}
