#pragma once

#include <popcorn/core/link_guard.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/audio.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <span>
#include <vector>

namespace popcorn::bind::detail
{
/** @brief Transmitted frames a UART link keeps before dropping the oldest, about 16 MiB. */
inline constexpr std::uint64_t uart_frame_limit = std::uint64_t{1} << 20;

/** @brief One frame a UART transmitted, as its queue keeps it. */
struct queued_uart_frame
{
  /** @brief Data bits. */
  std::uint16_t data{};
  /** @brief Virtual time its stop bit ended, in picoseconds. */
  std::uint64_t at_ps{};
};

/**
 * @brief Far end of a UART that queues transmitted frames for Python to drain, at most `limit`
 *        of them: a full queue drops its oldest frame and counts it. The engine fills it, the
 *        binding drains it, each under the queue's link lock.
 */
class queue_endpoint final : public periph::stm32::uart_endpoint
{
public:
  /** @brief Takes the link lock, blocking. */
  [[nodiscard]] auto lock() const -> core::link_guard
  {
    return core::link_guard{mutex_};
  }

  /** @brief Takes the link lock if it is free. */
  [[nodiscard]] auto try_lock() const -> core::link_guard
  {
    return core::link_guard{mutex_, std::try_to_lock};
  }

  void transmitted(std::uint16_t data, vtime at) override
  {
    auto const held = lock();
    trim(held, limit_ - 1);
    frames_.push_back({.data = data, .at_ps = at.ps});
  }

  /** @brief Frames not drained yet. */
  [[nodiscard]] auto available(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return frames_.size();
  }

  /**
   * @brief Takes the oldest frames, up to the first whose stop bit ended after @p until_ps.
   * @param count Most frames taken.
   * @param until_ps Latest stop bit end taken, in picoseconds.
   * @return The frames, oldest first.
   */
  auto take(
      core::link_guard const& held,
      std::uint64_t           count,
      std::uint64_t           until_ps = std::numeric_limits<std::uint64_t>::max())
      -> std::vector<queued_uart_frame> pre(held.guards(mutex_))
  {
    auto const first = frames_.begin();
    auto const last  = std::find_if(
        first, first + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(count, frames_.size())),
        [until_ps](queued_uart_frame const& frame) { return frame.at_ps > until_ps; });
    std::vector<queued_uart_frame> out(first, last);
    frames_.erase(first, last);
    return out;
  }

  /** @brief Frames dropped because the queue was full. */
  [[nodiscard]] auto dropped(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return dropped_;
  }

  /**
   * @brief Copies the low byte of each queued frame from @p start, at most @p count of them,
   *        without taking them.
   * @param held The guard of this queue's mutex.
   * @param start Queued frames skipped.
   * @param count Most bytes copied.
   * @return The bytes, fewer when the queue holds fewer.
   */
  [[nodiscard]] auto
      peek(core::link_guard const& held, std::uint64_t start, std::uint64_t count) const
      -> std::vector<std::uint8_t> pre(held.guards(mutex_))
  {
    auto const                first = std::min<std::uint64_t>(start, frames_.size());
    auto const                last = first + std::min<std::uint64_t>(count, frames_.size() - first);
    std::vector<std::uint8_t> out;
    out.reserve(last - first);
    for(auto index = first; index < last; ++index)
    {
      out.push_back(static_cast<std::uint8_t>(frames_[index].data & 0xFFu));
    }
    return out;
  }

  /**
   * @brief Sets the most frames kept, dropping the oldest beyond it.
   * @param held The queue's lock.
   * @param limit Frames, at least 1.
   */
  void set_limit(core::link_guard const& held, std::uint64_t limit) pre(held.guards(mutex_))
  {
    limit_ = limit;
    trim(held, limit);
  }

private:
  void trim(core::link_guard const&, std::uint64_t kept)
  {
    while(frames_.size() > kept)
    {
      frames_.pop_front();
      ++dropped_;
    }
  }

  mutable std::mutex            mutex_;
  std::deque<queued_uart_frame> frames_;
  std::uint64_t                 limit_ = uart_frame_limit;
  std::uint64_t                 dropped_{};
};

/**
 * @brief Transmitted frames an audio link keeps before dropping the oldest: 5.4 s at 48 kHz,
 *        4 MiB of 16-bit stereo, 18 MiB of 16-slot TDM, the widest frame.
 */
inline constexpr std::uint64_t audio_frame_limit = std::uint64_t{1} << 18;

/** @brief Consecutive kept frames sharing one layout. */
struct format_run
{
  /** @brief Layout of the frames. */
  periph::stm32::audio_format format{};
  /** @brief Words per frame. */
  std::uint8_t slots{};
  /** @brief Frames in the run. */
  std::uint64_t frames{};
};

/** @brief One frame an SAI audio block transmitted, as its queue hands it out. */
struct queued_audio_frame
{
  /** @brief One word per slot, data right-aligned. */
  std::vector<std::uint32_t> slots{};
  /** @brief Layout of the frame. */
  periph::stm32::audio_format format{};
  /** @brief Virtual time the frame started, in picoseconds. */
  std::uint64_t at_ps{};
};

/**
 * @brief Far end of an SAI audio block for Python: keeps transmitted frames flat, slot words
 *        back to back with one start time per frame and one layout per run of frames, at most
 *        `limit` frames, a full queue dropping its oldest frame and counting it. Serves words
 *        Python wrote to a receiving block in slot order, then 0. The engine and the binding
 *        touch it under its link lock.
 */
class audio_queue_endpoint final : public periph::stm32::audio_endpoint
{
public:
  /** @brief Takes the link lock, blocking. */
  [[nodiscard]] auto lock() const -> core::link_guard
  {
    return core::link_guard{mutex_};
  }

  /** @brief Takes the link lock if it is free. */
  [[nodiscard]] auto try_lock() const -> core::link_guard
  {
    return core::link_guard{mutex_, std::try_to_lock};
  }

  void transmitted(
      std::span<std::uint32_t const> slots,
      periph::stm32::audio_format    format,
      vtime                          at) override
  {
    auto const held = lock();
    trim(held, limit_ - 1);
    auto const count = static_cast<std::uint8_t>(slots.size());
    if(runs_.empty() or runs_.back().format != format or runs_.back().slots != count)
    {
      runs_.push_back({.format = format, .slots = count, .frames = 0});
    }
    ++runs_.back().frames;
    words_.insert(words_.end(), slots.begin(), slots.end());
    starts_.push_back(at.ps);
  }

  void receive(std::span<std::uint32_t> slots, periph::stm32::audio_format, vtime) override
  {
    auto const held = lock();
    for(auto& slot : slots)
    {
      if(pending_.empty())
      {
        slot = 0;
        continue;
      }
      slot = pending_.front();
      pending_.pop_front();
    }
  }

  /** @brief Frames transmitted and not taken yet. */
  [[nodiscard]] auto available(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return starts_.size();
  }

  /** @brief Frames dropped because the queue was full. */
  [[nodiscard]] auto dropped(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return dropped_;
  }

  /** @brief Words written and not served to the receiver yet. */
  [[nodiscard]] auto queued(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return pending_.size();
  }

  /**
   * @brief Takes the oldest kept frames.
   * @param held The queue's lock.
   * @param count Most frames taken.
   * @return The frames, oldest first.
   */
  auto take(core::link_guard const& held, std::uint64_t count)
      -> std::vector<queued_audio_frame> pre(held.guards(mutex_))
  {
    std::vector<queued_audio_frame> out;
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, starts_.size())));
    while(out.size() < count and not starts_.empty())
    {
      auto const& run = runs_.front();
      out.push_back({
          .slots  = {words_.begin(), words_.begin() + run.slots},
          .format = run.format,
          .at_ps  = starts_.front(),
      });
      pop_front(held);
    }
    return out;
  }

  /**
   * @brief Queues words for a receiving block.
   * @param words Words, one per slot in slot order.
   */
  void write(core::link_guard const& held, std::span<std::uint32_t const> words)
      pre(held.guards(mutex_))
  {
    pending_.insert(pending_.end(), words.begin(), words.end());
  }

  /** @brief Drops the kept frames and the words not served yet, and clears the drop count. */
  void discard(core::link_guard const& held) pre(held.guards(mutex_))
  {
    words_.clear();
    starts_.clear();
    runs_.clear();
    pending_.clear();
    dropped_ = 0;
  }

  /**
   * @brief Sets the most frames kept, dropping the oldest beyond it.
   * @param held The queue's lock.
   * @param limit Frames, at least 1.
   */
  void set_limit(core::link_guard const& held, std::uint64_t limit) pre(held.guards(mutex_))
  {
    limit_ = limit;
    trim(held, limit);
  }

private:
  void pop_front(core::link_guard const&)
  {
    auto& run = runs_.front();
    words_.erase(words_.begin(), words_.begin() + run.slots);
    starts_.pop_front();
    if(--run.frames == 0)
    {
      runs_.pop_front();
    }
  }

  void trim(core::link_guard const& held, std::uint64_t kept)
  {
    while(starts_.size() > kept)
    {
      pop_front(held);
      ++dropped_;
    }
  }

  mutable std::mutex        mutex_;
  std::deque<std::uint32_t> words_;
  std::deque<std::uint64_t> starts_;
  std::deque<format_run>    runs_;
  std::deque<std::uint32_t> pending_;
  std::uint64_t             limit_ = audio_frame_limit;
  std::uint64_t             dropped_{};
};
}
