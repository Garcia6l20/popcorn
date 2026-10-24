#pragma once

#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/core/window_barrier.hpp>
#include <popcorn/export.hpp>

#include <reflex/enum.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::core
{
/**
 * @brief A member of a linked system: a host of exactly one machine. The machine runs with
 *        machine_options::on_reset_request at reset_request_policy::stop, so every firmware or
 *        watchdog reset ends a slice and reaches the system, which applies on_reset.
 */
struct member_ref
{
  /** @brief The member's host, one member, not shared with another system. */
  std::shared_ptr<host> on;
  /** @brief Name of the member in errors. */
  std::string name;
  /** @brief What a reset request of the firmware does to a member that runs its core:
   * reset_and_continue resets it and runs on, stop ends its goal with the request. A member
   * whose core is held is always reset and runs on. */
  reset_request_policy on_reset{reset_request_policy::reset_and_continue};
};

/** @brief What one member runs to, the bounds of a lone machine's run. */
struct member_goal
{
  /** @brief Stop with stop_reason::address before executing this address. A goal that starts
   * there executes an instruction first, unless the address is in arrive_at. */
  std::optional<address> until{};
  /** @brief Further addresses that stop the goal as until does. */
  std::vector<address> until_also{};
  /** @brief Addresses of until and until_also the goal stops at when it starts there, 0
   * instructions executed: a wait the member was already running to. */
  std::vector<address> arrive_at{};
  /** @brief Instructions the goal may execute, halted time excluded. */
  std::optional<icount> budget{};
  /** @brief Stop with stop_reason::time at this system time. */
  std::optional<sys_time> until_time{};
  /** @brief Stop with stop_reason::budget after exactly this many instructions. */
  std::optional<icount> exact{};
  /** @brief Stop predicate, asked on the member's host thread after each dispatched event,
   * never at a barrier or a slice end. */
  std::move_only_function<bool()> stop_when{};
  /** @brief A goal a system IDLE does not resolve. */
  bool open{};
  /** @brief Virtual time the goal may run from its start: a stop_reason::time stop with detail
   * `goal cap` when it is reached first. */
  std::optional<sys_time> cap{};
  /** @brief Stop with stop_reason::returned at this return, slice_request::until_return. */
  std::optional<frame_return> until_return{};
};

/** @brief A goal reached: the member's stop and its system time. */
struct goal_reached
{
  /** @brief The stop that resolved the goal. */
  stop stopped{};
  /** @brief System time of the stop. */
  sys_time at{};
};

/** @brief Why a linked system paused. */
enum class[[= reflex::derive{reflex::Format}]] pause_reason : std::uint8_t
{
  delivered,
  system_time,
  idle,
  error,
  requested,
  closed,
};

/**
 * @brief Wall time of the members' work between barrier rounds, measured while
 *        system::measure_work() is on. A member's work in a round runs from its departure from
 *        the previous barrier, or from the resume, to its arrival: slices, drains and planning.
 */
struct work_timing
{
  /** @brief Barrier rounds measured, phase A, B and C arrivals alike. */
  std::uint64_t rounds{};
  /** @brief Sum over rounds of the work of every member, the cost of one thread doing it all. */
  std::chrono::nanoseconds serial{};
  /** @brief Sum over rounds of the work of the slowest member, the cost with no barrier wait. */
  std::chrono::nanoseconds ideal{};
  /** @brief Wall time every member spent draining its inbound wires. */
  std::chrono::nanoseconds drains{};
};

/** @brief Outcome of one system::resume(). */
struct pause
{
  /** @brief Why the system paused. */
  pause_reason reason{};
  /** @brief Member whose entry was delivered, empty for a requested or closed pause. */
  std::optional<std::size_t> member{};
  /** @brief System time of the delivered entry, of the pending barrier for a requested pause. */
  sys_time at{};
  /** @brief The member's stop for pause_reason::delivered. */
  stop stopped{};
  /** @brief The error for pause_reason::error. */
  std::exception_ptr error{};
  /** @brief Windows exchanged since the resume, bookkeeping that depends on the quantum. Under
   * sync_mode::clocks the decision rendezvous plus every member's clock waits, bookkeeping that
   * depends on thread timing. */
  std::uint64_t windows{};
  /** @brief Slices run since the resume over every member, bookkeeping. */
  std::uint64_t slices{};
  /** @brief Phase B rounds since the resume, halted members moved before a decision. */
  std::uint64_t phase_b{};
  /** @brief Work timing since the resume, zero unless system::measure_work() is on. */
  work_timing work{};
};

/** @brief How the members of a linked system keep in step. */
enum class[[= reflex::derive{reflex::Format}]] sync_mode : std::uint8_t
{
  /** @brief Each member runs to the earliest clock its peers published plus the latency of the
   * edge from them, and the members meet only to decide a pause. */
  clocks,
  /** @brief Every member meets the others at a barrier every quantum. */
  barrier,
};

class system;

/** @brief The system mutex, held by the current thread. Never moved to another thread. */
class POPCORN_API held_system
{
public:
  /** @brief Whether this holds the mutex of @p of. */
  [[nodiscard]] auto holds(system const& of) const noexcept -> bool;

private:
  friend class system;

  held_system(system const* of, std::unique_lock<std::mutex> lock);

  system const*                owner_;
  std::unique_lock<std::mutex> lock_;
};

/**
 * @brief Several members on one virtual clock, each run by its own host thread, linked by
 *        member_link wires. Under sync_mode::barrier members meet at a window_barrier every
 *        quantum Q, the smallest link latency, at system times k Q from system start. After each
 *        barrier every receiver drains its inbound wires of the window on its own thread, senders
 *        in member index order, then link order. A member whose core runs is held at each barrier
 *        with slice_request::hold_from_until, so every post reaches its receiver at its exact
 *        time whatever Q, the slicing or the thread timing. A member with no goal is halted: its
 *        core is held and it advances to the system's decision after each barrier.
 *
 *        Under sync_mode::clocks every member publishes its clock after each slice and runs to
 *        the earliest clock of its coupling edges plus the edge's latency: every wire, an off-link
 *        cut owner to both ends of its link, a member with a goal to every halted member at
 *        latency 0, and an edge of latency Q between members with no path between them otherwise.
 *        A member with nothing due before that bound publishes it as its lead without running.
 *        A drain hands the posts starting before the earlier of the receiver's reach and the
 *        sender's clock. The members meet at the window_barrier only to decide a pause. After a
 *        member stop or an error every member stands at the stop time plus its coupling distance
 *        from the stopping member, or at its own stop before that point, a position that depends
 *        on neither the slicing nor the thread timing. A requested pause puts every member at the
 *        latest member position. Frames, IDLE times and every member's state at a time pause equal
 *        those of sync_mode::barrier. Under sync_mode::clocks commands, stamps, injections and
 *        cuts reach a member while the system is paused.
 *
 *        Lock order: the system mutex first, taken only by threads that are not host threads,
 *        then at most one host mutex at a time. Host threads never take it.
 */
class POPCORN_API system
{
public:
  /**
   * @brief Links the members' hosts, one host mutex at a time. Each wire's sender half must be
   *        attached to its sender's port by whoever built it.
   * @param members One-member hosts, parked, member index order.
   * @param links Wires between members, at least one.
   * @param bounds Bounds of every slice a member runs.
   * @param before_arrival Called on a member's host thread before each of its barrier
   *        arrivals with its index, holding nothing, for tests that perturb thread timing. A
   *        throw fails the member with that error at that arrival, the system pauses on it, and
   *        the hook is not called again for the failed member. The pause is at the member's
   *        position at that barrier, which depends on the quantum and on pause requests.
   * @param before_sleep Called on a member's host thread with its index right before it asks
   *        its host to sleep on IDLE inside open goals, holding nothing, for tests that wake it
   *        in that window. A throw fails the member with that error, the member wakes the
   *        others and the system pauses on it.
   * @param mode How the members keep in step. Under sync_mode::clocks before_arrival is also
   *        called after every slice, before the member publishes its clock.
   * @param lookahead Under sync_mode::clocks, how far past its position a member publishes its
   *        clock at most: the host is an inbound wire of that latency, so a stamp at or after a
   *        member's position plus @p lookahead lands exactly. A member with nothing due moves to
   *        its position plus @p lookahead, executing nothing, when it leads that far.
   * @throws machine_busy when a member is busy.
   */
  system(
      std::vector<member_ref>                    members,
      std::vector<std::shared_ptr<member_link>>  links,
      slice_bounds                               bounds         = {},
      std::move_only_function<void(std::size_t)> before_arrival = {},
      std::move_only_function<void(std::size_t)> before_sleep   = {},
      sync_mode                                  mode           = sync_mode::barrier,
      sys_time lookahead = default_lookahead) pre(not members.empty()) pre(not links.empty())
      pre(lookahead.ps > 0);

  /** @brief The default lookahead of a system, 10 ms of system time. */
  static constexpr sys_time default_lookahead{10'000'000'000};

  /** @brief Closes, waits for every member to park, then unlinks the hosts. Not on a host
   * thread, holding no host mutex. */
  ~system();
  /** @brief Not copyable. */
  system(system const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(system const&) -> system& = delete;
  /** @brief Not movable. */
  system(system&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(system&&) -> system& = delete;

  /** @brief Blocks for the system mutex. Not on a host thread, holding no host mutex. */
  [[nodiscard]] auto lock() -> held_system pre(not detail::on_host_thread)
      pre(detail::host_locks_held == 0);

  /** @brief Takes the system mutex if free, otherwise an unowned held_system. */
  [[nodiscard]] auto try_lock() -> held_system pre(not detail::on_host_thread)
      pre(detail::host_locks_held == 0);

  /**
   * @brief Fixes each member's system time base: member X's local time now is system time
   *        @p offsets[X].
   */
  void start(held_system const& held, std::span<sys_time const> offsets) pre(held.holds(*this))
      pre(offsets.size() == members());

  /**
   * @brief Runs the system until the next pause, every member on its own host. Without
   *        @p system_until the members are halted. With it every member runs its core to that
   *        system time, and the pause comes when the last one reached it, at the latest stop point,
   *        unless an earlier entry comes first: a goal's stop or an error. Members with a goal run
   *        it, the others are halted, or with @p system_until run their core as a system TIME goal
   *        unless release() halted them. When every member sleeps with nothing due, the system
   *        pauses as pause_reason::idle at the latest quiescent point and every goal that is not
   *        open resolves there with stop_reason::idle. When every goal left is open, the system
   *        sleeps instead: each host waits as an idle free run does, and the first member woken, by
   *        a command, a stamp, a watched descriptor or wake(), wakes the others. Where the run
   *        resumes then depends on wall time. Every pause drains the wires up to its point before
   *        it is decided, so an error a wire dates before the delivered entry comes first. A member
   *        stop delivered without a member goal leaves it undecided as a goal's stop does.
   * @return Completes once every member parked, from the last host thread to park.
   * @throws std::logic_error before start(), while a resume is in flight, with an undecided
   *         member, after finish(), or after an error pause stopped the system.
   * @throws emulation_error when a member was reset behind the system's back since the last
   *         pause.
   * @throws machine_closed after close(), or once a member's host refused a goal, which ends
   *         the system: the members already started park and the pause completes closed.
   */
  auto resume(held_system const& held, std::optional<sys_time> system_until = {})
      -> host_future<pause> pre(held.holds(*this));

  /**
   * @brief Gives member @p member a goal while paused. It starts at the member's position: a
   *        member undecided at a delivered point is re-issued there, a halted member starts where
   *        the last decision left it. A goal posted at the last delivered point runs first on the
   *        next resume, up to the next known entry, so the next delivery is the smallest point
   *        over everything known whatever the quantum.
   * @return Completes after the pause that delivers the goal's stop, once every member parked.
   *         Fails with the error of an error pause, operation_cancelled on withdraw() or
   *         finish(), machine_closed on close().
   * @throws std::logic_error while a resume is in flight, after finish(), or when the member
   *         has a goal pending.
   */
  auto post_goal(held_system const& held, std::size_t member, member_goal goal)
      -> host_future<goal_reached> pre(held.holds(*this)) pre(member < members());

  /**
   * @brief Withdraws the pending goal of member @p member while paused: its future fails with
   *        operation_cancelled and the member is halted from where it stands, a position that
   *        depends on the quantum when it ran past the last delivered point.
   * @throws std::logic_error while a resume is in flight or when the member has no pending goal.
   */
  void withdraw(held_system const& held, std::size_t member) pre(held.holds(*this))
      pre(member < members());

  /**
   * @brief Releases member @p member, undecided after its goal was delivered. With @p halt false
   *        the next resume with a system_until runs its core from its stop as a system TIME goal.
   *        With @p halt true it is halted from where it stands, and runs no system TIME goal until
   *        it gets a goal again or is reset. Halting also takes a member that is not undecided.
   * @throws std::logic_error while a resume is in flight, when the member has a goal pending,
   *         or when @p halt is false and the member is not undecided.
   */
  void release(held_system const& held, std::size_t member, bool halt) pre(held.holds(*this))
      pre(member < members());

  /** @brief Members whose goal was delivered and which are neither re-issued nor released. */
  [[nodiscard]] auto undecided(held_system const& held) const
      -> std::vector<std::size_t> pre(held.holds(*this));

  /**
   * @brief Final stop at the last pause point S: every pending goal fails with
   *        operation_cancelled, every member's core hold is cleared with no time elapsed, and
   *        every wire settles its tap up to S. No resume after it.
   * @throws std::logic_error while a resume is in flight.
   */
  void finish(held_system const& held) pre(held.holds(*this));

  /**
   * @brief Appends the system's own state to @p out: the window, the pending barrier, each
   *        member's base and every link's state. Member machines are saved apart, with
   *        machine::snapshot(). Only with no goal pending, no member undecided and nothing
   *        found but not delivered, so a save never holds a goal or an error.
   * @throws std::logic_error before start(), while a resume is in flight, after an error or
   *         finish(), or with a goal, an undecided member or an entry pending.
   */
  void save(held_system const& held, std::vector<std::byte>& out) const pre(held.holds(*this));

  /**
   * @brief Restores a state save() wrote, after the member machines were restored from
   *        snapshots taken with it. Every member's reset count is taken as the system's own. The
   *        saving system's quantum need not match: its pending barrier stays, the barriers after
   *        it follow this system's quantum. After an error pause it clears the error, which
   *        ends the stop, and a resume runs again. A load that throws leaves the system as
   *        before.
   * @throws config_error when @p in is not such a state or does not match the members and
   *         links.
   * @throws std::logic_error as save(), except after an error pause.
   */
  void load(held_system const& held, std::span<std::byte const> in) pre(held.holds(*this));

  /**
   * @brief Checks that load() would accept @p in, changing nothing: the same refusals, the
   *        document, its members and links, and every link's state, each loaded and put back.
   *        For a caller that restores the member machines before load() and must know first.
   * @throws config_error, std::logic_error as load().
   */
  void check_state(held_system const& held, std::span<std::byte const> in) pre(held.holds(*this));

  /** @brief Asks for a pause at the next barrier, waking a system asleep on IDLE. Any thread
   * holding no host mutex, takes each host's mutex briefly. A request no resume consumed before
   * it paused is dropped by the next resume(). */
  void request_pause() noexcept;

  /**
   * @brief Wakes a system asleep on IDLE inside open goals: every member resumes from its
   *        quiescent point. For a caller that changed what a member will do without a command
   *        its host sees, such as an injection queued on a wire. Any thread holding no host
   *        mutex, takes each host's mutex briefly.
   */
  void wake() noexcept;

  /**
   * @brief Resets member @p member while paused: aborts its frame in flight, drops arrivals
   *        within L after the reset, resets the machine, and moves its base by its local time.
   *        The member is decided: a stop of it delivered or found and not delivered yet is
   *        dropped, and it runs its core on the next resume with a system time, as a member
   *        that never stopped.
   * @throws std::logic_error while a resume is in flight.
   */
  void reset(held_system const& held, std::size_t member, reset_kind kind) pre(held.holds(*this))
      pre(member < members());

  /** @brief Ends the system: every member parks at its next barrier or slice end and the pause
   * in flight completes as pause_reason::closed. Any thread holding no host mutex, takes each
   * host's mutex briefly to wake a sleeping member. */
  void close() noexcept;

  /**
   * @brief Turns pause::work timing on or off for the next resumes, for benches. Off by default:
   *        each member then reads no clock at its barrier arrivals or drains.
   * @throws std::logic_error while a resume is in flight.
   */
  void measure_work(held_system const& held, bool on) pre(held.holds(*this));

  /** @brief Number of members. */
  [[nodiscard]] auto members() const noexcept -> std::size_t;

  /** @brief The members' hosts, member index order, fixed at construction. Any thread. */
  [[nodiscard]] auto hosts() const -> std::vector<std::shared_ptr<host>>;

  /** @brief How the members keep in step, fixed at construction. */
  [[nodiscard]] auto mode() const noexcept -> sync_mode;

  /**
   * @brief Makes member @p member the cut owner of link @p link, the member whose callbacks may
   *        cut it while the system runs. Under sync_mode::clocks an owner that is neither end of
   *        the link bounds both ends by its clock plus the link's latency from the next resume.
   *        Each link's owner is its sender until set.
   * @param held The system lock.
   * @param link Index into the links the system was built with.
   * @param member Index of the new owner among the members.
   * @throws std::logic_error while a resume is in flight.
   * @throws std::out_of_range for no such link.
   */
  void set_cut_owner(held_system const& held, std::size_t link, std::size_t member)
      pre(held.holds(*this)) pre(member < members());

  /**
   * @brief The cut owner of link @p link. Any thread, no lock, also while the system runs.
   * @param link Index into the links the system was built with.
   * @return The owner's member index.
   * @throws std::out_of_range for no such link.
   */
  [[nodiscard]] auto cut_owner(std::size_t link) const -> std::size_t;

  /** @brief The quantum Q, the smallest link latency. */
  [[nodiscard]] auto quantum() const noexcept -> sys_time;

  /**
   * @brief The first barrier whose exchange has not happened, bookkeeping while paused. Under
   *        sync_mode::clocks the latest member position at the last pause.
   * @throws machine_running while a resume is in flight.
   */
  [[nodiscard]] auto pending_barrier(held_system const& held) const -> sys_time
      pre(held.holds(*this));

  /**
   * @brief System time of member @p member, parked.
   * @throws machine_running while a resume is in flight.
   */
  [[nodiscard]] auto time_of(held_system const& held, std::size_t member) const -> sys_time
      pre(held.holds(*this)) pre(member < members());

  /**
   * @brief Member @p member's base: its system time is its local time plus the base. Any
   *        thread, no lock, no wait, also while the system runs. Set by start(), moved by a
   *        reset of the member to the reset's system time, restored by load(). While the
   *        system runs, a reset under the reset policy moves it mid-run, so a read then may
   *        precede or follow that reset.
   */
  [[nodiscard]] auto base_of(std::size_t member) const noexcept -> sys_time pre(member < members());

  /**
   * @brief Instructions member @p member executed since the system started, parked.
   *        Sleep and halted time count none.
   * @throws machine_running while a resume is in flight.
   */
  [[nodiscard]] auto executed_of(held_system const& held, std::size_t member) const -> icount
      pre(held.holds(*this)) pre(member < members());

private:
  struct POPCORN_HIDDEN impl;
  std::indirect<impl>   impl_;
};
}
