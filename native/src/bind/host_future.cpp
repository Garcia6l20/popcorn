#include "host_future.hpp"

#include "errors.hpp"

#include <popcorn/core/errors.hpp>

#include <nanobind/nanobind.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;

constexpr auto signal_check = std::chrono::milliseconds{50};

struct host_registry
{
  mutable std::mutex                     mutex;
  std::vector<std::weak_ptr<core::host>> hosts;

  [[nodiscard]] auto lock() const -> core::link_guard
  {
    return core::link_guard{mutex};
  }

  [[nodiscard]] auto try_lock() const -> core::link_guard
  {
    return core::link_guard{mutex, std::try_to_lock};
  }
};

auto registry() -> host_registry&
{
  static host_registry hosts;
  return hosts;
}

std::atomic<bool>          hosts_shut_down{false};
std::atomic<std::uint64_t> live_host_threads{0};

[[noreturn]] void raise_timeout()
{
  PyErr_SetString(PyExc_TimeoutError, "the machine request did not complete in time");
  throw nb::python_error();
}
}

namespace detail
{
void wait_releasing_gil(pending_request& request, std::optional<double> timeout)
{
  if(request.done())
  {
    return;
  }
  if(core::detail::on_host_thread or core::detail::host_locks_held > 0)
  {
    throw popcorn::machine_running("a callback of a machine cannot wait for a machine request");
  }
  using clock      = std::chrono::steady_clock;
  auto const until = timeout ? clock::now()
                                   + std::chrono::duration_cast<clock::duration>(
                                       std::chrono::duration<double>{std::max(*timeout, 0.0)})
                             : clock::time_point::max();
  for(;;)
  {
    auto const left = until - clock::now();
    auto const step = std::min<clock::duration>(signal_check, std::max(left, clock::duration{}));
    auto       finished = false;
    {
      nb::gil_scoped_release const released;
      finished = request.wait_for(step);
    }
    if(finished)
    {
      return;
    }
    if(PyErr_CheckSignals() != 0)
    {
      nb::python_error const raised;
      request.cancel();
      throw raised;
    }
    if(clock::now() >= until)
    {
      raise_timeout();
    }
  }
}

auto python_exception(std::exception_ptr const& error) -> nb::object
{
  set_python_error(error);
  return nb::steal(PyErr_GetRaisedException());
}

auto python_thread_hooks(std::shared_ptr<std::atomic<bool>> live) -> core::host_hooks
{
  struct held_state
  {
    PyGILState_STATE outer{};
    PyThreadState*   saved = nullptr;
  };
  auto state = std::make_shared<held_state>();
  return {
      .thread_started =
          [state, live] {
            live->store(true);
            ++live_host_threads;
            state->outer = PyGILState_Ensure();
            state->saved = PyEval_SaveThread();
          },
      .thread_stopping =
          [state, live] {
            PyEval_RestoreThread(state->saved);
            PyGILState_Release(state->outer);
            live->store(false);
            --live_host_threads;
          },
      .before_block = [] { contract_assert(PyGILState_Check() == 0); },
  };
}

void register_host(std::shared_ptr<core::host> const& on)
{
  auto const& hosts = registry();
  link_locked(hosts, [&](core::link_guard const&) { registry().hosts.push_back(on); });
}

void unregister_host(std::shared_ptr<core::host> const& on)
{
  auto const& hosts = registry();
  link_locked(hosts, [&](core::link_guard const&) {
    std::erase_if(registry().hosts, [&on](std::weak_ptr<core::host> const& entry) {
      return entry.expired() or not(entry.owner_before(on) or on.owner_before(entry));
    });
  });
}

void abandon(std::shared_ptr<core::host> on) noexcept
{
  std::ignore = std::make_unique<std::shared_ptr<core::host>>(std::move(on)).release();
}

void require_host_start()
{
  if(hosts_shut_down.load() or Py_IsFinalizing() != 0)
  {
    throw std::runtime_error("the interpreter is shutting down");
  }
}
}

host_future_handle::host_future_handle(std::shared_ptr<detail::pending_request> request)
    : request_{std::move(request)}
{}

auto host_future_handle::done() const -> bool
{
  return request_->done();
}

auto host_future_handle::result(std::optional<double> timeout) -> nb::object
{
  detail::wait_releasing_gil(*request_, timeout);
  return request_->value();
}

void host_future_handle::add_done_callback(nb::object fn)
{
  auto callable = std::make_shared<detail::python_callable>(std::move(fn), nb::none());
  request_->on_done([callable = std::move(callable)]() noexcept {
    detail::gil_entry const held;
    try
    {
      (*callable)();
    }
    catch(nb::python_error& raised)
    {
      raised.discard_as_unraisable("HostFuture done callback");
    }
    catch(...)
    {
      PyErr_SetString(PyExc_RuntimeError, "a HostFuture done callback failed");
      PyErr_WriteUnraisable(nullptr);
    }
  });
}

void host_future_handle::cancel()
{
  request_->cancel();
}

void shutdown_hosts()
{
  hosts_shut_down.store(true);
  auto const& hosts = registry();
  auto const  live  = detail::link_locked(hosts, [&](core::link_guard const&) {
    std::vector<std::shared_ptr<core::host>> found;
    for(auto const& entry : registry().hosts)
    {
      if(auto on = entry.lock())
      {
        found.push_back(std::move(on));
      }
    }
    return found;
  });
  {
    nb::gil_scoped_release const released;
    for(auto const& on : live)
    {
      on->close();
    }
  }
}

auto host_threads() -> std::uint64_t
{
  return live_host_threads.load();
}

auto host_locks_held() -> std::uint64_t
{
  return static_cast<std::uint64_t>(core::detail::host_locks_held);
}
}
