#include <popcorn/core/host.hpp>

#include <algorithm>
#include <deque>
#include <ranges>
#include <stop_token>

namespace popcorn::core
{
namespace detail
{
struct waiter
{
  waiter_spec        spec;
  host_promise<stop> promise;
  std::atomic<bool>  dropped{false};
  bool               checked = false;
  bool               fired   = false;
  std::exception_ptr error{};

  [[nodiscard]] auto live() const noexcept -> bool
  {
    return not dropped.load(std::memory_order_relaxed) and not promise.done();
  }
};

struct command
{
  std::move_only_function<void(machine&)> body;
  host_promise<vtime>                     promise;
};

struct goal_run
{
  run_goal                  goal;
  host_promise<goal_result> promise;
  std::optional<icount>     remaining;
  std::optional<icount>     left;
  icount                    executed{};
};

using deferred    = std::vector<std::move_only_function<void()>>;
using waiter_list = std::vector<std::shared_ptr<waiter>>;

struct member_slot
{
  explicit member_slot(std::unique_ptr<machine> made) : engine{std::move(made)}
  {}

  std::unique_ptr<machine>  engine;
  std::atomic<host_state>   state{host_state::parked};
  bool                      with_host = false;
  std::unique_ptr<goal_run> goal{};
  bool                      session  = false;
  bool                      free_run = false;
  std::optional<held_end>   held{};
  waiter_list               waiters{};
  std::deque<command>       mail{};
  bool                      cancel_requested = false;
  bool                      halt_requested   = false;
  bool                      woken            = false;
  std::uint64_t             parks            = 0;
};

struct turn_plan
{
  std::deque<command> mail{};
  bool                cancel = false;
  bool                halt   = false;
  bool                slice  = false;
  waiter_list         waiters{};
};

namespace
{
void run_all(deferred& work)
{
  for(auto& step : work)
  {
    step();
  }
  work.clear();
}

void complete_with(host_promise<stop> const& promise, held_end const& end)
{
  if(auto const* stopped = std::get_if<stop>(&end))
  {
    promise.set(*stopped);
  }
  else
  {
    promise.fail(std::get<std::exception_ptr>(end));
  }
}

auto stop_here(machine& target, stop_reason reason) -> stop
{
  auto const pc = target.regs().pc();
  return stop{
      .reason       = reason,
      .where        = target.locate(pc),
      .instructions = target.virtual_clock().now(),
      .time         = target.virtual_clock().time(),
  };
}

auto cancelled(char const* what) -> std::exception_ptr
{
  return std::make_exception_ptr(operation_cancelled("{}", what));
}

auto closed_error() -> std::exception_ptr
{
  return std::make_exception_ptr(machine_closed("the host closed"));
}

auto remaining_after(std::optional<icount> bound, icount ran) -> std::optional<icount>
{
  if(not bound)
  {
    return bound;
  }
  return icount{bound->value - std::min(bound->value, ran.value)};
}
}

enum class fate : std::uint8_t
{
  undecided,
  joined,
  orphaned,
};

struct host_core
{
  host_core(std::vector<std::unique_ptr<machine>> machines, slice_bounds slices, host_hooks calls)
      : bounds{slices}, hooks{std::move(calls)}
  {
    members.reserve(machines.size());
    for(auto& made : machines)
    {
      members.push_back(std::make_unique<member_slot>(std::move(made)));
    }
  }

  std::mutex                                mutex;
  std::condition_variable_any               wake;
  std::condition_variable                   parked_cv;
  std::vector<std::unique_ptr<member_slot>> members;
  slice_bounds                              bounds;
  host_hooks                                hooks;
  bool                                      pending = false;
  bool                                      closed  = false;
  std::atomic<fate>                         ending{fate::undecided};

  void wake_locked(member_slot& slot)
  {
    slot.woken = true;
    pending    = true;
    wake.notify_all();
  }

  void notify_locked(member_slot& slot)
  {
    slot.engine->interrupt();
    wake_locked(slot);
  }

  void serve(std::stop_token const& stop)
  {
    on_host_thread = true;
    if(hooks.thread_started)
    {
      hooks.thread_started();
    }
    while(auto plans = take_turn(stop))
    {
      deferred done;
      for(auto const& [slot, plan] : std::views::zip(members, *plans))
      {
        if(not plan.mail.empty())
        {
          serve_mail(*slot, plan.mail);
        }
        if(plan.cancel or plan.halt)
        {
          park(*slot, done, [halt = plan.halt](member_slot& parked, deferred& later) {
            end_goal_cancelled(parked, later);
            if(halt)
            {
              parked.session  = false;
              parked.free_run = false;
              later.push_back([waiters = std::exchange(parked.waiters, {})] {
                for(auto const& waiting : waiters)
                {
                  waiting->promise.fail(cancelled("the machine was halted"));
                }
              });
            }
          });
          plan.slice = false;
        }
      }
      run_all(done);
      for(auto const& [slot, plan] : std::views::zip(members, *plans))
      {
        if(not plan.slice)
        {
          continue;
        }
        if(slot->goal)
        {
          goal_slice(*slot, done);
        }
        else if(slot->free_run)
        {
          free_slice(*slot, plan.waiters, done);
        }
        run_all(done);
      }
    }
    shut_down();
    if(hooks.thread_stopping)
    {
      hooks.thread_stopping();
    }
  }

  auto take_turn(std::stop_token const& stop) -> std::optional<std::vector<turn_plan>>
  {
    std::unique_lock lock{mutex};
    for(;;)
    {
      if(stop.stop_requested())
      {
        return std::nullopt;
      }
      pending = false;
      std::vector<turn_plan> plans(members.size());
      auto                   any = false;
      for(auto const& [slot, plan] : std::views::zip(members, plans))
      {
        if(not slot->with_host)
        {
          continue;
        }
        plan.mail   = std::exchange(slot->mail, {});
        plan.cancel = slot->cancel_requested and slot->goal;
        plan.halt   = slot->halt_requested;
        if(std::exchange(slot->woken, false) and slot->state == host_state::idle_wait)
        {
          slot->state = host_state::running;
        }
        plan.slice = slot->state == host_state::running;
        if(slot->free_run)
        {
          plan.waiters = slot->waiters;
        }
        any = any or not plan.mail.empty() or plan.cancel or plan.halt or plan.slice;
      }
      if(any)
      {
        return plans;
      }
      wake.wait(lock, stop, [this] { return pending; });
    }
  }

  void serve_mail(member_slot& slot, std::deque<command>& mail)
  {
    std::vector<std::pair<host_promise<vtime>, std::expected<vtime, std::exception_ptr>>> results;
    {
      owner_scope const owning{this};
      for(auto& posted : mail)
      {
        auto const at = slot.engine->virtual_clock().time();
        try
        {
          posted.body(*slot.engine);
          results.emplace_back(posted.promise, at);
        }
        catch(...)
        {
          results.emplace_back(posted.promise, std::unexpected{std::current_exception()});
        }
      }
    }
    mail.clear();
    for(auto& [promise, outcome] : results)
    {
      if(outcome)
      {
        promise.set(*outcome);
      }
      else
      {
        promise.fail(std::move(outcome).error());
      }
    }
  }

  template <typename Transition> void park(member_slot& slot, deferred& done, Transition transition)
  {
    std::unique_lock lock{mutex};
    while(not slot.mail.empty())
    {
      auto batch = std::exchange(slot.mail, {});
      lock.unlock();
      serve_mail(slot, batch);
      lock.lock();
    }
    transition(slot, done);
    slot.with_host        = false;
    slot.cancel_requested = false;
    slot.halt_requested   = false;
    slot.woken            = false;
    slot.state            = host_state::parked;
    ++slot.parks;
    lock.unlock();
    parked_cv.notify_all();
  }

  static void end_goal_cancelled(member_slot& slot, deferred& done)
  {
    if(slot.goal)
    {
      done.push_back([goal = std::move(slot.goal)] {
        goal->promise.fail(cancelled("the goal was cancelled"));
      });
    }
  }

  void goal_slice(member_slot& slot, deferred& done)
  {
    auto&               goal = *slot.goal;
    slice_request const request{
        .max_instructions   = bounds.max_instructions,
        .wall_budget        = bounds.wall_budget,
        .until              = goal.goal.until,
        .budget_left        = goal.remaining,
        .exact_instructions = goal.left,
        .until_time         = goal.goal.until_time,
        .stop_when   = goal.goal.stop_when ? std::function<bool()>{std::ref(goal.goal.stop_when)}
                                           : std::function<bool()>{},
        .leave_until = goal.executed.value == 0,
    };
    slice_result result;
    try
    {
      owner_scope const owning{this};
      result = slot.engine->run_slice(request);
    }
    catch(...)
    {
      park(slot, done, [error = std::current_exception()](member_slot& parked, deferred& later) {
        later.push_back([goal = std::move(parked.goal), error] mutable {
          goal->promise.fail(std::move(error));
        });
      });
      return;
    }
    goal.executed.value += result.executed.value;
    goal.remaining = remaining_after(goal.remaining, result.executed);
    if(goal.left)
    {
      goal.left =
          icount{std::max<std::uint64_t>(remaining_after(goal.left, result.executed)->value, 1)};
    }
    if(result.stopped)
    {
      goal_result const outcome{.stopped = std::move(*result.stopped), .executed = goal.executed};
      park(slot, done, [&outcome](member_slot& parked, deferred& later) {
        later.push_back([goal = std::move(parked.goal), outcome] { goal->promise.set(outcome); });
      });
    }
  }

  void free_slice(member_slot& slot, waiter_list& waiters, deferred& done)
  {
    std::erase_if(waiters, [](auto const& waiting) { return not waiting->live(); });
    std::vector<std::pair<std::shared_ptr<waiter>, stop>> resolved;
    {
      owner_scope const owning{this};
      for(auto const& waiting : waiters)
      {
        if(waiting->checked)
        {
          continue;
        }
        waiting->checked = true;
        if(waiting->spec.ready)
        {
          try
          {
            waiting->fired = waiting->spec.ready();
          }
          catch(...)
          {
            waiting->error = std::current_exception();
            waiting->fired = true;
          }
        }
        if(waiting->fired)
        {
          resolved.emplace_back(waiting, stop_here(*slot.engine, stop_reason::user));
        }
      }
    }
    std::erase_if(waiters, [](auto const& waiting) { return waiting->fired; });
    auto          predicates = std::make_shared<waiter_list>();
    slice_request request{
        .max_instructions = bounds.max_instructions,
        .wall_budget      = bounds.wall_budget,
    };
    for(auto const& waiting : waiters)
    {
      if(waiting->spec.deadline)
      {
        request.until_time =
            std::min(request.until_time.value_or(*waiting->spec.deadline), *waiting->spec.deadline);
      }
      if(waiting->spec.ready)
      {
        predicates->push_back(waiting);
      }
    }
    if(not predicates->empty())
    {
      request.stop_when = [predicates] {
        auto any = false;
        for(auto const& waiting : *predicates)
        {
          if(waiting->fired or waiting->dropped.load(std::memory_order_relaxed))
          {
            continue;
          }
          try
          {
            waiting->fired = waiting->spec.ready();
          }
          catch(...)
          {
            waiting->error = std::current_exception();
            waiting->fired = true;
          }
          any = any or waiting->fired;
        }
        return any;
      };
    }
    slice_result result;
    try
    {
      owner_scope const owning{this};
      result = slot.engine->run_slice(request);
    }
    catch(...)
    {
      settle(resolved);
      end_free_run(slot, held_end{std::current_exception()}, done);
      return;
    }
    request.stop_when = {};
    auto const reason = result.stopped ? std::optional{result.stopped->reason} : std::nullopt;
    auto const fired =
        std::ranges::any_of(*predicates, [](auto const& waiting) { return waiting->fired; });
    auto const goes_on = not reason
                      or reason == stop_reason::time
                      or reason == stop_reason::idle
                      or (reason == stop_reason::user and fired);
    if(not goes_on)
    {
      settle(resolved);
      end_free_run(slot, held_end{std::move(*result.stopped)}, done);
      return;
    }
    {
      owner_scope const owning{this};
      auto const        now = slot.engine->virtual_clock().time();
      for(auto const& waiting : waiters)
      {
        if(waiting->fired)
        {
          resolved.emplace_back(
              waiting, reason == stop_reason::user ? *result.stopped
                                                   : stop_here(*slot.engine, stop_reason::user));
        }
        else if(waiting->spec.deadline and now >= *waiting->spec.deadline)
        {
          resolved.emplace_back(
              waiting, reason == stop_reason::time ? *result.stopped
                                                   : stop_here(*slot.engine, stop_reason::time));
        }
      }
    }
    settle(resolved);
    waiter_list pruned;
    {
      std::scoped_lock const lock{mutex};
      auto const             gone =
          std::ranges::partition(slot.waiters, [](auto const& waiting) { return waiting->live(); });
      pruned.assign(std::make_move_iterator(gone.begin()), std::make_move_iterator(gone.end()));
      slot.waiters.erase(gone.begin(), gone.end());
      if(reason == stop_reason::idle and not std::exchange(slot.woken, false))
      {
        slot.state = host_state::idle_wait;
      }
    }
  }

  static void settle(std::vector<std::pair<std::shared_ptr<waiter>, stop>>& resolved)
  {
    for(auto const& [waiting, at] : resolved)
    {
      if(waiting->error)
      {
        waiting->promise.fail(waiting->error);
      }
      else
      {
        waiting->promise.set(at);
      }
    }
    resolved.clear();
  }

  auto close_session(std::unique_lock<std::mutex>& lock, member_slot& slot)
      -> std::optional<held_end>
  {
    slot.session          = false;
    slot.free_run         = false;
    slot.halt_requested   = false;
    slot.cancel_requested = false;
    auto end              = std::exchange(slot.held, std::nullopt);
    auto waiters          = std::exchange(slot.waiters, {});
    lock.unlock();
    for(auto const& waiting : waiters)
    {
      waiting->promise.fail(cancelled("the machine was halted"));
    }
    return end;
  }

  void before_block()
  {
    if(hooks.before_block)
    {
      hooks.before_block();
    }
  }

  void end_free_run(member_slot& slot, held_end end, deferred& done)
  {
    park(slot, done, [&end](member_slot& parked, deferred& later) {
      parked.free_run = false;
      parked.held     = end;
      later.push_back([waiters = std::exchange(parked.waiters, {}), end] {
        for(auto const& waiting : waiters)
        {
          complete_with(waiting->promise, end);
        }
      });
    });
  }

  void shut_down()
  {
    deferred done;
    {
      std::scoped_lock const lock{mutex};
      closed = true;
      for(auto const& slot : members)
      {
        done.push_back([mail = std::exchange(slot->mail, {}), goal = std::move(slot->goal),
                        waiters = std::exchange(slot->waiters, {}),
                        held    = std::exchange(slot->held, std::nullopt)] {
          for(auto const& posted : mail)
          {
            posted.promise.fail(closed_error());
          }
          if(goal)
          {
            goal->promise.fail(closed_error());
          }
          for(auto const& waiting : waiters)
          {
            waiting->promise.fail(closed_error());
          }
        });
        slot->with_host        = false;
        slot->session          = false;
        slot->free_run         = false;
        slot->cancel_requested = false;
        slot->halt_requested   = false;
        slot->state            = host_state::stopping;
        ++slot->parks;
      }
    }
    parked_cv.notify_all();
    run_all(done);
    ending.wait(fate::undecided);
    if(ending.load() == fate::orphaned)
    {
      for(auto const& slot : members)
      {
        slot->engine.reset();
      }
    }
  }
};
}

held_lock::held_lock(std::unique_lock<std::mutex> lock, detail::host_core const* of) noexcept
    : lock_{std::move(lock)}, of_{of}
{
  if(lock_.owns_lock())
  {
    ++detail::host_locks_held;
  }
}

held_lock::held_lock(held_lock&& other) noexcept
    : lock_{std::move(other.lock_)}, of_{std::exchange(other.of_, nullptr)}
{}

auto held_lock::operator=(held_lock&& other) noexcept -> held_lock&
{
  if(this != &other)
  {
    if(lock_.owns_lock())
    {
      lock_.unlock();
      --detail::host_locks_held;
    }
    lock_ = std::move(other.lock_);
    of_   = std::exchange(other.of_, nullptr);
  }
  return *this;
}

held_lock::~held_lock()
{
  if(lock_.owns_lock())
  {
    --detail::host_locks_held;
  }
}

auto held_lock::owns_lock() const noexcept -> bool
{
  return lock_.owns_lock();
}

auto held_lock::holds(host const& on) const noexcept -> bool
{
  return lock_.owns_lock() and of_ == on.core_.get();
}

void held_lock::unlock()
{
  lock_.unlock();
  --detail::host_locks_held;
}

owner_scope::owner_scope(host const& owner, held_lock const&)
    : previous_{std::exchange(detail::engine_owner, owner.core_.get())}
{}

owner_scope::owner_scope(detail::host_core const* owner) noexcept
    : previous_{std::exchange(detail::engine_owner, owner)}
{}

owner_scope::~owner_scope()
{
  detail::engine_owner = previous_;
}

waiter_handle::waiter_handle(std::shared_ptr<detail::waiter> waiter, host_future<stop> future)
    : waiter_{std::move(waiter)}, future_{std::move(future)}
{}

auto waiter_handle::future() const -> host_future<stop>
{
  return future_;
}

void waiter_handle::drop() noexcept
{
  waiter_->dropped.store(true, std::memory_order_relaxed);
  try
  {
    waiter_->promise.fail(detail::cancelled("the wait was dropped"));
  }
  catch(...)
  {}
}

host::host(std::vector<std::unique_ptr<machine>> members, slice_bounds bounds, host_hooks hooks)
    : core_{std::make_shared<detail::host_core>(std::move(members), bounds, std::move(hooks))}
{}

host::~host()
{
  if(not thread_.joinable())
  {
    return;
  }
  if(detail::on_host_thread or detail::host_locks_held > 0)
  {
    request_stop();
    decide(detail::fate::orphaned);
    thread_.detach();
    return;
  }
  close();
}

auto host::lock() -> held_lock
{
  return held_lock{std::unique_lock{core_->mutex}, core_.get()};
}

auto host::try_lock() -> held_lock
{
  std::unique_lock attempt{core_->mutex, std::try_to_lock};
  if(not attempt.owns_lock())
  {
    return {};
  }
  return held_lock{std::move(attempt), core_.get()};
}

auto host::members() const noexcept -> std::size_t
{
  return core_->members.size();
}

auto host::parked(held_lock const&, member_id who) const -> bool
{
  return not core_->members[who]->with_host;
}

auto host::busy(held_lock const&, member_id who) const -> bool
{
  auto const& slot = *core_->members[who];
  return slot.goal != nullptr or slot.session;
}

auto host::session_open(held_lock const&, member_id who) const -> bool
{
  return core_->members[who]->session;
}

auto host::member(held_lock const&, member_id who) -> machine&
{
  return *core_->members[who]->engine;
}

auto host::member_inline(member_id who) -> machine&
{
  return *core_->members[who]->engine;
}

auto host::post(held_lock const& held, member_id who, std::move_only_function<void(machine&)> body)
    -> host_future<vtime>
{
  auto&                       slot = *core_->members[who];
  detail::host_promise<vtime> promise;
  auto                        run_here = [&] {
    auto&      target = *slot.engine;
    auto const at     = target.virtual_clock().time();
    try
    {
      body(target);
      promise.set(at);
    }
    catch(...)
    {
      promise.fail(std::current_exception());
    }
  };
  if(owns_engine())
  {
    run_here();
  }
  else if(not slot.with_host)
  {
    owner_scope const owning{*this, held};
    run_here();
  }
  else
  {
    slot.mail.push_back({.body = std::move(body), .promise = promise});
    core_->notify_locked(slot);
  }
  return promise.future();
}

auto host::run(held_lock const& held, member_id who, run_goal goal) -> host_future<goal_result>
{
  auto& slot = *core_->members[who];
  if(core_->closed)
  {
    throw machine_closed("the host is closed");
  }
  if(busy(held, who))
  {
    throw machine_busy("the machine already runs a goal or a free run");
  }
  auto made             = std::make_unique<detail::goal_run>();
  made->remaining       = goal.budget;
  made->left            = goal.exact;
  made->goal            = std::move(goal);
  auto future           = made->promise.future();
  slot.goal             = std::move(made);
  slot.halt_requested   = false;
  slot.cancel_requested = false;
  slot.with_host        = true;
  slot.state            = host_state::running;
  ensure_thread();
  core_->wake_locked(slot);
  return future;
}

void host::start(held_lock const& held, member_id who)
{
  auto& slot = *core_->members[who];
  if(core_->closed)
  {
    throw machine_closed("the host is closed");
  }
  if(busy(held, who))
  {
    throw machine_busy("the machine already runs a goal or a free run");
  }
  slot.session          = true;
  slot.free_run         = true;
  slot.halt_requested   = false;
  slot.cancel_requested = false;
  slot.with_host        = true;
  slot.state            = host_state::running;
  ensure_thread();
  core_->wake_locked(slot);
}

auto host::wait(held_lock const&, member_id who, waiter_spec spec) -> waiter_handle
{
  auto& slot    = *core_->members[who];
  auto  waiting = std::make_shared<detail::waiter>();
  waiting->spec = std::move(spec);
  auto future   = waiting->promise.future();
  if(slot.held)
  {
    detail::complete_with(waiting->promise, *slot.held);
  }
  else
  {
    slot.waiters.push_back(waiting);
    core_->notify_locked(slot);
  }
  return waiter_handle{std::move(waiting), std::move(future)};
}

void host::cancel(member_id who)
{
  if(detail::host_locks_held > 0)
  {
    throw machine_running("cancel() from a thread that holds a host mutex");
  }
  auto& slot = *core_->members[who];
  core_->before_block();
  std::unique_lock lock{core_->mutex};
  if(not slot.goal)
  {
    return;
  }
  slot.cancel_requested = true;
  core_->notify_locked(slot);
  if(detail::on_host_thread)
  {
    return;
  }
  auto const parks = slot.parks;
  core_->parked_cv.wait(lock, [&slot, parks] { return slot.parks != parks; });
}

auto host::halt(member_id who) -> std::optional<held_end>
{
  if(detail::host_locks_held > 0)
  {
    throw machine_running("halt() from a thread that holds a host mutex");
  }
  auto& slot = *core_->members[who];
  core_->before_block();
  std::unique_lock lock{core_->mutex};
  if(not slot.goal and not slot.session)
  {
    return std::nullopt;
  }
  if(slot.with_host)
  {
    slot.halt_requested = true;
    core_->notify_locked(slot);
    if(detail::on_host_thread)
    {
      return std::nullopt;
    }
    auto const parks = slot.parks;
    core_->parked_cv.wait(lock, [&slot, parks] { return slot.parks != parks; });
  }
  return core_->close_session(lock, slot);
}

auto host::state(member_id who) const noexcept -> host_state
{
  return core_->members[who]->state.load(std::memory_order_relaxed);
}

auto host::owns_engine() const noexcept -> bool
{
  return detail::engine_owner == core_.get();
}

void host::close()
{
  if(detail::host_locks_held > 0)
  {
    throw machine_running("close() from a thread that holds a host mutex");
  }
  core_->before_block();
  {
    std::scoped_lock const lock{core_->mutex};
    core_->closed = true;
  }
  if(detail::on_host_thread)
  {
    request_stop();
    return;
  }
  std::scoped_lock const joining{join_mutex_};
  request_stop();
  decide(detail::fate::joined);
  if(thread_.joinable())
  {
    thread_.join();
  }
  std::scoped_lock const lock{core_->mutex};
  for(auto const& slot : core_->members)
  {
    slot->state = host_state::stopping;
  }
}

void host::ensure_thread()
{
  if(not thread_.joinable())
  {
    thread_ = std::jthread{[core = core_](std::stop_token const& stop) { core->serve(stop); }};
  }
}

void host::request_stop()
{
  for(auto const& slot : core_->members)
  {
    slot->engine->interrupt();
  }
  thread_.request_stop();
}

void host::decide(detail::fate ending)
{
  core_->ending.store(ending);
  core_->ending.notify_all();
}
}
