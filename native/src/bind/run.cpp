#include "machine.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;

auto record(stop const& stopped) -> stop_record
{
  stop_record out{
      .reason       = stopped.reason,
      .where        = owned(stopped.where),
      .instructions = stopped.instructions.value,
      .time_ps      = stopped.time.ps,
      .fault        = std::nullopt,
      .detail       = stopped.detail,
  };
  if(stopped.fault)
  {
    out.fault = fault_record{
        .kind          = stopped.fault->kind,
        .fault_address = stopped.fault->fault_address,
        .status        = stopped.fault->status,
        .detail        = stopped.fault->detail,
    };
  }
  return out;
}

auto predicate_of(std::optional<nb::object> callable) -> std::function<bool()>
{
  if(not callable or callable->is_none())
  {
    return {};
  }
  return [held = detail::held(std::move(*callable))] {
    detail::gil_entry const gil;
    return nb::cast<bool>((*held)());
  };
}

auto cancel_goal(std::weak_ptr<core::host> host) -> std::move_only_function<void()>
{
  return [host = std::move(host)] {
    if(auto const on = host.lock())
    {
      nb::gil_scoped_release const released;
      on->cancel(core::member_id{});
    }
  };
}

void refuse_inside(core::host const& on)
{
  if(on.owns_engine())
  {
    throw popcorn::machine_busy("a run from inside a callback of this machine");
  }
  if(core::detail::on_host_thread or core::detail::host_locks_held > 0)
  {
    throw popcorn::machine_running("a run from inside a callback of another machine");
  }
}

auto later(std::uint64_t now_ps, std::uint64_t delay_ps) -> vtime
{
  if(delay_ps > std::numeric_limits<std::uint64_t>::max() - now_ps)
  {
    throw config_error("run", 0, "for", "a virtual time within 2^64 picoseconds");
  }
  return vtime{now_ps + delay_ps};
}
}

auto machine_handle::lock_for_request() const -> core::held_lock
{
  refuse_inside(*host_);
  detail::require_host_start();
  auto held = lock_releasing();
  if(host_->busy(held, core::member_id{}))
  {
    throw popcorn::machine_busy("a run, step, wait or running() is already active on this machine");
  }
  return held;
}

auto machine_handle::lock_releasing() const -> core::held_lock
{
  return detail::require_owned(detail::lock_releasing_gil(*host_));
}

auto machine_handle::run_goal(
    std::optional<address>       until,
    std::optional<std::uint64_t> budget,
    std::optional<std::uint64_t> exact,
    std::optional<std::uint64_t> for_ps,
    std::optional<nb::object>    stop_when) -> host_future_handle
{
  core::run_goal goal{.until = until, .stop_when = predicate_of(std::move(stop_when))};
  if(budget)
  {
    goal.budget = icount{*budget};
  }
  if(exact)
  {
    goal.exact = icount{*exact};
  }
  auto future = [&] {
    auto const held = lock_for_request();
    if(for_ps)
    {
      auto const now  = host_->member(held, core::member_id{}).virtual_clock().time();
      goal.until_time = later(now.ps, *for_ps);
    }
    return host_->run(held, core::member_id{}, std::move(goal));
  }();
  return host_future_handle{std::make_shared<detail::future_request<core::goal_result>>(
      std::move(future),
      [](core::goal_result const& result) { return nb::cast(record(result.stopped)); },
      cancel_goal(host_))};
}

auto machine_handle::call_goal(
    arch::call_frame const&      frame,
    std::optional<std::uint64_t> budget,
    std::optional<nb::object>    prepare) -> host_future_handle
{
  auto           ended = std::make_shared<call_outcome>();
  core::run_goal goal{.finish = [ended](core::machine& target, stop const* stopped) {
    if(stopped != nullptr and stopped->reason == stop_reason::fault)
    {
      for(auto const& reg : target.regs().list())
      {
        ended->registers.push_back(target.regs().read(reg));
      }
    }
    if(target.in_call())
    {
      ended->bits = target.end_call();
    }
  }};
  if(budget)
  {
    goal.budget = icount{*budget};
  }
  auto future = [&] {
    auto const              held = lock_for_request();
    core::owner_scope const owning{*host_, held};
    auto&                   target = host_->member(held, core::member_id{});
    if(target.in_call())
    {
      throw popcorn::machine_busy("a call is already in progress on this machine");
    }
    if(prepare and not prepare->is_none())
    {
      (*prepare)();
    }
    target.begin_call(frame);
    return host_->run(held, core::member_id{}, std::move(goal));
  }();
  return host_future_handle{std::make_shared<detail::future_request<core::goal_result>>(
      std::move(future),
      [ended](core::goal_result const& result) {
        auto outcome    = *ended;
        outcome.stopped = record(result.stopped);
        return nb::cast(std::move(outcome));
      },
      cancel_goal(host_))};
}

void machine_handle::start()
{
  auto const held = lock_for_request();
  host_->start(held, core::member_id{});
  delivered_->reset();
}

auto machine_handle::halt() -> nb::object
{
  if(not core::detail::on_host_thread and core::detail::host_locks_held == 0)
  {
    auto const held = lock_releasing();
    if(host_->busy(held, core::member_id{}) and not host_->session_open(held, core::member_id{}))
    {
      throw popcorn::machine_busy(
          "a run, step or call is active, not a free run: cancel its task instead");
    }
  }
  return park();
}

auto machine_handle::park() -> nb::object
{
  std::optional<core::held_end> ended;
  {
    nb::gil_scoped_release const released;
    ended = host_->halt(core::member_id{});
  }
  if(not ended)
  {
    return nb::none();
  }
  if(auto const* const stopped = std::get_if<stop>(&*ended))
  {
    return nb::cast(record(*stopped));
  }
  return detail::python_exception(std::get<std::exception_ptr>(*ended));
}

auto machine_handle::unreported_end() -> nb::object
{
  std::optional<core::held_end> ended;
  {
    nb::gil_scoped_release const released;
    ended = host_->halt(core::member_id{});
  }
  if(not ended)
  {
    return nb::none();
  }
  return detail::report_of(*ended, *delivered_);
}

void machine_handle::mark_collected() noexcept
{
  collected_->store(true, std::memory_order_release);
}

auto machine_handle::wait(std::optional<nb::object> ready, std::optional<std::uint64_t> for_ps)
    -> host_future_handle
{
  core::waiter_spec spec{.ready = predicate_of(std::move(ready))};
  if(for_ps)
  {
    spec.after = vtime{*for_ps};
  }
  refuse_inside(*host_);
  auto const predicate = spec.ready;
  auto const held      = lock_releasing();
  if(not host_->session_open(held, core::member_id{}))
  {
    throw std::runtime_error("wait() needs a free run opened by start()");
  }
  auto       waiting = host_->wait(held, core::member_id{}, std::move(spec));
  auto       future  = waiting.future();
  auto const made    = delivered_->generation();
  return host_future_handle{std::make_shared<detail::future_request<stop>>(
      std::move(future),
      [delivered = delivered_, made](stop const& stopped) {
        delivered->returned(made, stopped);
        return nb::cast(record(stopped));
      },
      [waiting = std::move(waiting)] mutable { waiting.drop(); },
      [delivered = delivered_, made](std::exception_ptr const& error) {
        delivered->raised(made, error);
      })};
}

auto machine_handle::busy() const -> bool
{
  if(host_->owns_engine())
  {
    return core::detail::host_locks_held == 0;
  }
  auto const held = lock_releasing();
  return host_->busy(held, core::member_id{});
}

auto machine_handle::session_open() const -> bool
{
  if(host_->owns_engine() and core::detail::host_locks_held > 0)
  {
    return false;
  }
  auto const held = lock_releasing();
  return host_->session_open(held, core::member_id{});
}

auto machine_handle::submit(nb::object body, std::optional<std::uint64_t> at_ps)
    -> host_future_handle
{
  if(core::detail::on_host_thread or core::detail::host_locks_held > 0)
  {
    throw std::runtime_error("a handler accesses the machine directly");
  }
  auto callable = detail::held(std::move(body));
  auto command  = [callable](core::machine&) {
    detail::gil_entry const gil;
    std::ignore = (*callable)();
  };
  std::optional<vtime> at;
  if(at_ps)
  {
    at = vtime{*at_ps};
  }
  auto future = [&] {
    auto const held = lock_releasing();
    return host_->post(held, core::member_id{}, std::move(command), at);
  }();
  auto withdraw = [host = std::weak_ptr{host_}, future] {
    if(auto const on = host.lock())
    {
      auto const held = detail::lock_releasing_gil(*on);
      if(held.owns_lock())
      {
        std::ignore = on->withdraw(held, core::member_id{}, future);
      }
    }
  };
  return host_future_handle{std::make_shared<detail::future_request<vtime>>(
      std::move(future), [](vtime const& ran) { return nb::cast(ran.ps); }, std::move(withdraw))};
}

auto machine_handle::parked(nb::object fn) -> nb::object
{
  return access([&fn](core::machine&) { return fn(); });
}

auto machine_handle::instructions() const -> std::uint64_t
{
  return access([](core::machine& target) { return target.virtual_clock().now().value; });
}

auto machine_handle::time_ps() const -> std::uint64_t
{
  return access([](core::machine& target) { return target.virtual_clock().time().ps; });
}

auto machine_handle::pc() const -> address
{
  return access([](core::machine& target) { return target.regs().pc(); });
}
}
