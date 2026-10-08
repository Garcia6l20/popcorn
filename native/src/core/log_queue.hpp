#pragma once

#include "loop_waker.hpp"

#include <popcorn/core/log.hpp>
#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace popcorn::core
{
/** @brief A machine log record and the virtual time it was logged at. */
struct queued_log
{
  /** @brief The record. */
  log_record record;
  /** @brief Virtual time since reset when it was logged. */
  vtime time{};
};

/**
 * @brief The log records of one machine, appended by whichever thread runs it and taken by the
 *        thread that forwards them, in order. Bounded: past the bound the oldest records are kept
 *        and later ones only counted. When it goes from empty to non-empty it wakes the loop
 *        waker set last, with that waker's token. Knows nothing of Python. Its mutex is a leaf:
 *        the waker is notified after it is released.
 */
class log_queue
{
public:
  /** @brief Records kept at most. */
  static constexpr std::size_t default_bound = 4096;

  /** @brief An empty queue keeping at most @p bound records. */
  explicit log_queue(std::size_t bound = default_bound) : bound_{bound}
  {}

  /**
   * @brief Appends @p record, or counts it as dropped once the queue holds the bound.
   * @param record The record.
   * @param at Its virtual time.
   */
  void push(log_record record, vtime at)
  {
    std::weak_ptr<loop_waker> waking;
    std::uint64_t             token = 0;
    {
      std::scoped_lock const lock{mutex_};
      if(records_.size() >= bound_)
      {
        ++dropped_;
        return;
      }
      if(records_.empty())
      {
        waking = waker_;
        token  = token_;
      }
      records_.push_back({.record = std::move(record), .time = at});
    }
    if(auto const live = waking.lock())
    {
      live->notify(token);
    }
  }

  /** @brief Takes the oldest record, empty when there is none. */
  auto pop() -> std::optional<queued_log>
  {
    std::scoped_lock const lock{mutex_};
    if(records_.empty())
    {
      return std::nullopt;
    }
    auto taken = std::move(records_.front());
    records_.pop_front();
    return taken;
  }

  /** @brief Takes every record queued now, oldest first. Records pushed later stay queued. */
  auto take_all() -> std::deque<queued_log>
  {
    std::scoped_lock const lock{mutex_};
    return std::exchange(records_, {});
  }

  /**
   * @brief Puts @p records back before the queued ones, in their order: what a flush took and
   *        did not forward, and wakes the waker once for them and any dropped count, which a
   *        failed flush restores first. Ignores the bound: with the records pushed during the
   *        flush the queue may hold more than it until the next flush.
   */
  void restore_front(std::deque<queued_log> records)
  {
    {
      std::scoped_lock const lock{mutex_};
      records_.insert(
          records_.begin(), std::make_move_iterator(records.begin()),
          std::make_move_iterator(records.end()));
    }
    wake_if_pending();
  }

  /** @brief Adds @p count to the records dropped, a count a flush took and did not report.
   * Wakes nobody: the flush's restore_front() does. */
  void restore_dropped(std::uint64_t count)
  {
    std::scoped_lock const lock{mutex_};
    dropped_ += count;
  }

  /** @brief Takes the count of records dropped since the last call. */
  auto take_dropped() -> std::uint64_t
  {
    std::scoped_lock const lock{mutex_};
    return std::exchange(dropped_, 0);
  }

  /**
   * @brief Wakes @p waker with @p token from now on when a record lands in the empty queue, and
   *        at once when records or a dropped count already wait.
   * @param waker The loop waker, held weakly.
   * @param token The token its drain returns.
   */
  void wake_through(std::weak_ptr<loop_waker> waker, std::uint64_t token)
  {
    {
      std::scoped_lock const lock{mutex_};
      waker_ = std::move(waker);
      token_ = token;
    }
    wake_if_pending();
  }

  /** @brief Records kept at most. */
  [[nodiscard]] auto bound() const noexcept -> std::size_t
  {
    return bound_;
  }

private:
  void wake_if_pending()
  {
    std::weak_ptr<loop_waker> waking;
    std::uint64_t             token = 0;
    {
      std::scoped_lock const lock{mutex_};
      if(records_.empty() and dropped_ == 0)
      {
        return;
      }
      waking = waker_;
      token  = token_;
    }
    if(auto const live = waking.lock())
    {
      live->notify(token);
    }
  }

  std::size_t               bound_;
  std::mutex                mutex_;
  std::deque<queued_log>    records_;
  std::uint64_t             dropped_ = 0;
  std::weak_ptr<loop_waker> waker_;
  std::uint64_t             token_ = 0;
};
}
