#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>

#include <algorithm>
#include <cmath>
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
  return std::tuple{e.at.value, e.lane, e.seq};
}

auto instruction_of(std::uint64_t due_ps, clock const& time) -> icount
{
  if(due_ps <= time.time().ps)
  {
    return time.now();
  }
  return std::max(time.now(), time.to_icount(vtime{due_ps}));
}

auto external_instruction_of(std::uint64_t due_ps, clock const& time) -> icount
{
  auto const from = vtime{std::max(due_ps, time.epoch_time().ps)};
  return from.ps <= time.time().ps ? std::min(time.now(), time.to_icount(from))
                                   : time.to_icount(from);
}
}

clock::clock(std::uint64_t core_hz, double cpi)
{
  state_.core_hz = core_hz;
  state_.cpi     = cpi;
  refresh_rate();
}

void clock::refresh_rate()
{
  instruction_hz_ =
      static_cast<std::uint64_t>(std::llround(static_cast<double>(state_.core_hz) / state_.cpi));
  instruction_div_ = instruction_hz_ != 0 ? divisor{instruction_hz_} : divisor{};
}

clock::~clock() = default;

void clock::start_epoch()
{
  auto const elapsed = state_.now.value - state_.epoch_icount.value;
  auto const ahead   = charged_ahead_ ? std::min(charged_ahead_(), elapsed) : 0;
  state_.epoch_time =
      vtime{state_.epoch_time.ps + vtime_from_instructions(elapsed - ahead, instruction_hz()).ps};
  state_.epoch_icount = icount{state_.now.value - ahead};
}

void clock::set_core_hz(std::uint64_t hz)
{
  start_epoch();
  state_.core_hz = hz;
  refresh_rate();
  if(on_epoch_)
  {
    on_epoch_();
  }
}

void clock::set_cpi(double cpi)
{
  start_epoch();
  state_.cpi = cpi;
  refresh_rate();
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
  clock_.refresh_rate();
}

void clock_driver::on_rate_change(std::move_only_function<void()> hook)
{
  clock_.on_epoch_ = std::move(hook);
}

void clock_driver::on_charged_ahead(std::move_only_function<std::uint64_t()> ahead)
{
  clock_.charged_ahead_ = std::move(ahead);
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

auto scheduler::schedule_at(
    vtime         due,
    event_id      id,
    std::uint64_t payload,
    std::uint32_t lane,
    clock const&  time) -> std::uint64_t
{
  event const added{
      .at      = external_instruction_of(due.ps, time),
      .seq     = lane_seq_base + next_lane_seq_++,
      .id      = id,
      .payload = payload,
      .due_ps  = due.ps,
      .kind    = deadline::time,
      .lane    = lane};
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
      pending.at = pending.lane == 0 ? instruction_of(pending.due_ps, time)
                                     : external_instruction_of(pending.due_ps, time);
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

auto scheduler::next_lane_seq() const noexcept -> std::uint64_t
{
  return next_lane_seq_;
}

void scheduler::restore(
    std::span<event const> events,
    std::uint64_t          next_seq,
    std::uint64_t          next_lane_seq)
{
  queue_.assign(events.begin(), events.end());
  std::ranges::sort(queue_, {}, dispatch_key);
  next_seq_      = next_seq;
  next_lane_seq_ = next_lane_seq;
  refresh_earliest();
}
}
