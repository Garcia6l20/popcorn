#pragma once

/**
 * @file
 * @brief The event queue of a machine and the port through which a model schedules events.
 */

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <reflex/enum.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace popcorn::core
{
/** @brief Stable identity of a scheduled event, dispatched without any stored callable. */
struct event_id
{
  /** @brief Index of the model that scheduled the event and receives it. */
  std::uint32_t owner;
  /** @brief Event kind, passed to the owner's `periph::model::on_event`. */
  std::uint32_t kind;

  /** @brief Member-wise equality. */
  friend auto operator==(event_id const&, event_id const&) -> bool = default;
};

/** @brief What an event's deadline counts. */
enum class[[= reflex::derive{reflex::Format}]] deadline
{
  /** @brief Executed instructions: the event keeps its instruction count across rate
   * changes, as a core-cycle counter such as SysTick does. */
  instructions,
  /** @brief Virtual time: the instruction count follows every core rate change. */
  time,
};

/** @brief Event in the queue, plain data so a snapshot copies it as is. */
struct event
{
  /** @brief Instruction count at which the event is due. */
  icount at;
  /** @brief Sequence number from schedule(), orders events due at the same count. */
  std::uint64_t seq;
  /** @brief Owner and kind. */
  event_id id;
  /** @brief Payload passed to the owner's `periph::model::on_event`. */
  std::uint64_t payload;
  /** @brief Virtual due time in picoseconds since reset, for a deadline::time event, 0 for an
   * instruction one. */
  std::uint64_t due_ps{};
  /** @brief What the deadline counts. at is derived from due_ps for deadline::time. */
  deadline kind{deadline::instructions};
  /** @brief Delivery lane, 0 for an event of this machine, 1 and up for an event delivered from
   * another machine, host_lane for one a host command scheduled. Events due at the same count
   * fire by lane, then by seq. A lane event keeps its own count when its due time has already
   * passed. */
  std::uint32_t lane{};

  /** @brief Member-wise equality. */
  friend auto operator==(event const&, event const&) -> bool = default;
};

/**
 * @brief Lane of events a host command schedules at an exact virtual time, such as a bus hold
 *        posted from Python. Above every link lane, so such an event fires after the local and
 *        link events due at the same count, whatever boundary the command ran at.
 */
inline constexpr std::uint32_t host_lane = std::numeric_limits<std::uint32_t>::max();

static_assert(std::is_trivially_copyable_v<event_id>);
static_assert(std::is_trivially_copyable_v<event>);

/**
 * @brief Event queue of a machine, ordered by due count, then lane, then sequence number. Holds no
 *        callables: the run loop pops due events and dispatches each to its owner model.
 *        Neither copyable nor movable: event ports hold references to it.
 */
class POPCORN_API scheduler
{
public:
  /** @brief Creates an empty queue whose first sequence number is 0. */
  scheduler() = default;
  /** @brief Not copyable. */
  scheduler(scheduler const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(scheduler const&) -> scheduler& = delete;
  /** @brief Not movable. */
  scheduler(scheduler&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(scheduler&&) -> scheduler& = delete;
  /** @brief Destroys the queue. */
  ~scheduler();

  /**
   * @brief Schedules an event. An @p at already passed makes the event due at the next pop.
   * @param at Instruction count at which the event is due.
   * @param id Owner and kind.
   * @param payload Passed back to the owner.
   * @return Sequence number of the event, the handle for cancel(). Unique within one timeline:
   *         restore() rewinds the counter, so the seqs issued after a snapshot are issued again
   *         after restoring it, which keeps a replay deterministic.
   */
  auto schedule(icount at, event_id id, std::uint64_t payload) -> std::uint64_t;

  /**
   * @brief Schedules an event at a virtual time. Its instruction count follows every later core
   *        rate change, see rekey(). A time already passed makes it due at the next pop.
   * @param due Virtual time since reset.
   * @param id Owner and kind.
   * @param payload Passed back to the owner.
   * @param time Clock converting @p due at its current rate.
   * @return Sequence number, as schedule().
   */
  auto schedule_at(vtime due, event_id id, std::uint64_t payload, clock const& time)
      -> std::uint64_t;

  /**
   * @brief Schedules an event delivered from another machine at a virtual time, in a lane. A
   *        time already passed keeps its own count, clamped to the start of the current epoch,
   *        so the event fires in key order with the local events of that count still pending.
   * @param due Virtual time since reset.
   * @param id Owner and kind.
   * @param payload Passed back to the owner.
   * @param lane Delivery lane, 1 and up.
   * @param time Clock converting @p due at its current rate.
   * @return Sequence number, a cancel() handle from a range of its own, at or above
   *         lane_seq_base, so the sequence numbers of local events do not depend on when lane
   *         events were posted.
   */
  auto schedule_at(
      vtime         due,
      event_id      id,
      std::uint64_t payload,
      std::uint32_t lane,
      clock const&  time) -> std::uint64_t pre(lane > 0);

  /**
   * @brief Recomputes the instruction count of every deadline::time event at @p time's current
   *        rate, never before its current instruction for a lane 0 event, and keeps the tie
   *        order. A lane event follows the rule of the lane overload of schedule_at(). Called by
   *        the machine on every core rate change.
   * @param time Clock after the change.
   */
  void rekey(clock const& time);

  /**
   * @brief Removes a pending event. A no-op when @p seq is unknown, already fired or already
   *        cancelled, so a caller may cancel a timer without tracking whether it fired.
   * @param seq Sequence number from schedule().
   */
  void cancel(std::uint64_t seq);

  /**
   * @brief Removes a pending event if @p owner scheduled it, a no-op otherwise.
   * @param seq Sequence number from schedule().
   * @param owner Model index that must own the event.
   */
  void cancel(std::uint64_t seq, std::uint32_t owner);

  /**
   * @brief Due count of the earliest pending event, the maximum count when the queue is empty.
   *        Cached and updated by every change to the queue, so the block hook reads it every
   *        translated block, and an event scheduled during a slice shortens that slice.
   */
  [[nodiscard]] auto earliest() const noexcept -> icount
  {
    return earliest_;
  }

  /** @brief Due count of the earliest pending event, empty when the queue is empty. */
  [[nodiscard]] auto next_deadline() const noexcept -> std::optional<icount>;

  /**
   * @brief Removes and returns the earliest event due at or before @p now, for the run loop.
   * @param now Current instruction count.
   * @return The event, empty when none is due.
   */
  auto pop_due(icount now) -> std::optional<event>;

  /** @brief Pending events in dispatch order, for a snapshot. */
  [[nodiscard]] auto pending() const -> std::span<event const>;

  /** @brief Sequence number the next schedule() returns, for a snapshot. */
  [[nodiscard]] auto next_seq() const noexcept -> std::uint64_t;

  /** @brief Lane sequence numbers handed out so far, for a snapshot. The next lane event gets
   * lane_seq_base plus this count. */
  [[nodiscard]] auto next_lane_seq() const noexcept -> std::uint64_t;

  /** @brief First sequence number of lane events, above any local one. */
  static constexpr std::uint64_t lane_seq_base = std::uint64_t{1} << 63;

  /**
   * @brief Replaces the queue with a snapshot's content, tie order and sequence counter included.
   * @param events Pending events as pending() returned them.
   * @param next_seq Sequence number as next_seq() returned it.
   * @param next_lane_seq Lane count as next_lane_seq() returned it.
   */
  void restore(
      std::span<event const> events,
      std::uint64_t          next_seq,
      std::uint64_t          next_lane_seq = 0);

private:
  void refresh_earliest() noexcept;

  std::vector<event> queue_;
  std::uint64_t      next_seq_      = 0;
  std::uint64_t      next_lane_seq_ = 0;
  icount             earliest_{std::numeric_limits<std::uint64_t>::max()};
};

/**
 * @brief One model's access to the event queue. Stamps every event with the model's index, so
 *        dispatch returns it to that model, and refuses to cancel events of other models. Built
 *        by the machine for each model, the model never sees the scheduler itself.
 */
class event_port
{
public:
  /**
   * @brief Creates the port of one model.
   * @param queue Event queue, must outlive the port.
   * @param owner Index of the model.
   */
  event_port(scheduler& queue, std::uint32_t owner) noexcept : queue_{queue}, owner_{owner}
  {}

  /**
   * @brief Schedules an event for the owning model.
   * @param at Instruction count at which the event is due.
   * @param kind Event kind, passed back to `periph::model::on_event`.
   * @param payload Passed back to `periph::model::on_event`.
   * @return Sequence number, the handle for cancel().
   */
  auto schedule(icount at, std::uint32_t kind, std::uint64_t payload) -> std::uint64_t
  {
    return queue_.schedule(at, event_id{owner_, kind}, payload);
  }

  /**
   * @brief Schedules an event for the owning model at a virtual time, kept across core rate
   *        changes. The way to schedule a delay in time, rather than clock.to_icount().
   * @param due Virtual time since reset.
   * @param kind Event kind, passed back to `periph::model::on_event`.
   * @param payload Passed back to `periph::model::on_event`.
   * @param time The machine's clock, `model_context::clock`.
   * @return Sequence number, the handle for cancel().
   */
  auto schedule_at(vtime due, std::uint32_t kind, std::uint64_t payload, clock const& time)
      -> std::uint64_t
  {
    return queue_.schedule_at(due, event_id{owner_, kind}, payload, time);
  }

  /**
   * @brief Schedules an event delivered from another machine for the owning model, in a lane.
   * @param due Virtual time since reset.
   * @param kind Event kind, passed back to `periph::model::on_event`.
   * @param payload Passed back to `periph::model::on_event`.
   * @param lane Delivery lane, 1 and up.
   * @param time The machine's clock, `model_context::clock`.
   * @return Sequence number, the handle for cancel().
   */
  auto schedule_at(
      vtime         due,
      std::uint32_t kind,
      std::uint64_t payload,
      std::uint32_t lane,
      clock const&  time) -> std::uint64_t pre(lane > 0)
  {
    return queue_.schedule_at(due, event_id{owner_, kind}, payload, lane, time);
  }

  /**
   * @brief Cancels one of the owning model's pending events. A no-op for an unknown, fired or
   *        cancelled seq and for a seq another model owns.
   * @param seq Sequence number from schedule().
   */
  void cancel(std::uint64_t seq)
  {
    queue_.cancel(seq, owner_);
  }

  /** @brief Index of the owning model. */
  [[nodiscard]] auto owner() const noexcept -> std::uint32_t
  {
    return owner_;
  }

private:
  scheduler&    queue_;
  std::uint32_t owner_;
};
}
