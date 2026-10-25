#pragma once

#include <popcorn/core/member_link.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace popcorn::core::detail
{
/**
 * @brief Whether a successor at @p floor ran on a promise of a member whose published floor is
 *        @p published: it passed that floor plus their coupling distance @p distance, which no
 *        reader reaches without a promise of the member. A successor at distance 0 follows the
 *        member at its floor, a halted member behind a goal holder, and never counts.
 */
[[nodiscard]] constexpr auto
    promise_used(sys_time floor, sys_time published, sys_time distance) noexcept -> bool
{
  if(distance.ps <= 0)
  {
    return false;
  }
  if(published.ps > std::numeric_limits<std::int64_t>::max() - distance.ps)
  {
    return false;
  }
  return floor.ps > published.ps + distance.ps;
}

/**
 * @brief How often a sleeping member evaluates its promise. The publish after an evaluation
 *        judges it: a used promise sets the period to every publish, an unused one doubles it,
 *        up to max_period publishes.
 */
class promise_pace
{
public:
  /** @brief The most publishes between two evaluations. */
  static constexpr std::uint32_t max_period = 64;

  /** @brief Back to an evaluation at every publish, with nothing to judge. */
  constexpr void restart() noexcept
  {
    period_  = 1;
    skipped_ = 0;
    judging_ = false;
  }

  /** @brief Whether the last publish evaluated the promise and this one must judge it. */
  [[nodiscard]] constexpr auto judging() const noexcept -> bool
  {
    return judging_;
  }

  /**
   * @brief Judges the last evaluation.
   * @param used Whether a successor ran on the promise, promise_used().
   */
  constexpr void judge(bool used) noexcept
  {
    judging_ = false;
    period_  = used ? 1 : std::min(period_ * 2, max_period);
    skipped_ = 0;
  }

  /** @brief Counts one publish where the member could promise, and returns whether it evaluates. */
  [[nodiscard]] constexpr auto next() noexcept -> bool
  {
    auto const evaluated = ++skipped_ >= period_;
    if(evaluated)
    {
      skipped_ = 0;
      judging_ = true;
    }
    return evaluated;
  }

  /** @brief The publishes from one evaluation to the next. */
  [[nodiscard]] constexpr auto period() const noexcept -> std::uint32_t
  {
    return period_;
  }

private:
  std::uint32_t period_{1};
  std::uint32_t skipped_{};
  bool          judging_{};
};
}
