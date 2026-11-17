#include "profile_sink.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <ranges>
#include <utility>

namespace popcorn::obs
{
namespace
{
constexpr std::size_t initial_slots = 1024;

constexpr auto slot_of(address pc, std::size_t mask) noexcept -> std::size_t
{
  return static_cast<std::size_t>(
      (std::uint64_t{pc} * 0x9e37'79b9'7f4a'7c15u) >> std::countl_zero(std::uint64_t{mask}));
}

constexpr auto register_key(core::trace_record const& record) noexcept -> std::uint64_t
{
  return record.aux != core::trace_id_overflow ? record.aux
                                               : (std::uint64_t{1} << 32) | record.payload[1];
}

void add_gap(profile_gaps& into, std::uint64_t length) noexcept
{
  ++into.count;
  into.total += length;
  into.longest = std::max(into.longest, length);
}

auto sorted_registers(auto const& table) -> std::vector<profile_register>
{
  auto keys = table | std::views::keys | std::ranges::to<std::vector>();
  std::ranges::sort(keys);
  return keys
       | std::views::transform([&](std::uint64_t key) { return table.at(key).totals; })
       | std::ranges::to<std::vector>();
}
}

profile_sink::profile_sink(icount start, profile_options options)
    : options_{std::move(options)}, start_{start}, now_{start}, last_end_{start},
      slots_(initial_slots)
{
  for(auto const id : options_.logged_registers)
  {
    write_logs_.push_back({.id = id});
  }
}

profile_sink::~profile_sink() = default;

void profile_sink::feed(std::span<core::trace_record const> records)
{
  for(auto const& record : records)
  {
    if(record.kind == core::trace_kind::time)
    {
      time(icount{std::uint64_t{record.payload[0]} | std::uint64_t{record.payload[1]} << 32});
      continue;
    }
    now_.value += record.dt;
    elapsed_ += record.dt;
    ++records_;
    switch(record.kind)
    {
      case core::trace_kind::block:
        block(record);
        break;
      case core::trace_kind::irq:
        irq(record);
        break;
      case core::trace_kind::mmio:
        mmio(record);
        break;
      default:
        break;
    }
  }
}

auto profile_sink::sink() -> core::trace_sink
{
  return [this](std::span<core::trace_record const> records) { feed(records); };
}

void profile_sink::time(icount to)
{
  if(to > now_ and to.value - now_.value > std::numeric_limits<std::uint32_t>::max())
  {
    elapsed_ += to.value - now_.value;
    now_ = to;
    return;
  }
  now_ = to;
  cut();
}

void profile_sink::cut()
{
  ++cuts_;
  for(auto const& open : frames_)
  {
    ++exceptions_[open.number].totals.cut;
  }
  frames_.clear();
  last_end_ = now_;
  exit_at_.reset();
  for(auto& state : exceptions_ | std::views::values)
  {
    state.window.clear();
  }
  for(auto& state : cpu_registers_ | std::views::values)
  {
    state.last_written.reset();
  }
  for(auto& state : other_registers_ | std::views::values)
  {
    state.last_written.reset();
  }
}

void profile_sink::gap_before(icount start, profile_gaps& into)
{
  if(start > last_end_)
  {
    add_gap(into, start.value - last_end_.value);
  }
  last_end_ = std::max(last_end_, now_);
}

void profile_sink::block(core::trace_record const& record)
{
  auto const instructions = std::uint32_t{record.aux};
  gap_before(icount{now_.value - std::min<std::uint64_t>(now_.value, instructions)}, other_gaps_);
  exit_at_.reset();
  ++blocks_;
  instructions_ += instructions;
  count_block(record.payload[0], instructions);
}

void profile_sink::count_block(address pc, std::uint32_t instructions)
{
  auto const mask = slots_.size() - 1;
  for(auto at = slot_of(pc, mask);; at = (at + 1) & mask)
  {
    auto& slot = slots_[at];
    if(slot.used and slot.pc == pc)
    {
      ++slot.entries;
      slot.instructions += instructions;
      return;
    }
    if(not slot.used)
    {
      slot = {.pc = pc, .used = true, .entries = 1, .instructions = instructions};
      if(++used_slots_ * 2 > slots_.size())
      {
        grow();
      }
      return;
    }
  }
}

void profile_sink::grow()
{
  auto       old  = std::exchange(slots_, std::vector<block_slot>(slots_.size() * 2));
  auto const mask = slots_.size() - 1;
  for(auto const& slot : old)
  {
    if(not slot.used)
    {
      continue;
    }
    auto at = slot_of(slot.pc, mask);
    while(slots_[at].used)
    {
      at = (at + 1) & mask;
    }
    slots_[at] = slot;
  }
}

void profile_sink::irq(core::trace_record const& record)
{
  auto const number = record.aux;
  if(record.flags == 1)
  {
    auto& state = exceptions_[number];
    gap_before(now_, state.totals.wakes);
    auto& totals = state.totals;
    ++totals.entries;
    if(exit_at_ == now_)
    {
      ++totals.tail_chained;
    }
    exit_at_.reset();
    if(not frames_.empty())
    {
      ++exceptions_[frames_.back().number].totals.preempted_by[number];
    }
    frames_.push_back({.number = number, .entered = now_});
    totals.max_depth = std::max(totals.max_depth, static_cast<std::uint32_t>(frames_.size()));
    if(options_.burst_window.value != 0)
    {
      while(not state.window.empty()
            and now_.value - state.window.front().value >= options_.burst_window.value)
      {
        state.window.pop_front();
      }
      state.window.push_back(now_);
      totals.burst = std::max<std::uint64_t>(totals.burst, state.window.size());
    }
    return;
  }
  gap_before(now_, other_gaps_);
  exit_at_ = now_;
  if(frames_.empty() or frames_.back().number != number)
  {
    ++unmatched_exits_;
    return;
  }
  auto const closed = frames_.back();
  frames_.pop_back();
  auto const inclusive = now_.value - closed.entered.value;
  auto&      totals    = exceptions_[number].totals;
  totals.inclusive += inclusive;
  totals.self += inclusive - closed.nested;
  totals.longest = std::max(totals.longest, inclusive);
  if(not frames_.empty())
  {
    frames_.back().nested += inclusive;
  }
}

void profile_sink::mmio(core::trace_record const& record)
{
  auto const cpu      = record.payload[0] != 0;
  auto const write    = (record.flags & core::trace_write_flag) != 0;
  auto const value    = record.payload[2];
  auto&      table    = cpu ? cpu_registers_ : other_registers_;
  auto [found, added] = table.try_emplace(register_key(record));
  auto& state         = found->second;
  if(added)
  {
    state.totals.id            = record.aux;
    state.totals.first_address = record.payload[1];
  }
  state.totals.last_value = value;
  if(not write)
  {
    ++state.totals.reads;
    return;
  }
  ++state.totals.writes;
  if(state.last_written == value)
  {
    ++state.totals.same_writes;
  }
  state.last_written = value;
  if(not cpu or record.aux == core::trace_id_overflow)
  {
    return;
  }
  for(auto& log : write_logs_)
  {
    if(log.id != record.aux)
    {
      continue;
    }
    if(log.writes.size() < options_.write_log_limit)
    {
      log.writes.push_back({.at = now_, .value = value, .segment = cuts_});
    }
    else
    {
      ++log.dropped;
    }
  }
}

auto profile_sink::result() const -> profile_result
{
  profile_result out{
      .start           = start_,
      .elapsed         = elapsed_,
      .records         = records_,
      .blocks          = blocks_,
      .instructions    = instructions_,
      .cuts            = cuts_,
      .unmatched_exits = unmatched_exits_,
      .other_gaps      = other_gaps_,
      .cpu_registers   = sorted_registers(cpu_registers_),
      .other_registers = sorted_registers(other_registers_),
      .write_logs      = write_logs_,
  };
  for(auto const& slot : slots_)
  {
    if(slot.used)
    {
      out.code.push_back(
          {.pc = slot.pc, .entries = slot.entries, .instructions = slot.instructions});
    }
  }
  std::ranges::sort(out.code, {}, &profile_block::pc);
  for(auto const& [number, state] : exceptions_)
  {
    out.exceptions.emplace(number, state.totals);
  }
  for(auto const& open : frames_)
  {
    ++out.exceptions[open.number].open;
  }
  return out;
}
}
