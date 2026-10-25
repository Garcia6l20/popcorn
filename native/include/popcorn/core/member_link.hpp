#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
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
   * @param shown System time through which an observer of the link may see its frames, at or
   *        before @p barrier: no end of the link and no cut owner of it acts on a frame that
   *        ended by it any more.
   * @return The earliest system error the drain found, if any. Its time may lie before
   *         @p barrier: a fault on a peer frame is dated at the frame's sender start, up to L
   *         before the barrier whose drain reports it. Order errors by link_fault::at, not by
   *         the drain that found them.
   */
  virtual auto drain(
      std::uint64_t   through,
      sys_time const& barrier,
      sys_time const& next,
      time_map        receiver,
      sys_time const& shown) -> std::optional<link_fault> = 0;

  /**
   * @brief The earliest system time at which a later drain must hand the receiver something the
   *        link holds after the last drain: a kept post, a cut edge, an injection. Called on the
   *        receiver's host thread after its drain. The receiver runs no further before a drain
   *        whose barrier lies past it. A quiet() link can still be due: a cut edge yet to hand
   *        counts here and not in quiet().
   * @return Empty when the link holds nothing a drain would hand.
   */
  [[nodiscard]] virtual auto due() const -> std::optional<sys_time> = 0;

  /**
   * @brief Asks the link to call @p watcher with the system time of each post its sender makes,
   *        a frame start, an abort or a reset abort, once the post is counted: on the sender's
   *        host thread, or on a thread that holds the sender's host mutex while the sender is
   *        parked. The system calls it under the sender's host mutex when it is built, and with
   *        an empty watcher when it is destroyed or its construction fails.
   * @param watcher Called with each post's system time. An empty watcher stops the calls.
   * @return Whether the link calls @p watcher. The default returns false and never calls it.
   */
  virtual auto watch_posts([[maybe_unused]] std::move_only_function<void(sys_time)> watcher) -> bool
  {
    return false;
  }

  /**
   * @brief Refuses every input that would reach the receiver before @p at, such as a running
   *        injection, until the next call replaces the bound, then returns due(). Called on the
   *        receiver's host thread before the receiver promises its peers to post nothing before
   *        @p at unless an input reaches it, and at each resume with the receiver's time, which
   *        may lower the bound. The default only returns due(), for a link that takes no such
   *        input.
   * @param at System time before which the link refuses inputs to the receiver.
   * @return due() once the bound is set, so it holds every input accepted before the bound.
   */
  virtual auto hold_inputs_before([[maybe_unused]] sys_time at) -> std::optional<sys_time>
  {
    return due();
  }

  /**
   * @brief hold_inputs_before() over a group of one receiver's inbound links at one time: sets
   *        the bound of every link of @p links to @p at and stores each link's due() at its
   *        index of @p dues. The bounds and dues equal those of calling
   *        hold_inputs_before(@p at) on each link in turn, so an override may take a lock the
   *        links of the group share once for the whole call. Called on the receiver's host
   *        thread, on the first link of @p links, under the conditions of
   *        hold_inputs_before(). The default makes the per-link calls.
   * @param links The receiver's inbound links, this link first.
   * @param at System time before which every link of @p links refuses inputs to the receiver.
   * @param dues One entry per link of @p links, set to that link's due() once its bound is set.
   */
  virtual void hold_group_before(
      std::span<member_link* const>      links,
      sys_time                           at,
      std::span<std::optional<sys_time>> dues)
  {
    for(std::size_t index = 0; index < links.size() and index < dues.size(); ++index)
    {
      dues[index] = links[index]->hold_inputs_before(at);
    }
  }

  /**
   * @brief Whether the link holds nothing for its receiver: nothing posted, queued or kept
   *        that a later drain would hand. Called on the receiver's host thread after its drain,
   *        with the sender stopped.
   */
  [[nodiscard]] virtual auto quiet() const -> bool = 0;

  /**
   * @brief Shows an observer of the link every frame ended by @p at: the earliest pause
   *        position of the link's ends and cut owner, the members that can still change those
   *        frames, and under sync_mode::barrier also the pause point. Called on the receiver's
   *        host thread once the system decided to pause, after the pause's last drain, while no
   *        member sends. A link with no observer keeps the default, which does nothing.
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
