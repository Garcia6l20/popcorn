#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace popcorn::core
{
/**
 * @brief A machine's count of resets of every kind, see machine::reset_counter(). Any thread
 *        may read it while the machine runs, and after the machine is gone. A default one has
 *        no machine and reads 0.
 */
class reset_counter
{
public:
  /** @brief A counter with no machine, which reads 0. */
  reset_counter() noexcept = default;

  /**
   * @brief A counter reading @p count.
   * @param count The count, raised by its owner.
   */
  explicit reset_counter(std::shared_ptr<std::atomic<std::uint64_t> const> count) noexcept
      : count_{std::move(count)}
  {}

  /**
   * @brief The count now, with acquire ordering.
   * @return The resets so far, 0 without a machine.
   */
  [[nodiscard]] auto load() const noexcept -> std::uint64_t
  {
    return count_ ? count_->load(std::memory_order_acquire) : 0;
  }

private:
  std::shared_ptr<std::atomic<std::uint64_t> const> count_;
};
}
