#pragma once

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

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
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace popcorn::core
{
/** @brief The host closed before the request completed, or the request came after close(). */
class POPCORN_API machine_closed : public error
{
public:
  using error::error;

  /** @brief Destroys the error. Declared out-of-line: the key function pins the vtable. */
  ~machine_closed() override;
};

/** @brief The request was cancelled: a goal by host::cancel() or host::halt(), a waiter by
 * waiter_handle::drop() or host::halt(). */
class POPCORN_API operation_cancelled : public error
{
public:
  using error::error;

  /** @brief Destroys the error. Declared out-of-line: the key function pins the vtable. */
  ~operation_cancelled() override;
};

/**
 * @brief The host was built in the parent of a forked process: the child has no host thread and
 *        any host lock may stay held, so nothing in the child may use it.
 */
class POPCORN_API machine_forked : public error
{
public:
  using error::error;

  /** @brief Destroys the error. Declared out-of-line: the key function pins the vtable. */
  ~machine_forked() override;
};

/**
 * @brief A goal's finish failed while the goal ended by an error or a cancel: the finish error,
 *        with what the goal ended by, as Python's `__context__` chains them.
 */
class POPCORN_API chained_error final : public std::exception
{
public:
  /**
   * @param raised What the goal's finish threw.
   * @param during The error or cancel the goal ended by.
   */
  chained_error(std::exception_ptr raised, std::exception_ptr during) noexcept;

  /** @brief What the goal's finish threw. */
  [[nodiscard]] auto raised() const noexcept -> std::exception_ptr const&;

  /** @brief The error or cancel the goal ended by. */
  [[nodiscard]] auto during() const noexcept -> std::exception_ptr const&;

  /** @brief A fixed message, the two errors carry the detail. */
  [[nodiscard]] auto what() const noexcept -> char const* override;

private:
  std::exception_ptr raised_;
  std::exception_ptr during_;
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

/**
 * @brief Slices a goal for a scheduler that spans several hosts, one member per host. The host
 *        keeps serving the member's mail, stamps and boundary hook between slices.
 */
class POPCORN_API member_driver
{
public:
  /** @brief Reads the member's earliest pending stamp in local time, taking in the stamps
   * posted so far. Valid during plan() only, on the host thread. Takes the host mutex briefly:
   * call it holding no lock. */
  using earliest_stamp = std::function_ref<std::optional<vtime>()>;

  virtual ~member_driver();

  /**
   * @brief Bounds of the next slice, or empty to end the goal. Called on the host thread as the
   *        engine owner, with no host mutex held. May block, holding nothing, until abort().
   * @param target The member, the only machine the driver may touch.
   * @param stamp Reads the earliest pending stamp, which the slice must not pass: the host adds
   *        no stamp bound of its own. A driver that blocks reads it after its wait, so a stamp
   *        posted during the wait bounds the slice.
   */
  virtual auto plan(machine& target, earliest_stamp stamp) -> std::optional<slice_request> = 0;

  /**
   * @brief Outcome of the slice plan() returned, a stop_reason::time stop of a request with
   *        slice_request::internal_stop already cleared. Called on the host thread as the engine
   *        owner, with no host mutex held.
   */
  virtual void ran(machine& target, slice_result const& result) = 0;

  /**
   * @brief What plan(), the slice or ran() threw, in place of failing the goal: a scheduler
   *        that spans several hosts keeps every member in step. The goal goes on with the next
   *        plan(), which runs no further slice of a member its driver counts as failed. The
   *        error's time is the member's clock when this is called. Called on the host thread as
   *        the engine owner, with no host mutex held.
   */
  virtual void failed(machine& target, std::exception_ptr error) noexcept = 0;

  /** @brief Makes a blocked or later plan() return empty soon. Any thread, takes no lock. */
  virtual void abort() noexcept = 0;

  /**
   * @brief Asked when plan() returned empty: true keeps the goal and puts the member in
   *        host_state::idle_wait, as an idle free run, until a command, a stamp, a watched
   *        descriptor or host::wake() wakes it, then plan() is called again. False ends the goal.
   *        Called on the host thread with no host mutex held.
   */
  virtual auto sleep_requested() noexcept -> bool
  {
    return false;
  }

  /**
   * @brief Local time before which nothing may land on the member while a goal under the driver
   *        runs a slice: the host then serves its mail, accepts its stamps and calls the boundary
   *        hook at or after it. Read on the host thread between slices and while plan() reads a
   *        stamp.
   * @param target The member.
   * @return The member's time by default.
   */
  [[nodiscard]] virtual auto reach(machine const& target) const -> vtime;

  /** @brief Called under the host mutex when mail or a stamp is queued for the member, from any
   * thread: a driver blocked in plan() returns soon, so the host serves the mail at the
   * member's reach. Must not block or lock. */
  virtual void nudge() noexcept
  {}
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
  std::move_only_function<bool()> stop_when{};
  /** @brief slice_request::until_return of every slice. */
  std::optional<frame_return> until_return{};
  /** @brief Run one slice only, the goal completing when it ends, with or without a stop. Empty
   * for a goal over as many slices as it needs. */
  std::optional<single_slice> once{};
  /** @brief Called on the host thread as the engine owner when the goal ends, before the member
   * parks and before its future completes: with the stop, or with null when it ended without
   * one, by an error, a cancel or a yield. An exception it throws fails a goal that ended on a
   * stop. After an error or a cancel the goal fails with chained_error of both. A close of the
   * host fails the goal with machine_closed and skips it. Empty for none. */
  std::move_only_function<void(machine&, stop const*)> finish{};
  /** @brief Run first until the machine is machine::thread_ready(), then call then_thread on the
   * host thread as the engine owner and go on as the rest of the goal. The budget covers both
   * parts. A stop before thread mode ends the goal with then_thread never called. Never with
   * once. */
  bool until_thread{};
  /** @brief Called once when until_thread is met, before the next slice. An exception it throws
   * fails the goal. Empty for none. */
  std::move_only_function<void(machine&)> then_thread{};
  /** @brief Slices the goal in place of every field above, which stay empty. The goal never ends
   * on a stop by itself: it yields when plan() returns empty, and never fails by a throw of a
   * slice, plan() or ran(), which goes to member_driver::failed(). Empty for a goal the host
   * slices. */
  std::shared_ptr<member_driver> driver{};

  /** @brief Whether no field but driver is set, as a driver goal needs. */
  [[nodiscard]] auto driver_only() const noexcept -> bool
  {
    return not until
       and not budget
       and not exact
       and not until_time
       and not stop_when
       and not until_return
       and not once
       and not finish
       and not until_thread
       and not then_thread;
  }
};

/** @brief Outcome of a goal. */
struct goal_result
{
  /** @brief The stop that ended the goal, value-initialized when yielded. */
  stop stopped{};
  /** @brief Instructions the goal executed over all its slices. */
  icount executed{};
  /** @brief Whether a run_goal::once goal ended its slice without a stop, or a run_goal::driver
   * goal ended. */
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
[[nodiscard]] POPCORN_API auto condition_holds(condition const& tested, machine& target) -> bool;

/** @brief A wait on a free-running machine. */
struct waiter_spec
{
  /** @brief Called on the host thread at the end of every slice after the boundary that
   * registered the waiter, except one that ended where it began without idling, holding the GIL
   * when it wraps Python. The waiter completes with a stop_reason::user stop of that slice end
   * when it returns true. */
  std::move_only_function<bool()> ready{};
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
  /** @brief Called last on the host thread, after everything the thread destroys, boundary and
   * held included, which it destroys before this call. */
  std::move_only_function<void()> thread_stopping{};
  /** @brief Called on the caller of cancel(), halt() and close() before it may block. */
  std::move_only_function<void()> before_block{};
  /** @brief Called by ~host on the thread that drops the last share of a running host: false
   * gives the thread up as on a host thread instead of closing, which blocks. Empty for true. */
  std::move_only_function<bool()> may_block{};
  /** @brief Called on the host thread as the engine owner before and after every slice of a
   * goal or a free run, with the member. Under a member_driver only while the member stands at
   * or past member_driver::reach(). Empty for none. */
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

/** @brief The mutex of a host, timed so a cross-machine wait can check signals. */
using host_mutex = std::timed_mutex;
/** @brief Host mutexes the current thread holds through a held_lock. */
POPCORN_API extern constinit thread_local int host_locks_held;
/** @brief Host whose engine the current thread owns. */
POPCORN_API extern constinit thread_local host_core const* engine_owner;
/** @brief Whether the current thread is the thread of some host. Never cleared. */
POPCORN_API extern constinit thread_local bool on_host_thread;

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
   *        mutex. A caller completes under the host mutex it holds: a parked stamped post, a run,
   *        a start or supersede() failing stamps a parked reset superseded. Runs it at once on
   *        this thread when the future is done. Replaces an earlier callback not yet run, which
   *        is destroyed after the future's mutex is released.
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
class POPCORN_API held_lock
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

  held_lock(std::unique_lock<detail::host_mutex> lock, detail::host_core const* of);

  std::unique_lock<detail::host_mutex> lock_{};
  detail::host_core const*             of_{};
};

/**
 * @brief A thread's wait for a host mutex while it holds others, registered in the process lock
 *        graph until destroyed.
 */
class POPCORN_API lock_wait
{
public:
  /** @brief Not copyable. */
  lock_wait(lock_wait const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(lock_wait const&) -> lock_wait& = delete;
  /** @brief Deregisters the wait. */
  ~lock_wait();

private:
  friend class host;

  lock_wait() noexcept = default;
};

/**
 * @brief Marks the current thread as the owner of a parked host's engine for its lifetime, so
 *        an access from a callback of that section runs inline instead of locking the host
 *        mutex the thread already holds.
 */
class POPCORN_API owner_scope
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

/** @brief What host::request_halt() hands to host::wait_settled() and host::finish_halt(). */
struct halt_ticket
{
  /** @brief Whether there is a park to wait for and a session to close. */
  bool finish = false;
  /** @brief Whether the member ran on the host thread, so the park comes from it. */
  bool with_host = false;
  /** @brief The park count to wait past. */
  std::uint64_t parks = 0;
  /** @brief The free run count at the request: finish_halt() leaves a later free run open. */
  std::uint64_t session = 0;
};

/** @brief A wait registered by host::wait(). */
class POPCORN_API waiter_handle
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
   * @brief Whether the free run's own end completed the waiter, its stop or its exception, and
   *        not a boundary the waiter saw. Read it after future() completed, which orders the
   *        read. A stop equal to the end field by field still reads false when the waiter saw it.
   */
  [[nodiscard]] auto ended() const noexcept -> bool;

  /**
   * @brief When the waiter completes with a time stop: waiter_spec::deadline, or
   *        waiter_spec::after past the boundary that registered it. Read it after future()
   *        completed, which orders the read.
   * @return The deadline, empty without either, or when the free run ended before a boundary
   *         registered the waiter.
   */
  [[nodiscard]] auto deadline() const noexcept -> std::optional<vtime>;

  /** @brief Owners of the waiter, this handle included: the host's bookkeeping, for tests. */
  [[nodiscard]] auto holders() const noexcept -> long;

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
class POPCORN_API host final
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

  /** @brief Blocks for the host mutex, for a thread that holds no other one. */
  [[nodiscard]] auto lock() -> held_lock pre(detail::host_locks_held == 0);

  /** @brief Takes the host mutex if free, otherwise returns an unowned lock. */
  [[nodiscard]] auto try_lock() -> held_lock;

  /** @brief Waits up to @p limit for the host mutex, an unowned lock when it stays taken. */
  [[nodiscard]] auto lock_for(std::chrono::nanoseconds limit) -> held_lock;

  /**
   * @brief Registers the current thread, which holds other host mutexes, as waiting for this
   *        one. Checked against the process lock graph, recorded by held_lock: a thread that
   *        holds this mutex and waits, through other owners and waits, for a mutex this thread
   *        holds would deadlock with it.
   * @return The registration, kept while the thread waits.
   * @throws machine_running naming both machines when the wait would close a cycle.
   */
  [[nodiscard]] auto register_wait() -> lock_wait;

  /** @brief Names the host in the messages of refused waits. Written without synchronisation:
   * call it once, before another thread can reach the host. */
  void name(std::string label);

  /** @brief Waits register_wait() registered in the process so far, for tests. */
  [[nodiscard]] static auto waits_registered() noexcept -> std::uint64_t;

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

  /** @brief Member @p who, parked, for direct access under @p held.
   * @throws machine_closed once the host was abandoned: its thread frees the members. */
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
   * time order, equal stamps in posting order. A stamp equal to the machine's time runs at once,
   * unless an earlier or equal stamp is pending, or an event a held slice left due, see
   * machine::held_through(), is still pending: it then runs after the next slice dispatched
   * it.
   * A later stamp on a parked member stays pending until a goal or free run reaches it. A reset
   * or a restore fails every pending stamp with operation_cancelled. A body captures native
   * values only. A later stamp from the host thread in a callback, with no host mutex held, takes
   * this host's mutex blocking: native callers only, the binding refuses posts from a host
   * thread. While an event a slice_request::hold_from_until slice left held is pending, up to
   * machine::held_through(), due stamps wait for the next slice to dispatch it, so a body never
   * runs before the events due at its point. The mark is machine state, so a snapshot restore
   * and an unlinked member keep it.
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
   * @brief Fails the pending stamps of parked member @p who with operation_cancelled when a reset
   *        or a restore moved its timeline since they were taken, at once, under @p held. Does
   *        nothing once the host was abandoned: its members are gone.
   */
  void supersede(held_lock const& held, member_id who) pre(held.holds(*this)) pre(who < members());

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
   * @throws machine_busy when the member is busy, or linked and @p goal has no driver.
   * @throws machine_closed after close().
   */
  auto run(held_lock const& held, member_id who, run_goal goal)
      -> host_future<goal_result> pre(held.holds(*this)) pre(who < members())
          pre(not goal.driver or (goal.driver_only() and members() == 1));

  /**
   * @brief Opens a free-run session: slices with no bound but waiter deadlines until halt(). An
   *        idle core sleeps until a request. A stop other than idle, a waiter's time or user stop,
   *        or a handler exception ends the run and is held until halt().
   * @throws machine_busy when the member is busy or linked.
   * @throws machine_closed after close().
   */
  void start(held_lock const& held, member_id who) pre(held.holds(*this)) pre(who < members());

  /**
   * @brief Marks member @p who as a member of a linked system, or no longer one. While linked,
   *        run() without a driver and start() refuse it, until @p released holds when given:
   *        the system sets it once it has ended for good, before it reports that end. The
   *        closing system may still interrupt the member once after that, which ends one of its
   *        slices early. Parked access, post(), withdraw() and watch() work as before.
   * @throws machine_busy when @p on and the member is busy.
   */
  void link(
      held_lock const&         held,
      member_id                who,
      bool                     on,
      std::atomic<bool> const* released = nullptr) pre(held.holds(*this)) pre(who < members());

  /**
   * @brief Gives member @p who its core back when a linked system held it and has released
   *        it, as the first slice of the member's next goal or free run does. For a call begun
   *        under @p held, which runs before any slice. A no-op while the member is linked,
   *        busy or not parked, and on a machine no system linked since its last give-back.
   *        A hold set through the machine after the release and before that give-back is
   *        taken for the system's and dropped.
   * @throws What machine::hold_core() throws.
   */
  void give_back_core(held_lock const& held, member_id who) pre(held.holds(*this))
      pre(who < members());

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
   *        On a host thread it requests the end and returns. On an abandoned host it returns at
   *        once.
   * @throws machine_running when the thread holds a host mutex.
   * @throws machine_busy when the member runs a run_goal::driver goal.
   */
  void cancel(member_id who) pre(who < members());

  /**
   * @brief The first half of cancel(): requests the end of the goal of member @p who.
   * @return The park count wait_settled() waits past, empty when there is nothing to wait for:
   *         no goal, an abandoned host, or a host thread caller.
   * @throws machine_running when the thread holds a host mutex.
   * @throws machine_busy when the member runs a run_goal::driver goal.
   */
  auto request_cancel(member_id who) -> std::optional<std::uint64_t> pre(who < members());

  /**
   * @brief The first half of halt(): requests the end of member @p who's free run or goal.
   * @param refuse_goal Leave a goal running and throw instead, unless on a host thread: the
   *        caller halts free runs only, checked under the mutex.
   * @return What wait_settled() and finish_halt() take. Inactive with nothing to halt, on an
   *         abandoned host, or for a host thread caller of a running member.
   * @throws machine_running when the thread holds a host mutex.
   * @throws machine_busy with @p refuse_goal when a goal runs.
   * @throws machine_busy when the member runs a run_goal::driver goal.
   */
  auto request_halt(member_id who, bool refuse_goal = false) -> halt_ticket pre(who < members());

  /**
   * @brief Waits at most @p limit for the park @p ticket asks for, or for the host to be
   *        abandoned.
   * @return Whether the wait is over, true at once for an inactive ticket or on a host thread.
   */
  auto wait_settled(member_id who, halt_ticket const& ticket, std::chrono::nanoseconds limit)
      -> bool pre(who < members());

  /** @brief wait_settled() with no limit. */
  void wait_settled(member_id who, halt_ticket const& ticket) pre(who < members());

  /**
   * @brief The last half of halt(): closes the session once the member parked. A free run
   *        start() opened after request_halt() stays open: the park already closed the halted one.
   * @return The held end, cleared, when the free run had ended by itself, empty for an inactive
   *         ticket, an abandoned host or a free run started since the request.
   */
  auto finish_halt(member_id who, halt_ticket const& ticket)
      -> std::optional<held_end> pre(who < members());

  /**
   * @brief Gives the host thread up while it is blocked in a callback: the host closes at once,
   *        pending mail, stamps and waiters fail with machine_closed, a waiter's predicate left
   *        to the host thread, which may be running it, cancel(), halt() and close()
   *        return at once from then on, and the thread is detached. When the callback returns the
   *        thread fails its goal or session, releases its share of every member and exits. The
   *        failed requests' continuations run after join_mutex_ is released. Called without the
   *        GIL. A no-op once abandoned or without a thread to give up, never started or joined.
   */
  void abandon_thread();

  /**
   * @brief A share of member @p who: the host thread of an abandoned host releases only its own
   *        share, so a holder may keep reading the member's fixed data.
   */
  [[nodiscard]] auto member_owner(member_id who) const
      -> std::shared_ptr<machine> pre(who < members());

  /** @brief Whether abandon_thread() gave the host up, read without the mutex. */
  [[nodiscard]] auto abandoned() const noexcept -> bool;

  /**
   * @brief Whether the host was built before an `fork()` this process is the child of. Such a
   *        host refuses every lock, request and close with machine_forked, and its destruction
   *        leaks its thread handle and members instead of joining a thread that does not exist.
   */
  [[nodiscard]] auto forked() const noexcept -> bool;

  /** @brief @throws machine_forked when forked() is true. */
  void refuse_forked() const;

  /**
   * @brief The forks this process is a child of, counted by an `atfork` handler, for state that
   *        must tell what it inherited from the parent.
   */
  [[nodiscard]] static auto fork_generation() noexcept -> std::uint64_t;

  /**
   * @brief The thread abandon_thread() gave up, expired once it left the engine for good: from
   *        then on no thread runs the members, and a holder of a member_owner() share may edit
   *        their hooks. Valid from the destroyed host's copy as well.
   */
  [[nodiscard]] auto orphan() const -> std::weak_ptr<void const> pre(abandoned());

  /**
   * @brief The host thread's token, expired once the thread left the engine for good, and from
   *        the start when no thread was started. Safe while no other thread starts the host.
   */
  [[nodiscard]] auto thread_token() const -> std::weak_ptr<void const>;

  /**
   * @brief close(), waiting at most @p limit for the host thread to stop.
   * @return Whether the thread stopped and was joined. False leaves the host closed with its
   *         thread running: the caller abandons it or tries again.
   * @throws machine_running when the thread holds a host mutex.
   */
  auto close_for(std::chrono::nanoseconds limit) -> bool;

  /**
   * @brief Closes the session of member @p who, ending its free run at the next boundary, and
   *        waits until the member parks and the host thread has released what it held for that
   *        park, completed waiters included. Pending waiters fail with operation_cancelled.
   *        Cancels a goal as cancel() does. On a host thread it requests the end of a running
   *        member and returns, and closes the session of a parked one at once. Not
   *        machine::hold_core(), which keeps a member's clock running with its core held.
   * @param refuse_goal As for request_halt().
   * @return The held end, cleared, when the free run had ended by itself.
   * @throws machine_running when the thread holds a host mutex.
   * @throws machine_busy with @p refuse_goal when a goal runs.
   * @throws machine_busy when the member runs a run_goal::driver goal.
   */
  auto halt(member_id who, bool refuse_goal = false)
      -> std::optional<held_end> pre(who < members());

  /** @brief State of member @p who, read without the mutex. */
  [[nodiscard]] auto state(member_id who) const noexcept -> host_state pre(who < members());

  /** @brief Whether the current thread owns this host's engine. */
  [[nodiscard]] auto owns_engine() const noexcept -> bool;

  /**
   * @brief Wakes member @p who from host_state::idle_wait, or makes its next wait return at once,
   *        with no command. Any thread holding no other host mutex, takes this host's mutex
   *        briefly.
   */
  void wake(member_id who) pre(who < members());

  /**
   * @brief Stops the host thread and joins it. Pending requests fail with machine_closed. The
   *        members stay reachable parked. A run_goal::driver is aborted first, so a driver blocked
   *        in member_driver::plan() returns. Safe from several threads at once, one joins and the
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
   * @throws machine_forked in a process forked from the one that built the host.
   */
  auto watch(member_id who, int fd, std::move_only_function<bool() const> wants_write) -> watch_id
      pre(who < members()) pre(fd >= 0);

  /** @brief Stops polling the descriptor of @p id, ignoring an unknown id. Safe from any thread.
   * Does nothing in a forked process: its watch mutex may be held by a thread fork dropped. */
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
  auto stop_started() -> bool;
  void stop_unless_joining();

  std::shared_ptr<detail::host_core> core_;
  std::mutex                         join_mutex_;
  /** @brief The thread's token, written by ensure_thread() before the thread starts and read
   * by orphan() once abandoned() is true and by thread_token(), never written again. */
  std::weak_ptr<void const> running_;
  std::jthread              thread_;
};

/**
 * @brief Runs @p body on member @p who synchronously: inline for the engine owner, directly under
 *        the host mutex while parked. Never waits on a command.
 * @throws machine_running when the member is busy, or when the thread already holds another
 *         host's mutex and this one is taken.
 * @throws machine_closed once the host was abandoned.
 */
template <std::invocable<machine&> F>
auto access(host& on, member_id who, F&& body) -> std::invoke_result_t<F, machine&>
{
  if(on.owns_engine())
  {
    return std::invoke(std::forward<F>(body), on.member_inline(who));
  }
  if(on.abandoned())
  {
    throw machine_closed("the host was abandoned");
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
