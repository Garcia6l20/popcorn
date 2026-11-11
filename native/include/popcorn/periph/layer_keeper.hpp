#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstdint>

namespace popcorn::periph
{
/**
 * @brief Optional capability of a model, found by `dynamic_cast` over the machine's models: the
 *        model leaves some words no model claims as they are over some resets, their bits and
 *        their write-once lock, instead of bringing them to their reset value and unlocking
 *        them.
 */
class POPCORN_API layer_keeper
{
public:
  /** @brief Creates the interface. */
  layer_keeper() = default;
  /** @brief Not copyable. */
  layer_keeper(layer_keeper const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(layer_keeper const&) -> layer_keeper& = delete;
  /** @brief Not movable. */
  layer_keeper(layer_keeper&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(layer_keeper&&) -> layer_keeper& = delete;

  /**
   * @brief Whether a reset of @p kind leaves the register word at @p offset as it is. Asked of
   *        every unclaimed word of the peripheral at each reset, before the register layer
   *        resets the words.
   * @param offset Byte offset of the word in the peripheral, a multiple of 4.
   * @param kind Cause of the reset, as for `model::reset`.
   */
  [[nodiscard]] virtual auto keeps_over_reset(std::uint32_t offset, reset_kind kind) const
      -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~layer_keeper();
};
}
