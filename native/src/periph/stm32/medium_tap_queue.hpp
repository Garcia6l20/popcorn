#pragma once

#include <popcorn/core/link_guard.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief Items a link medium shows its observers, any thread. Holds at most a capacity of items,
 *        the oldest dropped and counted.
 * @tparam Item The item a medium's drain pushes, a phase or a frame.
 */
template <typename Item> class medium_tap_queue
{
public:
  /** @param capacity Items kept, above 0. */
  explicit medium_tap_queue(std::size_t capacity) pre(capacity > 0) : capacity_{capacity}
  {}

  /** @brief Appends @p item, dropping the oldest when full. */
  void push(Item const& item)
  {
    core::link_guard const held{mutex_};
    if(items_.size() == capacity_)
    {
      items_.pop_front();
      ++dropped_;
    }
    items_.push_back(item);
  }

  /** @brief Takes every item held, oldest first. */
  auto take() -> std::vector<Item>
  {
    core::link_guard const held{mutex_};
    std::vector<Item>      out{items_.begin(), items_.end()};
    items_.clear();
    return out;
  }

  /** @brief Items dropped so far because the queue was full. */
  [[nodiscard]] auto dropped() const -> std::uint64_t
  {
    core::link_guard const held{mutex_};
    return dropped_;
  }

private:
  mutable std::mutex mutex_;
  std::deque<Item>   items_;
  std::size_t        capacity_;
  std::uint64_t      dropped_{};
};
}
