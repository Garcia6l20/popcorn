#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace popcorn::core
{
/**
 * @brief Unbounded single-producer single-consumer queue of trivially copyable values, in
 *        fixed-size segments the producer links and the consumer frees. Counts number every push
 *        from 0 at construction, or from the first count assign() names, so a count names a
 *        position for good.
 *
 *        push() writes its slot, links a new segment when the last one is full, then publishes
 *        the count with a release store. pushed() loads it with acquire, so a thread that read a
 *        count sees every slot and segment link below it, and so does any thread that later
 *        synchronizes with that one. The consumer leaves a segment, and frees it, only when it
 *        pops the first value of the next one, which the producer linked before publishing that
 *        value. The producer never touches a segment it has left, so it never sees a freed one.
 *        At rest the queue holds one segment.
 * @tparam T The value type.
 */
template <class T>
  requires std::is_trivially_copyable_v<T> and std::default_initializable<T>
class spsc_queue
{
public:
  /** @brief Values per segment. */
  static constexpr std::size_t segment_slots = 256;

  /** @brief An empty queue of one segment. */
  spsc_queue() : head_{std::make_unique<segment>()}, tail_segment_{head_.get()}
  {}

  /** @brief Not copyable. */
  spsc_queue(spsc_queue const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(spsc_queue const&) -> spsc_queue& = delete;

  /** @brief Frees every segment, one at a time. */
  ~spsc_queue()
  {
    while(head_)
    {
      head_ = std::move(head_->next);
    }
  }

  /** @brief Producer: appends @p value, linking a new segment when the last one is full. */
  void push(T const& value)
  {
    auto const count = tail_.load(std::memory_order_relaxed);
    if(count - tail_base_ == segment_slots)
    {
      tail_segment_->next = std::make_unique<segment>();
      tail_segment_       = tail_segment_->next.get();
      tail_base_ += segment_slots;
    }
    tail_segment_->slots[count - tail_base_] = value;
    tail_.store(count + 1, std::memory_order_release);
  }

  /** @brief Any thread: count of values pushed so far, release-published by push(). */
  [[nodiscard]] auto pushed() const noexcept -> std::uint64_t
  {
    return tail_.load(std::memory_order_acquire);
  }

  /**
   * @brief Consumer: appends the values from the last pop up to count @p through to @p out, in
   *        push order, and frees every segment it left.
   * @param through A count read from pushed(), at or after the last pop's.
   * @param out Where the values go.
   */
  void pop_through(std::uint64_t through, std::vector<T>& out)
  {
    auto const available = tail_.load(std::memory_order_acquire);
    contract_assert(popped_ <= through and through <= available);
    out.reserve(out.size() + (through - popped_));
    for(; popped_ < through; ++popped_)
    {
      if(popped_ - head_base_ == segment_slots)
      {
        head_ = std::move(head_->next);
        head_base_ += segment_slots;
      }
      out.push_back(head_->slots[popped_ - head_base_]);
    }
  }

  /** @brief Consumer: whether every value pushed was popped. */
  [[nodiscard]] auto empty() const noexcept -> bool
  {
    return popped_ == tail_.load(std::memory_order_acquire);
  }

  /** @brief Neither side active: the values not popped yet, oldest first. */
  [[nodiscard]] auto contents() const -> std::vector<T>
  {
    std::vector<T> out;
    auto const     count = tail_.load(std::memory_order_acquire);
    out.reserve(count - popped_);
    auto const* at   = head_.get();
    auto        base = head_base_;
    for(auto index = popped_; index < count; ++index)
    {
      if(index - base == segment_slots)
      {
        at = at->next.get();
        base += segment_slots;
      }
      out.push_back(at->slots[index - base]);
    }
    return out;
  }

  /** @brief Consumer: count of values popped so far. */
  [[nodiscard]] auto popped() const noexcept -> std::uint64_t
  {
    return popped_;
  }

  /**
   * @brief Neither side active: replaces the content by @p values, counted from @p first, so
   *        popped() becomes @p first and pushed() @p first plus their number.
   * @param first Count of the first value.
   * @param values The values, oldest first.
   */
  void assign(std::uint64_t first, std::span<T const> values)
  {
    while(head_->next)
    {
      head_->next = std::move(head_->next->next);
    }
    tail_segment_ = head_.get();
    tail_base_    = first;
    head_base_    = first;
    popped_       = first;
    tail_.store(first, std::memory_order_relaxed);
    for(auto const& value : values)
    {
      push(value);
    }
  }

  /** @brief Neither side active: segments held, 1 and up. */
  [[nodiscard]] auto segments() const noexcept -> std::size_t
  {
    std::size_t count = 0;
    for(auto const* at = head_.get(); at != nullptr; at = at->next.get())
    {
      ++count;
    }
    return count;
  }

private:
  struct segment
  {
    std::array<T, segment_slots> slots{};
    std::unique_ptr<segment>     next;
  };

  std::unique_ptr<segment> head_;
  std::uint64_t            head_base_{};
  std::uint64_t            popped_{};
  alignas(64) segment* tail_segment_;
  std::uint64_t tail_base_{};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
};
}
