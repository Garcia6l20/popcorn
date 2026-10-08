#pragma once

#include "../core/loop_waker.hpp"
#include "python_callable.hpp"

#include <popcorn/core/host.hpp>

#include <reflex/py.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

namespace detail
{
/** @brief A host request a HostFuture stands for, type-erased. */
class pending_request
{
public:
  /** @brief Polymorphic. */
  pending_request() = default;
  /** @brief Not copyable. */
  pending_request(pending_request const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(pending_request const&) -> pending_request& = delete;
  /** @brief Polymorphic. */
  virtual ~pending_request() = default;

  /** @brief Whether the request completed. */
  [[nodiscard]] virtual auto done() const -> bool = 0;
  /** @brief Waits up to @p limit, the GIL released by the caller. @return Whether done. */
  virtual auto wait_for(std::chrono::nanoseconds limit) const -> bool = 0;
  /**
   * @brief The result as a Python object, the request done, the GIL held.
   * @throws The stored error, a Python one as a copy so it can be raised again.
   */
  [[nodiscard]] virtual auto value() const -> reflex::py::nb::object = 0;
  /** @brief Runs @p callback once on completion, on the completing thread. */
  virtual void on_done(std::move_only_function<void() noexcept> callback) = 0;
  /**
   * @brief Cancels the request and returns once it is done. Called with the GIL held, released
   *        while it blocks.
   * @return For a command, whether this call withdrew it: false when it was done already, the
   *         host took it, or its host mutex was contended, and it then runs. For a goal, whether
   *         a cancel was requested, and for a wait, whether it was dropped before it was done:
   *         either may still complete at the same boundary.
   */
  virtual auto cancel() -> bool = 0;
  /** @brief The error the request failed with and its note, a null error while running or
   * after a success. */
  [[nodiscard]] virtual auto failure() const -> std::pair<std::exception_ptr, std::string> = 0;
  /** @brief The virtual time a command ran at, empty for a goal, a wait, or while running. */
  [[nodiscard]] virtual auto ran_at() const -> std::optional<vtime>
  {
    return std::nullopt;
  }
};

/**
 * @brief A pending_request over a host_future.
 * @tparam T The future's value.
 */
template <typename T> class future_request final : public pending_request
{
public:
  /**
   * @brief Wraps @p future.
   * @param future The request's future.
   * @param convert Builds the Python result from the value, called with the GIL held.
   * @param cancel Cancels the request while it is not done, called with the GIL held, and
   *        returns what cancel() returns.
   * @param failed Sees the exception the request failed with when value() raises it, empty for
   *        none, called with the GIL held.
   */
  future_request(
      core::host_future<T>                            future,
      std::function<reflex::py::nb::object(T const&)> convert,
      std::move_only_function<bool()>                 cancel,
      std::function<void(std::exception_ptr const&)>  failed = {})
      : future_{std::move(future)}, convert_{std::move(convert)}, cancel_{std::move(cancel)},
        failed_{std::move(failed)}
  {}

  [[nodiscard]] auto done() const -> bool override
  {
    return future_.done();
  }

  auto wait_for(std::chrono::nanoseconds limit) const -> bool override
  {
    return future_.wait_for(limit);
  }

  [[nodiscard]] auto value() const -> reflex::py::nb::object override
  {
    try
    {
      return convert_(future_.get());
    }
    catch(reflex::py::nb::python_error const& raised)
    {
      saw(std::current_exception());
      throw reflex::py::nb::python_error(raised);
    }
    catch(...)
    {
      saw(std::current_exception());
      throw;
    }
  }

  void on_done(std::move_only_function<void() noexcept> callback) override
  {
    future_.on_done(std::move(callback));
  }

  auto cancel() -> bool override
  {
    return not future_.done() and cancel_ and cancel_();
  }

  [[nodiscard]] auto failure() const -> std::pair<std::exception_ptr, std::string> override
  {
    if(not future_.done())
    {
      return {};
    }
    try
    {
      std::ignore = future_.get();
    }
    catch(...)
    {
      return {std::current_exception(), {}};
    }
    return value_error_ ? value_error_() : std::pair<std::exception_ptr, std::string>{};
  }

  [[nodiscard]] auto ran_at() const -> std::optional<vtime> override
  {
    if constexpr(std::same_as<T, vtime>)
    {
      if(future_.done())
      {
        try
        {
          return future_.get();
        }
        catch(...)
        {
          return std::nullopt;
        }
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Sets how failure() finds a failure that a successful future carries in its value,
   *        read without the GIL, such as an op batch that stopped at a failed op, with its note.
   */
  void value_error(std::function<std::pair<std::exception_ptr, std::string>()> probe)
  {
    value_error_ = std::move(probe);
  }

  /** @brief The wrapped future. */
  [[nodiscard]] auto future() const -> core::host_future<T> const&
  {
    return future_;
  }

private:
  void saw(std::exception_ptr const& error) const
  {
    if(failed_)
    {
      failed_(error);
    }
  }

  core::host_future<T>                                        future_;
  std::function<reflex::py::nb::object(T const&)>             convert_;
  std::move_only_function<bool()>                             cancel_;
  std::function<void(std::exception_ptr const&)>              failed_;
  std::function<std::pair<std::exception_ptr, std::string>()> value_error_;
};

/**
 * @brief Waits for @p request with the GIL released, 50 ms at a time. Between waits it checks
 *        for signals: a pending KeyboardInterrupt cancels the request, then raises.
 * @param request The request.
 * @param timeout Seconds, empty for no limit.
 * @throws core::machine_running when not done and the thread is a host thread or holds a host
 *         mutex, which must never wait.
 * @throws reflex::py::nb::python_error for a signal, or TimeoutError past @p timeout.
 */
void wait_releasing_gil(pending_request& request, std::optional<double> timeout);

/**
 * @brief The Python exception object of @p error, translated as a raise would.
 * @param error A stored error.
 */
auto python_exception(std::exception_ptr const& error) -> reflex::py::nb::object;

/**
 * @brief Thread hooks of a host that may run Python: the thread starts without a Python thread
 *        state, its first gil_entry creates the persistent one, released when the thread stops,
 *        and the GIL is checked released before every blocking host call.
 * @param live Set while the thread runs.
 * @param gil_entries Incremented by every gil_entry on the thread.
 */
auto python_thread_hooks(
    std::shared_ptr<std::atomic<bool>>          live,
    std::shared_ptr<std::atomic<std::uint64_t>> gil_entries) -> core::host_hooks;

/** @brief Adds @p on to the registry shutdown_hosts() closes, with the slot whose log records
 * it forwards then. */
void register_host(std::shared_ptr<core::host> const& on, host_ref const& slot, std::string part);

/** @brief Removes @p on from the registry, and every expired entry, taking no ownership. */
void unregister_host(std::shared_ptr<core::host> const& on);

/**
 * @brief Keeps @p on alive for the rest of the process, its thread included, for a handle
 *        destroyed while the interpreter finalizes: joining a host thread then hangs, because
 *        the thread needs the GIL to release its thread state, and destroying the machine
 *        under a live thread is a use after free.
 */
void abandon(std::shared_ptr<core::host> on) noexcept;

/**
 * @brief Throws unless a host may start a thread now.
 * @throws std::runtime_error after shutdown_hosts() or while the interpreter finalizes.
 */
void require_host_start();
}

/**
 * @brief The eventfd and token queue an asyncio loop watches with `add_reader`, so a machine's
 *        thread wakes the loop without the GIL. One per loop.
 */
class[[= py::rename{"LoopWaker"}]] loop_waker_handle
{
public:
  /**
   * @brief Opens the eventfd.
   * @throws std::system_error when it cannot be created.
   */
  loop_waker_handle();

  /** @brief The eventfd, readable once a token is queued. */
  [[= py::doc{"The eventfd to watch for reading."}]] [[nodiscard]] auto fd() const -> int;

  /**
   * @brief Clears the eventfd's readiness, then takes the queued tokens in order.
   * @return The tokens, empty after a spurious wake.
   */
  [[= py::doc{"Clear the readiness, return the queued tokens in order."}]] auto drain()
      -> std::vector<std::uint64_t>;

  /** @brief The native waker, held weakly by the continuations that notify it. */
  [[= py::skip]] [[nodiscard]] auto native() const -> std::weak_ptr<core::loop_waker>;

private:
  std::shared_ptr<core::loop_waker> waker_;
};

/** @brief Result of a host request: a goal or a wait, completed on the machine's thread. */
class[[= py::rename{"HostFuture"}]] host_future_handle
{
public:
  /**
   * @brief Wraps a request.
   * @param request The request.
   * @param machine The slot of the machine it runs on, whose log records result() forwards
   *        first and whose log queue notify() points at the loop, null for none.
   */
  [[= py::skip]] explicit host_future_handle(
      std::shared_ptr<detail::pending_request> request,
      detail::host_ref                         machine = {});

  /** @brief Whether the request completed. */
  [[= py::doc{"Whether the request completed."}]] [[nodiscard]] auto done() const -> bool;

  /**
   * @brief The result, waiting with the GIL released, then forwards the machine's queued log
   *        records. A log filter or handler error met forwarding is kept and raised by the
   *        machine's next request, halt() or park(): the result, which a command may have taken
   *        from the machine, is returned.
   * @param timeout Seconds, None for no limit.
   * @throws TimeoutError past @p timeout.
   * @throws machine_running when not done, from a callback or a parked section.
   * @throws The error the request completed with.
   */
  [[= py::doc{"The result, waiting with the GIL released, timeout in seconds or None."}]] auto
      result(std::optional<double> timeout) -> reflex::py::nb::object;

  /**
   * @brief Calls @p fn once on completion, on the completing thread with the GIL held, at once
   *        when already done. An exception it raises goes to sys.unraisablehook.
   * @param fn Called with no argument.
   */
  [[= py::doc{"Call fn() once on completion, on the completing thread, GIL held."}]] void
      add_done_callback(reflex::py::nb::object fn);

  /**
   * @brief Has a later failure of the request reported to sys.unraisablehook with @p message.
   *        On completion, without the GIL, a failure is queued beside the machine's log records
   *        and reported at its next flush: a loop's drain, a result, halt, park, a parked
   *        access, exit or the machine's destruction. A success is silent. Holds no Python
   *        object. Replaces a callback or notify set earlier: for the future of a command its
   *        awaiter left, whose notify has no reader any more, and never one a user can reach.
   *        A request already done is reported at once, the GIL held by the caller, or queued
   *        when the caller is a machine's thread, a collection inside a handler.
   * @param message The unraisable report's message.
   * @param skip_closed Whether a failure because the machine closed goes unreported.
   * @param skip_cancelled Whether a cancel's own error, a withdrawn command, goes unreported.
   */
  [[= py::doc{"Have a later failure reported with message at the machine's next flush."}]] void
      report_late_failure(std::string message, bool skip_closed, bool skip_cancelled);

  /**
   * @brief Queues a warning beside the machine's log records, without the GIL, forwarded to the
   *        popcorn logger at the next flush, at the time the command ran. For a warning that
   *        may be raised on a machine's thread, where no log handler may run. Does nothing for
   *        a future that has no machine log.
   * @param message The warning.
   */
  [[= py::doc{
      "Queue a warning beside the machine's log records, forwarded at the next flush."}]] void
      warn_later(std::string message);

  /**
   * @brief Queues @p token on @p waker once on completion, on the completing thread without the
   *        GIL, at once when already done. A waker destroyed meanwhile is skipped. The machine's
   *        log queue wakes @p waker from now on, with its log token.
   * @param waker The loop's waker, held weakly.
   * @param token What its drain() returns for this request.
   */
  [[= py::doc{"Queue token on waker once on completion, without the GIL."}]] void
      notify(loop_waker_handle const& waker, std::uint64_t token);

  /**
   * @brief Cancels the request: halts a goal, drops a wait, withdraws a command. Returns when
   *        done.
   * @return For a command, whether this call withdrew it, false when it was done already or
   *         could not be withdrawn and runs. For a goal or a wait, whether it was cancelled
   *         before it was done, which may still complete at the same boundary.
   */
  [[= py::doc{"Cancel: halt a goal, drop a waiter, withdraw a command. Returns when done, and "
              "for a command whether this call withdrew it."}]] auto cancel() -> bool;

private:
  std::shared_ptr<detail::pending_request> request_;
  std::shared_ptr<detail::log_forwarding>  logs_;
};

/**
 * @brief Closes and joins every live host, and refuses to start new host threads. Waits at most
 *        10 s in all, checking signals every 50 ms: a host whose thread did not stop by then, a
 *        handler blocked, or at a signal, is abandoned and named on stderr.
 */
[[= py::doc{"Close and join every machine thread, refuse new ones. Run at interpreter exit."}]] void
    shutdown_hosts();

/** @brief Interruptible waits entered so far, for a request result or a park, for tests to
 * time a signal. */
[[= py::doc{"Interruptible waits entered so far, for a request result or a park, for tests."}]]
    [[nodiscard]] auto interruptible_waits() -> std::uint64_t;

/** @brief Throws a core::chained_error of a popcorn::error during an operation_cancelled, for
 * tests of its translation. */
[[= py::doc{"Raise a chained goal error, for tests."}]] [[noreturn]] void raise_chained_error();

/** @brief Waits for a host mutex registered so far, by a thread holding another one, for tests
 * to time a release. */
[[= py::doc{"Cross-machine lock waits registered so far, for tests."}]] [[nodiscard]] auto
    lock_waits() -> std::uint64_t;

namespace detail
{
/** @brief Counts one entry into an interruptible wait, for a request result or a park. */
void count_interruptible_wait() noexcept;
}

/** @brief Host threads running now, across every machine. */
[[= py::doc{"Number of machine threads running now."}]] [[nodiscard]] auto host_threads()
    -> std::uint64_t;

/** @brief Whether the calling thread is the thread of some machine. */
[[= py::doc{"Whether the calling thread is a machine thread, of any machine."}]] [[nodiscard]] auto
    on_host_thread() -> bool;

/** @brief Host mutexes the calling thread holds, above 0 inside a parked section. */
[[= py::doc{
    "Host mutexes the calling thread holds, above 0 inside a parked section."}]] [[nodiscard]] auto
    host_locks_held() -> std::uint64_t;
}
