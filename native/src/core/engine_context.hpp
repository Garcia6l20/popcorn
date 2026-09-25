#pragma once

#include <unicorn/unicorn.h>

namespace popcorn::core
{
/**
 * @brief CPU context of one engine, saved at construction. Valid only for the engine that saved
 *        it, Unicorn issue #2195. Freeing it does not touch the engine, so it may outlive it.
 */
class engine_context
{
public:
  /**
   * @brief Saves the CPU context of @p engine.
   * @throws emulation_error when the engine refuses the allocation or the save.
   */
  explicit engine_context(uc_struct* engine);
  /** @brief Not copyable. */
  engine_context(engine_context const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(engine_context const&) -> engine_context& = delete;
  /** @brief Frees the context. */
  ~engine_context();

  /** @brief The saved context. */
  [[nodiscard]] auto get() const noexcept -> uc_context*;

private:
  uc_context* context_ = nullptr;
};
}
