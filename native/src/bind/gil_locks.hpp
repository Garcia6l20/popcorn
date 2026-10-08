#pragma once

#include "../core/log_queue.hpp"
#include "casters.hpp"
#include "frames.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/link_guard.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
/**
 * @brief Runs @p body under the link lock of @p queue. Tries the lock first. On contention a
 *        thread holding the GIL releases it, blocks on the lock, runs @p body and unlocks before
 *        it takes the GIL back, so no thread waits on a link lock with the GIL held and none
 *        takes the GIL under one. A host thread without the GIL, running a posted command,
 *        blocks on the lock.
 * @param queue A link endpoint or USB hub, with lock() and try_lock() returning a
 *        core::link_guard.
 * @param body Called with the held core::link_guard. Copies native data out, creates and
 *        destroys no Python object.
 * @return What @p body returns.
 */
template <typename Queue, typename Body> auto link_locked(Queue const& queue, Body&& body)
{
  if(auto const held = queue.try_lock(); held.owns_lock())
  {
    return std::invoke(std::forward<Body>(body), held);
  }
  if(core::detail::on_host_thread and PyGILState_Check() == 0)
  {
    auto const held = queue.lock();
    return std::invoke(std::forward<Body>(body), held);
  }
  reflex::py::nb::gil_scoped_release const released;
  auto const                               held = queue.lock();
  return std::invoke(std::forward<Body>(body), held);
}

/**
 * @brief The Python thread state of a host thread, created by its first gil_entry and kept for
 *        the thread's life, and the counter of its handle that every gil_entry on the thread
 *        increments.
 */
struct host_python
{
  /** @brief Whether the thread state exists. */
  bool created = false;
  /** @brief The outer PyGILState_Ensure() that created it. */
  PyGILState_STATE outer{};
  /** @brief The state, saved while the thread runs no Python. */
  PyThreadState* saved = nullptr;
  /** @brief GIL entries of the machine whose thread this is, null off host threads. */
  std::atomic<std::uint64_t>* counter = nullptr;
};

/** @brief The host_python of the calling thread. */
inline auto this_host_python() noexcept -> host_python&
{
  thread_local host_python state;
  return state;
}

/**
 * @brief Takes the GIL on entry to a callback, refusing a thread that holds a link lock: a link
 *        lock is a leaf, its holder waits for nothing. On a host thread with no Python thread
 *        state yet, the first entry creates the persistent state with one PyGILState_Ensure()
 *        and saves it on exit, so a host thread that runs no Python callback never takes the
 *        GIL. Once the interpreter is finalizing such a thread blocks forever, as Python does to
 *        any thread that takes the GIL then: see available(). The finalizing check reads
 *        Py_IsInitialized() without the GIL and races the state's creation: the window is
 *        `_PyGILState_Fini`, accepted. An entry nested in a transient state, nanobind's own GIL
 *        take around a dying `python_error`, creates nothing and only releases that nesting.
 */
class gil_entry
{
public:
  /** @brief Takes the GIL, nested or not. */
  gil_entry() pre(core::detail::link_locks_held == 0)
  {
    if(not core::detail::on_host_thread)
    {
      held_.emplace();
      return;
    }
    auto& state = this_host_python();
    if(state.counter != nullptr)
    {
      state.counter->fetch_add(1, std::memory_order_relaxed);
    }
    if(state.created)
    {
      held_.emplace();
      return;
    }
    if(Py_IsInitialized() == 0 or Py_IsFinalizing() != 0)
    {
      for(;;)
      {
        std::this_thread::sleep_for(std::chrono::hours{1});
      }
    }
    auto const outer = PyGILState_Ensure();
    if(outer == PyGILState_LOCKED)
    {
      nested_ = outer;
      return;
    }
    state.outer   = outer;
    state.created = true;
    first_        = true;
  }
  /** @brief Not copyable. */
  gil_entry(gil_entry const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(gil_entry const&) -> gil_entry& = delete;
  /** @brief Releases the GIL, saving a thread state its entry created. */
  ~gil_entry()
  {
    if(first_)
    {
      this_host_python().saved = PyEval_SaveThread();
    }
    else if(nested_)
    {
      PyGILState_Release(*nested_);
    }
  }

  /**
   * @brief Whether a gil_entry can take the GIL now without blocking forever: false only on a
   *        host thread that never ran Python once the interpreter is finalizing. A destructor
   *        that finds it false leaks its Python references instead.
   */
  [[nodiscard]] static auto available() noexcept -> bool
  {
    return not core::detail::on_host_thread
        or this_host_python().created
        or (Py_IsInitialized() != 0 and Py_IsFinalizing() == 0);
  }

private:
  bool                                              first_ = false;
  std::optional<PyGILState_STATE>                   nested_;
  std::optional<reflex::py::nb::gil_scoped_acquire> held_;
};

/**
 * @brief Takes the mutex of @p on, never blocking on it with the GIL held. Tries it first. On
 *        contention it releases the GIL, blocks, and takes the GIL back with the mutex held, the
 *        allowed order. A thread that already holds another host's mutex first registers its
 *        wait in the process lock graph, then blocks in 50 ms steps, checking signals between
 *        them. Without a signal it waits as long as the mutex is held. The timed mutex queues
 *        no waiter between steps and is not fair: a holder re-taking it in a tight loop could
 *        starve the wait, which parked sections, paced by their user, do not do.
 * @return The lock, owned.
 * @throws popcorn::machine_running at once when the wait would close a cycle of host mutex
 *         holders, naming both machines.
 * @throws reflex::py::nb::python_error when a signal handler raised during the wait.
 */
inline auto lock_releasing_gil(core::host& on) -> core::held_lock
{
  using namespace std::chrono_literals;
  if(auto held = on.try_lock(); held.owns_lock())
  {
    return held;
  }
  auto const python = PyGILState_Check() != 0;
  if(core::detail::host_locks_held == 0)
  {
    if(not python)
    {
      return on.lock();
    }
    reflex::py::nb::gil_scoped_release const released;
    return on.lock();
  }
  auto const waiting = on.register_wait();
  while(true)
  {
    core::held_lock held;
    if(python)
    {
      reflex::py::nb::gil_scoped_release const released;
      held = on.lock_for(50ms);
    }
    else
    {
      held = on.lock_for(50ms);
    }
    if(held.owns_lock())
    {
      return held;
    }
    if(python and PyErr_CheckSignals() != 0)
    {
      throw reflex::py::nb::python_error();
    }
  }
}

/**
 * @brief lock_releasing_gil() for a removal that must never wait on another host: a thread
 *        that already holds a host mutex only tries.
 * @return The lock, unowned when the thread holds another host's mutex and this one is taken.
 */
inline auto try_lock_releasing_gil(core::host& on) -> core::held_lock
{
  if(auto held = on.try_lock(); held.owns_lock() or core::detail::host_locks_held > 0)
  {
    return held;
  }
  if(PyGILState_Check() == 0)
  {
    return on.lock();
  }
  reflex::py::nb::gil_scoped_release const released;
  return on.lock();
}

/** @brief What a log filter or handler error does at a flush. */
enum class log_errors
{
  /** @brief Raised from the flush: an await, halt() or park() with no end. */
  raise,
  /** @brief Sent to sys.unraisablehook: after an access that took effect. */
  report,
  /** @brief Kept for the next raising flush of the machine: the loop's drain. */
  defer,
};

/** @brief A command's failure no awaiter will see, waiting to be reported. */
struct late_failure
{
  /** @brief The error. */
  std::exception_ptr error;
  /** @brief A note the error gets in Python, empty for none. */
  std::string note;
  /** @brief The message of the unraisable report. */
  std::string message;
};

/**
 * @brief Where one machine's log records go: the queue its thread fills, the token it wakes a
 *        loop with, the forwarder Python installs once, and an error a deferred flush kept. The
 *        forwarder and the kept error are read and written with the GIL held. Late command
 *        failures wait beside the records under their own mutex, a leaf.
 */
struct log_forwarding
{
  /** @brief The fork generation it was made in: a forked child never flushes what it inherited,
   * the queue's mutex may stay held there. */
  std::uint64_t generation = core::host::fork_generation();
  /** @brief Guards late. */
  std::mutex late_mutex{};
  /** @brief Failures queued with no GIL, reported at the next flush. */
  std::vector<late_failure> late{};
  /** @brief The records, appended with no GIL by the thread that runs the machine. */
  std::shared_ptr<core::log_queue> queue = std::make_shared<core::log_queue>();
  /** @brief Token the queue wakes a loop with, distinct from every request token. */
  std::uint64_t token{};
  /** @brief Forwards one batch of records to Python in order, empty until installed. */
  std::function<void()> flush{};
  /** @brief The first error a deferred flush met, raised by the next raising flush. */
  std::exception_ptr deferred{};
};

/** @brief The host of a machine, empty until its handle hands the machine over. */
struct host_slot
{
  /** @brief The host, expired once the handle destroyed it. */
  std::weak_ptr<core::host> host;
  /** @brief Guards retired, a leaf: nothing is taken under it. */
  mutable std::mutex retired_mutex{};
  /** @brief Removals retire() could not run at once, run by the engine owner at the next
   * boundary or parked access, or once the host is closed. */
  mutable std::vector<std::move_only_function<void()>> retired{};
  /** @brief Whether retired may hold a removal, read without the mutex. */
  mutable std::atomic<bool> retiring{false};
  /** @brief The abandoned host's thread, set under retired_mutex before abandoned. */
  mutable std::weak_ptr<void const> orphan{};
  /** @brief Whether the host was abandoned: removals then wait until orphan expired. */
  mutable std::atomic<bool> abandoned{false};
  /** @brief Where the machine's log records go, shared with its HostFutures. */
  std::shared_ptr<log_forwarding> logs = std::make_shared<log_forwarding>();

  host_slot()                                    = default;
  host_slot(host_slot const&)                    = delete;
  auto operator=(host_slot const&) -> host_slot& = delete;
  /**
   * @brief Runs the removals an abandoned host left once its thread is gone, keeps them for the
   *        rest of the process otherwise.
   */
  ~host_slot();
};

/**
 * @brief Queues @p failure on @p logs and wakes the loop the records wake. Takes no GIL, for the
 *        thread that completes a command.
 */
inline void queue_late_failure(log_forwarding& logs, late_failure failure)
{
  {
    std::scoped_lock const lock{logs.late_mutex};
    logs.late.push_back(std::move(failure));
  }
  logs.queue->poke();
}

/**
 * @brief Sends every late failure queued on @p logs to sys.unraisablehook, on a thread holding
 *        the GIL, the caller's error indicator kept.
 */
void report_late_failures(log_forwarding& logs) noexcept;

/**
 * @brief Sends @p failures to sys.unraisablehook, on a thread holding the GIL, the caller's
 *        error indicator kept.
 */
void report_late_failures(std::span<late_failure const> failures) noexcept;

/**
 * @brief Forwards the queued log records of @p logs to Python, on a thread holding the GIL and
 *        no host mutex, never on a host thread: the records wait there for the next flush. Late
 *        command failures queued on @p logs are reported first.
 * @param logs The machine's log forwarding.
 * @param errors What a log filter or handler error does: raised, after an error a deferred flush
 *        kept, reported to sys.unraisablehook with a kept error before it, or kept for the next
 *        raising flush. A kept error is detached from its frames by release_frames() first: held
 *        natively, its frames would keep the machine they reach out of the collector's sight.
 *        A `__context__` whose frames still run is kept as it is, so a flush made inside an
 *        `except` block keeps that handler's frames until the error is raised or reported.
 *        A KeyboardInterrupt or SystemExit is never kept: raised with log_errors::raise and
 *        log_errors::defer, a KeyboardInterrupt re-armed for the next signal check with
 *        log_errors::report, the interrupted record kept for the next flush.
 * @throws reflex::py::nb::python_error with log_errors::raise, and a KeyboardInterrupt or
 *         SystemExit with log_errors::defer.
 */
inline void flush_logs(log_forwarding& logs, log_errors errors)
{
  if(core::detail::on_host_thread
     or core::detail::host_locks_held > 0
     or PyGILState_Check() == 0
     or logs.generation != core::host::fork_generation())
  {
    return;
  }
  report_late_failures(logs);
  if(not logs.flush)
  {
    return;
  }
  if(errors == log_errors::raise and logs.deferred)
  {
    std::rethrow_exception(std::exchange(logs.deferred, nullptr));
  }
  if(errors == log_errors::report and logs.deferred)
  {
    try
    {
      std::rethrow_exception(std::exchange(logs.deferred, nullptr));
    }
    catch(reflex::py::nb::python_error& kept)
    {
      kept.discard_as_unraisable("popcorn log forwarding");
    }
  }
  try
  {
    logs.flush();
  }
  catch(reflex::py::nb::python_error& raised)
  {
    if(errors == log_errors::raise)
    {
      throw;
    }
    if(not raised.matches(PyExc_Exception))
    {
      if(errors == log_errors::defer)
      {
        throw;
      }
      if(raised.matches(PyExc_KeyboardInterrupt))
      {
        PyErr_SetInterruptEx(SIGINT);
        return;
      }
    }
    if(errors == log_errors::defer and not logs.deferred)
    {
      release_frames(raised.value());
      logs.deferred = std::current_exception();
      return;
    }
    raised.discard_as_unraisable("popcorn log forwarding");
  }
}

/**
 * @brief Raises the log filter or handler error a deferred flush of @p logs kept, if any, on a
 *        thread holding the GIL.
 * @throws reflex::py::nb::python_error the kept error.
 */
inline void raise_kept_log_error(log_forwarding& logs)
{
  if(logs.deferred and PyGILState_Check() != 0)
  {
    std::rethrow_exception(std::exchange(logs.deferred, nullptr));
  }
}

/**
 * @brief Calls flush_logs() when it leaves scope, after a parked access released the host
 *        mutex. The access already took effect, so a log filter or handler error goes to
 *        sys.unraisablehook, never out of the access.
 */
class logs_flushed
{
public:
  /** @brief Flushes @p slot on scope exit. */
  explicit logs_flushed(host_slot const& slot) noexcept : logs_{*slot.logs}
  {}
  /** @brief Not copyable. */
  logs_flushed(logs_flushed const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(logs_flushed const&) -> logs_flushed& = delete;
  /** @brief Flushes. */
  ~logs_flushed()
  {
    try
    {
      flush_logs(logs_, log_errors::report);
    }
    catch(...)
    {}
  }

private:
  log_forwarding& logs_;
};

/**
 * @brief Keeps @p body, a removal of engine state, alive for the rest of the process without
 *        running it: the abandoned host's thread may still run the engine it would edit.
 */
inline void keep_forever(std::move_only_function<void()> body) noexcept
{
  std::ignore = std::make_unique<std::move_only_function<void()>>(std::move(body)).release();
}

/**
 * @brief Queues @p body, when set, on the slot of an abandoned host, then runs every queued
 *        removal once the host thread left the engine for good. The batch runs outside
 *        retired_mutex, since a removal may retire more: the GIL every caller holds keeps two
 *        batches of one slot apart. Never throws.
 */
inline void run_orphaned(host_slot const& slot, std::move_only_function<void()> body) noexcept
{
  std::vector<std::move_only_function<void()>> ready;
  {
    std::scoped_lock const lock{slot.retired_mutex};
    if(body)
    {
      slot.retired.push_back(std::move(body));
      slot.retiring.store(true, std::memory_order_release);
    }
    if(slot.orphan.expired())
    {
      ready = std::exchange(slot.retired, {});
      slot.retiring.store(false, std::memory_order_release);
    }
  }
  for(auto& removal : ready)
  {
    try
    {
      removal();
    }
    catch(...)
    {}
  }
}

/**
 * @brief Marks @p slot abandoned with @p orphan, the thread abandon_thread() gave up, and runs
 *        its queued removals if that thread is already gone. Never throws.
 */
inline void give_up(host_slot const& slot, std::weak_ptr<void const> orphan) noexcept
{
  {
    std::scoped_lock const lock{slot.retired_mutex};
    if(not slot.abandoned.load(std::memory_order_relaxed))
    {
      slot.orphan = std::move(orphan);
      slot.abandoned.store(true, std::memory_order_release);
    }
  }
  run_orphaned(slot, {});
}

inline host_slot::~host_slot()
{
  if(not abandoned.load(std::memory_order_acquire))
  {
    return;
  }
  auto const gone = orphan.expired();
  for(auto& body : std::exchange(retired, {}))
  {
    if(not gone)
    {
      keep_forever(std::move(body));
      continue;
    }
    try
    {
      body();
    }
    catch(...)
    {}
  }
}

/**
 * @brief Runs the removals queued on @p slot, on the engine owner or after the host closed.
 *        Never throws.
 */
inline void run_retired(host_slot const& slot) noexcept
{
  if(not slot.retiring.load(std::memory_order_acquire))
  {
    return;
  }
  std::vector<std::move_only_function<void()>> bodies;
  {
    std::scoped_lock const lock{slot.retired_mutex};
    bodies = std::exchange(slot.retired, {});
    slot.retiring.store(false, std::memory_order_release);
  }
  for(auto& body : bodies)
  {
    try
    {
      body();
    }
    catch(...)
    {}
  }
}

/** @brief Shared reference to a machine's host_slot, held by its views and links. */
using host_ref = std::shared_ptr<host_slot const>;

/**
 * @brief Fails the stamps a reset or restore superseded, from the destructor of a parked access,
 *        so one made by its body or nested inside it fails as the access ends.
 */
class superseding
{
public:
  /** @brief Supersedes on @p on, under @p held, at destruction. */
  superseding(core::host& on, core::held_lock const& held) noexcept : on_{on}, held_{held}
  {}
  superseding(superseding const&)                    = delete;
  auto operator=(superseding const&) -> superseding& = delete;
  /** @brief Supersedes, never throws. */
  ~superseding()
  {
    try
    {
      on_.supersede(held_, core::member_id{});
    }
    catch(...)
    {}
  }

private:
  core::host&            on_;
  core::held_lock const& held_;
};

/**
 * @brief Runs @p body on the only member of @p on synchronously: inline for the engine owner,
 *        directly under the host mutex while parked, taken with lock_releasing_gil(). A parked
 *        access first runs the removals queued on @p slot, as engine owner under the lock, and
 *        at its end fails the stamps a reset or restore inside it superseded.
 * @throws popcorn::machine_running while the member is busy, or when waiting for the mutex
 *         would deadlock, see lock_releasing_gil().
 */
template <typename Body>
auto host_access(core::host& on, host_slot const& slot, Body&& body)
    -> std::invoke_result_t<Body, core::machine&>
{
  if(on.owns_engine())
  {
    return std::invoke(std::forward<Body>(body), on.member_inline(core::member_id{}));
  }
  if(on.abandoned())
  {
    throw core::machine_closed("the host was abandoned");
  }
  auto const held = lock_releasing_gil(on);
  if(on.busy(held, core::member_id{}) or not on.parked(held, core::member_id{}))
  {
    throw popcorn::machine_running(
        "the machine is running: await m.read() or m.write(), or halt() it first");
  }
  core::owner_scope const owning{on, held};
  auto&                   engine = on.member(held, core::member_id{});
  run_retired(slot);
  superseding const ending{on, held};
  return std::invoke(std::forward<Body>(body), engine);
}

/**
 * @brief Whether the host of @p ref was never set: its machine is still being built.
 * @param ref The slot.
 */
inline auto host_never_set(host_ref const& ref) noexcept -> bool
{
  std::weak_ptr<core::host> const never;
  return not ref->host.owner_before(never) and not never.owner_before(ref->host);
}

/**
 * @brief host_access() through a machine's host_slot, for a view or link that reaches the
 *        engine through its own pointers. Before the host exists, while its machine is built on
 *        this thread, @p body runs directly.
 * @param ref The slot.
 * @param body Called with no argument.
 * @throws config_error once the machine's handle destroyed the host.
 */
template <typename Body>
auto host_access(host_ref const& ref, Body&& body) -> std::invoke_result_t<Body>
{
  if(auto const on = ref->host.lock())
  {
    logs_flushed const flushed{*ref};
    return host_access(*on, *ref, [&body](core::machine&) { return std::invoke(body); });
  }
  if(not host_never_set(ref))
  {
    throw config_error("machine", 0, "machine", "a machine that still exists");
  }
  return std::invoke(std::forward<Body>(body));
}

/**
 * @brief Destroys what @p body owns on the engine's side: a hook handle, and what its hook
 *        feeds. Inline for the engine owner. Otherwise under the host mutex, taken with
 *        lock_releasing_gil(), as a command: at once while parked, at the next boundary on the
 *        host thread while running. When the thread holds another host's mutex and this one is
 *        taken, queued on the slot for the next boundary or parked access. Never throws, for
 *        destructors on any thread.
 * @param ref The machine's host slot.
 * @param body Destroys or releases what it captured.
 */
inline void retire(host_ref const& ref, std::move_only_function<void()> body) noexcept
{
  try
  {
    auto const on = ref->host.lock();
    if(on != nullptr and on->owns_engine())
    {
      body();
      return;
    }
    if(on != nullptr and on->abandoned())
    {
      give_up(*ref, on->orphan());
    }
    if(ref->abandoned.load(std::memory_order_acquire))
    {
      run_orphaned(*ref, std::move(body));
      return;
    }
    if(on == nullptr)
    {
      body();
      return;
    }
    auto const held = try_lock_releasing_gil(*on);
    if(not held.owns_lock())
    {
      std::scoped_lock const lock{ref->retired_mutex};
      ref->retired.push_back(std::move(body));
      ref->retiring.store(true, std::memory_order_release);
      return;
    }
    std::ignore = on->post(
        held, core::member_id{}, [owned = std::move(body)](core::machine&) mutable { owned(); });
  }
  catch(...)
  {}
}
}
