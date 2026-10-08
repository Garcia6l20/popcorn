#pragma once

#include "../core/loop_waker.hpp"
#include "python_callable.hpp"

#include <popcorn/core/host.hpp>

#include <reflex/py.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
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
  /** @brief Cancels the request and returns once it is done. Called with the GIL held, released
   * while it blocks. */
  virtual void cancel() = 0;
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
   * @param cancel Cancels the request while it is not done, called with the GIL held.
   * @param failed Sees the exception the request failed with when value() raises it, empty for
   *        none, called with the GIL held.
   */
  future_request(
      core::host_future<T>                            future,
      std::function<reflex::py::nb::object(T const&)> convert,
      std::move_only_function<void()>                 cancel,
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

  void cancel() override
  {
    if(not future_.done() and cancel_)
    {
      cancel_();
    }
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

  core::host_future<T>                            future_;
  std::function<reflex::py::nb::object(T const&)> convert_;
  std::move_only_function<void()>                 cancel_;
  std::function<void(std::exception_ptr const&)>  failed_;
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

/** @brief Adds @p on to the registry shutdown_hosts() closes. */
void register_host(std::shared_ptr<core::host> const& on);

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
   */
  [[= py::skip]] explicit host_future_handle(std::shared_ptr<detail::pending_request> request);

  /** @brief Whether the request completed. */
  [[= py::doc{"Whether the request completed."}]] [[nodiscard]] auto done() const -> bool;

  /**
   * @brief The result, waiting with the GIL released.
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
   * @brief Queues @p token on @p waker once on completion, on the completing thread without the
   *        GIL, at once when already done. A waker destroyed meanwhile is skipped.
   * @param waker The loop's waker, held weakly.
   * @param token What its drain() returns for this request.
   */
  [[= py::doc{"Queue token on waker once on completion, without the GIL."}]] void
      notify(loop_waker_handle const& waker, std::uint64_t token);

  /** @brief Cancels the request: halts a goal, drops a wait. Returns when done. */
  [[= py::doc{"Cancel: halt a goal, drop a waiter. Returns when done."}]] void cancel();

private:
  std::shared_ptr<detail::pending_request> request_;
};

/** @brief Closes and joins every live host, and refuses to start new host threads. */
[[= py::doc{"Close and join every machine thread, refuse new ones. Run at interpreter exit."}]] void
    shutdown_hosts();

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
