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
 *        scheduler sees it. The sender half runs on the sender's host thread, the receiver half
 *        on the receiver's. Posts go into a queue in push order. posted_count() reads its count
 *        from any thread, and a drain hands only the posts below a count read after them, so
 *        a sender running on while its receiver drains pushes past the count and its posts wait
 *        for a later drain.
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
   * @brief Starts a slice on the sender's side. Called on the sender's host thread before each
   *        slice: posts until the next call convert their times through @p sender.
   * @param sender The sender's time map.
   */
  virtual void enter_slice(time_map sender) = 0;

  /**
   * @brief Posts made so far, any thread: a count drain() takes as its limit. Reading it
   *        synchronizes with the posts below it.
   */
  [[nodiscard]] virtual auto posted_count() const noexcept -> std::uint64_t = 0;

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
   * @brief Hands the posts up to count @p through to the receiver. A link may keep some for a
   *        later drain, as long as each still reaches the receiver on time. Called on the
   *        receiver's host thread, before the receiver runs on.
   * @param through A count posted_count() returned, at or after the last drain's.
   * @param barrier System time the receiver stands at: posts starting before it are handed.
   * @param next System time the receiver may run to before the next drain: what the link's
   *        drains reach so far.
   * @param receiver The receiver's time map.
   * @return The earliest system error the drain found, if any. Its time may lie before
   *         @p barrier: a fault on a peer frame is dated at the frame's sender start, up to L
   *         before the barrier whose drain reports it. Order errors by link_fault::at, not by
   *         the drain that found them.
   */
  virtual auto
      drain(std::uint64_t through, sys_time const& barrier, sys_time const& next, time_map receiver)
          -> std::optional<link_fault> = 0;

  /**
   * @brief The earliest system time at which a later drain must hand the receiver something the
   *        link holds after the last drain: a kept post, a cut edge, an injection. Called on the
   *        receiver's host thread after its drain. The receiver runs no further before a drain
   *        whose barrier lies past it.
   * @return Empty when the link holds nothing a drain would hand.
   */
  [[nodiscard]] virtual auto due() const -> std::optional<sys_time> = 0;

  /**
   * @brief Whether the link holds nothing for its receiver: nothing posted, queued or kept
   *        that a later drain would hand. Called on the receiver's host thread after its drain,
   *        with the sender stopped.
   */
  [[nodiscard]] virtual auto quiet() const -> bool = 0;

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
