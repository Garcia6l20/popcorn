#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/trace.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <meta>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>

namespace popcorn::core
{
namespace
{
auto time_record(icount now, std::uint8_t flags = 0) -> trace_record
{
  return {
      .kind  = trace_kind::time,
      .flags = flags,
      .payload =
          {static_cast<std::uint32_t>(now.value), static_cast<std::uint32_t>(now.value >> 32), 0},
  };
}

auto clock_record(std::uint64_t hz) -> trace_record
{
  return {
      .kind    = trace_kind::clock,
      .payload = {static_cast<std::uint32_t>(hz), static_cast<std::uint32_t>(hz >> 32), 0},
  };
}

constexpr auto initiator_flags(initiator by) noexcept -> std::uint8_t
{
  switch(by)
  {
    case initiator::dma:
      return trace_dma_flag;
    case initiator::debugger:
      return trace_debugger_flag;
    default:
      return 0;
  }
}

auto in_ranges(std::span<address_range const> ranges, address pc) noexcept -> bool
{
  return ranges.empty() or std::ranges::any_of(ranges, [pc](address_range const& range) {
           return range.start <= pc and std::uint64_t{pc} < std::uint64_t{range.start} + range.size;
         });
}

class delivery_scope
{
public:
  explicit delivery_scope(hook_registry& hooks) noexcept : hooks_{hooks}
  {
    ++hooks_.delivering;
  }
  delivery_scope(delivery_scope const&)                    = delete;
  auto operator=(delivery_scope const&) -> delivery_scope& = delete;
  ~delivery_scope()
  {
    if(--hooks_.delivering == 0 and not hooks_.running)
    {
      hooks_.drain();
    }
  }

private:
  hook_registry& hooks_;
};

constexpr auto mem_flags(std::uint8_t size, bool write) noexcept -> std::uint8_t
{
  return static_cast<std::uint8_t>(size | (write ? trace_write_flag : 0));
}
}

void machine::run_state::dt_stream::stamp(
    trace_record               record,
    icount                     now,
    std::vector<trace_record>& out)
{
  if(rebased)
  {
    out.push_back(time_record(last, trace_rebase_flag));
    rebased = false;
  }
  if(now < last or now.value - last.value > std::numeric_limits<std::uint32_t>::max())
  {
    out.push_back(time_record(now));
    last = now;
  }
  auto const dt = static_cast<std::uint32_t>(now.value - last.value);
  out.push_back(record);
  out.back().dt = dt;
  last          = now;
}

inline void
    machine::run_state::deliver(trace_record const& record, address pc, bool ranged, icount now)
{
  auto const& wanted = sinks_of_kind[std::to_underlying(record.kind)];
  if(wanted.empty()) [[likely]]
  {
    return;
  }
  std::optional<delivery_scope> scope;
  for(auto* const entry : wanted)
  {
    if(ranged and not in_ranges(entry->ranges, pc))
    {
      continue;
    }
    entry->stream.stamp(record, now, entry->batch);
    if(entry->batch.size() >= sink_batch) [[unlikely]]
    {
      if(not scope)
      {
        scope.emplace(*owner.hooks_);
      }
      flush_full(*entry);
    }
  }
}

void machine::run_state::flush_full(sink_entry& entry)
{
  if(in_engine)
  {
    guarded([&] {
      flush(entry);
      return false;
    });
  }
  else
  {
    flush(entry);
  }
}

void machine::run_state::deliver_to(sink_entry& entry, trace_record const& record, icount now)
{
  entry.stream.stamp(record, now, entry.batch);
  if(entry.batch.size() >= sink_batch)
  {
    delivery_scope const scope{*owner.hooks_};
    flush_full(entry);
  }
}

auto machine::run_state::in_block_ranges(std::span<address_range const> ranges, address pc) noexcept
    -> bool
{
  return in_ranges(ranges, pc);
}

void machine::run_state::emit(trace_record const& record, address pc, bool ring)
{
  emit_at(record, pc, ring, owner.clock_.now());
}

void machine::run_state::emit_at(trace_record const& record, address pc, bool ring, icount now)
{
  if(ring)
  {
    owner.trace_.append(record, now);
  }
  deliver(record, pc, true, now);
}

void machine::run_state::trace_mmio(
    peripheral_slot const& slot,
    address                addr,
    std::uint32_t          offset,
    std::uint8_t           size,
    std::uint32_t          value,
    bool                   write,
    initiator              by)
{
  auto const cpu = by == initiator::cpu;
  if(not cpu and sinks.empty())
  {
    return;
  }
  auto const   exact = not in_engine or stepping or exact_pc.users() != 0;
  auto const   pc    = not cpu ? address{} : exact ? arch->pc() : block_pc;
  trace_record record{
      .kind    = trace_kind::mmio,
      .flags   = static_cast<std::uint8_t>(mem_flags(size, write) | initiator_flags(by)),
      .aux     = slot.trace_id_at(offset),
      .payload = {pc, addr, value},
  };
  emit(record, pc, cpu);
}

void machine::run_state::flush(sink_entry& entry)
{
  if(entry.batch.empty())
  {
    return;
  }
  delivery_scope const scope{*owner.hooks_};
  if(entry.delivering)
  {
    auto const batch = std::exchange(entry.batch, {});
    entry.batch.reserve(sink_batch);
    entry.sink(batch);
    return;
  }
  std::swap(entry.batch, entry.spare);
  entry.batch.clear();
  if(entry.batch.capacity() < sink_batch)
  {
    entry.batch.reserve(sink_batch);
  }
  entry.delivering = true;
  try
  {
    entry.sink(entry.spare);
  }
  catch(...)
  {
    entry.delivering = false;
    throw;
  }
  entry.delivering = false;
}

void machine::run_state::flush_sinks()
{
  delivery_scope const scope{*owner.hooks_};
  auto const           ids = sinks | std::views::keys | std::ranges::to<std::vector>();
  for(auto const id : ids)
  {
    if(auto const found = sinks.find(id); found != sinks.end())
    {
      flush(*found->second);
    }
  }
}

void machine::run_state::rebase_sinks(icount now) noexcept
{
  auto const hz = owner.clock_.instruction_hz();
  for(auto const& [id, entry] : sinks)
  {
    entry->stream.last    = now;
    entry->stream.rebased = true;
    try
    {
      entry->batch.push_back(time_record(now, trace_rebase_flag));
      entry->stream.rebased = false;
      if(entry->kinds.contains(trace_kind::clock))
      {
        entry->stream.stamp(clock_record(hz), now, entry->batch);
      }
    }
    catch(...)
    {}
  }
}

void machine::run_state::end_sleep(std::uint16_t woke_by)
{
  auto const slept = std::exchange(asleep_since, std::nullopt);
  if(not slept or not sinks_want_sleep)
  {
    return;
  }
  auto const now    = owner.clock_.now();
  auto const length = std::min<std::uint64_t>(
      now.value - std::min(now.value, slept->first.value),
      std::numeric_limits<std::uint32_t>::max());
  deliver(
      {
          .kind    = trace_kind::sleep,
          .flags   = slept->second,
          .aux     = woke_by,
          .payload = {static_cast<std::uint32_t>(length), 0, 0}
  },
      address{}, false, now);
}

void machine::run_state::emit_clock()
{
  if(sinks_want_clock)
  {
    deliver(
        clock_record(owner.clock_.instruction_hz()), address{}, false,
        owner.clock_.to_icount(owner.clock_.epoch_time()));
  }
}

void machine::run_state::leave_record(address pc)
{
  auto const insns = std::exchange(open_record_insns, 0u);
  auto const from  = open_record_pc;
  if(insns == 0 or not sinks_want_cut or pc - from > max_insn_bytes * insns)
  {
    return;
  }
  auto const ran = arch->instructions_between(from, pc);
  if(not ran or *ran >= insns)
  {
    return;
  }
  emit(
      {
          .kind    = trace_kind::cut,
          .aux     = static_cast<std::uint16_t>(insns - *ran),
          .payload = {from, 0, 0}
  },
      from, false);
}

void machine::run_state::forget_sink(sink_entry const* entry) noexcept
{
  for(auto& list : sinks_of_kind)
  {
    std::erase(list, entry);
  }
}

void machine::run_state::update_sink_hooks()
{
  auto const wanted = [this](trace_kind kind) {
    return std::ranges::any_of(
        sinks, [kind](auto const& held) { return held.second->kinds.contains(kind); });
  };
  for(auto& list : sinks_of_kind)
  {
    list.clear();
  }
  for(auto const& [id, entry] : sinks)
  {
    template for(constexpr auto e :
                 std::define_static_array(std::meta::enumerators_of(^^trace_kind)))
    {
      if(entry->kinds.contains([:e:]))
      {
        sinks_of_kind[std::to_underlying([:e:])].push_back(entry.get());
      }
    }
  }
  sinks_want_block    = wanted(trace_kind::block);
  sinks_want_sleep    = wanted(trace_kind::sleep);
  sinks_want_clock    = wanted(trace_kind::clock);
  sinks_want_cut      = wanted(trace_kind::cut);
  block_records       = sinks_want_block or ring_blocks;
  auto const want_mem = std::ranges::any_of(
      sinks, [](auto const& held) { return held.second->kinds.contains(trace_kind::mem); });
  if(want_mem == sink_mem_active)
  {
    return;
  }
  if(not want_mem)
  {
    for(auto const hook : std::exchange(sink_mem_hooks, {}))
    {
      uc_hook_del(engine, hook);
    }
    release_exact_pc();
    sink_mem_active = false;
    return;
  }
  acquire_exact_pc();
  for(auto const& region : owner.memory_)
  {
    uc_hook    hook = 0;
    auto const err  = add_engine_hook<&on_sink_access>(
        hook, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, *this, region->desc->start,
        std::uint64_t{region->desc->start} + region->desc->size - 1);
    if(err != UC_ERR_OK)
    {
      for(auto const added : std::exchange(sink_mem_hooks, {}))
      {
        uc_hook_del(engine, added);
      }
      release_exact_pc();
      check(err, std::format("adding the trace hook over {}", region->desc->name));
    }
    sink_mem_hooks.push_back(hook);
  }
  sink_mem_active = true;
}

void machine::run_state::remove_sink(std::uint64_t id) noexcept
{
  auto const found = sinks.find(id);
  if(found == sinks.end())
  {
    return;
  }
  try
  {
    auto& entry = *found->second;
    if(asleep_since and entry.kinds.contains(trace_kind::sleep))
    {
      auto const now = owner.clock_.now();
      entry.stream.stamp(
          {
              .kind  = trace_kind::sleep,
              .flags = static_cast<std::uint8_t>(asleep_since->second | trace_sleep_partial),
              .payload =
                  {static_cast<std::uint32_t>(std::min<std::uint64_t>(
                       now.value - std::min(now.value, asleep_since->first.value),
                            std::numeric_limits<std::uint32_t>::max())),
                            0, 0}
      },
          now, entry.batch);
    }
    flush(entry);
  }
  catch(...)
  {}
  forget_sink(found->second.get());
  sinks.erase(found);
  try
  {
    update_sink_hooks();
  }
  catch(...)
  {}
}

void machine::run_state::on_sink_access(
    uc_struct*,
    uc_mem_type   type,
    std::uint64_t addr,
    int           size,
    std::int64_t  value,
    void*         user) noexcept
{
  auto& run = *static_cast<run_state*>(user);
  run.guarded([&] {
    auto const at    = static_cast<address>(addr);
    auto const bytes = static_cast<std::uint8_t>(size);
    auto const write = type == UC_MEM_WRITE;
    auto       seen  = static_cast<std::uint32_t>(value);
    if(not write)
    {
      seen = 0;
      if(auto const* const region = mapped_region::find(run.owner.memory_, at, bytes))
      {
        seen = gather_le({region->host(at), bytes});
      }
    }
    auto const pc = run.arch->pc();
    run.emit(
        {
            .kind = trace_kind::mem, .flags = mem_flags(bytes, write), .payload = {pc, at, seen}
    },
        pc, false);
    return false;
  });
}

auto machine::add_trace_sink(trace_kinds kinds, trace_sink sink, std::vector<address_range> ranges)
    -> hook_handle
{
  require_stopped(*this);
  if(hooks_->delivering != 0)
  {
    throw machine_running("a trace sink is being called");
  }
  if(not sink)
  {
    throw std::invalid_argument("add_trace_sink needs a sink to call");
  }
  auto const id    = ++hooks_->last_id;
  auto       entry = std::make_unique<run_state::sink_entry>(run_state::sink_entry{
      .kinds  = kinds,
      .sink   = std::move(sink),
      .ranges = std::move(ranges),
      .batch  = {},
      .stream = {.last = clock_.now(), .rebased = false},
  });
  entry->batch.reserve(run_state::sink_batch);
  if(kinds.contains(trace_kind::clock))
  {
    entry->stream.stamp(clock_record(clock_.instruction_hz()), clock_.now(), entry->batch);
  }
  if(kinds.contains(trace_kind::sleep) and run_->asleep_since and arch_->idle())
  {
    run_->asleep_since->second = arch_->idle_reason();
  }
  run_->sinks.emplace(id, std::move(entry));
  try
  {
    run_->update_sink_hooks();
  }
  catch(...)
  {
    run_->forget_sink(run_->sinks.at(id).get());
    run_->sinks.erase(id);
    throw;
  }
  return hook_handle{hooks_, id};
}
}
