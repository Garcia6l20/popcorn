#include <popcorn/core/host.hpp>

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <deque>
#include <format>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace popcorn::core
{
namespace detail
{
constinit thread_local int              host_locks_held = 0;
constinit thread_local host_core const* engine_owner    = nullptr;
constinit thread_local bool             on_host_thread  = false;
}

machine_closed::~machine_closed()           = default;
operation_cancelled::~operation_cancelled() = default;
machine_forked::~machine_forked()           = default;
member_driver::~member_driver()             = default;

namespace detail
{
struct waiter
{
  waiter_spec              spec;
  host_promise<stop>       promise;
  std::atomic<bool>        dropped{false};
  bool                     checked = false;
  bool                     fired   = false;
  std::uint64_t            members_fired{};
  std::atomic<bool>        ended{false};
  std::optional<vtime>     since{};
  std::uint64_t            since_slice{};
  std::exception_ptr       error{};
  std::weak_ptr<host_core> owner{};

  void register_at(vtime now, std::uint64_t slice)
  {
    if(since)
    {
      return;
    }
    since       = now;
    since_slice = slice;
    if(spec.after and not spec.deadline)
    {
      spec.deadline = vtime{now.ps + spec.after->ps};
    }
  }

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

template <typename T> auto compared(T left, T right, condition::op compare) noexcept -> bool
{
  switch(compare)
  {
    case condition::op::eq:
      return left == right;
    case condition::op::ne:
      return left != right;
    case condition::op::lt:
      return left < right;
    case condition::op::le:
      return left <= right;
    case condition::op::gt:
      return left > right;
    case condition::op::ge:
      return left >= right;
  }
  return false;
}

auto holds_now(waiter& waiting, machine& target) -> bool
{
  auto const& set     = *waiting.spec.when;
  auto const& members = set.members;
  auto const  tested  = [&](auto const& member) { return condition_holds(member, target); };
  auto const  held = set.combine == condition_set::mode::all ? std::ranges::all_of(members, tested)
                                                             : std::ranges::any_of(members, tested);
  if(held)
  {
    waiting.members_fired = 0;
    for(auto const& [index, member] : std::views::enumerate(members))
    {
      if(condition_holds(member, target))
      {
        waiting.members_fired |= std::uint64_t{1} << index;
      }
    }
  }
  return held;
}

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
using driver_cell = std::atomic<std::shared_ptr<member_driver>>;

struct member_slot
{
  explicit member_slot(std::unique_ptr<machine> made)
      : engine{std::move(made)}, mark{mark_of(*engine)}
  {}

  std::shared_ptr<machine>  engine;
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
  std::uint64_t             sessions         = 0;
  std::uint64_t             free_slices      = 0;
  std::vector<stamped>      stamps{};
  std::vector<stamped>      incoming{};
  std::uint64_t             next_seq = 0;
  timeline_mark             mark;
  bool                      linked = false;
  std::atomic<bool> const*  link_released{};
  bool                      system_held = false;
  driver_cell               driver{};

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

auto event_due(member_slot const& slot) -> bool
{
  auto const next   = slot.engine->next_event();
  auto const marked = slot.engine->held_through();
  return next and marked and *next <= *marked;
}

struct turn_plan
{
  std::deque<command> mail{};
  bool                cancel = false;
  bool                halt   = false;
  bool                slice  = false;
  /** @brief Whether the slice first gives back the core a system held and has released. Decided
   * under the host mutex, as the closer unlinks the member under it, and acted on before any
   * instruction of the member's lone goal or free run. */
  bool        unhold = false;
  waiter_list waiters{};
  waiter_list dropped{};
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
      .resets       = target.resets(),
  };
}

auto cancelled(char const* what) -> std::exception_ptr
{
  return std::make_exception_ptr(operation_cancelled("{}", what));
}

auto chained(std::exception_ptr const& finished, std::exception_ptr error) -> std::exception_ptr
{
  if(not finished)
  {
    return error;
  }
  return std::make_exception_ptr(chained_error{finished, std::move(error)});
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

auto linked(member_slot const& slot) noexcept -> bool
{
  return slot.linked
     and (slot.link_released == nullptr or not slot.link_released->load(std::memory_order_acquire));
}

void give_back(machine& engine)
{
  if(engine.core_held() and not engine.in_call())
  {
    engine.hold_core(false);
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

  host_mutex                                mutex;
  event_fd                                  wake;
  std::condition_variable_any               parked_cv;
  std::vector<std::unique_ptr<member_slot>> members;
  slice_bounds                              bounds;
  host_hooks                                hooks;
  bool                                      closed    = false;
  bool                                      abandoned = false;
  bool                                      exiting   = false;
  std::atomic<bool>                         abandoned_flag{false};
  bool                                      sleeping    = false;
  bool                                      signalled   = false;
  bool                                      poll_warned = false;
  std::string                               name        = "a machine";
  std::uint64_t                             generation  = host::fork_generation();
  std::atomic<fate>                         ending{fate::undecided};
  std::atomic<bool>                         drops{false};
  std::mutex                                watch_mutex;
  std::vector<fd_watch>                     watches;
  watch_id                                  next_watch = 0;

  void wake_locked(member_slot& slot)
  {
    slot.woken = true;
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
          auto const finished = slot->goal ? finish_goal(*slot, nullptr) : std::exception_ptr{};
          park(*slot, done, [halt = plan.halt, &finished](member_slot& parked, deferred& later) {
            end_goal_cancelled(parked, later, finished);
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
          goal_slice(*slot, plan.unhold, done);
        }
        else if(slot->free_run)
        {
          free_slice(*slot, std::exchange(plan.waiters, {}), plan.unhold, done);
        }
        run_all(done);
        publish_settled();
      }
    }
    shut_down();
    std::ignore = std::exchange(hooks.boundary, {});
    std::ignore = std::exchange(hooks.held, {});
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
        if(slot->free_run and plan.slice)
        {
          plan.waiters = slot->waiters;
        }
        plan.unhold = plan.slice
                  and not plan.cancel
                  and not plan.halt
                  and (slot->goal ? not slot->goal->goal.driver : slot->free_run)
                  and not linked(*slot)
                  and std::exchange(slot->system_held, false);
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
   * @return 0, or the errno when the watched descriptors could not be polled: the eventfd alone
   *         was polled for poll_retry instead, and every watched member is to be woken so its
   *         pump reads what arrived. Wakes are then late by up to poll_retry, never lost.
   */
  static auto poll_all(std::vector<pollfd>& polled) -> int
  {
    while(::poll(polled.data(), polled.size(), -1) < 0)
    {
      auto const failure = errno;
      if(failure == EINTR)
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
      return failure;
    }
    return 0;
  }

  void warn_poll_failed(int failure, std::span<member_id const> watched)
  {
    if(std::exchange(poll_warned, true))
    {
      return;
    }
    auto const       named = ::strerrorname_np(failure);
    log_record const record{
        .kind    = log_kind::model_warning,
        .message = std::format(
            "poll failed with {}, pty input may be late by up to {} ms",
            named != nullptr ? named : "an unknown error", poll_retry_ms)};
    owner_scope const owning{this};
    for(auto const who : watched)
    {
      try
      {
        members[who]->engine->log(record);
      }
      catch(...)
      {}
    }
  }

  void sleep(std::unique_lock<host_mutex>& lock)
  {
    std::vector<pollfd> polled{
        {.fd = wake.get(), .events = POLLIN, .revents = 0}
    };
    std::vector<watch_id>  owners;
    std::vector<member_id> watchers;
    std::unique_lock       watching{watch_mutex};
    for(auto& watched : watches)
    {
      if(watched.broken or members[watched.who]->state != host_state::idle_wait)
      {
        continue;
      }
      auto const events = watched.wants_write() ? POLLIN | POLLOUT : POLLIN;
      polled.push_back({.fd = watched.fd, .events = static_cast<short>(events), .revents = 0});
      owners.push_back(watched.id);
      if(not std::ranges::contains(watchers, watched.who))
      {
        watchers.push_back(watched.who);
      }
    }
    watching.unlock();
    sleeping = true;
    lock.unlock();
    auto const failure = poll_all(polled);
    auto const failed  = failure != 0;
    if(failed)
    {
      warn_poll_failed(failure, watchers);
    }
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
    slot.driver.store(nullptr);
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

  static void
      end_goal_cancelled(member_slot& slot, deferred& done, std::exception_ptr const& finished = {})
  {
    if(slot.goal)
    {
      done.push_back([goal = std::move(slot.goal), finished] {
        goal->promise.fail(chained(finished, cancelled("the goal was cancelled")));
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
    if(detail::event_due(slot))
    {
      return;
    }
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
    error = chained(finish_goal(slot, nullptr), std::move(error));
    park(slot, done, [&error](member_slot& parked, deferred& later) {
      later.push_back([goal = std::move(parked.goal), error = std::move(error)] mutable {
        goal->goal.stop_when = nullptr;
        goal->promise.fail(std::move(error));
      });
    });
  }

  void driver_slice(member_slot& slot, deferred& done)
  {
    auto&                        goal   = *slot.goal;
    auto&                        driver = *goal.goal.driver;
    std::optional<slice_request> planned;
    auto const                   earliest = [this, &slot] {
      accept_stamps(slot);
      return next_stamp(slot);
    };
    try
    {
      owner_scope const owning{this};
      planned = driver.plan(*slot.engine, earliest);
    }
    catch(...)
    {
      owner_scope const owning{this};
      driver.failed(*slot.engine, std::current_exception());
      return;
    }
    if(not planned and driver.sleep_requested())
    {
      std::scoped_lock const lock{mutex};
      if(not std::exchange(slot.woken, false))
      {
        slot.state = host_state::idle_wait;
      }
      return;
    }
    if(not planned)
    {
      goal_result const outcome{.stopped = {}, .executed = goal.executed, .yielded = true};
      park(slot, done, [&outcome](member_slot& parked, deferred& later) {
        later.push_back([goal = std::move(parked.goal), outcome] { goal->promise.set(outcome); });
      });
      return;
    }
    auto&        request = *planned;
    slice_result result;
    try
    {
      owner_scope const owning{this};
      at_boundary(slot);
      result = slot.engine->run_slice(request);
    }
    catch(...)
    {
      owner_scope const owning{this};
      driver.failed(*slot.engine, std::current_exception());
      return;
    }
    request.stop_when = {};
    after_slice(slot, request, result);
    goal.executed.value += result.executed.value;
    try
    {
      owner_scope const owning{this};
      at_boundary(slot);
      driver.ran(*slot.engine, result);
    }
    catch(...)
    {
      owner_scope const owning{this};
      driver.failed(*slot.engine, std::current_exception());
    }
  }

  void goal_slice(member_slot& slot, bool unhold, deferred& done)
  {
    accept_stamps(slot);
    apply_due(slot);
    if(slot.goal->goal.driver)
    {
      driver_slice(slot, done);
      return;
    }
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
        .stop_when    = goal.goal.stop_when ? std::function<bool()>{std::ref(goal.goal.stop_when)}
                                            : std::function<bool()>{},
        .until_return = goal.goal.until_return,
        .leave_until  = once ? once->leave_until : goal.executed.value == 0,
        .until_thread = goal.goal.until_thread,
    };
    add_stamp(request, next_stamp(slot));
    slice_result result;
    try
    {
      owner_scope const owning{this};
      if(unhold)
      {
        give_back(*slot.engine);
      }
      at_boundary(slot);
      result = slot.engine->run_slice(request);
    }
    catch(...)
    {
      fail_goal(slot, done, std::current_exception());
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
    if(result.thread_ready)
    {
      try
      {
        owner_scope const owning{this};
        auto&             target = *slot.engine;
        if(target.thread_ready())
        {
          goal.goal.until_thread = false;
          if(goal.goal.then_thread)
          {
            goal.goal.then_thread(target);
          }
        }
      }
      catch(...)
      {
        fail_goal(slot, done, std::current_exception());
      }
      return;
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
          goal->goal.stop_when = nullptr;
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

  void free_slice(member_slot& slot, waiter_list waiters, bool unhold, deferred& done)
  {
    accept_stamps(slot);
    apply_due(slot);
    std::erase_if(waiters, [](auto const& waiting) { return not waiting->live(); });
    std::vector<std::pair<std::shared_ptr<waiter>, stop>> resolved;
    auto const                                            slice = ++slot.free_slices;
    vtime                                                 registered{};
    {
      owner_scope const owning{this};
      auto const        now = slot.engine->virtual_clock().time();
      registered            = now;
      for(auto const& waiting : waiters)
      {
        waiting->register_at(now, slice);
      }
    }
    slice_request request{
        .max_instructions = bounds.max_instructions,
        .wall_budget      = bounds.wall_budget,
    };
    auto conditions = std::make_shared<waiter_list>();
    for(auto const& waiting : waiters)
    {
      if(waiting->spec.deadline)
      {
        auto const due     = std::max(*waiting->spec.deadline, registered);
        request.until_time = std::min(request.until_time.value_or(due), due);
      }
      if(waiting->spec.when and waiting->checked)
      {
        conditions->push_back(waiting);
      }
    }
    add_stamp(request, next_stamp(slot));
    if(not conditions->empty())
    {
      request.stop_when = [conditions, engine = slot.engine.get()] {
        auto any = false;
        for(auto const& waiting : *conditions)
        {
          if(not waiting->fired and not waiting->dropped.load(std::memory_order_relaxed))
          {
            waiting->fired = holds_now(*waiting, *engine);
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
      if(unhold)
      {
        give_back(*slot.engine);
      }
      at_boundary(slot);
      result = slot.engine->run_slice(request);
    }
    catch(...)
    {
      settle(resolved);
      end_free_run(slot, held_end{std::current_exception()}, done);
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
      end_free_run(slot, held_end{std::current_exception()}, done);
      return;
    }
    auto const reason = result.stopped ? std::optional{result.stopped->reason} : std::nullopt;
    auto const fired =
        std::ranges::any_of(*conditions, [](auto const& waiting) { return waiting->fired; });
    auto const goes_on = not reason
                      or reason == stop_reason::time
                      or reason == stop_reason::idle
                      or (reason == stop_reason::user and fired and result.asked);
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
        waiting->register_at(now, slice + 1);
        waiting->checked   = true;
        auto const expired = waiting->spec.deadline and now >= *waiting->spec.deadline;
        if(not waiting->fired and waiting->spec.when and not expired)
        {
          waiting->fired = holds_now(*waiting, *slot.engine);
        }
        else if(
            not waiting->fired
            and waiting->spec.ready
            and waiting->since_slice <= slice
            and (now > *waiting->since or reason == stop_reason::idle))
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
        else if(expired)
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

  auto close_session(std::unique_lock<host_mutex>& lock, member_slot& slot)
      -> std::optional<held_end>
  {
    slot.session  = false;
    slot.free_run = false;
    if(not slot.goal)
    {
      slot.halt_requested   = false;
      slot.cancel_requested = false;
    }
    auto end     = std::exchange(slot.held, std::nullopt);
    auto waiters = std::exchange(slot.waiters, {});
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
          if(not waiting->promise.done())
          {
            waiting->members_fired = 0;
            waiting->ended.store(true, std::memory_order_relaxed);
          }
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
        slot->driver.store(nullptr);
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
    {
      std::scoped_lock const lock{mutex};
      exiting = true;
    }
    parked_cv.notify_all();
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

chained_error::chained_error(std::exception_ptr raised, std::exception_ptr during) noexcept
    : raised_{std::move(raised)}, during_{std::move(during)}
{}

auto chained_error::raised() const noexcept -> std::exception_ptr const&
{
  return raised_;
}

auto chained_error::during() const noexcept -> std::exception_ptr const&
{
  return during_;
}

auto chained_error::what() const noexcept -> char const*
{
  return "a goal's finish failed while the goal ended by another error";
}

namespace
{
struct lock_graph
{
  std::mutex                                                    leaf;
  std::unordered_map<detail::host_core const*, std::thread::id> owners;
  std::unordered_map<std::thread::id, detail::host_core const*> waits;
};

std::atomic<std::uint64_t> registered_waits{0};

auto graph() -> lock_graph&
{
  static lock_graph shared;
  return shared;
}

std::atomic<std::uint64_t> forks{0};

void before_fork() noexcept
{
  graph().leaf.lock();
}

void after_fork_in_parent() noexcept
{
  graph().leaf.unlock();
}

void after_fork_in_child() noexcept
{
  auto& on = graph();
  on.owners.clear();
  on.waits.clear();
  forks.fetch_add(1, std::memory_order_relaxed);
  on.leaf.unlock();
}

void watch_forks() noexcept
{
  static int const registered =
      ::pthread_atfork(before_fork, after_fork_in_parent, after_fork_in_child);
  std::ignore = registered;
}

void record_hold(detail::host_core const* of)
{
  auto&                  on = graph();
  std::scoped_lock const lock{on.leaf};
  on.owners.insert_or_assign(of, std::this_thread::get_id());
}

void forget_hold(detail::host_core const* of) noexcept
{
  auto&                  on = graph();
  std::scoped_lock const lock{on.leaf};
  on.owners.erase(of);
}
}

held_lock::held_lock(std::unique_lock<detail::host_mutex> lock, detail::host_core const* of)
    : lock_{std::move(lock)}, of_{of}
{
  if(lock_.owns_lock())
  {
    record_hold(of_);
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
      forget_hold(of_);
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
    forget_hold(of_);
    --detail::host_locks_held;
  }
}

lock_wait::~lock_wait()
{
  auto&                  on = graph();
  std::scoped_lock const lock{on.leaf};
  on.waits.erase(std::this_thread::get_id());
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
  forget_hold(of_);
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

auto waiter_handle::fired() const noexcept -> std::uint64_t
{
  return waiter_->members_fired;
}

auto waiter_handle::ended() const noexcept -> bool
{
  return waiter_->ended.load(std::memory_order_relaxed);
}

auto waiter_handle::deadline() const noexcept -> std::optional<vtime>
{
  if(waiter_->spec.after and not waiter_->since)
  {
    return std::nullopt;
  }
  return waiter_->spec.deadline;
}

auto waiter_handle::holders() const noexcept -> long
{
  return waiter_.use_count();
}

auto condition_holds(condition const& tested, machine& target) -> bool
{
  std::array<std::byte, 8> bytes{};
  if(not target.mem().read_block(tested.where, std::span{bytes}.first(tested.size)))
  {
    return false;
  }
  std::uint64_t word = 0;
  for(auto const [index, byte] : std::views::enumerate(std::span{bytes}.first(tested.size)))
  {
    word |= std::uint64_t{std::to_integer<std::uint8_t>(byte)} << (8 * index);
  }
  word >>= tested.shift;
  if(tested.bits < 64)
  {
    word &= (std::uint64_t{1} << tested.bits) - 1;
  }
  if(tested.masked)
  {
    return detail::compared(word & tested.mask, tested.value, tested.compare);
  }
  switch(tested.encoding)
  {
    case condition::kind::unsigned_int:
      return detail::compared(word, tested.value, tested.compare);
    case condition::kind::signed_int:
    {
      if(tested.bits < 64 and ((word >> (tested.bits - 1)) & 1) != 0)
      {
        word |= ~((std::uint64_t{1} << tested.bits) - 1);
      }
      return detail::compared(
          std::bit_cast<std::int64_t>(word), std::bit_cast<std::int64_t>(tested.value),
          tested.compare);
    }
    case condition::kind::floating:
    {
      auto const real = tested.size == 4
                          ? double{std::bit_cast<float>(static_cast<std::uint32_t>(word))}
                          : std::bit_cast<double>(word);
      return detail::compared(real, tested.real, tested.compare);
    }
  }
  return false;
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
{
  watch_forks();
}

host::~host()
{
  if(forked())
  {
    std::ignore = std::make_unique<std::jthread>(std::move(thread_)).release();
    std::ignore = std::make_unique<std::shared_ptr<detail::host_core>>(std::move(core_)).release();
    return;
  }
  if(not thread_.joinable())
  {
    return;
  }
  if(detail::on_host_thread
     or detail::host_locks_held > 0
     or (core_->hooks.may_block and not core_->hooks.may_block()))
  {
    request_stop();
    decide(detail::fate::orphaned);
    thread_.detach();
    return;
  }
  close();
}

auto host::forked() const noexcept -> bool
{
  return core_->generation != fork_generation();
}

auto host::fork_generation() noexcept -> std::uint64_t
{
  return forks.load(std::memory_order_relaxed);
}

void host::refuse_forked() const
{
  if(forked())
  {
    throw machine_forked(
        "{} was built before os.fork, in the parent process: build machines in the process "
        "that uses them",
        core_->name);
  }
}

auto host::lock() -> held_lock
{
  refuse_forked();
  return held_lock{std::unique_lock{core_->mutex}, core_.get()};
}

auto host::try_lock() -> held_lock
{
  refuse_forked();
  std::unique_lock attempt{core_->mutex, std::try_to_lock};
  if(not attempt.owns_lock())
  {
    return {};
  }
  return held_lock{std::move(attempt), core_.get()};
}

auto host::lock_for(std::chrono::nanoseconds limit) -> held_lock
{
  refuse_forked();
  std::unique_lock attempt{core_->mutex, limit};
  if(not attempt.owns_lock())
  {
    return {};
  }
  return held_lock{std::move(attempt), core_.get()};
}

auto host::register_wait() -> lock_wait
{
  auto&                  on = graph();
  auto const             me = std::this_thread::get_id();
  std::scoped_lock const lock{on.leaf};
  auto const*            at = core_.get();
  for(auto steps = on.owners.size(); steps > 0; --steps)
  {
    auto const owner = on.owners.find(at);
    if(owner == on.owners.end())
    {
      break;
    }
    if(owner->second == me)
    {
      throw machine_running(
          "{} is held by a thread that waits for {}, which this thread holds: the access would "
          "deadlock",
          core_->name, at->name);
    }
    auto const waiting = on.waits.find(owner->second);
    if(waiting == on.waits.end())
    {
      break;
    }
    at = waiting->second;
  }
  on.waits.insert_or_assign(me, core_.get());
  registered_waits.fetch_add(1, std::memory_order_relaxed);
  return lock_wait{};
}

auto host::waits_registered() noexcept -> std::uint64_t
{
  return registered_waits.load(std::memory_order_relaxed);
}

void host::name(std::string label)
{
  core_->name = std::move(label);
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
  if(core_->abandoned)
  {
    throw machine_closed("the host was abandoned");
  }
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
  if(core_->abandoned_flag.load(std::memory_order_acquire))
  {
    promise.fail(detail::closed_error());
    return promise.future();
  }
  auto run_here = [&] {
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
  if(at == now
     and not detail::event_due(slot)
     and not(slot.next_stamp() and *slot.next_stamp() <= at))
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

void host::supersede(held_lock const&, member_id who)
{
  if(core_->abandoned)
  {
    return;
  }
  detail::supersede_parked(*core_->members[who]);
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
  if(detail::linked(slot) and not goal.driver)
  {
    throw machine_busy("the machine is a member of a linked system");
  }
  auto made             = std::make_unique<detail::goal_run>();
  made->remaining       = goal.budget;
  made->left            = goal.exact;
  made->goal            = std::move(goal);
  auto future           = made->promise.future();
  auto driver           = made->goal.driver;
  slot.goal             = std::move(made);
  slot.halt_requested   = false;
  slot.cancel_requested = false;
  detail::supersede_parked(slot);
  slot.with_host = true;
  slot.state     = host_state::running;
  ensure_thread();
  slot.driver.store(std::move(driver));
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
  if(detail::linked(slot))
  {
    throw machine_busy("the machine is a member of a linked system");
  }
  ++slot.sessions;
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

void host::link(held_lock const& held, member_id who, bool on, std::atomic<bool> const* released)
{
  if(on and busy(held, who))
  {
    throw machine_busy("the machine runs a goal or a free run");
  }
  core_->members[who]->linked        = on;
  core_->members[who]->link_released = on ? released : nullptr;
  if(on)
  {
    core_->members[who]->system_held = true;
  }
}

void host::give_back_core(held_lock const& held, member_id who)
{
  auto& slot = *core_->members[who];
  if(core_->abandoned
     or busy(held, who)
     or not parked(held, who)
     or detail::linked(slot)
     or not std::exchange(slot.system_held, false))
  {
    return;
  }
  owner_scope const owning{*this, held};
  detail::give_back(*slot.engine);
}

auto host::wait(held_lock const&, member_id who, waiter_spec spec) -> waiter_handle
{
  auto& slot     = *core_->members[who];
  auto  waiting  = std::make_shared<detail::waiter>();
  waiting->spec  = std::move(spec);
  waiting->owner = core_;
  auto future    = waiting->promise.future();
  auto handle    = waiting;
  if(core_->closed or core_->abandoned)
  {
    waiting->settling().fail(detail::closed_error());
  }
  else if(slot.held)
  {
    waiting->ended.store(true, std::memory_order_relaxed);
    detail::complete_with(waiting->settling(), *slot.held);
  }
  else
  {
    auto const ends_slice = waiting->spec.when or waiting->spec.after or waiting->spec.deadline;
    slot.waiters.push_back(std::move(waiting));
    if(ends_slice)
    {
      core_->notify_locked(slot);
    }
    else
    {
      core_->wake_locked(slot);
    }
  }
  return waiter_handle{std::move(handle), std::move(future)};
}

auto host::request_cancel(member_id who) -> std::optional<std::uint64_t>
{
  refuse_forked();
  if(detail::host_locks_held > 0)
  {
    throw machine_running("cancel() from a thread that holds a host mutex");
  }
  auto& slot = *core_->members[who];
  core_->before_block();
  std::scoped_lock const lock{core_->mutex};
  if(core_->abandoned or not slot.goal)
  {
    return std::nullopt;
  }
  if(slot.goal->goal.driver)
  {
    throw machine_busy("a linked system runs the machine");
  }
  slot.cancel_requested = true;
  core_->notify_locked(slot);
  if(detail::on_host_thread)
  {
    return std::nullopt;
  }
  return slot.parks;
}

auto host::request_halt(member_id who, bool refuse_goal) -> halt_ticket
{
  refuse_forked();
  if(detail::host_locks_held > 0)
  {
    throw machine_running("halt() from a thread that holds a host mutex");
  }
  auto& slot = *core_->members[who];
  core_->before_block();
  std::scoped_lock const lock{core_->mutex};
  if(core_->abandoned or (not slot.goal and not slot.session))
  {
    return {};
  }
  if(slot.goal and slot.goal->goal.driver)
  {
    throw machine_busy("a linked system runs the machine");
  }
  if(refuse_goal and slot.goal and not detail::on_host_thread)
  {
    throw machine_busy("a run, step or call is active, not a free run: cancel its task instead");
  }
  if(slot.with_host)
  {
    slot.halt_requested = true;
    core_->notify_locked(slot);
    if(detail::on_host_thread)
    {
      return {};
    }
    return {.finish = true, .with_host = true, .parks = slot.parks, .session = slot.sessions};
  }
  return {.finish = true, .with_host = false, .parks = slot.parks, .session = slot.sessions};
}

auto host::wait_settled(member_id who, halt_ticket const& ticket, std::chrono::nanoseconds limit)
    -> bool
{
  if(not ticket.finish or detail::on_host_thread)
  {
    return true;
  }
  auto&            slot = *core_->members[who];
  std::unique_lock lock{core_->mutex};
  return core_->parked_cv.wait_for(lock, limit, [this, &slot, &ticket] {
    return core_->abandoned
        or (ticket.with_host ? slot.settled > ticket.parks : slot.settled == slot.parks);
  });
}

void host::wait_settled(member_id who, halt_ticket const& ticket)
{
  if(not ticket.finish or detail::on_host_thread)
  {
    return;
  }
  auto&            slot = *core_->members[who];
  std::unique_lock lock{core_->mutex};
  core_->parked_cv.wait(lock, [this, &slot, &ticket] {
    return core_->abandoned
        or (ticket.with_host ? slot.settled > ticket.parks : slot.settled == slot.parks);
  });
}

auto host::finish_halt(member_id who, halt_ticket const& ticket) -> std::optional<held_end>
{
  if(not ticket.finish)
  {
    return std::nullopt;
  }
  std::unique_lock lock{core_->mutex};
  auto&            slot = *core_->members[who];
  if(core_->abandoned or slot.sessions != ticket.session)
  {
    return std::nullopt;
  }
  return core_->close_session(lock, slot);
}

void host::cancel(member_id who)
{
  if(auto const parks = request_cancel(who))
  {
    wait_settled(who, halt_ticket{.finish = true, .with_host = true, .parks = *parks});
  }
}

auto host::halt(member_id who, bool refuse_goal) -> std::optional<held_end>
{
  auto const ticket = request_halt(who, refuse_goal);
  wait_settled(who, ticket);
  return finish_halt(who, ticket);
}

auto host::abandoned() const noexcept -> bool
{
  return core_->abandoned_flag.load(std::memory_order_acquire);
}

void host::abandon_thread()
{
  if(forked())
  {
    return;
  }
  detail::deferred done;
  {
    std::scoped_lock const joining{join_mutex_};
    if(core_->abandoned_flag.load() or not thread_.joinable())
    {
      return;
    }
    {
      std::scoped_lock const lock{core_->mutex};
      core_->closed    = true;
      core_->abandoned = true;
      core_->abandoned_flag.store(true, std::memory_order_release);
      for(auto const& slot : core_->members)
      {
        done.push_back([mail = std::exchange(slot->mail, {}), waiters = slot->waiters,
                        stamps = slot->take_stamps()] {
          for(auto const& posted : mail)
          {
            posted.promise.fail(detail::closed_error());
          }
          detail::fail_all(stamps, detail::closed_error());
          for(auto const& waiting : waiters)
          {
            waiting->promise.fail(detail::closed_error());
          }
        });
      }
    }
    request_stop();
    decide(detail::fate::orphaned);
    thread_.detach();
  }
  core_->parked_cv.notify_all();
  detail::run_all(done);
}

auto host::orphan() const -> std::weak_ptr<void const>
{
  return running_;
}

auto host::member_owner(member_id who) const -> std::shared_ptr<machine>
{
  return core_->members[who]->engine;
}

auto host::close_for(std::chrono::nanoseconds limit) -> bool
{
  refuse_forked();
  if(detail::host_locks_held > 0)
  {
    throw machine_running("close() from a thread that holds a host mutex");
  }
  core_->before_block();
  {
    std::scoped_lock const lock{core_->mutex};
    if(core_->abandoned)
    {
      return true;
    }
    core_->closed = true;
  }
  if(detail::on_host_thread)
  {
    stop_unless_joining();
    return true;
  }
  if(stop_started())
  {
    std::unique_lock lock{core_->mutex};
    if(not core_->parked_cv.wait_for(
           lock, limit, [this] { return core_->exiting or core_->abandoned; }))
    {
      return false;
    }
  }
  close();
  return true;
}

auto host::stop_started() -> bool
{
  std::scoped_lock const joining{join_mutex_};
  if(core_->abandoned_flag.load() or not thread_.joinable())
  {
    return false;
  }
  request_stop();
  return true;
}

void host::stop_unless_joining()
{
  std::unique_lock const joining{join_mutex_, std::try_to_lock};
  if(joining.owns_lock() and not core_->abandoned_flag.load() and thread_.joinable())
  {
    request_stop();
  }
}

auto host::state(member_id who) const noexcept -> host_state
{
  return core_->members[who]->state.load(std::memory_order_relaxed);
}

auto host::owns_engine() const noexcept -> bool
{
  return detail::engine_owner == core_.get();
}

void host::wake(member_id who)
{
  std::scoped_lock const lock{core_->mutex};
  core_->wake_locked(*core_->members[who]);
}

void host::close()
{
  refuse_forked();
  if(detail::host_locks_held > 0)
  {
    throw machine_running("close() from a thread that holds a host mutex");
  }
  core_->before_block();
  {
    std::scoped_lock const lock{core_->mutex};
    if(core_->abandoned)
    {
      return;
    }
    core_->closed = true;
  }
  if(detail::on_host_thread)
  {
    stop_unless_joining();
    return;
  }
  if(stop_started())
  {
    std::unique_lock lock{core_->mutex};
    core_->parked_cv.wait(lock, [this] { return core_->exiting or core_->abandoned; });
  }
  {
    std::scoped_lock const joining{join_mutex_};
    if(core_->abandoned_flag.load())
    {
      return;
    }
    decide(detail::fate::joined);
    if(thread_.joinable())
    {
      thread_.join();
    }
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

namespace
{
/**
 * @brief What a host thread holds. running is declared first, so it expires after core, the
 *        thread's last share of the members, is released.
 */
struct serving
{
  std::shared_ptr<char const>        running = std::make_shared<char const>();
  std::shared_ptr<detail::host_core> core;
};
}

void host::ensure_thread()
{
  if(not thread_.joinable())
  {
    serving held{.core = core_};
    running_ = held.running;
    thread_  = std::jthread{
        [held = std::move(held)](std::stop_token const& stop) { held.core->serve(stop); }};
  }
}

void host::request_stop()
{
  for(auto const& slot : core_->members)
  {
    if(auto const driving = slot->driver.load())
    {
      driving->abort();
    }
  }
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
  refuse_forked();
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
  if(forked())
  {
    return;
  }
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
  auto expected = detail::fate::undecided;
  if(core_->ending.compare_exchange_strong(expected, ending))
  {
    core_->ending.notify_all();
  }
}
}
