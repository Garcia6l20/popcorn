#include "profile_sink.hpp"

#include "tb.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>

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

constexpr auto child_key(std::uint32_t parent, frame_kind kind, std::uint32_t key) noexcept
    -> std::uint64_t
{
  return std::uint64_t{parent} << 34 | std::uint64_t{std::to_underlying(kind)} << 32 | key;
}

constexpr auto shape_key(address pc, std::uint32_t instructions) noexcept -> std::uint64_t
{
  return std::uint64_t{pc} << 32 | instructions;
}

constexpr auto span_ps(std::uint64_t instructions, std::uint64_t hz) noexcept -> std::uint64_t
{
  return hz == 0 ? 0 : vtime_from_instructions(instructions, hz).ps;
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
    : options_{
          std::move(options)
},
      start_{start}, now_{start}, last_end_{start}, base_{start}, slots_(initial_slots),
      nodes_{profile_frame{.kind = frame_kind::root}},
      last_child_{{~std::uint64_t{0}, 0}}, tracking_{static_cast<bool>(options_.code)}
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
      time(
          icount{std::uint64_t{record.payload[0]} | std::uint64_t{record.payload[1]} << 32},
          (record.flags & core::trace_rebase_flag) != 0);
      continue;
    }
    now_.value += record.dt;
    elapsed_ += record.dt;
    if(record.kind == core::trace_kind::clock and record.aux != 0)
    {
      domain_clock(record);
      continue;
    }
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
      case core::trace_kind::sleep:
        sleep(record);
        break;
      case core::trace_kind::clock:
        clock(std::uint64_t{record.payload[0]} | std::uint64_t{record.payload[1]} << 32);
        break;
      case core::trace_kind::cut:
        cut_block(record);
        break;
      case core::trace_kind::loop:
        loop(record);
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

void profile_sink::time(icount to, bool rebase)
{
  if(not rebase)
  {
    elapsed_ = elapsed_ + to.value - now_.value;
    now_     = to;
    return;
  }
  close_clock_span();
  segments_ps_ += ps_at(now_);
  marks_.clear();
  now_  = to;
  base_ = to;
  cut();
}

void profile_sink::clock(std::uint64_t hz)
{
  close_clock_span();
  clock_elapsed_ = elapsed_;
  marks_.push_back({.at = now_, .ps = ps_at(now_), .hz = hz});
  clocks_.push_back({.at = now_, .segment = cuts_, .hz = hz});
}

void profile_sink::domain_clock(core::trace_record const& record)
{
  if(not options_.write_domain or record.aux != options_.write_domain->index + 1)
  {
    return;
  }
  domain_hz_ = std::uint64_t{record.payload[0]} | std::uint64_t{record.payload[1]} << 32;
  domain_rates_.push_back(
      {.at = now_, .segment = cuts_, .ps = profile_ps(now_), .hz = domain_hz_, .record = records_});
}

void profile_sink::close_clock_span()
{
  if(marks_.empty())
  {
    return;
  }
  auto& last        = clocks_.back();
  last.instructions = elapsed_ - clock_elapsed_;
  last.ps           = span_ps(last.instructions, last.hz);
}

auto profile_sink::ps_at(icount at) const -> std::uint64_t
{
  if(marks_.empty())
  {
    return 0;
  }
  auto const found = at >= marks_.back().at
                       ? marks_.end()
                       : std::ranges::upper_bound(marks_, at, {}, &rate_mark::at);
  if(found == marks_.begin())
  {
    return marks_.front().ps;
  }
  auto const& mark = *std::prev(found);
  return mark.ps + span_ps(at.value - mark.at.value, mark.hz);
}

auto profile_sink::ps_between(icount from, icount to) const -> std::uint64_t
{
  return to > from ? ps_at(to) - ps_at(from) : 0;
}

auto profile_sink::profile_ps(icount at) const -> std::uint64_t
{
  return segments_ps_ + ps_at(at);
}

void profile_sink::add_gap(profile_gaps& into, icount from, icount to) const
{
  auto const length = to.value - from.value;
  auto const ps     = ps_between(from, to);
  ++into.count;
  into.total += length;
  into.longest = std::max(into.longest, length);
  into.total_ps += ps;
  into.longest_ps = std::max(into.longest_ps, ps);
}

void profile_sink::sleep(core::trace_record const& record)
{
  auto const start =
      std::max(icount{now_.value - std::min<std::uint64_t>(now_.value, record.payload[0])}, base_);
  gap_before(start, other_gaps_);
  exit_at_.reset();
  auto const reason = static_cast<std::size_t>(record.flags & ~core::trace_sleep_partial & 0xff);
  if(reason < sleep_reasons_.size())
  {
    ++sleep_reasons_[reason];
  }
  if((record.flags & core::trace_sleep_partial) != 0)
  {
    ++partial_sleeps_;
  }
  add_gap(sleeps_, start, now_);
  if(record.aux != 0)
  {
    add_gap(exceptions_[record.aux].totals.wakes, start, now_);
  }
}

void profile_sink::loop(core::trace_record const& record)
{
  auto& entry = loops_[record.payload[0]];
  entry.head  = record.payload[0];
  entry.last  = std::max(entry.last, record.payload[0] + record.flags);
  entry.runs += 1;
  entry.passes += record.payload[1];
  auto const covered = std::min<std::uint64_t>(record.payload[2], now_.value - base_.value);
  entry.instructions += covered;
  entry.ps += ps_between(icount{now_.value - covered}, now_);
  if(auto const ender = std::size_t{record.aux} >> core::trace_loop_ender_shift;
     ender < entry.enders.size())
  {
    ++entry.enders[ender];
  }
}

void profile_sink::cut_block(core::trace_record const& record)
{
  auto const unrun = std::uint64_t{record.aux};
  auto const pc    = record.payload[0];
  auto const mask  = slots_.size() - 1;
  for(auto at = slot_of(pc, mask); slots_[at].used; at = (at + 1) & mask)
  {
    if(slots_[at].pc == pc)
    {
      slots_[at].instructions -= std::min(slots_[at].instructions, unrun);
      break;
    }
  }
  instructions_ -= std::min(instructions_, unrun);
  if(tracking_ and pending_.start == pc)
  {
    pending_ = {};
  }
  if(tracking_ and walked_ == pc)
  {
    auto& self = nodes_[current_].self;
    self -= std::min(self, unrun);
  }
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
  clear_stack();
  contexts_.clear();
  for(auto& state : exceptions_ | std::views::values)
  {
    state.window.clear();
  }
  for(auto& state : cpu_registers_ | std::views::values)
  {
    state.last_written.reset();
  }
  for(auto& state : dma_registers_ | std::views::values)
  {
    state.last_written.reset();
  }
  for(auto& state : debugger_registers_ | std::views::values)
  {
    state.last_written.reset();
  }
}

void profile_sink::gap_before(icount start, profile_gaps& into)
{
  if(start > last_end_)
  {
    add_gap(into, last_end_, start);
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
  auto const at = count_block(record.payload[0], instructions);
  if(tracking_)
  {
    track(at, instructions);
  }
}

auto profile_sink::count_block(address pc, std::uint32_t instructions) -> std::size_t
{
  for(;;)
  {
    auto const mask = slots_.size() - 1;
    for(auto at = slot_of(pc, mask);; at = (at + 1) & mask)
    {
      auto& slot = slots_[at];
      if(slot.used and slot.pc == pc)
      {
        ++slot.entries;
        slot.instructions += instructions;
        return at;
      }
      if(not slot.used)
      {
        if((used_slots_ + 1) * 2 > slots_.size())
        {
          break;
        }
        ++used_slots_;
        slot = {
            .pc           = pc,
            .used         = true,
            .entries      = 1,
            .instructions = instructions,
            .first_ps =
                profile_ps(icount{now_.value - std::min<std::uint64_t>(now_.value, instructions)})};
        return at;
      }
    }
    grow();
  }
}

void profile_sink::grow()
{
  auto old        = std::exchange(slots_, std::vector<block_slot>(slots_.size() * 2));
  auto old_shapes = std::exchange(shaped_, {});
  if(not old_shapes.empty())
  {
    shaped_.resize(slots_.size());
  }
  auto const mask = slots_.size() - 1;
  for(auto const [index, slot] : std::views::enumerate(old))
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
    if(not old_shapes.empty())
    {
      shaped_[at] = old_shapes[static_cast<std::size_t>(index)];
    }
  }
}

void profile_sink::track(std::size_t at, std::uint32_t instructions)
{
  walk(instructions, shape_of(at, instructions), slots_[at].pc);
}

auto profile_sink::shape_of(std::size_t at, std::uint32_t instructions) -> block_shape
{
  if(shaped_.empty())
  {
    shaped_.resize(slots_.size());
  }
  auto& held = shaped_[at];
  if(held.known and held.instructions == instructions)
  {
    return held.shape;
  }
  auto const pc       = slots_[at].pc;
  auto [found, added] = shapes_.try_emplace(shape_key(pc, instructions));
  if(added)
  {
    found->second = options_.code(pc, instructions);
  }
  held = {.known = true, .instructions = instructions, .shape = found->second};
  return found->second;
}

auto profile_sink::child(std::uint32_t parent, frame_kind kind, std::uint32_t key) -> std::uint32_t
{
  auto const wanted = child_key(parent, kind, key);
  if(last_child_[parent].first == wanted)
  {
    return last_child_[parent].second;
  }
  auto const [found, added] =
      children_.try_emplace(wanted, static_cast<std::uint32_t>(nodes_.size()));
  if(added)
  {
    nodes_.push_back({.parent = parent, .kind = kind, .key = key});
    last_child_.emplace_back(~std::uint64_t{0}, 0);
  }
  last_child_[parent] = {wanted, found->second};
  return found->second;
}

void profile_sink::walk(std::uint32_t instructions, block_shape const& shape, address pc)
{
  auto const was  = std::exchange(pending_, {});
  auto const went = pc <= was.start or pc > was.next;
  if(was.exit == block_exit::call and went)
  {
    push_call(was.next, shape.function);
  }
  else if(was.exit == block_exit::ret and went)
  {
    pop_to(pc);
  }
  if(reroot_)
  {
    reroot_  = false;
    current_ = child(current_, frame_kind::entry, shape.function);
    ++nodes_[current_].entries;
  }
  nodes_[current_].self += instructions;
  walked_ = pc;
  if(shape.exit != block_exit::other)
  {
    pending_ = {.exit = shape.exit, .start = pc, .next = shape.next};
  }
}

void profile_sink::push_call(address next, address function)
{
  if(call_frames_ >= options_.max_call_depth)
  {
    ++deep_calls_;
    ++deep_open_;
    return;
  }
  ++call_frames_;
  ++calls_;
  stack_.push_back({.next = next, .node = current_});
  current_ = child(current_, frame_kind::call, function);
  ++nodes_[current_].entries;
}

void profile_sink::pop_to(address pc)
{
  if(deep_open_ > 0)
  {
    --deep_open_;
    return;
  }
  for(auto at = stack_.size(); at-- > 0;)
  {
    auto const& held = stack_[at];
    if(held.exception)
    {
      break;
    }
    if(held.next == pc)
    {
      current_ = held.node;
      call_frames_ -= static_cast<std::uint32_t>(stack_.size() - at);
      stack_.resize(at);
      return;
    }
  }
  ++unmatched_returns_;
  auto const marker =
      std::ranges::find(stack_ | std::views::reverse, true, &stack_entry::exception);
  auto const keep = static_cast<std::size_t>(std::ranges::distance(marker, stack_.rend()));
  call_frames_ -= static_cast<std::uint32_t>(stack_.size() - keep);
  stack_.resize(keep);
  current_   = keep == 0 ? 0 : stack_.back().inner;
  reroot_    = true;
  deep_open_ = 0;
}

void profile_sink::enter_frame(std::uint16_t number, address context)
{
  if(context != 0)
  {
    save_context(context);
  }
  stack_.push_back(
      {.node        = current_,
       .exception   = true,
       .held        = std::exchange(pending_, {}),
       .held_reroot = reroot_,
       .held_deep   = std::exchange(deep_open_, 0)});
  current_ = child(current_, frame_kind::exception, number);
  ++nodes_[current_].entries;
  stack_.back().inner = current_;
  reroot_             = true;
}

void profile_sink::exit_frame(address context)
{
  auto const marker =
      std::ranges::find(stack_ | std::views::reverse, true, &stack_entry::exception);
  if(marker == stack_.rend())
  {
    clear_stack();
  }
  else
  {
    auto const at   = static_cast<std::size_t>(std::ranges::distance(marker, stack_.rend())) - 1;
    auto const held = stack_[at];
    call_frames_ -= static_cast<std::uint32_t>(stack_.size() - at - 1);
    stack_.resize(at);
    current_   = held.node;
    pending_   = held.held;
    reroot_    = held.held_reroot;
    deep_open_ = held.held_deep;
  }
  if(context != 0)
  {
    restore_context(context);
  }
}

auto profile_sink::thread_part() const -> std::size_t
{
  auto const marker =
      std::ranges::find(stack_ | std::views::reverse, true, &stack_entry::exception);
  return static_cast<std::size_t>(std::ranges::distance(marker, stack_.rend()));
}

void profile_sink::save_context(address context)
{
  auto const keep  = thread_part();
  auto&      saved = contexts_[context];
  saved.frames.assign(stack_.begin() + static_cast<std::ptrdiff_t>(keep), stack_.end());
  saved.current = current_;
  saved.reroot  = reroot_;
  saved.held    = std::exchange(pending_, {});
  saved.deep    = std::exchange(deep_open_, 0);
  call_frames_ -= static_cast<std::uint32_t>(stack_.size() - keep);
  stack_.resize(keep);
  reroot_ = true;
}

void profile_sink::restore_context(address context)
{
  auto const keep = thread_part();
  call_frames_ -= static_cast<std::uint32_t>(stack_.size() - keep);
  stack_.resize(keep);
  auto const found = contexts_.find(context);
  if(found == contexts_.end())
  {
    current_   = keep == 0 ? 0 : stack_.back().inner;
    reroot_    = true;
    pending_   = {};
    deep_open_ = 0;
    return;
  }
  auto& saved = found->second;
  call_frames_ += static_cast<std::uint32_t>(saved.frames.size());
  stack_.append_range(saved.frames);
  current_   = saved.current;
  reroot_    = saved.reroot;
  pending_   = saved.held;
  deep_open_ = saved.deep;
  contexts_.erase(found);
}

void profile_sink::clear_stack()
{
  stack_.clear();
  call_frames_ = 0;
  current_     = 0;
  reroot_      = true;
  pending_     = {};
  deep_open_   = 0;
}

void profile_sink::irq(core::trace_record const& record)
{
  auto const number = record.aux;
  if(options_.code)
  {
    if(record.flags == 1)
    {
      enter_frame(number, record.payload[0]);
    }
    else
    {
      exit_frame(record.payload[0]);
    }
  }
  if(record.flags == 1)
  {
    auto& state = exceptions_[number];
    gap_before(now_, other_gaps_);
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
    if(options_.burst_window_ps != 0)
    {
      auto const at = ps_at(now_);
      while(not state.window.empty() and at - state.window.front() >= options_.burst_window_ps)
      {
        state.window.pop_front();
      }
      state.window.push_back(at);
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
  auto const inclusive    = now_.value - closed.entered.value;
  auto const inclusive_ps = ps_between(closed.entered, now_);
  auto&      totals       = exceptions_[number].totals;
  totals.inclusive += inclusive;
  totals.self += inclusive - closed.nested;
  totals.longest = std::max(totals.longest, inclusive);
  totals.inclusive_ps += inclusive_ps;
  totals.self_ps += inclusive_ps - closed.nested_ps;
  totals.longest_ps = std::max(totals.longest_ps, inclusive_ps);
  if(not frames_.empty())
  {
    frames_.back().nested += inclusive;
    frames_.back().nested_ps += inclusive_ps;
  }
}

void profile_sink::mmio(core::trace_record const& record)
{
  auto const dma      = (record.flags & core::trace_dma_flag) != 0;
  auto const debugger = (record.flags & core::trace_debugger_flag) != 0;
  auto const cpu      = not dma and not debugger;
  auto const write    = (record.flags & core::trace_write_flag) != 0;
  auto const value    = record.payload[2];
  auto&      table    = dma ? dma_registers_ : debugger ? debugger_registers_ : cpu_registers_;
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
      log.writes.push_back(
          {.at       = now_,
           .value    = value,
           .segment  = cuts_,
           .pc       = record.payload[0],
           .ps       = profile_ps(now_),
           .clock_hz = domain_hz_,
           .record   = records_});
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
      .start              = start_,
      .elapsed            = elapsed_,
      .records            = records_,
      .blocks             = blocks_,
      .instructions       = instructions_,
      .cuts               = cuts_,
      .unmatched_exits    = unmatched_exits_,
      .other_gaps         = other_gaps_,
      .sleeps             = sleeps_,
      .sleep_reasons      = sleep_reasons_,
      .partial_sleeps     = partial_sleeps_,
      .clocks             = clocks_,
      .cpu_registers      = sorted_registers(cpu_registers_),
      .dma_registers      = sorted_registers(dma_registers_),
      .debugger_registers = sorted_registers(debugger_registers_),
      .write_logs         = write_logs_,
      .domain_rates       = domain_rates_,
  };
  for(auto const& slot : slots_)
  {
    if(slot.used)
    {
      out.code.push_back(
          {.pc           = slot.pc,
           .entries      = slot.entries,
           .instructions = slot.instructions,
           .first_ps     = slot.first_ps});
    }
  }
  if(not marks_.empty())
  {
    auto& last        = out.clocks.back();
    last.instructions = elapsed_ - clock_elapsed_;
    last.ps           = span_ps(last.instructions, last.hz);
  }
  for(auto const& held : out.clocks)
  {
    out.elapsed_ps += held.ps;
  }
  std::ranges::sort(out.code, {}, &profile_block::pc);
  out.loops = loops_ | std::views::values | std::ranges::to<std::vector>();
  if(options_.code)
  {
    out.frames = nodes_;
    for(auto& node : out.frames)
    {
      node.inclusive = node.self;
    }
    for(auto at = out.frames.size(); at-- > 1;)
    {
      out.frames[out.frames[at].parent].inclusive += out.frames[at].inclusive;
    }
    out.calls             = calls_;
    out.unmatched_returns = unmatched_returns_;
    out.deep_calls        = deep_calls_;
  }
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

auto machine_code_reader(core::machine& target) -> code_reader
{
  std::vector<std::pair<address, address>> functions;
  for(auto const& held : target.observed_images())
  {
    for(auto const& symbol : held.symbols)
    {
      if(symbol.kind == image::symbol_kind::function and symbol.size != 0)
      {
        functions.emplace_back(symbol.value, symbol.value + symbol.size);
      }
    }
  }
  std::ranges::sort(functions);
  auto const* const analysis = arch::find_code_analysis(target.description().core);
  return [&target, analysis,
          functions = std::move(functions)](address pc, std::uint32_t instructions) -> block_shape {
    block_shape shape{.function = pc};
    auto const  after =
        std::ranges::upper_bound(functions, pc, {}, &std::pair<address, address>::first);
    if(after != functions.begin() and pc < std::prev(after)->second)
    {
      shape.function = std::prev(after)->first;
    }
    if(analysis == nullptr or instructions == 0)
    {
      return shape;
    }
    code_bytes code{
        target.loaded_images(),
        [&target](address at, std::span<std::byte> out) -> std::size_t {
          auto const read = target.mem().read_block(at, out);
          return read ? out.size() : read.error().at - at;
        },
        analysis};
    auto const                        bytes  = code.read(pc, instructions * 4);
    std::size_t                       offset = 0;
    std::optional<arch::decoded_insn> last;
    for(std::uint32_t index = 0; index < instructions; ++index)
    {
      last = analysis->decode(
          pc + static_cast<address>(offset), bytes.subspan(std::min(offset, bytes.size())));
      if(not last)
      {
        return shape;
      }
      offset += last->size;
    }
    shape.next = last->at + last->size;
    shape.exit = last->call ? block_exit::call : last->ret ? block_exit::ret : block_exit::other;
    return shape;
  };
}
}
