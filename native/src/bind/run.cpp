#include "machine.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>

#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
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
  auto held = detail::require_owned(detail::lock_releasing_gil(*host_));
  if(host_->busy(held, core::member_id{}))
  {
    throw popcorn::machine_busy("the machine already runs a goal or a free run");
  }
  return held;
}

auto machine_handle::run_slice(slice_args const& args, std::optional<nb::object> stop_when)
    -> slice_outcome
{
  core::run_goal goal{
      .until      = args.until,
      .budget     = std::nullopt,
      .exact      = std::nullopt,
      .until_time = std::nullopt,
      .stop_when  = predicate_of(std::move(stop_when)),
      .once =
          core::single_slice{
                             .bounds =
                  {.max_instructions = icount{args.max_instructions},
                   .wall_budget      = std::chrono::nanoseconds{args.wall_budget_ns}},
                             .leave_until = args.leave_until,
                             },
  };
  if(args.budget_left)
  {
    goal.budget = icount{*args.budget_left};
  }
  if(args.exact_instructions)
  {
    goal.exact = icount{*args.exact_instructions};
  }
  if(args.until_time_ps)
  {
    goal.until_time = vtime{*args.until_time_ps};
  }
  auto future = [&] {
    auto const held = lock_for_request();
    return host_->run(held, core::member_id{}, std::move(goal));
  }();
  detail::future_request<core::goal_result> request{
      future, [](core::goal_result const&) { return nb::none(); }, cancel_goal(host_)};
  detail::wait_releasing_gil(request, std::nullopt);
  core::goal_result result;
  try
  {
    result = future.get();
  }
  catch(nb::python_error const& raised)
  {
    throw nb::python_error(raised);
  }
  slice_outcome out{.stopped = std::nullopt, .executed = result.executed.value};
  if(not result.yielded)
  {
    out.stopped = record(result.stopped);
  }
  return out;
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

void machine_handle::start()
{
  auto const held = lock_for_request();
  host_->start(held, core::member_id{});
}

auto machine_handle::halt() -> nb::object
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

auto machine_handle::wait(std::optional<nb::object> ready, std::optional<std::uint64_t> deadline_ps)
    -> host_future_handle
{
  core::waiter_spec spec{.ready = predicate_of(std::move(ready))};
  if(deadline_ps)
  {
    spec.deadline = vtime{*deadline_ps};
  }
  refuse_inside(*host_);
  auto const held = detail::require_owned(detail::lock_releasing_gil(*host_));
  if(not host_->session_open(held, core::member_id{}))
  {
    throw std::runtime_error("wait() needs a free run opened by start()");
  }
  auto waiting = host_->wait(held, core::member_id{}, std::move(spec));
  auto future  = waiting.future();
  return host_future_handle{std::make_shared<detail::future_request<stop>>(
      std::move(future), [](stop const& stopped) { return nb::cast(record(stopped)); },
      [waiting = std::move(waiting)] mutable { waiting.drop(); })};
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
