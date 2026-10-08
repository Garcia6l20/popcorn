#include "host_future.hpp"

#include "errors.hpp"

#include <popcorn/core/errors.hpp>

#include <nanobind/nanobind.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;

constexpr auto signal_check = std::chrono::milliseconds{50};

struct registered_host
{
  std::weak_ptr<core::host>             host;
  std::weak_ptr<detail::log_forwarding> logs;
  std::string                           part;
};

constexpr auto exit_wait = std::chrono::seconds{10};

struct host_registry
{
  mutable std::mutex           mutex;
  std::vector<registered_host> hosts;

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
  count_interruptible_wait();
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

auto python_thread_hooks(
    std::shared_ptr<std::atomic<bool>>          live,
    std::shared_ptr<std::atomic<std::uint64_t>> gil_entries) -> core::host_hooks
{
  return {
      .thread_started =
          [live, gil_entries] {
            live->store(true);
            ++live_host_threads;
            this_host_python().counter = gil_entries.get();
          },
      .thread_stopping =
          [live, gil_entries] {
            if(auto& state = this_host_python(); state.created)
            {
              PyEval_RestoreThread(state.saved);
              PyGILState_Release(state.outer);
              state = {};
            }
            this_host_python().counter = nullptr;
            live->store(false);
            --live_host_threads;
          },
      .before_block = [] { contract_assert(PyGILState_Check() == 0); },
  };
}

void register_host(std::shared_ptr<core::host> const& on, host_ref const& slot, std::string part)
{
  auto const& hosts = registry();
  link_locked(hosts, [&](core::link_guard const&) {
    registry().hosts.push_back({.host = on, .logs = slot->logs, .part = std::move(part)});
  });
}

void unregister_host(std::shared_ptr<core::host> const& on)
{
  auto const& hosts = registry();
  link_locked(hosts, [&](core::link_guard const&) {
    std::erase_if(registry().hosts, [&on](registered_host const& entry) {
      return entry.host.expired()
          or not(entry.host.owner_before(on) or on.owner_before(entry.host));
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

host_future_handle::host_future_handle(
    std::shared_ptr<detail::pending_request> request,
    detail::host_ref                         machine)
    : request_{std::move(request)}, logs_{machine == nullptr ? nullptr : machine->logs}
{}

auto host_future_handle::done() const -> bool
{
  return request_->done();
}

auto host_future_handle::result(std::optional<double> timeout) -> nb::object
{
  detail::wait_releasing_gil(*request_, timeout);
  nb::object value;
  try
  {
    value = request_->value();
  }
  catch(...)
  {
    if(logs_ != nullptr)
    {
      detail::flush_logs(*logs_, detail::log_errors::defer);
    }
    throw;
  }
  if(logs_ != nullptr)
  {
    detail::flush_logs(*logs_, detail::log_errors::defer);
  }
  return value;
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

loop_waker_handle::loop_waker_handle() : waker_{std::make_shared<core::loop_waker>()}
{}

auto loop_waker_handle::fd() const -> int
{
  return waker_->fd();
}

auto loop_waker_handle::drain() -> std::vector<std::uint64_t>
{
  return waker_->drain();
}

auto loop_waker_handle::native() const -> std::weak_ptr<core::loop_waker>
{
  return waker_;
}

void host_future_handle::notify(loop_waker_handle const& waker, std::uint64_t token)
{
  if(logs_ != nullptr)
  {
    logs_->queue->wake_through(waker.native(), logs_->token);
  }
  request_->on_done([target = waker.native(), token]() noexcept {
    if(auto const live = target.lock())
    {
      live->notify(token);
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
  auto const&                                                      hosts = registry();
  std::vector<std::pair<std::shared_ptr<core::host>, std::string>> live;
  std::vector<std::shared_ptr<detail::log_forwarding>>             logs;
  detail::link_locked(hosts, [&](core::link_guard const&) {
    for(auto const& entry : registry().hosts)
    {
      if(auto on = entry.host.lock())
      {
        live.emplace_back(std::move(on), entry.part);
      }
      if(auto forwarding = entry.logs.lock())
      {
        logs.push_back(std::move(forwarding));
      }
    }
  });
  auto const until       = std::chrono::steady_clock::now() + exit_wait;
  auto       interrupted = false;
  while(not live.empty() and not interrupted and std::chrono::steady_clock::now() < until)
  {
    {
      nb::gil_scoped_release const released;
      std::erase_if(live, [](auto const& entry) { return entry.first->close_for(signal_check); });
    }
    if(PyErr_CheckSignals() != 0)
    {
      PyErr_Clear();
      interrupted = true;
    }
  }
  for(auto const& [on, part] : live)
  {
    PySys_WriteStderr(
        "popcorn: the machine thread of %s did not stop at exit, a handler is blocked, the "
        "process exits without joining it\n",
        part.c_str());
    nb::gil_scoped_release const released;
    on->abandon_thread();
  }
  for(auto const& forwarding : logs)
  {
    detail::flush_logs(*forwarding, detail::log_errors::report);
  }
}

namespace
{
std::atomic<std::uint64_t> interruptible_entries{0};
}

void detail::count_interruptible_wait() noexcept
{
  interruptible_entries.fetch_add(1, std::memory_order_relaxed);
}

auto interruptible_waits() -> std::uint64_t
{
  return interruptible_entries.load(std::memory_order_relaxed);
}

auto host_threads() -> std::uint64_t
{
  return live_host_threads.load();
}

auto on_host_thread() -> bool
{
  return core::detail::on_host_thread;
}

auto host_locks_held() -> std::uint64_t
{
  return static_cast<std::uint64_t>(core::detail::host_locks_held);
}
}
