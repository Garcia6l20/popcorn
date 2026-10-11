#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/system.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace popcorn::core
{
namespace
{
enum class entry_kind : std::uint8_t
{
  stopped,
  error,
  system_time,
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

auto earlier_of(sys_time a, sys_time b) noexcept -> sys_time
{
  return b < a ? b : a;
}

enum class phase : std::uint8_t
{
  begin,
  run,
  halted,
  arrive_b,
  after_b,
};

struct report
{
  sys_time           position{};
  bool               done{};
  bool               quiescent{};
  bool               has_work{};
  std::vector<entry> found{};
};

struct decision
{
  bool                 pause{};
  bool                 need_b{};
  sys_time             target{};
  std::optional<entry> head{};
  sys_time             confirm{};
  std::uint64_t        window{};
  sys_time             barrier{};
  sys_time             next{};
};
}

struct system::impl
{
  class runner;

  struct slot
  {
    member_ref                    ref;
    machine*                      target{};
    std::shared_ptr<impl::runner> driver{};
    time_map                      map{};
    std::uint64_t                 resets{};
    std::vector<member_link*>     outbound{};
    std::vector<member_link*>     inbound{};
    bool                          runs_core{};
    bool                          released{};
    std::optional<sys_time>       held_to{};
    bool                          resolved{};
    bool                          failed{};
    bool                          warned_reset{};
    report                        rep{};
    std::uint64_t                 slices{};
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
      system const*                             owner,
      std::vector<member_ref>                   members,
      std::vector<std::shared_ptr<member_link>> wires,
      wait_policy                               wait,
      slice_bounds                              slicing,
      std::function<void(std::size_t)>          hook);

  [[nodiscard]] auto deliverable(entry const& head) const noexcept -> bool;
  [[nodiscard]] auto best_entry() const -> std::optional<entry>;
  void               merge_found() noexcept;
  void               set_pause(entry const& head) noexcept;
  void               settle_confirm() noexcept;
  void               complete_a() noexcept;
  void               complete_b() noexcept;
  void               complete_c() noexcept;
  void               finish_pause() noexcept;
  void               reset_member(slot& one, machine& target, reset_kind kind);
  [[nodiscard]] auto in_flight() const noexcept -> bool;

  system const*                             owner;
  std::vector<slot>                         slots;
  std::vector<std::shared_ptr<member_link>> links;
  sys_time                                  q{};
  window_barrier                            barrier;
  slice_bounds                              bounds;
  std::function<void(std::size_t)>          before_arrival;
  std::mutex                                mutex;
  std::uint64_t                             window{};
  sys_time                                  pending{};
  std::optional<sys_time>                   until{};
  std::vector<entry>                        held{};
  decision                                  chosen{};
  pause                                     result{};
  std::atomic<bool>                         pause_requested{false};
  std::atomic<bool>                         closed{false};
  bool                                      started{};
  bool                                      stopped{};
  std::exception_ptr                        stopped_by{};
  detail::host_promise<pause>               promise{};
  host_future<pause>                        future{};
  std::atomic<std::size_t>                  outstanding{0};
  std::atomic<bool>                         host_closed{false};
  completion                                on_a{this, &impl::complete_a};
  completion                                on_b{this, &impl::complete_b};
  completion                                on_c{this, &impl::complete_c};
};

class system::impl::runner final : public member_driver
{
public:
  runner(system::impl& owner, std::size_t index) : sys_{owner}, index_{index}
  {}

  void restart() noexcept
  {
    phase_ = phase::begin;
  }

  auto plan(machine& target, earliest_stamp) -> std::optional<slice_request> override
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
          if(target.core_held() == one.runs_core)
          {
            target.hold_core(not one.runs_core);
          }
          continue;
        case phase::run:
          if(auto request = running(target))
          {
            return request;
          }
          if(not arrive(target, sys_.on_a))
          {
            return std::nullopt;
          }
          phase_ = sys_.chosen.need_b ? phase::halted : phase::after_b;
          continue;
        case phase::halted:
          if(auto request = halted(target))
          {
            return request;
          }
          phase_ = phase::arrive_b;
          continue;
        case phase::arrive_b:
          if(not arrive(target, sys_.on_b))
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
          if(not arrive(target, sys_.on_c))
          {
            return std::nullopt;
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
      if(not one.runs_core or one.ref.on_reset == reset_request_policy::reset_and_continue)
      {
        if(not one.runs_core and not one.warned_reset)
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
    one.resolved = true;
    one.rep.found.push_back(
        {.at      = position(target),
         .member  = index_,
         .kind    = entry_kind::stopped,
         .stopped = stopped});
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

private:
  void fail(machine& target, std::exception_ptr error) noexcept
  {
    auto& one  = sys_.slots[index_];
    one.failed = true;
    one.rep.found.push_back(
        {.at = position(target), .member = index_, .kind = entry_kind::error, .error = error});
  }

  [[nodiscard]] auto position(machine const& target) const noexcept -> sys_time
  {
    return sys_.slots[index_].map.to_system(target.virtual_clock().time());
  }

  void enter_windows(std::uint64_t window)
  {
    auto& one = sys_.slots[index_];
    for(auto* link : one.outbound)
    {
      link->enter_window(window, one.map);
    }
  }

  auto request_to(sys_time bound, bool quiescent, std::uint64_t window) -> slice_request
  {
    auto& one = sys_.slots[index_];
    enter_windows(window);
    one.held_to = bound;
    return slice_request{
        .max_instructions      = sys_.bounds.max_instructions,
        .wall_budget           = sys_.bounds.wall_budget,
        .until_time            = one.map.to_local(bound),
        .hold_from_until       = true,
        .internal_stop         = true,
        .stop_quiescent        = quiescent,
        .stop_when_events_only = true,
    };
  }

  auto running(machine const& target) -> std::optional<slice_request>
  {
    auto& one = sys_.slots[index_];
    if(not one.runs_core or one.resolved or one.failed or one.rep.quiescent)
    {
      return std::nullopt;
    }
    auto bound = sys_.pending;
    if(sys_.until)
    {
      bound = earlier_of(bound, *sys_.until);
    }
    if(not sys_.held.empty())
    {
      bound = earlier_of(bound, sys_.held.front().at);
    }
    return toward(target, bound, false, sys_.window);
  }

  auto toward(machine const& target, sys_time bound, bool quiescent, std::uint64_t window)
      -> std::optional<slice_request>
  {
    auto const& one = sys_.slots[index_];
    if(position(target) < bound or (one.held_to and *one.held_to < bound))
    {
      return request_to(bound, quiescent and position(target) < bound, window);
    }
    return std::nullopt;
  }

  auto halted(machine const& target) -> std::optional<slice_request>
  {
    auto& one = sys_.slots[index_];
    if(one.runs_core or one.failed or one.rep.quiescent)
    {
      return std::nullopt;
    }
    return toward(target, sys_.chosen.target, not sys_.chosen.pause, sys_.chosen.window);
  }

  auto arrive(machine const& target, system::impl::completion const& complete) -> bool
  {
    auto& one        = sys_.slots[index_];
    one.rep.position = position(target);
    one.rep.done     = sys_.until and one.rep.position >= *sys_.until;
    auto const next  = target.next_event();
    one.rep.has_work = not one.runs_core
                   and next
                   and one.rep.position < sys_.pending
                   and *next <= target.virtual_clock().to_icount(one.map.to_local(sys_.pending));
    if(sys_.before_arrival)
    {
      sys_.before_arrival(index_);
    }
    return sys_.barrier.arrive_and_wait(complete);
  }

  void drain(std::uint64_t window, sys_time barrier, sys_time next)
  {
    auto& one = sys_.slots[index_];
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
  std::atomic<bool> aborted_{false};
};

system::impl::impl(
    system const*                             owner,
    std::vector<member_ref>                   members,
    std::vector<std::shared_ptr<member_link>> wires,
    wait_policy                               wait,
    slice_bounds                              slicing,
    std::function<void(std::size_t)>          hook)
    : owner{owner}, links{std::move(wires)}, barrier{members.size(), wait}, bounds{slicing},
      before_arrival{std::move(hook)}
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
    for(; linked < slots.size(); ++linked)
    {
      auto& one       = slots[linked];
      auto  held_host = one.ref.on->lock();
      one.ref.on->link(held_host, 0, true);
      one.target = &one.ref.on->member(held_host, 0);
      one.resets = one.target->resets();
    }
  }
  catch(...)
  {
    for(std::size_t index = 0; index < linked; ++index)
    {
      auto held_host = slots[index].ref.on->lock();
      slots[index].ref.on->link(held_host, 0, false);
    }
    throw;
  }
  for(std::size_t index = 0; index < slots.size(); ++index)
  {
    slots[index].driver = std::make_shared<runner>(*this, index);
    slots[index].rep.found.reserve(slots[index].inbound.size() + 2);
  }
}

auto system::impl::in_flight() const noexcept -> bool
{
  return future.valid() and not future.done();
}

auto system::impl::deliverable(entry const& head) const noexcept -> bool
{
  return std::ranges::all_of(slots, [&head](slot const& one) {
    return not one.runs_core
        or one.resolved
        or one.failed
        or one.rep.quiescent
        or one.rep.position >= head.at;
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
    auto const running = [](slot const& one) {
      return one.runs_core and not one.resolved and not one.failed;
    };
    if(std::ranges::none_of(slots, running))
    {
      entry const reached{.at = *until, .kind = entry_kind::system_time};
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

void system::impl::complete_a() noexcept
{
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
  else
  {
    chosen.target = pending;
  }
  chosen.need_b = std::ranges::any_of(slots, [this](slot const& one) {
    return not one.runs_core and not one.failed and (chosen.pause or one.rep.has_work);
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
  merge_found();
  if(auto const head = best_entry(); head and (not chosen.head or earlier(*head, *chosen.head)))
  {
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
  settle_confirm();
}

void system::impl::complete_c() noexcept
{
  merge_found();
  if(auto const head = best_entry(); head and (not chosen.head or earlier(*head, *chosen.head)))
  {
    chosen.head = *head;
  }
  result.slices = 0;
  for(auto const& one : slots)
  {
    result.slices += one.slices;
  }
  if(not chosen.head)
  {
    result.reason = pause_reason::requested;
    result.at     = chosen.target;
    return;
  }
  auto const head = *chosen.head;
  if(auto const found = std::ranges::find_if(
         held,
         [&head](entry const& one) { return one.at == head.at and one.member == head.member; });
     found != held.end() and head.kind != entry_kind::system_time)
  {
    held.erase(found);
  }
  result.member = head.member;
  result.at     = head.at;
  switch(head.kind)
  {
    case entry_kind::stopped:
      result.reason               = pause_reason::delivered;
      result.stopped              = head.stopped;
      slots[head.member].released = true;
      break;
    case entry_kind::error:
      result.reason = pause_reason::error;
      result.error  = head.error;
      stopped       = true;
      stopped_by    = head.error;
      break;
    case entry_kind::system_time:
      result.reason = pause_reason::system_time;
      break;
  }
}

void system::impl::finish_pause() noexcept
{
  auto const completing = promise;
  auto       ended      = result;
  if(closed.load() or host_closed.load())
  {
    ended.reason = pause_reason::closed;
    ended.member.reset();
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
  one.map     = time_map{at};
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
    std::vector<member_ref>                   members,
    std::vector<std::shared_ptr<member_link>> links,
    wait_policy                               wait,
    slice_bounds                              bounds,
    std::function<void(std::size_t)>          before_arrival)
    : impl_{std::in_place, this,   std::move(members),       std::move(links),
            wait,          bounds, std::move(before_arrival)}
{}

system::~system()
{
  close();
  if(impl_->future.valid())
  {
    std::ignore = impl_->future.wait_for(std::chrono::hours{24 * 365});
  }
  for(auto& one : impl_->slots)
  {
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
  if(impl_->in_flight())
  {
    throw std::logic_error("the system is running");
  }
  for(auto&& [one, offset] : std::views::zip(impl_->slots, offsets))
  {
    auto const held_host = one.ref.on->lock();
    auto const local     = one.ref.on->member(held_host, 0).virtual_clock().time();
    one.map              = time_map{sys_time{offset.ps - static_cast<std::int64_t>(local.ps)}};
    one.held_to          = offset;
  }
  impl_->started = true;
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
  if(self.in_flight())
  {
    throw std::logic_error("a resume is in flight");
  }
  if(self.stopped)
  {
    throw std::logic_error("the system stopped on an error");
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
    auto const index  = static_cast<std::size_t>(&one - self.slots.data());
    auto const queued = [&self, index](entry_kind kind) {
      return std::ranges::any_of(self.held, [index, kind](entry const& found) {
        return found.member == index and found.kind == kind;
      });
    };
    one.runs_core     = system_until.has_value() and not one.released;
    one.resolved      = queued(entry_kind::stopped);
    one.failed        = queued(entry_kind::error);
    one.slices        = 0;
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

void system::request_pause() noexcept
{
  impl_->pause_requested.store(true);
}

void system::reset(held_system const&, std::size_t member, reset_kind kind)
{
  auto& self = *impl_;
  if(self.in_flight())
  {
    throw std::logic_error("the system is running");
  }
  auto&      one       = self.slots[member];
  auto const held_host = one.ref.on->lock();
  self.reset_member(one, one.ref.on->member(held_host, 0), kind);
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
}

auto system::members() const noexcept -> std::size_t
{
  return impl_->slots.size();
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
