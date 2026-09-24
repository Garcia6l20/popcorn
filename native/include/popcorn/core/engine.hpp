#pragma once

#include <memory>
#include <stdexcept>

struct uc_struct;

namespace popcorn
{
/** @brief Raised when the Unicorn engine cannot be opened or configured. */
class EngineError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};
}

namespace popcorn::core
{
/** @brief Closes a Unicorn engine. */
struct engine_closer
{
  void operator()(uc_struct* engine) const noexcept;
};

/** @brief Move-only owner of an open Unicorn engine. */
using engine_handle = std::unique_ptr<uc_struct, engine_closer>;

/**
 * @brief Opens a Cortex-M33 engine.
 * @throws EngineError when the linked Unicorn lacks the dev WFI instruction hook.
 */
auto open_m33() -> engine_handle;
}
