#pragma once

#include <popcorn/core/member_link.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace popcorn::core::detail
{
/**
 * @brief What a member of a linked system tells its peers under sync_mode::clocks, all in system
 *        time: it posts nothing before floor, and nothing before general or, on each outbound
 *        wire, before that wire's value unless an input reaches it first. consumed holds, per
 *        inbound link, the barrier of its last drain: every post starting before it is in the
 *        member's machine. heard holds, per member, the general effective promise of that member
 *        as this one last read it.
 */
struct promise_record
{
  /** @brief Unconditional: no post before it. */
  sys_time floor{};
  /** @brief Conditional on no input, for cut changes and the members coupled by no wire. */
  sys_time general{};
  /** @brief Conditional on no input, per outbound wire, picoseconds. */
  std::vector<std::int64_t> wires{};
  /** @brief Barrier of the last drain per inbound link, picoseconds. */
  std::vector<std::int64_t> consumed{};
  /** @brief General effective promise of each member as last read, picoseconds. */
  std::vector<std::int64_t> heard{};
};

/**
 * @brief A promise_record that one thread at a time writes and any thread reads as one
 *        snapshot, a sequence lock. The writer never waits. A reader retries while a write is
 *        in progress or completed during its read. The fields are atomics, so there is no data
 *        race. The writer's field stores are releases, so its odd version store is ordered before
 *        them, and the reader's field loads are acquires, so they are ordered before it reads the
 *        version again. No standalone fence, which the thread sanitizer does not model.
 */
class published_record
{
public:
  /** @brief Sizes the record: outbound wires, inbound links, members. Not concurrent. */
  void shape(std::size_t wires, std::size_t consumed, std::size_t heard)
  {
    wires_    = std::make_unique<std::atomic<std::int64_t>[]>(wires);
    consumed_ = std::make_unique<std::atomic<std::int64_t>[]>(consumed);
    heard_    = std::make_unique<std::atomic<std::int64_t>[]>(heard);
    sizes_    = {wires, consumed, heard};
  }

  /** @brief Publishes @p record, sized as shape() said. One writer at a time. */
  void write(promise_record const& record) noexcept
  {
    auto const version = version_.load(std::memory_order_relaxed);
    version_.store(version + 1, std::memory_order_relaxed);
    floor_.store(record.floor.ps, std::memory_order_release);
    general_.store(record.general.ps, std::memory_order_release);
    for(std::size_t index = 0; index < sizes_.wires; ++index)
    {
      wires_[index].store(record.wires[index], std::memory_order_release);
    }
    for(std::size_t index = 0; index < sizes_.consumed; ++index)
    {
      consumed_[index].store(record.consumed[index], std::memory_order_release);
    }
    for(std::size_t index = 0; index < sizes_.heard; ++index)
    {
      heard_[index].store(record.heard[index], std::memory_order_release);
    }
    version_.store(version + 2, std::memory_order_release);
  }

  /** @brief Reads one published record into @p out, any thread. */
  void read(promise_record& out) const noexcept
  {
    out.wires.resize(sizes_.wires);
    out.consumed.resize(sizes_.consumed);
    out.heard.resize(sizes_.heard);
    for(;;)
    {
      auto const version = version_.load(std::memory_order_acquire);
      if(version % 2 != 0)
      {
        std::this_thread::yield();
        continue;
      }
      out.floor   = sys_time{floor_.load(std::memory_order_acquire)};
      out.general = sys_time{general_.load(std::memory_order_acquire)};
      for(std::size_t index = 0; index < sizes_.wires; ++index)
      {
        out.wires[index] = wires_[index].load(std::memory_order_acquire);
      }
      for(std::size_t index = 0; index < sizes_.consumed; ++index)
      {
        out.consumed[index] = consumed_[index].load(std::memory_order_acquire);
      }
      for(std::size_t index = 0; index < sizes_.heard; ++index)
      {
        out.heard[index] = heard_[index].load(std::memory_order_acquire);
      }
      if(version_.load(std::memory_order_relaxed) == version)
      {
        return;
      }
    }
  }

  /**
   * @brief A floor the writer published, read on any thread while it may publish: its last
   *        floor or an earlier one.
   */
  [[nodiscard]] auto floor_seen() const noexcept -> sys_time
  {
    return sys_time{floor_.load(std::memory_order_relaxed)};
  }

  /** @brief The published floor, on the writer's thread or with the writer stopped. */
  [[nodiscard]] auto floor() const noexcept -> sys_time
  {
    return sys_time{floor_.load(std::memory_order_relaxed)};
  }

  /** @brief The published barrier of inbound link @p index, any thread, alone. */
  [[nodiscard]] auto consumed(std::size_t index) const noexcept -> sys_time
  {
    return sys_time{consumed_[index].load(std::memory_order_acquire)};
  }

private:
  struct sizes
  {
    std::size_t wires{};
    std::size_t consumed{};
    std::size_t heard{};
  };

  alignas(64) std::atomic<std::uint32_t> version_{};
  std::atomic<std::int64_t>                    floor_{};
  std::atomic<std::int64_t>                    general_{};
  std::unique_ptr<std::atomic<std::int64_t>[]> wires_{};
  std::unique_ptr<std::atomic<std::int64_t>[]> consumed_{};
  std::unique_ptr<std::atomic<std::int64_t>[]> heard_{};
  sizes                                        sizes_{};
};
}
