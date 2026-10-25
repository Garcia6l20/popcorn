#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/system.hpp>

#include <reflex/enum.hpp>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <format>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace popcorn::core
{
member_link::~member_link() = default;

namespace
{
enum class entry_kind : std::uint8_t
{
  stopped,
  error,
  system_time,
  idle,
};

struct entry
{
  sys_time           at{};
  std::size_t        member{};
  entry_kind         kind{};
  popcorn::stop      stopped{};
  std::exception_ptr error{};
};

auto earlier(entry const& a, entry const& b) noexcept -> bool
{
  return std::pair{a.at, a.member} < std::pair{b.at, b.member};
}

auto earlier_of(sys_time const& a, sys_time const& b) noexcept -> sys_time
{
  return b < a ? b : a;
}

auto left(std::optional<icount> const& total, std::uint64_t done) -> std::optional<icount>
{
  if(not total)
  {
    return std::nullopt;
  }
  return icount{total->value - std::min(total->value, done)};
}

enum class phase : std::uint8_t
{
  begin,
  run,
  halted,
  arrive_b,
  after_b,
  idle_to,
  park,
  sleeping,
};

struct report
{
  sys_time           position{};
  bool               done{};
  bool               quiescent{};
  bool               has_work{};
  bool               clean{};
  bool               quiet{};
  std::vector<entry> found{};
};

struct decision
{
  bool                 pause{};
  bool                 need_b{};
  bool                 idle{};
  bool                 exchange{};
  bool                 sleep{};
  sys_time             target{};
  std::optional<entry> head{};
  sys_time             confirm{};
  std::uint64_t        window{};
  sys_time             barrier{};
  sys_time             next{};
};

struct goal_run
{
  member_goal                        goal;
  detail::host_promise<goal_reached> promise{};
  sys_time                           start{};
  std::uint64_t                      executed{};
  bool                               system{};
};

using settled = std::expected<goal_reached, std::exception_ptr>;

struct saved_member
{
  sys_time                base{};
  std::optional<sys_time> held_to{};
  bool                    released{};
};

struct saved_system
{
  std::uint32_t                       format{1};
  std::uint64_t                       window{};
  sys_time                            pending{};
  std::optional<sys_time>             last_point{};
  std::vector<saved_member>           members{};
  std::vector<std::vector<std::byte>> links{};
};
}

struct system::impl
{
  class runner;

  struct slot
  {
    member_ref                                                            ref;
    machine*                                                              target{};
    std::shared_ptr<impl::runner>                                         driver{};
    time_map                                                              map{};
    std::uint64_t                                                         resets{};
    std::vector<member_link*>                                             outbound{};
    std::vector<member_link*>                                             inbound{};
    std::optional<goal_run>                                               goal{};
    bool                                                                  undecided{};
    bool                                                                  released{};
    std::optional<sys_time>                                               held_to{};
    bool                                                                  resolved{};
    bool                                                                  failed{};
    bool                                                                  warned_reset{};
    report                                                                rep{};
    std::uint64_t                                                         slices{};
    icount                                                                executed{};
    std::chrono::nanoseconds                                              busy{};
    std::chrono::nanoseconds                                              drained{};
    std::optional<std::pair<detail::host_promise<goal_reached>, settled>> outcome{};
    std::unique_ptr<std::atomic<std::int64_t>>                            base_ps =
        std::make_unique<std::atomic<std::int64_t>>();

    void rebase(time_map moved) noexcept
    {
      map = moved;
      base_ps->store(moved.base().ps, std::memory_order_release);
    }

    [[nodiscard]] auto running() const noexcept -> bool
    {
      return goal and not resolved and not failed;
    }
  };

  struct completion
  {
    impl* self;
    void (impl::*step)() noexcept;

    void operator()() const noexcept
    {
      (self->*step)();
    }
  };

  impl(
      system const*                              owner,
      std::vector<member_ref>                    members,
      std::vector<std::shared_ptr<member_link>>  wires,
      slice_bounds                               slicing,
      std::move_only_function<void(std::size_t)> hook,
      std::move_only_function<void(std::size_t)> sleep_hook);

  [[nodiscard]] auto deliverable(entry const& head) const noexcept -> bool;
  [[nodiscard]] auto parks(slot const& one) const noexcept -> bool;
  [[nodiscard]] auto best_entry() const -> std::optional<entry>;
  [[nodiscard]] auto idle_entry() const -> std::optional<entry>;
  void               merge_found() noexcept;
  void               account() noexcept;
  void               set_pause(entry const& head) noexcept;
  void               reconsider() noexcept;
  void               settle_confirm() noexcept;
  void               complete_a() noexcept;
  void               complete_b() noexcept;
  void               complete_c() noexcept;
  void               deliver(entry const& head) noexcept;
  void               finish_pause() noexcept;
  void               reset_member(slot& one, machine& target, reset_kind kind);
  void               require_paused() const;
  void               require_savable() const;
  auto               load_links(std::span<std::byte const> in, bool keep) -> saved_system;
  void               wake_all(std::optional<std::size_t> except = {}) noexcept;
  [[nodiscard]] auto in_flight() const noexcept -> bool;

  system const*                              owner;
  std::vector<slot>                          slots;
  std::vector<std::shared_ptr<member_link>>  links;
  sys_time                                   q{};
  window_barrier                             barrier;
  slice_bounds                               bounds;
  std::move_only_function<void(std::size_t)> before_arrival;
  std::move_only_function<void(std::size_t)> before_sleep;
  std::mutex                                 mutex;
  std::uint64_t                              window{};
  sys_time                                   pending{};
  std::optional<sys_time>                    until{};
  std::vector<entry>                         held{};
  decision                                   chosen{};
  pause                                      result{};
  std::optional<sys_time>                    last_point{};
  std::atomic<bool>                          pause_requested{false};
  std::atomic<bool>                          closed{false};
  bool                                       started{};
  bool                                       stopped{};
  bool                                       finished{};
  detail::host_promise<pause>                promise{};
  host_future<pause>                         future{};
  std::atomic<std::size_t>                   outstanding{0};
  std::atomic<bool>                          host_closed{false};
  std::atomic<bool>                          members_released{false};
  std::atomic<bool>                          woke{false};
  bool                                       measuring{};
  completion                                 on_a{this, &impl::complete_a};
  completion                                 on_b{this, &impl::complete_b};
  completion                                 on_c{this, &impl::complete_c};
};

class system::impl::runner final : public member_driver
{
public:
  runner(system::impl& owner, std::size_t index) : sys_{owner}, index_{index}
  {}

  void restart() noexcept
  {
    phase_ = phase::begin;
    if(sys_.measuring)
    {
      left_ = std::chrono::steady_clock::now();
    }
  }

  auto plan(machine& target, earliest_stamp stamp) -> std::optional<slice_request> override
  {
    auto& one = sys_.slots[index_];
    for(;;)
    {
      if(aborted_.load(std::memory_order_acquire))
      {
        return std::nullopt;
      }
      switch(phase_)
      {
        case phase::begin:
          phase_ = phase::run;
          if(target.core_held() == one.goal.has_value())
          {
            target.hold_core(not one.goal);
          }
          continue;
        case phase::run:
          if(auto request = running(target, stamp))
          {
            return request;
          }
          resolve_time(target);
          if(not arrive(target, stamp, sys_.on_a))
          {
            return std::nullopt;
          }
          phase_ = sys_.chosen.need_b ? phase::halted : phase::after_b;
          continue;
        case phase::halted:
          if(auto request = halted(target, stamp))
          {
            return request;
          }
          phase_ = phase::arrive_b;
          continue;
        case phase::arrive_b:
          if(not arrive(target, stamp, sys_.on_b))
          {
            return std::nullopt;
          }
          phase_ = phase::after_b;
          continue;
        case phase::after_b:
          if(not sys_.chosen.pause)
          {
            drain(sys_.chosen.window, sys_.chosen.barrier, sys_.chosen.next);
            one.rep.quiescent = false;
            phase_            = phase::run;
            continue;
          }
          drain(sys_.chosen.window, sys_.chosen.confirm, sys_.chosen.confirm);
          one.rep.quiet = std::ranges::all_of(
                              one.inbound, [](member_link const* link) { return link->quiet(); })
                      and target.pending_events() == 0
                      and not stamp();
          if(not arrive(target, stamp, sys_.on_c))
          {
            return std::nullopt;
          }
          if(sys_.chosen.sleep)
          {
            phase_    = phase::sleeping;
            sleeping_ = true;
            if(sys_.before_sleep)
            {
              sys_.before_sleep(index_);
            }
            return std::nullopt;
          }
          if(not sys_.chosen.pause)
          {
            if(sys_.chosen.exchange)
            {
              drain(sys_.chosen.window, sys_.chosen.barrier, sys_.chosen.next);
            }
            one.rep.quiescent = false;
            phase_            = phase::run;
            continue;
          }
          phase_ = sys_.chosen.idle ? phase::idle_to : sys_.parks(one) ? phase::park : phase::begin;
          if(phase_ == phase::begin)
          {
            return std::nullopt;
          }
          continue;
        case phase::sleeping:
          sleeping_ = false;
          if(sys_.measuring)
          {
            left_ = std::chrono::steady_clock::now();
          }
          if(not sys_.woke.exchange(true))
          {
            sys_.wake_all(index_);
          }
          one.rep.quiescent = false;
          phase_            = phase::run;
          continue;
        case phase::idle_to:
          if(position(target) < sys_.chosen.target)
          {
            window_ = sys_.window;
            return request_to(sys_.chosen.target, false, false, false);
          }
          settle_idle(target);
          phase_ = phase::begin;
          return std::nullopt;
        case phase::park:
          if(position(target) < sys_.chosen.head->at)
          {
            window_ = sys_.window;
            return request_to(sys_.chosen.head->at, false, true, false);
          }
          phase_ = phase::begin;
          return std::nullopt;
      }
    }
  }

  void ran(machine& target, slice_result const& result) override
  {
    auto& one = sys_.slots[index_];
    ++one.slices;
    one.executed.value += result.executed.value;
    if(one.goal)
    {
      one.goal->executed += result.executed.value;
    }
    if(target.resets() != one.resets)
    {
      fail(
          target, std::make_exception_ptr(emulation_error(
                      std::format("member {} was reset behind the system's back", one.ref.name))));
      return;
    }
    if(not result.stopped)
    {
      return;
    }
    auto const& stopped = *result.stopped;
    if(stopped.reason == stop_reason::idle)
    {
      one.rep.quiescent = true;
      return;
    }
    if(stopped.reason == stop_reason::reset_request)
    {
      auto const kind = reflex::to_enum_value<reset_kind>(stopped.detail);
      if(not one.goal or one.ref.on_reset == reset_request_policy::reset_and_continue)
      {
        if(not one.goal and not one.warned_reset)
        {
          one.warned_reset = true;
          target.log(
              {.kind    = log_kind::model_warning,
               .message = std::format(
                   "member {} requested a {} reset while halted, reset and continued", one.ref.name,
                   stopped.detail)});
        }
        sys_.reset_member(one, target, kind.value_or(reset_kind::software));
        return;
      }
    }
    if((stopped.reason == stop_reason::address
        or stopped.reason == stop_reason::budget
        or stopped.reason == stop_reason::returned)
       and held_last_
       and stopped.time >= until_last_)
    {
      return;
    }
    if(not one.goal)
    {
      fail(
          target, std::make_exception_ptr(emulation_error(
                      std::format(
                          "member {} stopped while halted: {}", one.ref.name,
                          reflex::enum_value_name(stopped.reason)))));
      return;
    }
    resolve(target, stopped);
  }

  void failed(machine& target, std::exception_ptr error) noexcept override
  {
    fail(target, std::move(error));
  }

  void abort() noexcept override
  {
    aborted_.store(true, std::memory_order_release);
    sys_.barrier.abort();
  }

  auto sleep_requested() noexcept -> bool override
  {
    return sleeping_ and not aborted_.load(std::memory_order_acquire);
  }

private:
  [[nodiscard]] auto found_at(machine const& target) const noexcept -> sys_time
  {
    auto const at = position(target);
    if(phase_ == phase::park and at < sys_.chosen.head->at)
    {
      return sys_.chosen.head->at;
    }
    if(phase_ == phase::idle_to and at < sys_.chosen.target)
    {
      return sys_.chosen.target;
    }
    return at;
  }

  void fail(machine const& target, std::exception_ptr error) noexcept
  {
    auto& one  = sys_.slots[index_];
    one.failed = true;
    one.rep.found.push_back(
        {.at = found_at(target), .member = index_, .kind = entry_kind::error, .error = error});
  }

  void resolve(machine& target, popcorn::stop stopped)
  {
    auto& one    = sys_.slots[index_];
    one.resolved = true;
    one.rep.found.push_back(
        {.at      = found_at(target),
         .member  = index_,
         .kind    = entry_kind::stopped,
         .stopped = std::move(stopped)});
  }

  [[nodiscard]] static auto stop_here(machine& target, stop_reason reason, std::string detail)
      -> popcorn::stop
  {
    return popcorn::stop{
        .reason       = reason,
        .where        = target.locate(target.regs().pc()),
        .instructions = target.virtual_clock().now(),
        .time         = target.virtual_clock().time(),
        .resets       = target.resets(),
        .detail       = std::move(detail),
    };
  }

  [[nodiscard]] auto time_bound() const -> std::optional<std::pair<sys_time, bool>>
  {
    auto const& one = sys_.slots[index_];
    if(not one.goal or one.goal->system)
    {
      return std::nullopt;
    }
    std::optional<std::pair<sys_time, bool>> bound;
    if(one.goal->goal.until_time)
    {
      bound = std::pair{*one.goal->goal.until_time, false};
    }
    if(one.goal->goal.cap)
    {
      auto const capped = sys_time{one.goal->start.ps + one.goal->goal.cap->ps};
      if(not bound or capped < bound->first)
      {
        bound = std::pair{capped, true};
      }
    }
    return bound;
  }

  void resolve_time(machine& target)
  {
    auto const& one = sys_.slots[index_];
    if(not one.running())
    {
      return;
    }
    auto const bound = time_bound();
    if(bound and position(target) >= bound->first and one.held_to and *one.held_to >= bound->first)
    {
      resolve(target, stop_here(target, stop_reason::time, bound->second ? "goal cap" : ""));
    }
  }

  void settle_idle(machine& target)
  {
    auto& one = sys_.slots[index_];
    if(not one.running() or one.goal->goal.open)
    {
      return;
    }
    one.outcome.emplace(
        one.goal->promise,
        settled{
            goal_reached{
                         .stopped = stop_here(target, stop_reason::idle, {}), .at = position(target)}
    });
    one.goal.reset();
    one.undecided = true;
  }

  [[nodiscard]] auto position(machine const& target) const noexcept -> sys_time
  {
    return sys_.slots[index_].map.to_system(target.virtual_clock().time());
  }

  [[nodiscard]] auto stamp_of(earliest_stamp const& stamp) const -> std::optional<sys_time>
  {
    auto const local = stamp();
    if(not local)
    {
      return std::nullopt;
    }
    return sys_.slots[index_].map.to_system(*local);
  }

  void enter_windows()
  {
    auto& one = sys_.slots[index_];
    for(auto* link : one.outbound)
    {
      link->enter_window(window_, one.map);
    }
  }

  auto request_to(sys_time bound, bool quiescent, bool hold, bool executes = true) -> slice_request
  {
    auto& one = sys_.slots[index_];
    enter_windows();
    one.held_to = bound;
    held_last_  = hold;
    until_last_ = one.map.to_local(bound);
    slice_request request{
        .max_instructions      = sys_.bounds.max_instructions,
        .wall_budget           = sys_.bounds.wall_budget,
        .until_time            = until_last_,
        .hold_from_until       = hold,
        .internal_stop         = true,
        .stop_quiescent        = quiescent,
        .stop_when_events_only = true,
        .execute               = executes,
    };
    if(executes and one.running())
    {
      auto const& wanted         = one.goal->goal;
      request.until              = wanted.until;
      request.until_also         = wanted.until_also;
      request.until_return       = wanted.until_return;
      request.arrive_at          = wanted.arrive_at;
      request.budget_left        = left(wanted.budget, one.goal->executed);
      request.exact_instructions = left(wanted.exact, one.goal->executed);
      request.leave_until        = one.goal->executed == 0;
      if(wanted.stop_when)
      {
        request.stop_when = std::ref(one.goal->goal.stop_when);
      }
    }
    return request;
  }

  auto due_stamp(machine const& target, std::optional<sys_time> const& at)
      -> std::optional<std::optional<slice_request>>
  {
    if(not at or *at > position(target))
    {
      return std::nullopt;
    }
    if(position(target) >= sys_.pending)
    {
      return std::optional<slice_request>{};
    }
    return std::optional{request_to(position(target), false, false)};
  }

  auto running(machine const& target, earliest_stamp const& stamp) -> std::optional<slice_request>
  {
    auto& one = sys_.slots[index_];
    window_   = sys_.window;
    if(not one.running() or one.rep.quiescent)
    {
      return std::nullopt;
    }
    auto const at = stamp_of(stamp);
    if(auto due = due_stamp(target, at))
    {
      return std::move(*due);
    }
    auto bound = sys_.pending;
    if(not sys_.held.empty())
    {
      bound = earlier_of(bound, sys_.held.front().at);
    }
    if(sys_.until)
    {
      bound = earlier_of(bound, *sys_.until);
    }
    auto const timed = time_bound();
    if(timed)
    {
      bound = earlier_of(bound, timed->first);
    }
    auto const quiescent =
        bound == sys_.pending and not timed and not sys_.until and not(at and *at <= bound);
    if(at)
    {
      bound = earlier_of(bound, *at);
    }
    return toward(target, bound, quiescent);
  }

  auto toward(machine const& target, sys_time bound, bool quiescent) -> std::optional<slice_request>
  {
    auto const& one = sys_.slots[index_];
    if(position(target) < bound or (one.held_to and *one.held_to < bound))
    {
      return request_to(bound, quiescent and position(target) < bound, true);
    }
    return std::nullopt;
  }

  auto halted(machine const& target, earliest_stamp const& stamp) -> std::optional<slice_request>
  {
    auto& one = sys_.slots[index_];
    window_   = sys_.chosen.window;
    if(one.goal or one.failed or one.rep.quiescent)
    {
      return std::nullopt;
    }
    auto const at = stamp_of(stamp);
    if(auto due = due_stamp(target, at))
    {
      return std::move(*due);
    }
    auto       bound     = sys_.chosen.target;
    auto const quiescent = not sys_.chosen.pause and not(at and *at <= bound);
    if(at)
    {
      bound = earlier_of(bound, *at);
    }
    return toward(target, bound, quiescent);
  }

  auto arrive(
      machine const&                  target,
      earliest_stamp const&           stamp,
      system::impl::completion const& complete) -> bool
  {
    auto& one          = sys_.slots[index_];
    one.rep.position   = position(target);
    one.rep.done       = sys_.until and one.rep.position >= *sys_.until;
    auto const next    = target.next_event();
    auto const stamped = stamp_of(stamp);
    auto const due_by  = [&](sys_time const& bound) {
      return (next and *next <= target.virtual_clock().to_icount(one.map.to_local(bound)))
          or (stamped and *stamped <= bound);
    };
    one.rep.has_work = not one.goal and one.rep.position < sys_.pending and due_by(sys_.pending);
    one.rep.clean    = target.pending_events() == 0 and not stamped;
    if(sys_.measuring)
    {
      one.busy = std::chrono::steady_clock::now() - left_;
    }
    if(sys_.before_arrival and not one.failed)
    {
      try
      {
        sys_.before_arrival(index_);
      }
      catch(...)
      {
        fail(target, std::current_exception());
      }
    }
    auto const passed = sys_.barrier.arrive_and_wait(complete);
    if(sys_.measuring)
    {
      left_ = std::chrono::steady_clock::now();
    }
    return passed;
  }

  void drain(std::uint64_t window, sys_time const& barrier, sys_time const& next)
  {
    auto& one = sys_.slots[index_];
    if(not sys_.measuring)
    {
      drain_inbound(one, window, barrier, next);
      return;
    }
    auto const from = std::chrono::steady_clock::now();
    drain_inbound(one, window, barrier, next);
    one.drained += std::chrono::steady_clock::now() - from;
  }

  void drain_inbound(slot& one, std::uint64_t window, sys_time const& barrier, sys_time const& next)
  {
    for(auto* link : one.inbound)
    {
      if(auto fault = link->drain(window, barrier, next, one.map))
      {
        one.rep.found.push_back(
            {.at     = fault->at,
             .member = fault->member,
             .kind   = entry_kind::error,
             .error  = fault->error});
      }
    }
  }

  system::impl&     sys_;
  std::size_t       index_;
  phase             phase_{phase::begin};
  bool              held_last_{};
  bool              sleeping_{};
  std::uint64_t     window_{};
  vtime             until_last_{};
  std::atomic<bool> aborted_{false};

  std::chrono::steady_clock::time_point left_{};
};

system::impl::impl(
    system const*                              owner,
    std::vector<member_ref>                    members,
    std::vector<std::shared_ptr<member_link>>  wires,
    slice_bounds                               slicing,
    std::move_only_function<void(std::size_t)> hook,
    std::move_only_function<void(std::size_t)> sleep_hook)
    : owner{owner}, links{std::move(wires)}, barrier{members.size()}, bounds{slicing},
      before_arrival{std::move(hook)}, before_sleep{std::move(sleep_hook)}
{
  q = sys_time{std::numeric_limits<std::int64_t>::max()};
  for(auto const& link : links)
  {
    contract_assert(link->sender() < members.size() and link->receiver() < members.size());
    contract_assert(link->latency().ps > 0);
    q = earlier_of(q, link->latency());
  }
  pending = q;
  slots.reserve(members.size());
  for(auto& member : members)
  {
    contract_assert(member.on and member.on->members() == 1);
    slots.push_back(slot{.ref = std::move(member)});
  }
  auto ordered = links
               | std::views::transform([](auto const& link) { return link.get(); })
               | std::ranges::to<std::vector>();
  std::ranges::stable_sort(ordered, {}, [](member_link const* link) { return link->sender(); });
  for(auto* link : ordered)
  {
    slots[link->sender()].outbound.push_back(link);
    slots[link->receiver()].inbound.push_back(link);
  }
  std::size_t linked = 0;
  try
  {
    for(std::size_t index = 0; index < slots.size(); ++index)
    {
      slots[index].driver = std::make_shared<runner>(*this, index);
      slots[index].rep.found.reserve(slots[index].inbound.size() + 2);
    }
    for(auto& one : slots)
    {
      auto const held_host = one.ref.on->lock();
      one.ref.on->link(held_host, 0, true, &members_released);
      ++linked;
      one.target = &one.ref.on->member(held_host, 0);
      one.resets = one.target->resets();
    }
  }
  catch(...)
  {
    for(std::size_t index = 0; index < linked; ++index)
    {
      auto const held_host = slots[index].ref.on->lock();
      slots[index].ref.on->link(held_host, 0, false);
    }
    throw;
  }
}

auto system::impl::in_flight() const noexcept -> bool
{
  return future.valid() and not future.done();
}

void system::impl::wake_all(std::optional<std::size_t> except) noexcept
{
  for(std::size_t index = 0; index < slots.size(); ++index)
  {
    if(except == index)
    {
      continue;
    }
    try
    {
      slots[index].ref.on->wake(0);
    }
    catch(...)
    {
      continue;
    }
  }
}

void system::impl::require_savable() const
{
  require_paused();
  if(not started or stopped)
  {
    throw std::logic_error("the system is not started or stopped on an error");
  }
  if(not held.empty()
     or std::ranges::any_of(slots, [](slot const& one) { return one.goal or one.undecided; }))
  {
    throw std::logic_error("a goal, an undecided member or an entry is pending");
  }
}

void system::impl::require_paused() const
{
  if(in_flight())
  {
    throw std::logic_error("the system is running");
  }
  if(finished)
  {
    throw std::logic_error("the system was finished");
  }
}

auto system::impl::deliverable(entry const& head) const noexcept -> bool
{
  return std::ranges::all_of(slots, [&head](slot const& one) {
    return not one.running() or one.rep.quiescent or one.rep.position >= head.at;
  });
}

auto system::impl::parks(slot const& one) const noexcept -> bool
{
  return chosen.pause
     and chosen.head
     and chosen.head->kind == entry_kind::stopped
     and one.running()
     and one.rep.quiescent
     and one.rep.position < chosen.head->at;
}

auto system::impl::best_entry() const -> std::optional<entry>
{
  std::optional<entry> best;
  if(not held.empty() and deliverable(held.front()))
  {
    best = held.front();
  }
  if(until)
  {
    auto const running = [](slot const& one) { return one.running(); };
    if(std::ranges::none_of(slots, running))
    {
      entry reached{
          .at     = *until,
          .member = std::numeric_limits<std::size_t>::max(),
          .kind   = entry_kind::system_time};
      for(auto const& one : slots)
      {
        if(reached.at < one.rep.position)
        {
          reached.at = one.rep.position;
        }
      }
      if(not best or earlier(reached, *best))
      {
        best = reached;
      }
    }
    else if(std::ranges::all_of(slots, [&running](slot const& one) {
              return not running(one) or one.rep.done;
            }))
    {
      entry last{.at = sys_time{std::numeric_limits<std::int64_t>::min()}};
      for(std::size_t index = 0; index < slots.size(); ++index)
      {
        auto const& one = slots[index];
        if(running(one) and last.at < one.rep.position)
        {
          last = {.at = one.rep.position, .member = index, .kind = entry_kind::system_time};
        }
      }
      if(not best or earlier(last, *best))
      {
        best = last;
      }
    }
  }
  return best;
}

auto system::impl::idle_entry() const -> std::optional<entry>
{
  if(until or not held.empty())
  {
    return std::nullopt;
  }
  auto const idle = std::ranges::all_of(slots, [](slot const& one) {
    return not one.failed
       and not one.resolved
       and one.rep.clean
       and (not one.goal or one.rep.quiescent);
  });
  if(not idle)
  {
    return std::nullopt;
  }
  entry last{.at = sys_time{std::numeric_limits<std::int64_t>::min()}, .kind = entry_kind::idle};
  for(std::size_t index = 0; index < slots.size(); ++index)
  {
    if(last.at < slots[index].rep.position)
    {
      last.at     = slots[index].rep.position;
      last.member = index;
    }
  }
  return last;
}

void system::impl::merge_found() noexcept
{
  for(auto& one : slots)
  {
    for(auto& found : one.rep.found)
    {
      held.insert(std::ranges::upper_bound(held, found, earlier), std::move(found));
    }
    one.rep.found.clear();
  }
}

void system::impl::set_pause(entry const& head) noexcept
{
  chosen.pause  = true;
  chosen.head   = head;
  chosen.target = head.at;
  chosen.idle   = head.kind == entry_kind::idle;
}

void system::impl::reconsider() noexcept
{
  auto const head = best_entry();
  if(not head or (chosen.head and not chosen.idle and not earlier(*head, *chosen.head)))
  {
    return;
  }
  if(not chosen.pause)
  {
    --window;
    pending = chosen.barrier;
    --result.windows;
  }
  auto const target = chosen.target;
  set_pause(*head);
  chosen.target = target;
}

void system::impl::settle_confirm() noexcept
{
  if(not chosen.pause)
  {
    return;
  }
  auto at = chosen.head ? chosen.head->at : chosen.target;
  for(auto const& one : slots)
  {
    at = earlier_of(at, one.rep.position);
  }
  chosen.confirm = at;
}

void system::impl::account() noexcept
{
  if(not measuring)
  {
    return;
  }
  auto&                    work = result.work;
  std::chrono::nanoseconds slowest{};
  ++work.rounds;
  for(auto const& one : slots)
  {
    work.serial += one.busy;
    slowest = std::max(slowest, one.busy);
  }
  work.ideal += slowest;
}

void system::impl::complete_a() noexcept
{
  account();
  woke.store(false);
  merge_found();
  chosen = decision{.window = window, .barrier = pending, .next = {pending.ps + q.ps}};
  if(auto const head = best_entry())
  {
    set_pause(*head);
  }
  else if(pause_requested.exchange(false))
  {
    chosen.pause  = true;
    chosen.target = pending;
  }
  else if(auto const idle = idle_entry())
  {
    set_pause(*idle);
  }
  else
  {
    chosen.target = pending;
  }
  chosen.need_b = std::ranges::any_of(slots, [this](slot const& one) {
    return not one.goal and not one.failed and (chosen.pause or one.rep.has_work);
  });
  if(not chosen.pause)
  {
    ++window;
    pending = chosen.next;
    ++result.windows;
  }
  settle_confirm();
}

void system::impl::complete_b() noexcept
{
  account();
  ++result.phase_b;
  merge_found();
  reconsider();
  settle_confirm();
}

void system::impl::complete_c() noexcept
{
  account();
  merge_found();
  reconsider();
  if(chosen.idle and not std::ranges::all_of(slots, [](slot const& one) { return one.rep.quiet; }))
  {
    chosen.pause    = false;
    chosen.idle     = false;
    chosen.exchange = true;
    chosen.head.reset();
    ++window;
    pending = chosen.next;
    ++result.windows;
    return;
  }
  if(chosen.idle
     and std::ranges::any_of(slots, [](slot const& one) { return one.running(); })
     and std::ranges::all_of(
         slots, [](slot const& one) { return not one.running() or one.goal->goal.open; }))
  {
    chosen.pause = false;
    chosen.idle  = false;
    chosen.sleep = true;
    chosen.head.reset();
    return;
  }
  result.slices      = 0;
  result.work.drains = {};
  for(auto const& one : slots)
  {
    result.slices += one.slices;
    result.work.drains += one.drained;
  }
  if(not chosen.head)
  {
    result.reason = pause_reason::requested;
    result.at     = chosen.target;
    last_point    = result.at;
    return;
  }
  deliver(*chosen.head);
}

void system::impl::deliver(entry const& head) noexcept
{
  if(auto const found = std::ranges::find_if(
         held,
         [&head](entry const& one) {
           return one.at == head.at and one.member == head.member and one.kind == head.kind;
         });
     found != held.end())
  {
    held.erase(found);
  }
  if(head.member != std::numeric_limits<std::size_t>::max())
  {
    result.member = head.member;
  }
  result.at  = head.at;
  last_point = head.at;
  switch(head.kind)
  {
    case entry_kind::stopped:
    {
      result.reason  = pause_reason::delivered;
      result.stopped = head.stopped;
      auto& one      = slots[head.member];
      if(one.goal and not one.goal->system)
      {
        one.outcome.emplace(
            one.goal->promise, settled{
                                   goal_reached{.stopped = head.stopped, .at = head.at}
        });
      }
      one.goal.reset();
      one.resolved  = false;
      one.undecided = true;
      break;
    }
    case entry_kind::error:
      result.reason = pause_reason::error;
      result.error  = head.error;
      stopped       = true;
      for(auto& one : slots)
      {
        if(one.goal and not one.goal->system)
        {
          one.outcome.emplace(one.goal->promise, std::unexpected{head.error});
        }
        one.goal.reset();
      }
      held.clear();
      break;
    case entry_kind::system_time:
      result.reason = pause_reason::system_time;
      break;
    case entry_kind::idle:
      result.reason = pause_reason::idle;
      break;
  }
}

void system::impl::finish_pause() noexcept
{
  auto const completing = promise;
  auto       ended      = result;
  auto const was_closed = closed.load() or host_closed.load();
  if(was_closed)
  {
    ended.reason = pause_reason::closed;
    ended.member.reset();
    members_released.store(true, std::memory_order_release);
  }
  std::vector<std::pair<detail::host_promise<goal_reached>, settled>> outcomes;
  for(auto& one : slots)
  {
    if(one.outcome)
    {
      outcomes.push_back(*std::exchange(one.outcome, std::nullopt));
    }
    if(was_closed and one.goal)
    {
      if(not one.goal->system)
      {
        outcomes.emplace_back(
            one.goal->promise,
            std::unexpected{std::make_exception_ptr(machine_closed("the system was closed"))});
      }
      one.goal.reset();
    }
    if(one.goal and one.goal->system)
    {
      one.goal.reset();
    }
  }
  for(auto& [settling, outcome] : outcomes)
  {
    if(outcome)
    {
      std::ignore = settling.set(*outcome);
    }
    else
    {
      std::ignore = settling.fail(outcome.error());
    }
  }
  std::ignore = completing.set(std::move(ended));
}

void system::impl::reset_member(slot& one, machine& target, reset_kind kind)
{
  auto const local = target.virtual_clock().time();
  auto const at    = one.map.to_system(local);
  for(auto* link : one.outbound)
  {
    link->sender_reset(at);
  }
  for(auto* link : one.inbound)
  {
    link->receiver_reset(at);
  }
  target.reset(kind);
  one.rebase(time_map{at});
  one.held_to = at;
  one.resets  = target.resets();
}

held_system::held_system(system const* of, std::unique_lock<std::mutex> lock)
    : owner_{of}, lock_{std::move(lock)}
{}

auto held_system::holds(system const& of) const noexcept -> bool
{
  return owner_ == &of and lock_.owns_lock();
}

system::system(
    std::vector<member_ref>                    members,
    std::vector<std::shared_ptr<member_link>>  links,
    slice_bounds                               bounds,
    std::move_only_function<void(std::size_t)> before_arrival,
    std::move_only_function<void(std::size_t)> before_sleep)
    : impl_{std::in_place,          this,   std::move(members),
            std::move(links),       bounds, std::move(before_arrival),
            std::move(before_sleep)}
{}

system::~system()
{
  close();
  if(impl_->future.valid())
  {
    std::ignore = impl_->future.wait_for(std::chrono::hours{24 * 365});
  }
  impl_->members_released.store(true, std::memory_order_release);
  for(auto& one : impl_->slots)
  {
    if(one.goal and not one.goal->system)
    {
      std::ignore =
          one.goal->promise.fail(std::make_exception_ptr(machine_closed("the system was closed")));
    }
    try
    {
      auto held_host = one.ref.on->lock();
      one.ref.on->link(held_host, 0, false);
    }
    catch(...)
    {
      continue;
    }
  }
}

auto system::lock() -> held_system
{
  return held_system{this, std::unique_lock{impl_->mutex}};
}

auto system::try_lock() -> held_system
{
  return held_system{
      this, std::unique_lock{impl_->mutex, std::try_to_lock}
  };
}

void system::start(held_system const&, std::span<sys_time const> offsets)
{
  impl_->require_paused();
  for(auto&& [one, offset] : std::views::zip(impl_->slots, offsets))
  {
    auto const held_host = one.ref.on->lock();
    auto const local     = one.ref.on->member(held_host, 0).virtual_clock().time();
    one.rebase(time_map{sys_time{offset.ps - static_cast<std::int64_t>(local.ps)}});
    one.held_to = offset;
  }
  impl_->started = true;
}

auto system::post_goal(held_system const&, std::size_t member, member_goal goal)
    -> host_future<goal_reached>
{
  auto& self = *impl_;
  self.require_paused();
  if(self.stopped)
  {
    throw std::logic_error("the system stopped on an error");
  }
  auto& one = self.slots[member];
  if(one.goal)
  {
    throw std::logic_error(std::format("member {} has a goal pending", one.ref.name));
  }
  auto const held_host = one.ref.on->lock();
  auto const at        = one.map.to_system(one.ref.on->member(held_host, 0).virtual_clock().time());
  one.goal.emplace(goal_run{.goal = std::move(goal), .start = at});
  one.undecided = false;
  one.released  = false;
  return one.goal->promise.future();
}

void system::withdraw(held_system const&, std::size_t member)
{
  auto& self = *impl_;
  self.require_paused();
  auto& one = self.slots[member];
  if(not one.goal or one.goal->system)
  {
    throw std::logic_error(std::format("member {} has no goal pending", one.ref.name));
  }
  std::erase_if(self.held, [member](entry const& found) {
    return found.member == member and found.kind == entry_kind::stopped;
  });
  auto const withdrawn = std::move(one.goal->promise);
  one.goal.reset();
  one.resolved = false;
  std::ignore =
      withdrawn.fail(std::make_exception_ptr(operation_cancelled("the goal was withdrawn")));
}

void system::release(held_system const&, std::size_t member, bool halt)
{
  auto& self = *impl_;
  self.require_paused();
  auto& one = self.slots[member];
  if(one.goal)
  {
    throw std::logic_error(std::format("member {} has a goal pending", one.ref.name));
  }
  if(not halt and not one.undecided)
  {
    throw std::logic_error(std::format("member {} is not undecided", one.ref.name));
  }
  one.undecided = false;
  one.released  = halt;
}

auto system::undecided(held_system const&) const -> std::vector<std::size_t>
{
  std::vector<std::size_t> out;
  for(std::size_t index = 0; index < impl_->slots.size(); ++index)
  {
    if(impl_->slots[index].undecided)
    {
      out.push_back(index);
    }
  }
  return out;
}

void system::finish(held_system const&)
{
  auto& self = *impl_;
  self.require_paused();
  auto const at = self.last_point.value_or(self.pending);
  for(auto& one : self.slots)
  {
    if(one.goal and not one.goal->system)
    {
      auto const cancelled = std::move(one.goal->promise);
      one.goal.reset();
      std::ignore =
          cancelled.fail(std::make_exception_ptr(operation_cancelled("the system was finished")));
    }
    one.goal.reset();
    auto const held_host = one.ref.on->lock();
    auto&      target    = one.ref.on->member(held_host, 0);
    if(target.core_held())
    {
      target.hold_core(false);
    }
  }
  for(auto const& link : self.links)
  {
    link->finish(at);
  }
  self.finished = true;
}

auto system::resume(held_system const&, std::optional<sys_time> system_until) -> host_future<pause>
{
  auto& self = *impl_;
  if(self.closed.load() or self.host_closed.load())
  {
    throw machine_closed("the system was closed");
  }
  if(not self.started)
  {
    throw std::logic_error("the system was not started");
  }
  self.require_paused();
  if(self.stopped)
  {
    throw std::logic_error("the system stopped on an error");
  }
  for(auto const& one : self.slots)
  {
    if(one.undecided)
    {
      throw std::logic_error(
          std::format("member {} is undecided: re-issue or release it first", one.ref.name));
    }
  }
  for(auto& one : self.slots)
  {
    auto const held_host = one.ref.on->lock();
    if(one.ref.on->member(held_host, 0).resets() != one.resets)
    {
      throw emulation_error(
          std::format("member {} was reset behind the system's back", one.ref.name));
    }
  }
  self.pause_requested.store(false);
  self.until   = system_until;
  self.result  = pause{};
  self.promise = detail::host_promise<pause>{};
  self.future  = self.promise.future();
  self.outstanding.store(self.slots.size());
  for(auto& one : self.slots)
  {
    if(system_until and not one.goal and not one.released)
    {
      auto const held_host = one.ref.on->lock();
      auto const at = one.map.to_system(one.ref.on->member(held_host, 0).virtual_clock().time());
      one.goal.emplace(goal_run{.goal = {}, .start = at, .system = true});
    }
    auto const index  = static_cast<std::size_t>(&one - self.slots.data());
    auto const queued = [&self, index](entry_kind kind) {
      return std::ranges::any_of(self.held, [index, kind](entry const& found) {
        return found.member == index and found.kind == kind;
      });
    };
    one.resolved      = queued(entry_kind::stopped);
    one.failed        = queued(entry_kind::error);
    one.slices        = 0;
    one.drained       = {};
    one.rep.quiescent = false;
    one.driver->restart();
  }
  auto const future = self.future;
  for(auto& one : self.slots)
  {
    auto const               held_host = one.ref.on->lock();
    host_future<goal_result> running;
    try
    {
      running = one.ref.on->run(held_host, 0, run_goal{.driver = one.driver});
    }
    catch(...)
    {
      self.host_closed.store(true);
      self.barrier.abort();
      auto const unstarted = self.slots.size() - static_cast<std::size_t>(&one - self.slots.data());
      if(self.outstanding.fetch_sub(unstarted) == unstarted)
      {
        self.finish_pause();
      }
      throw;
    }
    running.on_done([&self, running]() noexcept {
      if(not running.done())
      {
        return;
      }
      try
      {
        std::ignore = running.get();
      }
      catch(...)
      {
        self.host_closed.store(true);
        self.barrier.abort();
      }
      if(self.outstanding.fetch_sub(1) == 1)
      {
        self.finish_pause();
      }
    });
  }
  return future;
}

void system::save(held_system const&, std::vector<std::byte>& out) const
{
  auto const& self = *impl_;
  self.require_savable();
  saved_system state{.window = self.window, .pending = self.pending, .last_point = self.last_point};
  for(auto const& one : self.slots)
  {
    state.members.push_back(
        {.base = one.map.base(), .held_to = one.held_to, .released = one.released});
  }
  for(auto const& link : self.links)
  {
    link->save(state.links.emplace_back());
  }
  reflex::serde::bson::serializer{out}.dump(state);
}

auto system::impl::load_links(std::span<std::byte const> in, bool keep) -> saved_system
{
  if(not stopped)
  {
    require_savable();
  }
  require_paused();
  saved_system state;
  try
  {
    state = reflex::serde::bson::deserializer{in}.load<saved_system>();
  }
  catch(std::runtime_error const& failure)
  {
    throw config_error(
        "system state", 0, "bytes", std::format("a state system::save wrote: {}", failure.what()));
  }
  if(state.format != saved_system{}.format)
  {
    throw config_error(
        "system state", 0, "format",
        std::format(
            "a state of format {}, this system loads format {}", state.format,
            saved_system{}.format));
  }
  if(state.members.size() != slots.size() or state.links.size() != links.size())
  {
    throw config_error(
        "system state", 0, "bytes", "a state saved by a system of the same members and links");
  }
  std::vector<std::vector<std::byte>> before(links.size());
  for(auto&& [link, kept] : std::views::zip(links, before))
  {
    link->save(kept);
  }
  std::size_t loaded = 0;
  try
  {
    for(; loaded < links.size(); ++loaded)
    {
      links[loaded]->load(state.links[loaded]);
    }
  }
  catch(...)
  {
    for(std::size_t index = 0; index < loaded; ++index)
    {
      links[index]->load(before[index]);
    }
    throw;
  }
  if(not keep)
  {
    for(auto&& [link, kept] : std::views::zip(links, before))
    {
      link->load(kept);
    }
  }
  return state;
}

void system::check_state(held_system const&, std::span<std::byte const> in)
{
  std::ignore = impl_->load_links(in, false);
}

void system::load(held_system const&, std::span<std::byte const> in)
{
  auto&      self  = *impl_;
  auto const state = self.load_links(in, true);
  for(auto&& [one, saved] : std::views::zip(self.slots, state.members))
  {
    auto const held_host = one.ref.on->lock();
    one.rebase(time_map{saved.base});
    one.held_to  = saved.held_to;
    one.released = saved.released;
    one.resets   = one.ref.on->member(held_host, 0).resets();
  }
  for(auto& one : self.slots)
  {
    one.undecided = false;
  }
  self.held.clear();
  self.stopped    = false;
  self.window     = state.window;
  self.pending    = state.pending;
  self.last_point = state.last_point;
}

void system::request_pause() noexcept
{
  impl_->pause_requested.store(true);
  impl_->wake_all();
}

void system::reset(held_system const&, std::size_t member, reset_kind kind)
{
  if(kind == reset_kind::peripheral)
  {
    throw config_error(
        "reset", 0, "kind", "a machine reset kind, reset_kind::peripheral resets one peripheral");
  }
  auto& self = *impl_;
  self.require_paused();
  auto&      one       = self.slots[member];
  auto const held_host = one.ref.on->lock();
  self.reset_member(one, one.ref.on->member(held_host, 0), kind);
  std::erase_if(self.held, [member](entry const& found) {
    return found.member == member and found.kind == entry_kind::stopped;
  });
  one.undecided = false;
  one.released  = false;
}

void system::close() noexcept
{
  auto& self = *impl_;
  self.closed.store(true);
  self.barrier.abort();
  for(auto& one : self.slots)
  {
    one.driver->abort();
    if(one.target != nullptr)
    {
      one.target->interrupt();
    }
  }
  self.wake_all();
}

void system::wake() noexcept
{
  impl_->wake_all();
}

auto system::members() const noexcept -> std::size_t
{
  return impl_->slots.size();
}

void system::measure_work(held_system const&, bool on)
{
  auto& self = *impl_;
  self.require_paused();
  self.measuring = on;
}

auto system::quantum() const noexcept -> sys_time
{
  return impl_->q;
}

auto system::hosts() const -> std::vector<std::shared_ptr<host>>
{
  std::vector<std::shared_ptr<host>> out;
  for(auto const& one : impl_->slots)
  {
    out.push_back(one.ref.on);
  }
  return out;
}

auto system::pending_barrier(held_system const&) const -> sys_time
{
  if(impl_->in_flight())
  {
    throw machine_running("the linked system is running");
  }
  return impl_->pending;
}

auto system::executed_of(held_system const&, std::size_t member) const -> icount
{
  if(impl_->in_flight())
  {
    throw machine_running("the linked system is running");
  }
  return impl_->slots[member].executed;
}

auto system::base_of(std::size_t member) const noexcept -> sys_time
{
  return sys_time{impl_->slots[member].base_ps->load(std::memory_order_acquire)};
}

auto system::time_of(held_system const&, std::size_t member) const -> sys_time
{
  if(impl_->in_flight())
  {
    throw machine_running("the linked system is running");
  }
  auto const& one       = impl_->slots[member];
  auto const  held_host = one.ref.on->lock();
  return one.map.to_system(one.ref.on->member(held_host, 0).virtual_clock().time());
}
}
