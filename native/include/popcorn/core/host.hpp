#pragma once

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace popcorn::core
{
/** @brief The host closed before the request completed, or the request came after close(). */
class machine_closed : public error
{
public:
  using error::error;
};

/** @brief The request was cancelled: a goal by host::cancel() or host::halt(), a waiter by
 * waiter_handle::drop() or host::halt(). */
class operation_cancelled : public error
{
public:
  using error::error;
};

/** @brief Index of a machine among the members of its host. */
using member_id = std::size_t;

/** @brief A descriptor watched by host::watch(). */
using watch_id = std::uint64_t;

/**
 * @brief State of a hosted machine. parked: no goal and no free run, or a free run whose end is
 *        held, callers touch the machine directly under the host mutex. running: the host thread
 *        runs the machine, callers post commands. idle_wait: a free run idles with nothing due,
 *        the host thread sleeps until a request. stopping: the host closed, the machine stays
 *        reachable by direct access and nothing runs it.
 */
enum class[[= reflex::derive{reflex::Format}]] host_state : std::uint8_t
{
  parked,
  running,
  idle_wait,
  stopping,
};

/** @brief Bounds of every slice the host runs, how often it serves its mailbox. */
struct slice_bounds
{
  /** @brief slice_request::max_instructions of each slice. */
  icount max_instructions{1'000'000};
  /** @brief slice_request::wall_budget of each slice. */
  std::chrono::nanoseconds wall_budget{std::chrono::milliseconds{1}};
};

/** @brief A goal of exactly one slice, for a driver that slices on its own. */
struct single_slice
{
  /** @brief Bounds of the slice, in place of the host's. */
  slice_bounds bounds{};
  /** @brief slice_request::leave_until of the slice. */
  bool leave_until{};
};

/** @brief A run to a stop, carried across slices by the host as one request. */
struct run_goal
{
  /** @brief slice_request::until of every slice. leave_until is set until the goal executed an
   * instruction. */
  std::optional<address> until{};
  /** @brief Instruction budget of the whole goal. */
  std::optional<icount> budget{};
  /** @brief Exact instruction count of the whole goal, for stepping. */
  std::optional<icount> exact{};
  /** @brief slice_request::until_time of every slice. */
  std::optional<vtime> until_time{};
  /** @brief slice_request::stop_when of every slice, called on the host thread. */
  std::function<bool()> stop_when{};
  /** @brief Run one slice only, the goal completing when it ends, with or without a stop. Empty
   * for a goal over as many slices as it needs. */
  std::optional<single_slice> once{};
  /** @brief Called on the host thread as the engine owner when the goal ends, before the member
   * parks and before its future completes: with the stop, or with null when it ended without
   * one, by an error, a cancel or a yield. An exception it throws fails a goal that ended on a
   * stop, and is dropped after an error or a cancel, which the goal fails with. Empty for
   * none. */
  std::function<void(machine&, stop const*)> finish{};
};

/** @brief Outcome of a goal. */
struct goal_result
{
  /** @brief The stop that ended the goal, value-initialized when yielded. */
  stop stopped{};
  /** @brief Instructions the goal executed over all its slices. */
  icount executed{};
  /** @brief Whether a run_goal::once goal ended its slice without a stop. */
  bool yielded{};
};

/**
 * @brief One comparison of a scalar in plain memory against a constant. The scalar is @p size
 *        bytes read little-endian at @p where, shifted right by @p shift, kept to its low
 *        @p bits bits and sign-extended for kind::signed_int.
 */
struct condition
{
  /** @brief How the scalar compares. */
  enum class kind : std::uint8_t
  {
    unsigned_int,
    signed_int,
    floating,
  };

  /** @brief The comparison, scalar on the left. */
  enum class op : std::uint8_t
  {
    eq,
    ne,
    lt,
    le,
    gt,
    ge,
  };

  /** @brief First byte, inside one mapped memory region with all @p size bytes. */
  address where{};
  /** @brief Bytes read, 1 to 8, 4 or 8 for kind::floating. */
  std::uint8_t size{};
  /** @brief Bits dropped from the low end of the word read. */
  std::uint8_t shift{};
  /** @brief Bits of the scalar after the shift, 1 to 64. */
  std::uint8_t bits{64};
  /** @brief How the scalar compares when not masked. */
  kind encoding{};
  /** @brief The comparison. */
  op compare{};
  /** @brief Whether the scalar is ANDed with @p mask and compared unsigned. */
  bool masked{};
  /** @brief Mask applied when @p masked. */
  std::uint64_t mask{};
  /** @brief Integer constant, a signed one as its two's complement bits. */
  std::uint64_t value{};
  /** @brief Constant of kind::floating. */
  double real{};
};

/** @brief Flat conditions evaluated natively together, at most 64. */
struct condition_set
{
  /** @brief How the members combine. */
  enum class mode : std::uint8_t
  {
    any,
    all,
  };

  /** @brief Whether the set holds when any member holds or when every member holds. */
  mode combine{mode::any};
  /** @brief The conditions, bit i of waiter_handle::fired() for member i. */
  std::vector<condition> members{};
};

/**
 * @brief Evaluates @p tested on @p target, which must be stopped at an instruction boundary or
 *        inside a handler of its slice.
 * @return Whether the comparison holds, false when the read fails.
 */
[[nodiscard]] auto condition_holds(condition const& tested, machine& target) -> bool;

/** @brief A wait on a free-running machine. */
struct waiter_spec
{
  /** @brief Called on the host thread at the end of every slice after the boundary that
   * registered the waiter, except one that ended where it began without idling, holding the GIL
   * when it wraps Python. The waiter completes with a stop_reason::user stop of that slice end
   * when it returns true. */
  std::function<bool()> ready{};
  /** @brief Virtual time at which the waiter completes with a stop_reason::time stop. */
  std::optional<vtime> deadline{};
  /** @brief Virtual time after the boundary that registers the waiter at which it completes with
   * a stop_reason::time stop, for a caller that cannot read the running clock. Ignored when
   * deadline is set. */
  std::optional<vtime> after{};
  /** @brief Conditions evaluated on the host thread as the engine owner, with no GIL and no
   * lock, after every dispatched event and at slice ends, instead of @p ready. The waiter
   * completes with a stop_reason::user stop of the first boundary where the set holds. At its
   * deadline it completes with a stop_reason::time stop, the set not evaluated there, as a
   * goal's until_time stops. */
  std::optional<condition_set> when{};
};

/** @brief How a free run ended by itself: the stop, or the exception a handler threw. */
using held_end = std::variant<stop, std::exception_ptr>;

/** @brief Calls the host makes into its embedder, which must own no Python reference. */
struct host_hooks
{
  /** @brief Called first on the host thread. */
  std::move_only_function<void()> thread_started{};
  /** @brief Called last on the host thread, after everything the thread destroys. */
  std::move_only_function<void()> thread_stopping{};
  /** @brief Called on the caller of cancel(), halt() and close() before it may block. */
  std::move_only_function<void()> before_block{};
  /** @brief Called on the host thread as the engine owner before and after every slice of a
   * goal or a free run, with the member. Empty for none. */
  std::move_only_function<void(member_id)> boundary{};
  /** @brief Called on the host thread with no host mutex held, with the member and the
   * exception that ends its free run, before it parks holding it. It may replace the exception
   * with the one to hold. Must not throw. Empty for none. */
  std::move_only_function<void(member_id, std::exception_ptr&)> held{};
};

class host;
class held_lock;
class owner_scope;
template <typename T> class host_future;

namespace detail
{
struct host_core;
struct waiter;
struct member_slot;
template <typename T> class host_promise;
enum class fate : std::uint8_t;

/** @brief Host mutexes the current thread holds through a held_lock. */
inline thread_local int host_locks_held = 0;
/** @brief Host whose engine the current thread owns. */
inline thread_local host_core const* engine_owner = nullptr;
/** @brief Whether the current thread is the thread of some host. Never cleared. */
inline thread_local bool on_host_thread = false;

/** @brief Shared state of a host_future. */
template <typename T> struct future_state
{
  /** @brief Guards outcome and continuation. */
  std::mutex mutex;
  /** @brief Signalled on completion. */
  std::condition_variable completed;
  /** @brief The value or the error, set once. */
  std::optional<std::expected<T, std::exception_ptr>> outcome{};
  /** @brief Run once on completion, outside the mutex. */
  std::move_only_function<void() noexcept> continuation{};
  /** @brief Whether outcome is set, readable without the mutex. */
  std::atomic<bool> ready{false};

  /**
   * @brief Stores @p value unless an outcome exists, then runs the continuation on this thread.
   * @return Whether this call completed the future.
   */
  auto complete(std::expected<T, std::exception_ptr> value) -> bool
  {
    std::move_only_function<void() noexcept> next;
    {
      std::scoped_lock const lock{mutex};
      if(outcome)
      {
        return false;
      }
      outcome = std::move(value);
      next    = std::move(continuation);
      ready.store(true, std::memory_order_release);
    }
    completed.notify_all();
    if(next)
    {
      next();
    }
    return true;
  }
};

/** @brief Producer side of a host_future. */
template <typename T> class host_promise
{
public:
  /** @brief Creates a fresh shared state. */
  host_promise() : state_{std::make_shared<future_state<T>>()}
  {}

  /** @brief Completes with @p value. @return Whether this call completed the future. */
  auto set(T value) const -> bool
  {
    return state_->complete(std::move(value));
  }

  /** @brief Completes with @p error. @return Whether this call completed the future. */
  auto fail(std::exception_ptr error) const -> bool
  {
    return state_->complete(std::unexpected{std::move(error)});
  }

  /** @brief Whether the future completed. */
  [[nodiscard]] auto done() const noexcept -> bool
  {
    return state_->ready.load(std::memory_order_acquire);
  }

  /** @brief The consumer side. */
  [[nodiscard]] auto future() const -> host_future<T>;

  /** @brief Whether @p other is the consumer side of this promise. */
  [[nodiscard]] auto feeds(host_future<T> const& other) const noexcept -> bool;

private:
  std::shared_ptr<future_state<T>> state_;
};
}

/**
 * @brief Result of a host request, completed on the host thread or at once. Copyable, every copy
 *        sees the same outcome. A blocking wait needs the thread to hold no host mutex, unless
 *        the future is already done.
 */
template <typename T> class host_future
{
public:
  /** @brief An empty future, valid() false. */
  host_future() = default;

  /** @brief Whether the future has a shared state. */
  [[nodiscard]] auto valid() const noexcept -> bool
  {
    return state_ != nullptr;
  }

  /** @brief Whether the outcome is set. */
  [[nodiscard]] auto done() const noexcept -> bool pre(valid())
  {
    return state_->ready.load(std::memory_order_acquire);
  }

  /**
   * @brief Waits up to @p limit for the outcome.
   * @return Whether the future is done.
   */
  auto wait_for(std::chrono::nanoseconds limit) const -> bool pre(valid())
      pre(done() or detail::host_locks_held == 0)
  {
    std::unique_lock lock{state_->mutex};
    return state_->completed.wait_for(lock, limit, [this] { return state_->outcome.has_value(); });
  }

  /**
   * @brief Waits for the outcome.
   * @return A copy of the value.
   * @throws The stored error.
   */
  auto get() const -> T pre(valid()) pre(done() or detail::host_locks_held == 0)
  {
    std::unique_lock lock{state_->mutex};
    state_->completed.wait(lock, [this] { return state_->outcome.has_value(); });
    if(not *state_->outcome)
    {
      std::rethrow_exception(state_->outcome->error());
    }
    return **state_->outcome;
  }

  /**
   * @brief Sets the callback run once on completion, on the completing thread after the value is
   *        stored, outside the future's own mutex. The host thread completes outside every host
   *        mutex. A caller completes under the host mutex it holds: a parked stamped post, a run
   *        or a start failing stamps a parked reset superseded. Runs it at once on this thread
   *        when the future is done. Replaces an earlier callback not yet run,
   *        which is destroyed after the future's mutex is released.
   */
  void on_done(std::move_only_function<void() noexcept> callback) pre(valid())
  {
    {
      std::scoped_lock const lock{state_->mutex};
      if(not state_->outcome)
      {
        std::swap(state_->continuation, callback);
        return;
      }
    }
    callback();
  }

private:
  friend class detail::host_promise<T>;

  explicit host_future(std::shared_ptr<detail::future_state<T>> state) : state_{std::move(state)}
  {}

  std::shared_ptr<detail::future_state<T>> state_{};
};

template <typename T> auto detail::host_promise<T>::future() const -> host_future<T>
{
  return host_future<T>{state_};
}

template <typename T>
auto detail::host_promise<T>::feeds(host_future<T> const& other) const noexcept -> bool
{
  return other.state_ == state_;
}

/**
 * @brief A host mutex held by the current thread, from host::lock() or host::try_lock(). Kept
 *        in detail::host_locks_held while owned. Never moved to another thread.
 */
class held_lock
{
public:
  /** @brief Holds nothing. */
  held_lock() noexcept = default;
  /** @brief Takes over @p other's lock. */
  held_lock(held_lock&& other) noexcept;
  /** @brief Releases the lock held, then takes over @p other's. */
  auto operator=(held_lock&& other) noexcept -> held_lock&;
  /** @brief Not copyable. */
  held_lock(held_lock const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(held_lock const&) -> held_lock& = delete;
  /** @brief Releases the lock held. */
  ~held_lock();

  /** @brief Whether a mutex is held. */
  [[nodiscard]] auto owns_lock() const noexcept -> bool;

  /** @brief Whether the mutex held is @p on's. */
  [[nodiscard]] auto holds(host const& on) const noexcept -> bool;

  /** @brief Releases the mutex. */
  void unlock() pre(owns_lock());

private:
  friend class host;

  held_lock(std::unique_lock<std::mutex> lock, detail::host_core const* of) noexcept;

  std::unique_lock<std::mutex> lock_{};
  detail::host_core const*     of_{};
};

/**
 * @brief Marks the current thread as the owner of a parked host's engine for its lifetime, so
 *        an access from a callback of that section runs inline instead of locking the host
 *        mutex the thread already holds.
 */
class owner_scope
{
public:
  /** @brief Owns @p owner's engine while @p held keeps it parked. */
  owner_scope(host const& owner, held_lock const& held) pre(held.holds(owner));
  /** @brief Restores the previous owner. */
  ~owner_scope();
  /** @brief Not copyable. */
  owner_scope(owner_scope const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(owner_scope const&) -> owner_scope& = delete;

private:
  friend struct detail::host_core;

  explicit owner_scope(detail::host_core const* owner) noexcept;

  detail::host_core const* previous_;
};

/** @brief A wait registered by host::wait(). */
class waiter_handle
{
public:
  /** @brief Completes with the waiter's stop, or with the free run's held end. */
  [[nodiscard]] auto future() const -> host_future<stop>;

  /**
   * @brief Members of waiter_spec::when that held at the boundary that completed the waiter, bit
   *        i for member i. Read it after future() completed, which orders the read.
   * @return The mask, 0 for a waiter without conditions or completed otherwise, the free run's
   *         own end included.
   */
  [[nodiscard]] auto fired() const noexcept -> std::uint64_t;

  /**
   * @brief Removes the waiter: its future fails with operation_cancelled at once, and the host,
   *        woken even from an idle wait, stops evaluating it and releases its predicate. The free
   *        run continues. Takes no host mutex, so it is safe from a destructor on any thread.
   */
  void drop() noexcept;

private:
  friend class host;

  waiter_handle(std::shared_ptr<detail::waiter> waiter, host_future<stop> future);

  std::shared_ptr<detail::waiter> waiter_;
  host_future<stop>               future_;
};

/**
 * @brief Thread that owns machines and serves requests on them. Internal, no public C++ API.
 *
 * A member is parked or running. Parked, callers hold the host mutex and touch it directly.
 * Running, the host thread runs its slices without the mutex and callers post commands, served
 * at the next block boundary. The thread starts at the first run() or start(): a machine
 * only accessed parked never gets one. The host thread takes its mutex only to change state and
 * pop mail, never while it calls into a machine or completes a future.
 *
 * Methods that need the mutex take the caller's held_lock, so the caller chooses how to acquire
 * it. cancel(), halt() and close() take it themselves and may wait: they throw machine_running
 * when the thread holds any host mutex, and on any host thread they only request and return.
 */
class host final
{
public:
  /**
   * @brief Hosts @p members, built on the caller thread.
   * @param members Machines, member_id indexes them.
   * @param bounds Bounds of every slice.
   * @param hooks Thread and blocking hooks.
   */
  host(
      std::vector<std::unique_ptr<machine>> members,
      slice_bounds                          bounds = {},
      host_hooks                            hooks  = {}) pre(not members.empty());
  /**
   * @brief Closes and joins. On any host thread, or while the thread holds a host mutex, it does
   *        not join: the host thread is told to stop, destroys the members and exits alone.
   *        Without a started thread the members are destroyed here.
   */
  ~host();
  /** @brief Not copyable. */
  host(host const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(host const&) -> host& = delete;
  /** @brief Not movable. */
  host(host&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(host&&) -> host& = delete;

  /** @brief Blocks for the host mutex. A thread holds at most one host mutex. */
  [[nodiscard]] auto lock() -> held_lock pre(detail::host_locks_held == 0);

  /** @brief Takes the host mutex if free, otherwise returns an unowned lock. */
  [[nodiscard]] auto try_lock() -> held_lock;

  /** @brief Number of members. */
  [[nodiscard]] auto members() const noexcept -> std::size_t;

  /** @brief Whether callers may touch member @p who directly: the host thread does not run it. */
  [[nodiscard]] auto parked(held_lock const& held, member_id who) const
      -> bool pre(held.holds(*this)) pre(who < members());

  /** @brief Whether a goal is active or a free-run session is open, even one whose end is held. */
  [[nodiscard]] auto busy(held_lock const& held, member_id who) const -> bool pre(held.holds(*this))
      pre(who < members());

  /** @brief Whether start() was called and halt() not yet. */
  [[nodiscard]] auto session_open(held_lock const& held, member_id who) const
      -> bool pre(held.holds(*this)) pre(who < members());

  /** @brief Member @p who, parked, for direct access under @p held. */
  [[nodiscard]] auto member(held_lock const& held, member_id who) -> machine& pre(held.holds(*this))
      pre(who < members()) pre(parked(held, who));

  /** @brief Member @p who, for a thread that owns the engine: the host thread in a slice or a
   * command, or a caller inside its owner_scope. */
  [[nodiscard]] auto member_inline(member_id who) -> machine& pre(owns_engine())
      pre(who < members());

  /**
   * @brief Runs @p body on member @p who as one command. The future resolves to the virtual time
   *        read right before the body ran, or fails with what the body threw.
   *
   * Without @p at: from a thread that owns the engine, and from any thread while parked, the body
   * runs at once on the calling thread and the future is returned done. While running, the
   * command is queued, the slice interrupted, and the body runs on the host thread at the next
   * boundary, best effort.
   *
   * With @p at, a stamp: the body runs at the first block boundary at or after @p at, exactly at
   * @p at when the core is idle there, before a stop of that boundary is reported. Stamps run in
   * time order, equal stamps in posting order. A stamp equal to the machine's time runs at once.
   * A later stamp on a parked member stays pending until a goal or free run reaches it. A reset
   * or a restore fails every pending stamp with operation_cancelled. A body captures native
   * values only.
   *
   * @throws std::invalid_argument when @p at is before the machine's time, checked here unless
   *         the member is running. While running the check happens at the next boundary and
   *         fails the future instead.
   * @throws machine_running for a later stamp from a thread that owns the engine through a
   *         callback without holding @p held, outside the host thread.
   */
  auto post(
      held_lock const&                        held,
      member_id                               who,
      std::move_only_function<void(machine&)> body,
      std::optional<vtime> at = {}) -> host_future<vtime> pre(owns_engine() or held.holds(*this))
      pre(who < members());

  /**
   * @brief Removes a command of member @p who whose body has not run, best effort or stamped. Its
   *        future fails with operation_cancelled.
   * @return Whether @p pending was found and removed, false when the host already took it: its
   *         body ran or will run.
   */
  auto withdraw(held_lock const& held, member_id who, host_future<vtime> const& pending)
      -> bool pre(held.holds(*this)) pre(who < members()) pre(pending.valid());

  /**
   * @brief Runs member @p who to a stop over as many slices as needed. A stop_reason::idle stop
   *        ends the goal.
   * @return Completes with the stop and the instructions executed, or fails with what a handler
   *         threw, the member parked and resumable either way.
   * @throws machine_busy when the member is busy.
   * @throws machine_closed after close().
   */
  auto run(held_lock const& held, member_id who, run_goal goal)
      -> host_future<goal_result> pre(held.holds(*this)) pre(who < members());

  /**
   * @brief Opens a free-run session: slices with no bound but waiter deadlines until halt(). An
   *        idle core sleeps until a request. A stop other than idle, a waiter's time or user stop,
   *        or a handler exception ends the run and is held until halt().
   * @throws machine_busy when the member is busy.
   * @throws machine_closed after close().
   */
  void start(held_lock const& held, member_id who) pre(held.holds(*this)) pre(who < members());

  /**
   * @brief Waits on the free run of member @p who. A waiter with conditions or a deadline ends
   *        the running slice at its next block boundary, which registers it: conditions are first
   *        evaluated there and waiter_spec::after counts from there. Any other waiter registers
   *        at the next slice boundary.
   * @return The waiter, already done with the held end when there is one.
   */
  auto wait(held_lock const& held, member_id who, waiter_spec spec) -> waiter_handle
      pre(held.holds(*this)) pre(who < members()) pre(session_open(held, who));

  /**
   * @brief Ends the goal of member @p who at the next boundary, its future failing with
   *        operation_cancelled, and waits until the member parks. Returns at once without a goal.
   *        On a host thread it requests the end and returns.
   * @throws machine_running when the thread holds a host mutex.
   */
  void cancel(member_id who) pre(who < members());

  /**
   * @brief Closes the session of member @p who, ending its free run at the next boundary, and
   *        waits until the member parks and the host thread has released what it held for that
   *        park, completed waiters included. Pending waiters fail with operation_cancelled.
   *        Cancels a goal as cancel() does. On a host thread it requests the end of a running
   *        member and returns, and closes the session of a parked one at once.
   * @return The held end, cleared, when the free run had ended by itself.
   * @throws machine_running when the thread holds a host mutex.
   */
  auto halt(member_id who) -> std::optional<held_end> pre(who < members());

  /** @brief State of member @p who, read without the mutex. */
  [[nodiscard]] auto state(member_id who) const noexcept -> host_state pre(who < members());

  /** @brief Whether the current thread owns this host's engine. */
  [[nodiscard]] auto owns_engine() const noexcept -> bool;

  /**
   * @brief Stops the host thread and joins it. Pending requests fail with machine_closed. The
   *        members stay reachable parked. Safe from several threads at once, one joins and the
   *        others wait for it. On a host thread it requests the stop and returns: the host
   *        thread then waits until the host is destroyed or closed from another thread.
   * @throws machine_running when the thread holds a host mutex.
   */
  void close();

  /**
   * @brief Wakes member @p who when @p fd turns readable, or writable while @p wants_write
   *        returns true, as long as its free run sleeps idle. A parked member is not polled:
   *        nothing reads for it. A running one is served at its next boundary anyway. Safe from
   *        any thread, with or without a host mutex.
   * @param who Member woken.
   * @param fd Descriptor polled. Closing it after unwatch() returned is safe: a poll in
   *        progress holds its own reference to the file and is woken by unwatch().
   * @param wants_write Called on the host thread under the host mutex, so it takes no lock.
   * @return Id for unwatch().
   */
  auto watch(member_id who, int fd, std::move_only_function<bool() const> wants_write) -> watch_id
      pre(who < members()) pre(fd >= 0);

  /** @brief Stops polling the descriptor of @p id, ignoring an unknown id. Safe from any thread. */
  void unwatch(watch_id id) noexcept;

private:
  friend class held_lock;
  friend class owner_scope;

  void ensure_thread();
  void request_stop();
  void post_stamped(
      held_lock const&                        held,
      detail::member_slot&                    slot,
      std::move_only_function<void(machine&)> body,
      vtime                                   at,
      detail::host_promise<vtime> const&      promise);
  void decide(detail::fate ending);

  std::shared_ptr<detail::host_core> core_;
  std::mutex                         join_mutex_;
  std::jthread                       thread_;
};

/**
 * @brief Runs @p body on member @p who synchronously: inline for the engine owner, directly under
 *        the host mutex while parked. Never waits on a command.
 * @throws machine_running when the member is busy, or when the thread already holds another
 *         host's mutex and this one is taken.
 */
template <std::invocable<machine&> F>
auto access(host& on, member_id who, F&& body) -> std::invoke_result_t<F, machine&>
{
  if(on.owns_engine())
  {
    return std::invoke(std::forward<F>(body), on.member_inline(who));
  }
  auto held = detail::host_locks_held == 0 ? on.lock() : on.try_lock();
  if(not held.owns_lock() or on.busy(held, who) or not on.parked(held, who))
  {
    throw machine_running("the machine is running");
  }
  owner_scope const owning{on, held};
  return std::invoke(std::forward<F>(body), on.member(held, who));
}
}
