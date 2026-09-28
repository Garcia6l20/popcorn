#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <ranges>
#include <tuple>
#include <utility>

namespace popcorn::core
{
namespace
{
auto dispatch_key(event const& e) noexcept
{
  return std::tuple{e.at.value, e.seq};
}

auto instruction_of(std::uint64_t due_ps, clock const& time) -> icount
{
  if(due_ps <= time.time().ps)
  {
    return time.now();
  }
  return std::max(time.now(), time.to_icount(vtime{due_ps}));
}
}

clock::clock(std::uint64_t core_hz, double cpi)
{
  state_.core_hz = core_hz;
  state_.cpi     = cpi;
}

clock::~clock() = default;

void clock::start_epoch()
{
  state_.epoch_time   = time();
  state_.epoch_icount = state_.now;
}

void clock::set_core_hz(std::uint64_t hz)
{
  start_epoch();
  state_.core_hz = hz;
  if(on_epoch_)
  {
    on_epoch_();
  }
}

void clock::set_cpi(double cpi)
{
  start_epoch();
  state_.cpi = cpi;
  if(on_epoch_)
  {
    on_epoch_();
  }
}

auto clock::domain(std::string_view name) -> clock_domain
{
  auto const found = std::ranges::find(state_.domain_names, name);
  if(found != state_.domain_names.end())
  {
    return clock_domain{
        static_cast<std::uint32_t>(std::distance(state_.domain_names.begin(), found))};
  }
  state_.domain_names.emplace_back(name);
  state_.domain_hz.push_back(0);
  return clock_domain{static_cast<std::uint32_t>(state_.domain_names.size() - 1)};
}

void clock::set_hz(clock_domain domain, std::uint64_t hz)
{
  state_.domain_hz[domain.index] = hz;
}

void clock_driver::restore(clock_state const& saved)
{
  auto&      current = clock_.state_;
  auto const common  = std::min(current.domain_names.size(), saved.domain_names.size());
  contract_assert(
      std::ranges::equal(
          current.domain_names | std::views::take(common),
          saved.domain_names | std::views::take(common)));

  clock_state next = saved;
  for(std::size_t index = saved.domain_names.size(); index < current.domain_names.size(); ++index)
  {
    next.domain_names.push_back(current.domain_names[index]);
    next.domain_hz.push_back(0);
  }
  current = std::move(next);
}

void clock_driver::on_rate_change(std::function<void()> hook)
{
  clock_.on_epoch_ = std::move(hook);
}

scheduler::~scheduler() = default;

void scheduler::refresh_earliest() noexcept
{
  earliest_ =
      queue_.empty() ? icount{std::numeric_limits<std::uint64_t>::max()} : queue_.front().at;
}

auto scheduler::schedule(icount at, event_id id, std::uint64_t payload) -> std::uint64_t
{
  event const added{
      .at      = at,
      .seq     = next_seq_++,
      .id      = id,
      .payload = payload,
      .due_ps  = 0,
      .kind    = deadline::instructions};
  queue_.insert(std::ranges::upper_bound(queue_, dispatch_key(added), {}, dispatch_key), added);
  refresh_earliest();
  return added.seq;
}

auto scheduler::schedule_at(vtime due, event_id id, std::uint64_t payload, clock const& time)
    -> std::uint64_t
{
  event const added{
      .at      = instruction_of(due.ps, time),
      .seq     = next_seq_++,
      .id      = id,
      .payload = payload,
      .due_ps  = due.ps,
      .kind    = deadline::time};
  queue_.insert(std::ranges::upper_bound(queue_, dispatch_key(added), {}, dispatch_key), added);
  refresh_earliest();
  return added.seq;
}

void scheduler::rekey(clock const& time)
{
  for(auto& pending : queue_)
  {
    if(pending.kind == deadline::time)
    {
      pending.at = instruction_of(pending.due_ps, time);
    }
  }
  std::ranges::sort(queue_, {}, dispatch_key);
  refresh_earliest();
}

void scheduler::cancel(std::uint64_t seq)
{
  std::erase_if(queue_, [seq](event const& e) { return e.seq == seq; });
  refresh_earliest();
}

void scheduler::cancel(std::uint64_t seq, std::uint32_t owner)
{
  std::erase_if(
      queue_, [seq, owner](event const& e) { return e.seq == seq and e.id.owner == owner; });
  refresh_earliest();
}

auto scheduler::next_deadline() const noexcept -> std::optional<icount>
{
  if(queue_.empty())
  {
    return std::nullopt;
  }
  return earliest_;
}

auto scheduler::pop_due(icount now) -> std::optional<event>
{
  if(queue_.empty() or queue_.front().at.value > now.value)
  {
    return std::nullopt;
  }
  event const due = queue_.front();
  queue_.erase(queue_.begin());
  refresh_earliest();
  return due;
}

auto scheduler::pending() const -> std::span<event const>
{
  return queue_;
}

auto scheduler::next_seq() const noexcept -> std::uint64_t
{
  return next_seq_;
}

void scheduler::restore(std::span<event const> events, std::uint64_t next_seq)
{
  queue_.assign(events.begin(), events.end());
  std::ranges::sort(queue_, {}, dispatch_key);
  next_seq_ = next_seq;
  refresh_earliest();
}
}
