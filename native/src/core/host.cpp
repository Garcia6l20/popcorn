#include <popcorn/core/host.hpp>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <deque>
#include <format>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <tuple>

namespace popcorn::core
{
namespace detail
{
struct waiter
{
  waiter_spec              spec;
  host_promise<stop>       promise;
  std::atomic<bool>        dropped{false};
  bool                     checked = false;
  bool                     fired   = false;
  std::exception_ptr       error{};
  std::weak_ptr<host_core> owner{};

  [[nodiscard]] auto live() const noexcept -> bool
  {
    return not dropped.load(std::memory_order_relaxed) and not promise.done();
  }

  void release() noexcept
  {
    spec.ready = nullptr;
    error      = nullptr;
  }

  [[nodiscard]] auto settling() -> host_promise<stop> const&
  {
    release();
    return promise;
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

struct stamped
{
  vtime                                   at;
  std::uint64_t                           seq;
  std::move_only_function<void(machine&)> body;
  host_promise<vtime>                     promise;
};

struct later_first
{
  auto operator()(stamped const& a, stamped const& b) const noexcept -> bool
  {
    return std::tie(a.at.ps, a.seq) > std::tie(b.at.ps, b.seq);
  }
};

struct timeline_mark
{
  std::uint64_t generation{};
  std::uint64_t resets{};
};

auto mark_of(machine const& target) -> timeline_mark
{
  return {.generation = target.generation(), .resets = target.resets()};
}

using deferred    = std::vector<std::move_only_function<void()>>;
using waiter_list = std::vector<std::shared_ptr<waiter>>;

struct member_slot
{
  explicit member_slot(std::unique_ptr<machine> made)
      : engine{std::move(made)}, mark{mark_of(*engine)}
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
  std::uint64_t             settled          = 0;
  std::vector<stamped>      stamps{};
  std::vector<stamped>      incoming{};
  std::uint64_t             next_seq = 0;
  timeline_mark             mark;

  void push_stamp(stamped made)
  {
    stamps.push_back(std::move(made));
    std::ranges::push_heap(stamps, later_first{});
  }

  [[nodiscard]] auto next_stamp() const -> std::optional<vtime>
  {
    return stamps.empty() ? std::nullopt : std::optional{stamps.front().at};
  }

  auto take_stamps() -> std::vector<stamped>
  {
    auto all = std::exchange(stamps, {});
    std::ranges::move(incoming, std::back_inserter(all));
    incoming.clear();
    return all;
  }

  auto timeline_moved() -> char const*
  {
    auto const  now = mark_of(*engine);
    char const* why = nullptr;
    if(now.generation != mark.generation)
    {
      why = "superseded by a restore";
    }
    else if(now.resets != mark.resets)
    {
      why = "superseded by a reset";
    }
    mark = now;
    return why;
  }
};

struct turn_plan
{
  std::deque<command> mail{};
  bool                cancel = false;
  bool                halt   = false;
  bool                slice  = false;
  waiter_list         waiters{};
  waiter_list         dropped{};
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

auto in_ms(vtime at) -> std::string
{
  return std::format("{:.6f} ms", static_cast<double>(at.ps) / 1e9);
}

auto past_stamp(vtime at, vtime now) -> std::invalid_argument
{
  return std::invalid_argument(
      std::format("stamp {} is before machine time {}", in_ms(at), in_ms(now)));
}

void fail_all(std::vector<stamped> const& dropped, std::exception_ptr const& error)
{
  for(auto const& pending : dropped)
  {
    pending.promise.fail(error);
  }
}

void supersede_parked(member_slot& slot)
{
  if(auto const* const why = slot.timeline_moved(); why != nullptr)
  {
    fail_all(slot.take_stamps(), cancelled(why));
  }
}

void add_stamp(slice_request& request, std::optional<vtime> stamp)
{
  if(stamp and (not request.until_time or *stamp < *request.until_time))
  {
    request.until_time    = stamp;
    request.internal_stop = true;
  }
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

class event_fd
{
public:
  event_fd() : fd_{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)}
  {
    if(fd_ < 0)
    {
      throw std::system_error(errno, std::generic_category(), "eventfd");
    }
  }

  event_fd(event_fd const&)                    = delete;
  auto operator=(event_fd const&) -> event_fd& = delete;

  ~event_fd()
  {
    ::close(fd_);
  }

  [[nodiscard]] auto get() const noexcept -> int
  {
    return fd_;
  }

  void signal() const noexcept
  {
    std::uint64_t const one = 1;
    std::ignore             = ::write(fd_, &one, sizeof one);
  }

  void drain() const noexcept
  {
    std::uint64_t count{};
    std::ignore = ::read(fd_, &count, sizeof count);
  }

private:
  int fd_;
};

inline constexpr int poll_retry_ms = 10;

struct fd_watch
{
  watch_id                              id;
  member_id                             who;
  int                                   fd;
  std::move_only_function<bool() const> wants_write;
  bool                                  broken = false;
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
  event_fd                                  wake;
  std::condition_variable                   parked_cv;
  std::vector<std::unique_ptr<member_slot>> members;
  slice_bounds                              bounds;
  host_hooks                                hooks;
  bool                                      pending   = false;
  bool                                      closed    = false;
  bool                                      sleeping  = false;
  bool                                      signalled = false;
  std::atomic<fate>                         ending{fate::undecided};
  std::atomic<bool>                         drops{false};
  std::mutex                                watch_mutex;
  std::vector<fd_watch>                     watches;
  watch_id                                  next_watch = 0;

  void wake_locked(member_slot& slot)
  {
    slot.woken = true;
    pending    = true;
    if(sleeping and not std::exchange(signalled, true))
    {
      wake.signal();
    }
  }

  void waiter_dropped() noexcept
  {
    drops.store(true, std::memory_order_release);
    wake.signal();
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
      for(auto& plan : *plans)
      {
        for(auto const& gone : plan.dropped)
        {
          gone->release();
        }
        plan.dropped = {};
      }
      deferred done;
      for(auto const& [slot, plan] : std::views::zip(members, *plans))
      {
        if(not plan.mail.empty())
        {
          serve_mail(*slot, plan.mail);
        }
        if(plan.cancel or plan.halt)
        {
          if(slot->goal)
          {
            std::ignore = finish_goal(*slot, nullptr);
          }
          park(*slot, done, [halt = plan.halt](member_slot& parked, deferred& later) {
            end_goal_cancelled(parked, later);
            if(halt)
            {
              parked.session  = false;
              parked.free_run = false;
              later.push_back([waiters = std::exchange(parked.waiters, {})] {
                for(auto const& waiting : waiters)
                {
                  waiting->settling().fail(cancelled("the machine was halted"));
                }
              });
            }
          });
          plan.slice   = false;
          plan.waiters = {};
        }
      }
      run_all(done);
      publish_settled();
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
          free_slice(*slot, std::exchange(plan.waiters, {}), done);
        }
        run_all(done);
        publish_settled();
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
      auto                   any     = false;
      auto const             pruning = drops.exchange(false, std::memory_order_acquire);
      for(auto const& [slot, plan] : std::views::zip(members, plans))
      {
        if(not slot->with_host)
        {
          continue;
        }
        if(pruning)
        {
          auto const gone = std::ranges::partition(
              slot->waiters, [](auto const& waiting) { return waiting->live(); });
          plan.dropped.assign(
              std::make_move_iterator(gone.begin()), std::make_move_iterator(gone.end()));
          slot->waiters.erase(gone.begin(), gone.end());
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
        any = any
           or not plan.mail.empty()
           or not plan.dropped.empty()
           or plan.cancel
           or plan.halt
           or plan.slice;
      }
      if(any)
      {
        return plans;
      }
      sleep(lock);
    }
  }

  /**
   * @brief Polls @p polled, the eventfd first, until one is ready.
   * @return False when the watched descriptors could not be polled: the eventfd alone was
   *         polled for poll_retry instead, and every watched member is to be woken so its pump
   *         reads what arrived. Wakes are then late by up to poll_retry, never lost.
   */
  static auto poll_all(std::vector<pollfd>& polled) -> bool
  {
    while(::poll(polled.data(), polled.size(), -1) < 0)
    {
      if(errno == EINTR)
      {
        continue;
      }
      for(auto& entry : polled)
      {
        entry.revents = 0;
      }
      while(::poll(polled.data(), 1, poll_retry_ms) < 0 and errno == EINTR)
      {
      }
      return false;
    }
    return true;
  }

  void sleep(std::unique_lock<std::mutex>& lock)
  {
    std::vector<pollfd> polled{
        {.fd = wake.get(), .events = POLLIN, .revents = 0}
    };
    std::vector<watch_id> owners;
    std::unique_lock      watching{watch_mutex};
    for(auto& watched : watches)
    {
      if(watched.broken or members[watched.who]->state != host_state::idle_wait)
      {
        continue;
      }
      auto const events = watched.wants_write() ? POLLIN | POLLOUT : POLLIN;
      polled.push_back({.fd = watched.fd, .events = static_cast<short>(events), .revents = 0});
      owners.push_back(watched.id);
    }
    watching.unlock();
    sleeping = true;
    lock.unlock();
    auto const failed = not poll_all(polled);
    if(polled.front().revents != 0)
    {
      wake.drain();
    }
    lock.lock();
    sleeping  = false;
    signalled = false;
    watching.lock();
    for(auto const& [ready, id] : std::views::zip(polled | std::views::drop(1), owners))
    {
      auto const watched = std::ranges::find(watches, id, &fd_watch::id);
      if((ready.revents == 0 and not failed) or watched == watches.end())
      {
        continue;
      }
      members[watched->who]->woken = true;
      if((ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      {
        watched->broken = true;
      }
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
    while(not slot.mail.empty() or not slot.incoming.empty())
    {
      auto batch = std::exchange(slot.mail, {});
      auto fresh = std::exchange(slot.incoming, {});
      lock.unlock();
      serve_mail(slot, batch);
      accept_incoming(slot, std::move(fresh));
      apply_due(slot);
      lock.lock();
    }
    transition(slot, done);
    slot.with_host        = false;
    slot.cancel_requested = false;
    slot.halt_requested   = false;
    slot.woken            = false;
    slot.state            = host_state::parked;
    ++slot.parks;
  }

  auto finish_goal(member_slot& slot, stop const* stopped) -> std::exception_ptr
  {
    if(not slot.goal or not slot.goal->goal.finish)
    {
      return {};
    }
    try
    {
      owner_scope const owning{this};
      slot.goal->goal.finish(*slot.engine, stopped);
      return {};
    }
    catch(...)
    {
      return std::current_exception();
    }
  }

  auto member_of(member_slot const& slot) const -> member_id
  {
    auto const found = std::ranges::find_if(
        members, [&slot](auto const& member) { return member.get() == &slot; });
    return static_cast<member_id>(found - members.begin());
  }

  void at_boundary(member_slot const& slot)
  {
    if(not hooks.boundary)
    {
      return;
    }
    hooks.boundary(member_of(slot));
  }

  void publish_settled()
  {
    {
      std::scoped_lock const lock{mutex};
      for(auto const& slot : members)
      {
        slot->settled = slot->parks;
      }
    }
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

  void supersede_if_moved(member_slot& slot)
  {
    auto const* const why = slot.timeline_moved();
    if(why == nullptr)
    {
      return;
    }
    std::vector<stamped> dropped;
    {
      std::scoped_lock const lock{mutex};
      dropped = slot.take_stamps();
    }
    fail_all(dropped, cancelled(why));
  }

  void accept_stamps(member_slot& slot)
  {
    supersede_if_moved(slot);
    std::vector<stamped> fresh;
    {
      std::scoped_lock const lock{mutex};
      fresh = std::exchange(slot.incoming, {});
    }
    accept_incoming(slot, std::move(fresh));
  }

  void accept_incoming(member_slot& slot, std::vector<stamped> fresh)
  {
    if(fresh.empty())
    {
      return;
    }
    auto const           now = slot.engine->virtual_clock().time();
    std::vector<stamped> late;
    {
      std::scoped_lock const lock{mutex};
      for(auto& posted : fresh)
      {
        if(posted.at < now)
        {
          late.push_back(std::move(posted));
        }
        else
        {
          slot.push_stamp(std::move(posted));
        }
      }
    }
    for(auto const& posted : late)
    {
      posted.promise.fail(std::make_exception_ptr(past_stamp(posted.at, now)));
    }
  }

  void apply_due(member_slot& slot)
  {
    auto const           now = slot.engine->virtual_clock().time();
    std::vector<stamped> due;
    {
      std::scoped_lock const lock{mutex};
      while(not slot.stamps.empty() and slot.stamps.front().at <= now)
      {
        std::ranges::pop_heap(slot.stamps, later_first{});
        due.push_back(std::move(slot.stamps.back()));
        slot.stamps.pop_back();
      }
    }
    if(due.empty())
    {
      return;
    }
    std::vector<std::pair<host_promise<vtime>, std::expected<vtime, std::exception_ptr>>> results;
    {
      owner_scope const owning{this};
      for(auto& posted : due)
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
    due.clear();
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

  auto next_stamp(member_slot const& slot) -> std::optional<vtime>
  {
    std::scoped_lock const lock{mutex};
    return slot.next_stamp();
  }

  void after_slice(member_slot& slot, slice_request const& request, slice_result& result)
  {
    supersede_if_moved(slot);
    apply_due(slot);
    if(request.internal_stop and result.stopped and result.stopped->reason == stop_reason::time)
    {
      result.stopped.reset();
    }
  }

  void fail_goal(member_slot& slot, deferred& done, std::exception_ptr error)
  {
    std::ignore = finish_goal(slot, nullptr);
    park(slot, done, [&error](member_slot& parked, deferred& later) {
      later.push_back([goal = std::move(parked.goal), error = std::move(error)] mutable {
        goal->promise.fail(std::move(error));
      });
    });
  }

  void goal_slice(member_slot& slot, deferred& done)
  {
    accept_stamps(slot);
    apply_due(slot);
    auto&         goal  = *slot.goal;
    auto const&   once  = goal.goal.once;
    auto const    limit = once ? once->bounds : bounds;
    slice_request request{
        .max_instructions   = limit.max_instructions,
        .wall_budget        = limit.wall_budget,
        .until              = goal.goal.until,
        .budget_left        = goal.remaining,
        .exact_instructions = goal.left,
        .until_time         = goal.goal.until_time,
        .stop_when   = goal.goal.stop_when ? std::function<bool()>{std::ref(goal.goal.stop_when)}
                                           : std::function<bool()>{},
        .leave_until = once ? once->leave_until : goal.executed.value == 0,
    };
    add_stamp(request, next_stamp(slot));
    slice_result result;
    try
    {
      owner_scope const owning{this};
      at_boundary(slot);
      result = slot.engine->run_slice(request);
    }
    catch(...)
    {
      fail_goal(slot, done, std::current_exception());
      return;
    }
    after_slice(slot, request, result);
    try
    {
      owner_scope const owning{this};
      at_boundary(slot);
    }
    catch(...)
    {
      fail_goal(slot, done, std::current_exception());
      return;
    }
    goal.executed.value += result.executed.value;
    goal.remaining = remaining_after(goal.remaining, result.executed);
    if(goal.left)
    {
      goal.left =
          icount{std::max<std::uint64_t>(remaining_after(goal.left, result.executed)->value, 1)};
    }
    if(result.stopped or once)
    {
      goal_result const outcome{
          .stopped  = result.stopped ? std::move(*result.stopped) : stop{},
          .executed = goal.executed,
          .yielded  = not result.stopped,
      };
      auto const failed = finish_goal(slot, outcome.yielded ? nullptr : &outcome.stopped);
      park(slot, done, [&outcome, &failed](member_slot& parked, deferred& later) {
        later.push_back([goal = std::move(parked.goal), outcome, failed] {
          if(failed)
          {
            goal->promise.fail(failed);
          }
          else
          {
            goal->promise.set(outcome);
          }
        });
      });
    }
  }

  void free_slice(member_slot& slot, waiter_list waiters, deferred& done)
  {
    accept_stamps(slot);
    apply_due(slot);
    std::erase_if(waiters, [](auto const& waiting) { return not waiting->live(); });
    std::vector<std::pair<std::shared_ptr<waiter>, stop>> resolved;
    {
      owner_scope const owning{this};
      auto const        now = slot.engine->virtual_clock().time();
      for(auto const& waiting : waiters)
      {
        if(not waiting->checked and waiting->spec.after and not waiting->spec.deadline)
        {
          waiting->spec.deadline = vtime{now.ps + waiting->spec.after->ps};
        }
      }
    }
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
      if(waiting->spec.ready and waiting->checked)
      {
        predicates->push_back(waiting);
      }
    }
    add_stamp(request, next_stamp(slot));
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
      at_boundary(slot);
      result = slot.engine->run_slice(request);
    }
    catch(...)
    {
      settle(resolved);
      end_free_run(slot, held_end{std::current_exception()}, done);
      return;
    }
    request.stop_when = {};
    after_slice(slot, request, result);
    try
    {
      owner_scope const owning{this};
      at_boundary(slot);
    }
    catch(...)
    {
      end_free_run(slot, held_end{std::current_exception()}, done);
      return;
    }
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
    waiter_list current;
    {
      std::scoped_lock const lock{mutex};
      current = slot.waiters;
    }
    std::erase_if(current, [](auto const& waiting) { return not waiting->live(); });
    {
      owner_scope const owning{this};
      auto const        now = slot.engine->virtual_clock().time();
      for(auto const& waiting : current)
      {
        if(not waiting->checked and waiting->spec.after and not waiting->spec.deadline)
        {
          waiting->spec.deadline = vtime{now.ps + waiting->spec.after->ps};
        }
        if(not std::exchange(waiting->checked, true) and waiting->spec.ready)
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
    for(auto const& gone : pruned)
    {
      gone->release();
    }
  }

  static void settle(std::vector<std::pair<std::shared_ptr<waiter>, stop>>& resolved)
  {
    for(auto const& [waiting, at] : resolved)
    {
      if(auto error = std::exchange(waiting->error, {}))
      {
        waiting->settling().fail(std::move(error));
      }
      else
      {
        waiting->settling().set(at);
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
      waiting->settling().fail(cancelled("the machine was halted"));
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
    if(auto* error = std::get_if<std::exception_ptr>(&end); error and hooks.held)
    {
      hooks.held(member_of(slot), *error);
    }
    park(slot, done, [&end](member_slot& parked, deferred& later) {
      parked.free_run = false;
      parked.held     = end;
      later.push_back([waiters = std::exchange(parked.waiters, {}), end] {
        for(auto const& waiting : waiters)
        {
          complete_with(waiting->settling(), end);
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
                        held    = std::exchange(slot->held, std::nullopt),
                        stamps  = slot->take_stamps()] {
          for(auto const& posted : mail)
          {
            posted.promise.fail(closed_error());
          }
          fail_all(stamps, closed_error());
          if(goal)
          {
            goal->promise.fail(closed_error());
          }
          for(auto const& waiting : waiters)
          {
            waiting->settling().fail(closed_error());
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
    run_all(done);
    publish_settled();
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
  if(auto const on = waiter_->owner.lock())
  {
    on->waiter_dropped();
  }
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

auto host::post(
    held_lock const&                        held,
    member_id                               who,
    std::move_only_function<void(machine&)> body,
    std::optional<vtime>                    at) -> host_future<vtime>
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
  if(at)
  {
    post_stamped(held, slot, std::move(body), *at, promise);
  }
  else if(owns_engine())
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

void host::post_stamped(
    held_lock const&                        held,
    detail::member_slot&                    slot,
    std::move_only_function<void(machine&)> body,
    vtime                                   at,
    detail::host_promise<vtime> const&      promise)
{
  auto const inline_owner = owns_engine();
  if(not inline_owner and slot.with_host)
  {
    slot.incoming.push_back(
        {.at = at, .seq = slot.next_seq++, .body = std::move(body), .promise = promise});
    core_->notify_locked(slot);
    return;
  }
  if(not inline_owner)
  {
    detail::supersede_parked(slot);
  }
  auto const now = slot.engine->virtual_clock().time();
  if(at < now)
  {
    throw detail::past_stamp(at, now);
  }
  if(at == now)
  {
    std::optional<owner_scope> owning;
    if(not inline_owner)
    {
      owning.emplace(*this, held);
    }
    try
    {
      body(*slot.engine);
      promise.set(now);
    }
    catch(...)
    {
      promise.fail(std::current_exception());
    }
    return;
  }
  detail::stamped made{.at = at, .seq = 0, .body = std::move(body), .promise = promise};
  if(held.holds(*this))
  {
    made.seq = slot.next_seq++;
    slot.push_stamp(std::move(made));
  }
  else if(detail::on_host_thread and detail::host_locks_held == 0)
  {
    std::scoped_lock const lock{core_->mutex};
    made.seq = slot.next_seq++;
    slot.push_stamp(std::move(made));
  }
  else
  {
    throw machine_running("a later stamp from a callback needs the host mutex");
  }
  if(inline_owner and detail::on_host_thread)
  {
    slot.engine->interrupt();
  }
}

auto host::withdraw(held_lock const&, member_id who, host_future<vtime> const& pending) -> bool
{
  auto&      slot    = *core_->members[who];
  auto const removed = [&pending](auto& queue) -> std::optional<detail::host_promise<vtime>> {
    auto const found = std::ranges::find_if(
        queue, [&pending](auto const& posted) { return posted.promise.feeds(pending); });
    if(found == queue.end())
    {
      return std::nullopt;
    }
    auto promise = found->promise;
    queue.erase(found);
    return promise;
  };
  auto promise = removed(slot.mail);
  if(not promise)
  {
    promise = removed(slot.incoming);
  }
  if(not promise)
  {
    promise = removed(slot.stamps);
    if(promise)
    {
      std::ranges::make_heap(slot.stamps, detail::later_first{});
    }
  }
  if(not promise)
  {
    return false;
  }
  promise->fail(detail::cancelled("the command was withdrawn"));
  return true;
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
  detail::supersede_parked(slot);
  slot.with_host = true;
  slot.state     = host_state::running;
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
  detail::supersede_parked(slot);
  slot.with_host = true;
  slot.state     = host_state::running;
  ensure_thread();
  core_->wake_locked(slot);
}

auto host::wait(held_lock const&, member_id who, waiter_spec spec) -> waiter_handle
{
  auto& slot     = *core_->members[who];
  auto  waiting  = std::make_shared<detail::waiter>();
  waiting->spec  = std::move(spec);
  waiting->owner = core_;
  auto future    = waiting->promise.future();
  if(slot.held)
  {
    detail::complete_with(waiting->settling(), *slot.held);
  }
  else
  {
    slot.waiters.push_back(waiting);
    core_->wake_locked(slot);
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
  core_->parked_cv.wait(lock, [&slot, parks] { return slot.settled > parks; });
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
    core_->parked_cv.wait(lock, [&slot, parks] { return slot.settled > parks; });
  }
  else if(not detail::on_host_thread)
  {
    core_->parked_cv.wait(lock, [&slot] { return slot.settled == slot.parks; });
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
  std::vector<detail::stamped> stranded;
  {
    std::scoped_lock const lock{core_->mutex};
    for(auto const& slot : core_->members)
    {
      slot->state = host_state::stopping;
      std::ranges::move(slot->take_stamps(), std::back_inserter(stranded));
    }
  }
  detail::fail_all(stranded, detail::closed_error());
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
  core_->wake.signal();
}

auto host::watch(member_id who, int fd, std::move_only_function<bool() const> wants_write)
    -> watch_id
{
  watch_id made{};
  {
    std::scoped_lock const lock{core_->watch_mutex};
    made = core_->next_watch++;
    core_->watches.push_back(
        {.id = made, .who = who, .fd = fd, .wants_write = std::move(wants_write)});
  }
  core_->wake.signal();
  return made;
}

void host::unwatch(watch_id id) noexcept
{
  std::move_only_function<bool() const> dropped;
  {
    std::scoped_lock const lock{core_->watch_mutex};
    auto const             found = std::ranges::find(core_->watches, id, &detail::fd_watch::id);
    if(found == core_->watches.end())
    {
      return;
    }
    dropped = std::move(found->wants_write);
    core_->watches.erase(found);
  }
  core_->wake.signal();
}

void host::decide(detail::fate ending)
{
  core_->ending.store(ending);
  core_->ending.notify_all();
}
}
