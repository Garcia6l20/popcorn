#include "machine.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <chrono>
#include <concepts>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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
    return nb::cast<bool>(detail::call_python([&] { return (*held)(); }));
  };
}

constexpr auto signal_check = std::chrono::milliseconds{50};

void wait_interruptibly(
    core::host&                 on,
    core::halt_ticket const&    ticket,
    detail::host_abandon const& abandon)
{
  detail::count_interruptible_wait();
  for(;;)
  {
    auto settled = false;
    {
      nb::gil_scoped_release const released;
      settled = on.wait_settled(core::member_id{}, ticket, signal_check);
    }
    if(settled)
    {
      return;
    }
    if(PyErr_CheckSignals() != 0)
    {
      nb::python_error const raised;
      abandon();
      throw raised;
    }
  }
}

auto cancel_goal(std::weak_ptr<core::host> host, detail::host_abandon abandon)
    -> std::move_only_function<bool()>
{
  return [host = std::move(host), abandon = std::move(abandon)] {
    auto const on = host.lock();
    if(on == nullptr)
    {
      return false;
    }
    std::optional<std::uint64_t> parks;
    {
      nb::gil_scoped_release const released;
      parks = on->request_cancel(core::member_id{});
    }
    if(parks)
    {
      wait_interruptibly(*on, {.finish = true, .with_host = true, .parks = *parks}, abandon);
    }
    return parks.has_value() or (core::detail::on_host_thread and not on->abandoned());
  };
}

void report_cancelled_finish(core::host_future<core::goal_result> const& watched)
{
  if(not watched.done())
  {
    return;
  }
  try
  {
    std::ignore = watched.get();
  }
  catch(core::chained_error const& chained)
  {
    try
    {
      std::rethrow_exception(chained.during());
    }
    catch(core::operation_cancelled const&)
    {
      try
      {
        auto raised = detail::python_exception(chained.raised());
        auto cause  = nb::module_::import_("asyncio").attr("CancelledError")();
        PyException_SetContext(raised.ptr(), cause.release().ptr());
        PyErr_SetRaisedException(raised.release().ptr());
      }
      catch(nb::python_error& failed)
      {
        failed.restore();
      }
      PyErr_WriteUnraisable(nullptr);
    }
    catch(...)
    {}
  }
  catch(...)
  {}
}

auto cancel_reporting_finish(
    std::move_only_function<bool()>      cancel,
    core::host_future<core::goal_result> watched) -> std::move_only_function<bool()>
{
  return [cancel = std::move(cancel), watched = std::move(watched)] mutable {
    auto const cancelled = cancel();
    report_cancelled_finish(watched);
    return cancelled;
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
  detail::raise_kept_log_error(*slot_->logs);
  if(host_->abandoned())
  {
    throw core::machine_closed("the host was abandoned");
  }
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
  return host_future_handle{
      std::make_shared<detail::future_request<core::goal_result>>(
          std::move(future),
          [](core::goal_result const& result) { return nb::cast(record(result.stopped)); },
          cancel_goal(host_, abandoner())),
      slot_};
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
  auto cancel = cancel_reporting_finish(cancel_goal(host_, abandoner()), future);
  return host_future_handle{
      std::make_shared<detail::future_request<core::goal_result>>(
          std::move(future),
          [ended](core::goal_result const& result) {
            auto outcome    = *ended;
            outcome.stopped = record(result.stopped);
            return nb::cast(std::move(outcome));
          },
          std::move(cancel)),
      slot_};
}

auto machine_handle::failing_finish_goal(std::uint64_t budget) -> host_future_handle
{
  core::run_goal goal{.budget = icount{budget}, .finish = [](core::machine&, stop const*) {
                        throw popcorn::error("the goal's finish failed");
                      }};
  auto           future = [&] {
    auto const held = lock_for_request();
    return host_->run(held, core::member_id{}, std::move(goal));
  }();
  auto cancel = cancel_reporting_finish(cancel_goal(host_, abandoner()), future);
  return host_future_handle{
      std::make_shared<detail::future_request<core::goal_result>>(
          std::move(future), [](core::goal_result const&) { return nb::none(); },
          std::move(cancel)),
      slot_};
}

void machine_handle::start()
{
  auto const held = lock_for_request();
  host_->start(held, core::member_id{});
  delivered_->reset();
}

auto machine_handle::halt() -> nb::object
{
  if(host_->abandoned())
  {
    return nb::none();
  }
  return halt_or_park(true);
}

auto machine_handle::park() -> nb::object
{
  return halt_or_park(false);
}

auto machine_handle::halt_or_park(bool refuse_goal) -> nb::object
{
  core::halt_ticket ticket;
  {
    nb::gil_scoped_release const released;
    ticket = host_->request_halt(core::member_id{}, refuse_goal);
  }
  wait_interruptibly(*host_, ticket, abandoner());
  std::optional<core::held_end> ended;
  {
    nb::gil_scoped_release const released;
    ended = host_->finish_halt(core::member_id{}, ticket);
  }
  if(not ended)
  {
    detail::flush_logs(*slot_->logs, detail::log_errors::raise);
    return nb::none();
  }
  flush_logs_unraisable();
  if(auto const* const stopped = std::get_if<stop>(&*ended))
  {
    return nb::cast(record(*stopped));
  }
  return detail::python_exception(std::get<std::exception_ptr>(*ended));
}

void machine_handle::flush_logs_unraisable() const noexcept
{
  try
  {
    detail::flush_logs(*slot_->logs, detail::log_errors::report);
  }
  catch(...)
  {}
}

auto machine_handle::unreported_end() -> nb::object
{
  std::optional<core::held_end> ended;
  {
    nb::gil_scoped_release const released;
    ended = host_->halt(core::member_id{});
  }
  flush_logs_unraisable();
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
  return waiting_on({.ready = predicate_of(std::move(ready))}, for_ps, false);
}

auto machine_handle::wait_predicate(
    native_predicate const&      ready,
    std::optional<std::uint64_t> for_ps) -> host_future_handle
{
  return waiting_on({.ready = ready.test()}, for_ps, false);
}

auto machine_handle::run_predicate(
    native_predicate const&      ready,
    std::optional<std::uint64_t> budget,
    std::optional<std::uint64_t> for_ps) -> host_future_handle
{
  core::run_goal goal{.stop_when = ready.test()};
  if(budget)
  {
    goal.budget = icount{*budget};
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
  return host_future_handle{
      std::make_shared<detail::future_request<core::goal_result>>(
          std::move(future),
          [](core::goal_result const& result) { return nb::cast(record(result.stopped)); },
          cancel_goal(host_, abandoner())),
      slot_};
}

auto machine_handle::predicate_pc_at(address at) const -> native_predicate
{
  return native_predicate{[engine = &engine(), at] { return engine->regs().pc() == at; }, slot_}
      .described("the core to reach an address");
}

auto machine_handle::wait_when(core::condition_set when, std::optional<std::uint64_t> for_ps)
    -> host_future_handle
{
  if(when.members.empty() or when.members.size() > 64)
  {
    throw std::invalid_argument("a wait takes 1 to 64 conditions");
  }
  return waiting_on({.when = std::move(when)}, for_ps, true);
}

auto machine_handle::waiting_on(
    core::waiter_spec            spec,
    std::optional<std::uint64_t> for_ps,
    bool                         masked) -> host_future_handle
{
  if(for_ps)
  {
    spec.after = vtime{*for_ps};
  }
  if(host_->abandoned())
  {
    throw core::machine_closed("the host was abandoned");
  }
  detail::raise_kept_log_error(*slot_->logs);
  refuse_inside(*host_);
  auto const held = lock_releasing();
  if(not host_->session_open(held, core::member_id{}))
  {
    throw std::runtime_error("wait() needs a free run opened by start()");
  }
  auto       waiting = host_->wait(held, core::member_id{}, std::move(spec));
  auto       future  = waiting.future();
  auto const made    = delivered_->generation();
  return host_future_handle{
      std::make_shared<detail::future_request<stop>>(
          std::move(future),
          [delivered = delivered_, made, masked, waiting](stop const& stopped) {
            delivered->returned(made, waiting.ended());
            if(masked)
            {
              return nb::object{nb::make_tuple(record(stopped), waiting.fired())};
            }
            return nb::cast(record(stopped));
          },
          [waiting] mutable {
            waiting.drop();
            return true;
          },
          [delivered = delivered_, made](std::exception_ptr const& error) {
            delivered->raised(made, error);
          }),
      slot_};
}

auto machine_handle::is_plain_memory(address addr, std::uint8_t size) const -> bool
{
  if(size < 1 or size > 8)
  {
    throw std::invalid_argument("an access of 1 to 8 bytes");
  }
  return engine().mem().is_plain_memory(addr, size);
}

auto machine_handle::busy() const -> bool
{
  if(host_->abandoned())
  {
    return false;
  }
  if(host_->owns_engine())
  {
    return core::detail::host_locks_held == 0;
  }
  auto const held = lock_releasing();
  return host_->busy(held, core::member_id{});
}

auto machine_handle::session_open() const -> bool
{
  if(host_->abandoned())
  {
    return false;
  }
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
  detail::raise_kept_log_error(*slot_->logs);
  if(core::detail::on_host_thread or core::detail::host_locks_held > 0)
  {
    throw std::runtime_error("a handler accesses the machine directly");
  }
  auto callable = detail::held(std::move(body));
  auto command  = [callable](core::machine&) {
    detail::gil_entry const gil;
    std::ignore = detail::call_python([&] { return (*callable)(); });
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
    auto const on = host.lock();
    if(on == nullptr)
    {
      return false;
    }
    auto const held = detail::lock_releasing_gil(*on);
    return held.owns_lock() and on->withdraw(held, core::member_id{}, future);
  };
  return host_future_handle{
      std::make_shared<detail::future_request<vtime>>(
          std::move(future), [](vtime const& ran) { return nb::cast(ran.ps); },
          std::move(withdraw)),
      slot_};
}

auto machine_handle::submit_ops(
    std::vector<access_op>       ops,
    std::vector<std::string>     notes,
    std::optional<std::uint64_t> at_ps) -> host_future_handle
{
  detail::raise_kept_log_error(*slot_->logs);
  if(core::detail::on_host_thread or core::detail::host_locks_held > 0)
  {
    throw std::runtime_error("a handler accesses the machine directly");
  }
  if(notes.size() != ops.size())
  {
    throw std::invalid_argument("submit_ops takes one note per op");
  }
  struct outcome
  {
    std::vector<op_value> values;
    std::size_t           failed = 0;
    std::exception_ptr    error;
  };
  auto ran     = std::make_shared<outcome>();
  auto command = [ops = std::move(ops), ran](core::machine& target) {
    for(auto const& [index, op] : std::views::enumerate(ops))
    {
      op_value value;
      try
      {
        value = op.run(target);
      }
      catch(...)
      {
        ran->failed = static_cast<std::size_t>(index);
        ran->error  = std::current_exception();
        return;
      }
      ran->values.push_back(std::move(value));
    }
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
    auto const on = host.lock();
    if(on == nullptr)
    {
      return false;
    }
    auto const held = detail::lock_releasing_gil(*on);
    return held.owns_lock() and on->withdraw(held, core::member_id{}, future);
  };
  auto kept_notes = notes;
  auto request    = std::make_shared<detail::future_request<vtime>>(
      std::move(future),
      [ran, notes = std::move(notes)](vtime const& time) -> nb::object {
        if(ran->error)
        {
          auto const raised = detail::python_exception(ran->error);
          if(auto const& note = notes[ran->failed]; not note.empty())
          {
            if(auto const noted =
                   nb::steal(PyObject_CallMethod(raised.ptr(), "add_note", "s", note.c_str()));
               not noted.is_valid())
            {
              PyErr_Clear();
            }
          }
          PyErr_SetRaisedException(raised.inc_ref().ptr());
          throw nb::python_error();
        }
        nb::list values;
        for(auto const& value : ran->values)
        {
          values.append(
              std::visit(
                  [](auto const& held) -> nb::object {
                    using kind = std::remove_cvref_t<decltype(held)>;
                    if constexpr(std::same_as<kind, std::monostate>)
                    {
                      return nb::none();
                    }
                    else if constexpr(std::same_as<kind, std::vector<std::byte>>)
                    {
                      return nb::bytes(held.data(), held.size());
                    }
                    else if constexpr(std::same_as<kind, std::function<nb::object()>>)
                    {
                      return held();
                    }
                    else
                    {
                      return nb::cast(held);
                    }
                  },
                  value));
        }
        return nb::make_tuple(time.ps, values);
      },
      std::move(withdraw));
  request->value_error([ran, noted = std::move(kept_notes)] {
    if(not ran->error)
    {
      return std::pair<std::exception_ptr, std::string>{};
    }
    return std::pair{ran->error, noted[ran->failed]};
  });
  return host_future_handle{std::move(request), slot_};
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
