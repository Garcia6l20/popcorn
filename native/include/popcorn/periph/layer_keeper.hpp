#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstdint>

namespace popcorn::periph
{
/**
 * @brief Optional capability of a model, found by `dynamic_cast` over the machine's models: the
 *        model leaves some words as they are over some resets, instead of bringing them to their
 *        reset value and unlocking them. A word no model claims keeps its bits and its write-once
 *        lock, a claimed word its write-once lock.
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
   *        every unclaimed word of the peripheral and every claimed word with a write-once bit
   *        at each reset, before the register layer resets the words. For a claimed word the
   *        answer decides its write-once lock only, the model resets its bits.
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
