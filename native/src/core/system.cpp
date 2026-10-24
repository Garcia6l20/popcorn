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
  sys_time           anchor{};
  std::size_t        anchored{};
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
  jump,
  halted,
  arrive_b,
  after_b,
  idle_to,
  sleeping,
};

struct report
{
  sys_time           position{};
  sys_time           reach{};
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
  sys_time             barrier{};
  sys_time             next{};
};

constexpr auto never = sys_time{std::numeric_limits<std::int64_t>::max()};

auto later_by(sys_time const& at, std::int64_t by) noexcept -> sys_time
{
  if(at.ps > never.ps - by)
  {
    return never;
  }
  return {at.ps + by};
}

struct member_sync
{
  alignas(64) std::atomic<std::int64_t> clock{};
  alignas(64) std::atomic<std::uint32_t> epoch{};
};

struct coupling
{
  std::size_t source{};
  sys_time    latency{};
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
  std::uint32_t                       format{2};
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
    std::vector<std::uint64_t>                                            sealed{};
    std::vector<coupling>                                                 coupled{};
    std::vector<std::size_t>                                              successors{};
    std::uint64_t                                                         waits{};
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
      std::move_only_function<void(std::size_t)> sleep_hook,
      sync_mode                                  how);

  [[nodiscard]] auto clocked() const noexcept -> bool
  {
    return mode == sync_mode::clocks;
  }

  [[nodiscard]] static auto frozen(slot const& one) noexcept -> bool
  {
    return one.resolved or one.failed;
  }

  [[nodiscard]] auto distance(std::size_t from, std::size_t to) const noexcept -> sys_time
  {
    return distances[from * slots.size() + to];
  }

  void               couple();
  void               bump(std::size_t member) noexcept;
  void               bump_all() noexcept;
  void               found(sys_time at) noexcept;
  void               set_global() noexcept;
  [[nodiscard]] auto pause_points(entry const* head, std::optional<sys_time> requested) const
      -> std::vector<sys_time>;
  void               align(entry const* head, std::optional<sys_time> requested) noexcept;
  void               complete_clock() noexcept;
  [[nodiscard]] auto deliverable(entry const& head) const noexcept -> bool;
  [[nodiscard]] auto best_entry() const -> std::optional<entry>;
  [[nodiscard]] auto idle_entry() const -> std::optional<entry>;
  void               merge_found() noexcept;
  void               account() noexcept;
  void               set_pause(entry const& head) noexcept;
  void               reconsider() noexcept;
  void               settle_confirm() noexcept;
  void               seal() noexcept;
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

  system const*                               owner;
  std::vector<slot>                           slots;
  std::vector<std::shared_ptr<member_link>>   links;
  sys_time                                    q{};
  window_barrier                              barrier;
  slice_bounds                                bounds;
  std::move_only_function<void(std::size_t)>  before_arrival;
  std::move_only_function<void(std::size_t)>  before_sleep;
  std::mutex                                  mutex;
  std::uint64_t                               window{};
  sys_time                                    pending{};
  std::optional<sys_time>                     until{};
  std::vector<entry>                          held{};
  decision                                    chosen{};
  pause                                       result{};
  std::optional<sys_time>                     last_point{};
  std::atomic<bool>                           pause_requested{false};
  std::atomic<bool>                           closed{false};
  bool                                        started{};
  bool                                        stopped{};
  bool                                        finished{};
  detail::host_promise<pause>                 promise{};
  host_future<pause>                          future{};
  std::atomic<std::size_t>                    outstanding{0};
  std::atomic<bool>                           host_closed{false};
  std::atomic<bool>                           members_released{false};
  std::atomic<bool>                           woke{false};
  bool                                        measuring{};
  completion                                  on_a{this, &impl::complete_a};
  completion                                  on_b{this, &impl::complete_b};
  completion                                  on_c{this, &impl::complete_c};
  completion                                  on_clock{this, &impl::complete_clock};
  sync_mode                                   mode{};
  std::unique_ptr<member_sync[]>              syncs{};
  std::unique_ptr<std::atomic<std::size_t>[]> owners{};
  alignas(64) std::atomic<std::int64_t> stop_at{never.ps};
  std::atomic<bool>        decide{false};
  std::atomic<std::size_t> leading{0};
  std::vector<sys_time>    distances{};
  std::vector<sys_time>    targets{};
  std::vector<sys_time>    points{};
  std::optional<sys_time>  requested_at{};
  bool                     aligning{};
};

class system::impl::runner final : public member_driver
{
public:
  runner(system::impl& owner, std::size_t index)
      : sys_{owner}, index_{index}, clocks_(owner.slots.size(), sys_time{})
  {}

  void restart() noexcept
  {
    phase_     = phase::begin;
    lead_      = sys_time{std::numeric_limits<std::int64_t>::min()};
    published_ = lead_;
    counted_   = false;
    calm_      = false;
    jumping_   = false;
    std::ranges::fill(clocks_, sys_time{});
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
          if(sys_.clocked())
          {
            if(auto request = clock_run(target, stamp))
            {
              return request;
            }
            if(aborted_.load(std::memory_order_acquire) or not arrive(target, stamp, sys_.on_clock))
            {
              return std::nullopt;
            }
            if(sys_.chosen.pause)
            {
              phase_ = sys_.chosen.idle ? phase::after_b : phase::jump;
            }
            continue;
          }
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
        case phase::jump:
          if(auto const to = sys_.points[index_];
             not system::impl::frozen(one) and position(target) < to)
          {
            jumping_ = true;
            return request_to(to, false, true);
          }
          phase_ = phase::after_b;
          continue;
        case phase::after_b:
          if(not sys_.chosen.pause)
          {
            drain(sys_.chosen.barrier, sys_.chosen.next);
            one.rep.quiescent = false;
            phase_            = phase::run;
            continue;
          }
          if(sys_.clocked())
          {
            confirm_clocked();
          }
          else
          {
            drain(sys_.chosen.confirm, sys_.chosen.confirm);
          }
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
            if(sys_.clocked())
            {
              phase_ = phase::run;
              continue;
            }
            if(sys_.chosen.exchange)
            {
              drain(sys_.chosen.barrier, sys_.chosen.next);
            }
            one.rep.quiescent = false;
            phase_            = phase::run;
            continue;
          }
          phase_ = sys_.chosen.idle ? phase::idle_to : phase::begin;
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
          unlead();
          phase_ = phase::run;
          continue;
        case phase::idle_to:
          if(position(target) < sys_.chosen.target)
          {
            return request_to(sys_.chosen.target, false, false);
          }
          settle_idle(target);
          phase_ = phase::begin;
          return std::nullopt;
      }
    }
  }

  void ran(machine& target, slice_result const& result) override
  {
    if(jumping_)
    {
      jumping_ = false;
      contract_assert(result.executed.value == 0);
    }
    settle(target, result);
    if(sys_.clocked())
    {
      publish(target, true);
    }
  }

  void failed(machine& target, std::exception_ptr error) noexcept override
  {
    fail(target, std::move(error));
    if(sys_.clocked())
    {
      publish(target, false);
    }
  }

  void abort() noexcept override
  {
    aborted_.store(true, std::memory_order_release);
    sys_.barrier.abort();
    if(sys_.syncs)
    {
      sys_.bump(index_);
    }
  }

  auto sleep_requested() noexcept -> bool override
  {
    return sleeping_ and not aborted_.load(std::memory_order_acquire);
  }

private:
  void settle(machine& target, slice_result const& result)
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

  void fail(machine const& target, std::exception_ptr error) noexcept
  {
    auto&      one = sys_.slots[index_];
    auto const at  = reach(target);
    one.failed     = true;
    one.rep.found.push_back(
        {.at       = position(target),
         .member   = index_,
         .kind     = entry_kind::error,
         .error    = error,
         .anchor   = at,
         .anchored = index_});
    if(sys_.clocked())
    {
      sys_.found(one.rep.found.back().at);
    }
  }

  void resolve(machine& target, popcorn::stop stopped)
  {
    auto&      one = sys_.slots[index_];
    auto const at  = position(target);
    one.resolved   = true;
    one.rep.found.push_back(
        {.at       = at,
         .member   = index_,
         .kind     = entry_kind::stopped,
         .stopped  = std::move(stopped),
         .anchor   = at,
         .anchored = index_});
    if(sys_.clocked())
    {
      sys_.found(at);
    }
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

  void enter_slice()
  {
    auto& one = sys_.slots[index_];
    for(auto* link : one.outbound)
    {
      link->enter_slice(one.map);
    }
  }

  auto request_to(sys_time bound, bool quiescent, bool hold) -> slice_request
  {
    auto& one = sys_.slots[index_];
    enter_slice();
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
    };
    if(one.running())
    {
      auto const& goal           = one.goal->goal;
      request.until              = goal.until;
      request.until_also         = goal.until_also;
      request.until_return       = goal.until_return;
      request.arrive_at          = goal.arrive_at;
      request.budget_left        = left(goal.budget, one.goal->executed);
      request.exact_instructions = left(goal.exact, one.goal->executed);
      request.leave_until        = one.goal->executed == 0;
      if(goal.stop_when)
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
    one.rep.reach      = reach(target);
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

  void drain(sys_time const& barrier, sys_time const& next)
  {
    auto& one = sys_.slots[index_];
    if(not sys_.measuring)
    {
      drain_inbound(one, barrier, next);
      return;
    }
    auto const from = std::chrono::steady_clock::now();
    drain_inbound(one, barrier, next);
    one.drained += std::chrono::steady_clock::now() - from;
  }

  void drain_inbound(slot& one, sys_time const& barrier, sys_time const& next)
  {
    for(std::size_t index = 0; index < one.inbound.size(); ++index)
    {
      take_fault(
          *one.inbound[index],
          one.inbound[index]->drain(one.sealed[index], barrier, next, one.map));
    }
  }

  auto take_fault(member_link const& link, std::optional<link_fault> fault) -> bool
  {
    if(not fault)
    {
      return false;
    }
    auto& one = sys_.slots[index_];
    one.rep.found.push_back(
        {.at       = fault->at,
         .member   = fault->member,
         .kind     = entry_kind::error,
         .error    = fault->error,
         .anchor   = later_by(fault->at, link.latency().ps),
         .anchored = index_});
    if(sys_.clocked())
    {
      sys_.found(fault->at);
    }
    return true;
  }

  [[nodiscard]] auto reach(machine const& target) const noexcept -> sys_time
  {
    return std::max(position(target), lead_);
  }

  void publish(machine const& target, bool hook)
  {
    auto& one = sys_.slots[index_];
    if(hook and sys_.before_arrival and not one.failed)
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
    auto const at = reach(target);
    if(at <= published_ or at.ps <= sys_.syncs[index_].clock.load(std::memory_order_relaxed))
    {
      return;
    }
    published_ = at;
    sys_.syncs[index_].clock.store(at.ps, std::memory_order_release);
    for(auto const next : one.successors)
    {
      sys_.bump(next);
    }
  }

  void unlead() noexcept
  {
    sys_.slots[index_].rep.quiescent = false;
    if(counted_)
    {
      counted_ = false;
      sys_.leading.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  void count_lead() noexcept
  {
    if(counted_)
    {
      return;
    }
    counted_ = true;
    if(sys_.leading.fetch_add(1, std::memory_order_acq_rel) + 1 == sys_.slots.size())
    {
      sys_.decide.store(true, std::memory_order_release);
      sys_.bump_all();
    }
  }

  void settle_quiet() noexcept
  {
    auto const& one = sys_.slots[index_];
    auto const  calm =
        std::ranges::all_of(one.inbound, [](member_link const* link) { return link->quiet(); });
    if(calm and not calm_ and sys_.leading.load(std::memory_order_acquire) == sys_.slots.size())
    {
      sys_.decide.store(true, std::memory_order_release);
      sys_.bump_all();
    }
    calm_ = calm;
  }

  void confirm_clocked()
  {
    auto&      one = sys_.slots[index_];
    auto const from =
        sys_.measuring ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for(std::size_t index = 0; index < one.inbound.size(); ++index)
    {
      auto* const link    = one.inbound[index];
      auto const  barrier = std::min(sys_.points[index_], sys_.points[link->sender()]);
      take_fault(*link, link->drain(one.sealed[index], barrier, barrier, one.map));
    }
    if(sys_.measuring)
    {
      one.drained += std::chrono::steady_clock::now() - from;
    }
  }

  auto clock_run(machine& target, earliest_stamp const& stamp) -> std::optional<slice_request>
  {
    auto& one  = sys_.slots[index_];
    auto& mine = sys_.syncs[index_];
    for(;;)
    {
      auto const seen = mine.epoch.load(std::memory_order_acquire);
      if(aborted_.load(std::memory_order_acquire) or system::impl::frozen(one))
      {
        return std::nullopt;
      }
      auto const here  = position(target);
      auto const ahead = reach(target);
      auto const goal =
          std::min(sys_time{sys_.stop_at.load(std::memory_order_acquire)}, sys_.targets[index_]);
      if(sys_.decide.load(std::memory_order_acquire))
      {
        return std::nullopt;
      }
      if(ahead >= goal)
      {
        if(here >= goal and one.held_to and *one.held_to < goal)
        {
          return request_to(goal, false, true);
        }
        return std::nullopt;
      }
      auto const at = stamp_of(stamp);
      if(at and *at <= here)
      {
        return request_to(here, false, false);
      }
      auto bound = goal;
      for(auto const& edge : one.coupled)
      {
        clocks_[edge.source] =
            sys_time{sys_.syncs[edge.source].clock.load(std::memory_order_acquire)};
        bound = std::min(bound, later_by(clocks_[edge.source], edge.latency.ps));
      }
      auto const timed = one.running() ? time_bound() : std::nullopt;
      if(timed)
      {
        bound = std::min(bound, timed->first);
      }
      if(at)
      {
        bound = std::min(bound, *at);
      }
      auto const from    = sys_.measuring ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
      auto       faulted = false;
      for(std::size_t index = 0; index < one.inbound.size(); ++index)
      {
        auto* const link    = one.inbound[index];
        auto const  through = link->posted_count();
        one.sealed[index]   = through;
        faulted =
            take_fault(
                *link,
                link->drain(through, std::min(ahead, clocks_[link->sender()]), bound, one.map))
            or faulted;
      }
      if(sys_.measuring)
      {
        one.drained += std::chrono::steady_clock::now() - from;
      }
      if(system::impl::frozen(one))
      {
        continue;
      }
      if(faulted)
      {
        auto const lowered =
            std::min(sys_time{sys_.stop_at.load(std::memory_order_acquire)}, sys_.targets[index_]);
        if(sys_.decide.load(std::memory_order_acquire) or ahead >= lowered)
        {
          continue;
        }
        bound = std::min(bound, lowered);
      }
      for(auto const* link : one.inbound)
      {
        if(auto const due = link->due())
        {
          bound = std::min(bound, *due);
        }
      }
      auto const allowed = not sys_.until and not timed and not(at and *at <= bound);
      if(one.rep.quiescent)
      {
        auto const next = target.next_event();
        auto const busy =
            (next and *next < target.virtual_clock().to_icount(one.map.to_local(bound)))
            or (at and *at < bound);
        if(allowed and not busy)
        {
          if(bound > lead_)
          {
            lead_ = bound;
            publish(target, false);
          }
          count_lead();
          settle_quiet();
          if(lead_ >= goal)
          {
            continue;
          }
          ++one.waits;
          mine.epoch.wait(seen, std::memory_order_acquire);
          continue;
        }
        unlead();
      }
      if(here < bound or (one.held_to and *one.held_to < bound))
      {
        return request_to(bound, allowed and here < bound, true);
      }
      if(timed and here >= timed->first)
      {
        resolve_time(target);
        if(one.resolved)
        {
          continue;
        }
      }
      ++one.waits;
      mine.epoch.wait(seen, std::memory_order_acquire);
    }
  }

  system::impl&         sys_;
  std::size_t           index_;
  phase                 phase_{phase::begin};
  bool                  held_last_{};
  bool                  sleeping_{};
  bool                  counted_{};
  bool                  calm_{};
  bool                  jumping_{};
  sys_time              lead_{std::numeric_limits<std::int64_t>::min()};
  sys_time              published_{std::numeric_limits<std::int64_t>::min()};
  std::vector<sys_time> clocks_{};
  vtime                 until_last_{};
  std::atomic<bool>     aborted_{false};

  std::chrono::steady_clock::time_point left_{};
};

system::impl::impl(
    system const*                              owner,
    std::vector<member_ref>                    members,
    std::vector<std::shared_ptr<member_link>>  wires,
    slice_bounds                               slicing,
    std::move_only_function<void(std::size_t)> hook,
    std::move_only_function<void(std::size_t)> sleep_hook,
    sync_mode                                  how)
    : owner{owner}, links{std::move(wires)}, barrier{members.size()}, bounds{slicing},
      before_arrival{std::move(hook)}, before_sleep{std::move(sleep_hook)}, mode{how},
      syncs{std::make_unique<member_sync[]>(members.size())},
      owners{std::make_unique<std::atomic<std::size_t>[]>(links.size())},
      targets(members.size(), never), points(members.size(), never)
{
  q = sys_time{std::numeric_limits<std::int64_t>::max()};
  for(auto const& link : links)
  {
    contract_assert(link->sender() < members.size() and link->receiver() < members.size());
    contract_assert(link->latency().ps > 0);
    q = earlier_of(q, link->latency());
  }
  for(std::size_t index = 0; index < links.size(); ++index)
  {
    owners[index].store(links[index]->sender(), std::memory_order_relaxed);
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
    slots[link->receiver()].sealed.push_back(0);
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

void system::impl::couple()
{
  auto const            n = slots.size();
  std::vector<sys_time> direct(n * n, never);
  auto const            add = [&](std::size_t from, std::size_t to, sys_time latency) {
    auto& cell = direct[from * n + to];
    cell       = std::min(cell, latency);
  };
  for(std::size_t index = 0; index < links.size(); ++index)
  {
    auto const& link = *links[index];
    add(link.sender(), link.receiver(), link.latency());
    auto const by = owners[index].load(std::memory_order_relaxed);
    if(by != link.sender() and by != link.receiver())
    {
      add(by, link.sender(), link.latency());
      add(by, link.receiver(), link.latency());
    }
  }
  for(std::size_t from = 0; from < n; ++from)
  {
    for(std::size_t to = 0; to < n; ++to)
    {
      if(slots[from].goal and not slots[to].goal)
      {
        add(from, to, sys_time{0});
      }
    }
  }
  std::vector<bool> reaches(n * n, false);
  for(std::size_t from = 0; from < n; ++from)
  {
    reaches[from * n + from] = true;
    for(std::size_t to = 0; to < n; ++to)
    {
      reaches[from * n + to] = reaches[from * n + to] or direct[from * n + to] < never;
    }
  }
  for(std::size_t via = 0; via < n; ++via)
  {
    for(std::size_t from = 0; from < n; ++from)
    {
      for(std::size_t to = 0; to < n; ++to)
      {
        if(reaches[from * n + via] and reaches[via * n + to])
        {
          reaches[from * n + to] = true;
        }
      }
    }
  }
  for(std::size_t from = 0; from < n; ++from)
  {
    for(std::size_t to = 0; to < n; ++to)
    {
      if(not reaches[from * n + to])
      {
        add(from, to, q);
      }
    }
  }
  distances = direct;
  for(std::size_t index = 0; index < n; ++index)
  {
    distances[index * n + index] = sys_time{0};
  }
  for(std::size_t via = 0; via < n; ++via)
  {
    for(std::size_t from = 0; from < n; ++from)
    {
      for(std::size_t to = 0; to < n; ++to)
      {
        auto const left  = distances[from * n + via];
        auto const right = distances[via * n + to];
        if(left < never and right < never)
        {
          distances[from * n + to] = std::min(distances[from * n + to], later_by(left, right.ps));
        }
      }
    }
  }
  for(std::size_t to = 0; to < n; ++to)
  {
    auto& one = slots[to];
    one.coupled.clear();
    one.successors.clear();
    for(std::size_t from = 0; from < n; ++from)
    {
      if(direct[from * n + to] < never)
      {
        one.coupled.push_back({.source = from, .latency = direct[from * n + to]});
      }
      if(direct[to * n + from] < never)
      {
        one.successors.push_back(from);
      }
    }
  }
}

void system::impl::bump(std::size_t member) noexcept
{
  syncs[member].epoch.fetch_add(1, std::memory_order_release);
  syncs[member].epoch.notify_one();
}

void system::impl::bump_all() noexcept
{
  for(std::size_t index = 0; index < slots.size(); ++index)
  {
    bump(index);
  }
}

void system::impl::found(sys_time at) noexcept
{
  if(aligning)
  {
    decide.store(true, std::memory_order_release);
  }
  else
  {
    auto seen = stop_at.load(std::memory_order_acquire);
    while(at.ps < seen
          and not stop_at.compare_exchange_weak(seen, at.ps, std::memory_order_acq_rel))
    {
    }
  }
  bump_all();
}

void system::impl::set_global() noexcept
{
  auto at = until.value_or(never);
  if(not held.empty())
  {
    at = earlier_of(at, held.front().at);
  }
  stop_at.store(at.ps, std::memory_order_release);
}

auto system::impl::pause_points(entry const* head, std::optional<sys_time> requested) const
    -> std::vector<sys_time>
{
  auto const n     = slots.size();
  auto const timed = head and head->kind == entry_kind::system_time;
  std::vector<std::pair<sys_time, std::size_t>> sources;
  if(head and not timed)
  {
    sources.emplace_back(head->anchor, head->anchored);
  }
  for(std::size_t from = 0; from < n; ++from)
  {
    if(frozen(slots[from]))
    {
      sources.emplace_back(slots[from].rep.reach, from);
    }
  }
  for(auto const& one : held)
  {
    if((head and earlier(one, *head)) or (not head and requested and one.at <= *requested))
    {
      sources.emplace_back(one.anchor, one.anchored);
    }
  }
  std::vector<sys_time> out(n, never);
  for(std::size_t to = 0; to < n; ++to)
  {
    if(frozen(slots[to]) or (timed and slots[to].goal))
    {
      out[to] = slots[to].rep.reach;
      continue;
    }
    out[to] = timed ? head->at : requested.value_or(never);
    for(auto const& [at, from] : sources)
    {
      out[to] = std::min(out[to], later_by(at, distance(from, to).ps));
    }
    if(until and not timed)
    {
      out[to] = std::min(out[to], *until);
    }
  }
  return out;
}

void system::impl::align(entry const* head, std::optional<sys_time> requested) noexcept
{
  auto       aimed = pause_points(head, requested);
  auto const there =
      std::ranges::all_of(std::views::iota(std::size_t{0}, slots.size()), [&](std::size_t index) {
        return frozen(slots[index]) or slots[index].rep.reach >= aimed[index];
      });
  if(not there)
  {
    if(head and head->kind == entry_kind::system_time)
    {
      for(std::size_t index = 0; index < slots.size(); ++index)
      {
        if(slots[index].goal)
        {
          auto const raised = std::max(slots[index].rep.reach, head->at);
          syncs[index].clock.store(raised.ps, std::memory_order_release);
        }
      }
    }
    targets  = std::move(aimed);
    aligning = true;
    stop_at.store(never.ps, std::memory_order_release);
    return;
  }
  if(head)
  {
    set_pause(*head);
  }
  else
  {
    chosen.pause  = true;
    chosen.target = *requested;
  }
  requested_at.reset();
  aligning = false;
  std::ranges::fill(targets, never);
  points  = std::move(aimed);
  pending = std::ranges::max(points);
  set_global();
}

void system::impl::complete_clock() noexcept
{
  seal();
  account();
  woke.store(false);
  decide.store(false, std::memory_order_release);
  merge_found();
  ++result.windows;
  chosen = decision{};
  if(auto const head = best_entry())
  {
    align(&*head, std::nullopt);
    return;
  }
  if(requested_at)
  {
    align(nullptr, requested_at);
    return;
  }
  if(pause_requested.exchange(false))
  {
    auto latest = sys_time{std::numeric_limits<std::int64_t>::min()};
    for(auto const& one : slots)
    {
      latest = std::max(latest, one.rep.reach);
    }
    requested_at = latest;
    align(nullptr, latest);
    return;
  }
  if(auto const idle = idle_entry())
  {
    set_pause(*idle);
    std::ranges::fill(points, idle->at);
    pending = idle->at;
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
  if(clocked())
  {
    return std::ranges::all_of(
        slots, [&head](slot const& one) { return frozen(one) or one.rep.reach >= head.at; });
  }
  return std::ranges::all_of(slots, [&head](slot const& one) {
    return not one.running() or one.rep.quiescent or one.rep.position >= head.at;
  });
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
  if(not chosen.idle)
  {
    for(auto const& one : slots)
    {
      at = earlier_of(at, one.rep.position);
    }
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

void system::impl::seal() noexcept
{
  for(auto& one : slots)
  {
    for(std::size_t index = 0; index < one.inbound.size(); ++index)
    {
      one.sealed[index] = one.inbound[index]->posted_count();
    }
  }
}

void system::impl::complete_a() noexcept
{
  seal();
  account();
  woke.store(false);
  merge_found();
  chosen = decision{.barrier = pending, .next = {pending.ps + q.ps}};
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
  seal();
  account();
  ++result.phase_b;
  merge_found();
  reconsider();
  settle_confirm();
}

void system::impl::complete_c() noexcept
{
  seal();
  account();
  merge_found();
  if(clocked())
  {
    ++result.windows;
    if(auto const head = best_entry();
       head and (not chosen.head or chosen.idle or earlier(*head, *chosen.head)))
    {
      chosen = decision{};
      align(&*head, std::nullopt);
      if(chosen.pause)
      {
        chosen   = decision{};
        targets  = points;
        aligning = true;
        stop_at.store(never.ps, std::memory_order_release);
      }
      return;
    }
  }
  else
  {
    reconsider();
  }
  if(chosen.idle and not std::ranges::all_of(slots, [](slot const& one) { return one.rep.quiet; }))
  {
    chosen.pause    = false;
    chosen.idle     = false;
    chosen.exchange = true;
    chosen.head.reset();
    if(not clocked())
    {
      ++window;
      pending = chosen.next;
      ++result.windows;
    }
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
    result.windows += one.waits;
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
    std::move_only_function<void(std::size_t)> before_sleep,
    sync_mode                                  mode)
    : impl_{std::in_place,           this,   std::move(members),
            std::move(links),        bounds, std::move(before_arrival),
            std::move(before_sleep), mode}
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
    one.waits         = 0;
    one.drained       = {};
    one.rep.quiescent = false;
    one.driver->restart();
  }
  if(self.clocked())
  {
    self.couple();
    for(std::size_t index = 0; index < self.slots.size(); ++index)
    {
      auto&      one       = self.slots[index];
      auto const held_host = one.ref.on->lock();
      auto const at = one.map.to_system(one.ref.on->member(held_host, 0).virtual_clock().time());
      self.syncs[index].clock.store(at.ps, std::memory_order_relaxed);
    }
    self.decide.store(false);
    self.leading.store(0);
    self.aligning = false;
    self.requested_at.reset();
    std::ranges::fill(self.targets, never);
    self.set_global();
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
            "a state of format {}, this one has format {}", saved_system{}.format, state.format));
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
  if(impl_->clocked())
  {
    impl_->decide.store(true, std::memory_order_release);
    impl_->bump_all();
  }
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
  if(impl_->clocked())
  {
    impl_->bump_all();
  }
  impl_->wake_all();
}

auto system::mode() const noexcept -> sync_mode
{
  return impl_->mode;
}

void system::set_cut_owner(held_system const&, std::size_t link, std::size_t member)
{
  auto& self = *impl_;
  if(self.in_flight())
  {
    throw std::logic_error("the system is running");
  }
  if(link >= self.links.size())
  {
    throw std::out_of_range(std::format("no link {}", link));
  }
  self.owners[link].store(member, std::memory_order_relaxed);
}

auto system::cut_owner(std::size_t link) const -> std::size_t
{
  if(link >= impl_->links.size())
  {
    throw std::out_of_range(std::format("no link {}", link));
  }
  return impl_->owners[link].load(std::memory_order_relaxed);
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
