#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <vector>

namespace popcorn::core
{
/** @brief Time on the clock a linked system shares, in picoseconds from system start. */
struct sys_time
{
  std::int64_t ps;

  /** @brief Member-wise ordering and equality. */
  friend auto operator<=>(sys_time const&, sys_time const&) = default;
};

/**
 * @brief Converts one member's virtual time since reset to system time and back:
 *        `system = base + local`. Owned by the member's slot, the base moves at each reset of the
 *        member.
 */
class time_map
{
public:
  /** @brief A member whose local time equals system time. */
  constexpr time_map() = default;

  /** @param base System time of the member's local time 0. */
  constexpr explicit time_map(sys_time base) : base_{base}
  {}

  /** @brief System time of the member's local time 0. */
  [[nodiscard]] constexpr auto base() const noexcept -> sys_time
  {
    return base_;
  }

  /** @brief System time of local time @p local. */
  [[nodiscard]] constexpr auto to_system(vtime local) const noexcept -> sys_time
  {
    return {base_.ps + static_cast<std::int64_t>(local.ps)};
  }

  /** @brief Local time of system time @p at, which must not lie before the member's time 0. */
  [[nodiscard]] constexpr auto to_local(sys_time at) const noexcept -> vtime pre(at.ps >= base_.ps)
  {
    return {static_cast<std::uint64_t>(at.ps - base_.ps)};
  }

private:
  sys_time base_{0};
};

/** @brief A failure a link found at a drain: a system error at a time, on a member. */
struct link_fault
{
  /** @brief System time of the error. */
  sys_time at{};
  /** @brief Member the error is reported on. */
  std::size_t member{};
  /** @brief The error. */
  std::exception_ptr error{};
};

/**
 * @brief One direction of a link between two members of a linked system, as the system's
 *        scheduler sees it. The sender half is touched only by the sender's host thread while it
 *        runs a window, the receiver half only by the receiver's host thread after a barrier.
 *        The barrier orders the halves: posts of window k are drained after barrier k, before
 *        the sender writes the same mailbox again in window k + 2. That holds only while every
 *        member arrives at every barrier: a member skipping a round would let a sender refill a
 *        mailbox its receiver has not drained.
 */
class POPCORN_API member_link
{
public:
  /** @brief Creates the link. */
  member_link() = default;
  /** @brief Not copyable. */
  member_link(member_link const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(member_link const&) -> member_link& = delete;
  /** @brief Not movable. */
  member_link(member_link&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(member_link&&) -> member_link& = delete;
  /** @brief Destroys the link. */
  virtual ~member_link();

  /** @brief Index of the sending member. */
  [[nodiscard]] virtual auto sender() const noexcept -> std::size_t = 0;

  /** @brief Index of the receiving member. */
  [[nodiscard]] virtual auto receiver() const noexcept -> std::size_t = 0;

  /** @brief Latency L: a post at sender system time t reaches the receiver at t + L. Above 0. */
  [[nodiscard]] virtual auto latency() const noexcept -> sys_time = 0;

  /**
   * @brief Starts a window on the sender's side. Called on the sender's host thread before each
   *        slice. Posts go to that window's mailbox until the next call, those of a block that
   *        overshoots the barrier included.
   * @param window Index of the window the slice starts in.
   * @param sender The sender's time map.
   */
  virtual void enter_window(std::uint64_t window, time_map sender) = 0;

  /**
   * @brief The sender is about to be reset at @p at. Called on the sender's host thread before
   *        its base moves: a frame in flight then is aborted at @p at, frames ended stay.
   */
  virtual void sender_reset(sys_time at) = 0;

  /**
   * @brief The receiver was reset at @p at. Called on the receiver's host thread. Frames whose
   *        arrival is still on the line at @p at or starts before @p at plus L are dropped,
   *        whether drained before the reset or after, so the dropped set depends on @p at and L
   *        only.
   */
  virtual void receiver_reset(sys_time at) = 0;

  /**
   * @brief Hands the posts of a window it delivers to the receiver. A link may keep some for a
   *        later drain, as long as each still reaches the receiver on time. Called on the
   *        receiver's host thread after the barrier closing @p window, before the receiver runs
   *        on.
   * @param window Index of the window whose mailbox is drained.
   * @param barrier System time of that barrier.
   * @param next System time of the barrier after it: what the link's drains reach so far.
   * @param receiver The receiver's time map.
   * @return The earliest system error the drain found, if any. Its time may lie before
   *         @p barrier: a fault on a peer frame is dated at the frame's sender start, up to L
   *         before the barrier whose drain reports it. Order errors by link_fault::at, not by
   *         the drain that found them.
   */
  virtual auto
      drain(std::uint64_t window, sys_time const& barrier, sys_time const& next, time_map receiver)
          -> std::optional<link_fault> = 0;

  /**
   * @brief Whether the link holds nothing for its receiver: nothing posted, queued or kept
   *        that a later drain would hand. Called on the receiver's host thread after its drain,
   *        with every sender stopped at a barrier.
   */
  [[nodiscard]] virtual auto quiet() const -> bool = 0;

  /**
   * @brief Shows an observer of the link every frame ended by the pause point @p at, whatever
   *        barrier the drains handed frames at. Called once per pause that delivered a member
   *        stop, paused IDLE or paused on an error, after every member stopped, with no drain()
   *        or finish() of the link running: on the host thread that finished the pause last, or
   *        on the caller of system::resume() when every member finished before it returned. At
   *        a member stop and at IDLE every member then stands at or past @p at, so no later post
   *        or injection ends before @p at. An error pause stops the system, so no member runs
   *        after it. The system calls it at those three kinds only. After a TIME or requested
   *        pause an observer holds the frames ended by the earliest member's position. A frame
   *        a later drain hands is shown once. Runs where no exception may leave, an allocation
   *        failure terminates. A link with no observer keeps the default, which does nothing.
   * @param at The pause point, system time.
   */
  virtual void release_through([[maybe_unused]] sys_time at)
  {}

  /**
   * @brief Settles what an observer sees at the system's final stop @p at, called on the
   *        receiver's host thread with every member stopped.
   */
  virtual void finish(sys_time at) = 0;

  /** @brief Appends the link's state to @p out, with every member stopped and no injection
   * in flight. */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores a state save() wrote, with every member stopped and no injection in
   *        flight.
   * @throws config_error when @p in is not such a state.
   */
  virtual void load(std::span<std::byte const> in) = 0;
};
}
