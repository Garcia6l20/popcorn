#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/trace.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

namespace popcorn::core
{
namespace
{
auto time_record(icount now) -> trace_record
{
  return {
      .kind = trace_kind::time,
      .payload =
          {static_cast<std::uint32_t>(now.value), static_cast<std::uint32_t>(now.value >> 32), 0},
  };
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
    out.push_back(time_record(last));
    rebased = false;
  }
  if(now < last or now.value - last.value > std::numeric_limits<std::uint32_t>::max())
  {
    out.push_back(time_record(now));
    last = now;
  }
  record.dt = static_cast<std::uint32_t>(now.value - last.value);
  out.push_back(record);
  last = now;
}

void machine::run_state::emit(trace_record const& record, address pc, bool ring)
{
  auto const now = owner.clock_.now();
  if(ring)
  {
    owner.trace_.append(record, now);
  }
  if(sinks.empty()) [[likely]]
  {
    return;
  }
  delivery_scope const scope{*owner.hooks_};
  for(auto const& [id, entry] : sinks)
  {
    if(not entry->kinds.contains(record.kind) or not in_ranges(entry->ranges, pc))
    {
      continue;
    }
    entry->stream.stamp(record, now, entry->batch);
    if(entry->batch.size() >= sink_batch)
    {
      if(in_engine)
      {
        guarded([&] {
          flush(*entry);
          return false;
        });
      }
      else
      {
        flush(*entry);
      }
    }
  }
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
      .flags   = mem_flags(size, write),
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
  auto const           batch = std::exchange(entry.batch, {});
  entry.batch.reserve(sink_batch);
  entry.sink(batch);
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
  for(auto const& [id, entry] : sinks)
  {
    entry->stream.last    = now;
    entry->stream.rebased = true;
  }
}

void machine::run_state::update_sink_hooks()
{
  sinks_want_block = std::ranges::any_of(
      sinks, [](auto const& held) { return held.second->kinds.contains(trace_kind::block); });
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
    auto const err  = uc_hook_add(
        engine, &hook, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
        reinterpret_cast<void*>(&on_sink_access), this, region->desc->start,
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
    flush(*found->second);
  }
  catch(...)
  {}
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
        for(auto const byte : std::span{region->host(at), bytes} | std::views::reverse)
        {
          seen = seen << 8 | std::to_integer<std::uint32_t>(byte);
        }
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
  auto const id    = ++hooks_->last_id;
  auto       entry = std::make_unique<run_state::sink_entry>(run_state::sink_entry{
      .kinds  = kinds,
      .sink   = std::move(sink),
      .ranges = std::move(ranges),
      .batch  = {},
      .stream = {.last = clock_.now(), .rebased = false},
  });
  entry->batch.reserve(run_state::sink_batch);
  run_->sinks.emplace(id, std::move(entry));
  try
  {
    run_->update_sink_hooks();
  }
  catch(...)
  {
    run_->sinks.erase(id);
    throw;
  }
  return hook_handle{hooks_, id};
}
}
