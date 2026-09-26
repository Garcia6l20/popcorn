#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>

namespace popcorn::core
{
namespace
{
class handler_scope
{
public:
  explicit handler_scope(bool& in_handler) noexcept
      : in_handler_{in_handler}, outer_{std::exchange(in_handler, true)}
  {}
  handler_scope(handler_scope const&)                    = delete;
  auto operator=(handler_scope const&) -> handler_scope& = delete;
  ~handler_scope()
  {
    in_handler_ = outer_;
  }

private:
  bool& in_handler_;
  bool  outer_;
};

constexpr auto hook_types(watch_kind kind) noexcept -> int
{
  switch(kind)
  {
    case watch_kind::read:
      return UC_HOOK_MEM_READ;
    case watch_kind::write:
      return UC_HOOK_MEM_WRITE;
    case watch_kind::access:
      return UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE;
  }
  return 0;
}

constexpr auto reports(watch_kind kind, bool write) noexcept -> bool
{
  switch(kind)
  {
    case watch_kind::read:
      return not write;
    case watch_kind::write:
      return write;
    case watch_kind::access:
      return true;
  }
  return false;
}

constexpr auto size_mask(std::uint32_t size) noexcept -> std::uint32_t
{
  return size >= 4 ? 0xffff'ffffu : (std::uint32_t{1} << (8 * size)) - 1;
}
}

void machine::run_state::add_to_site(
    address                                       at,
    std::variant<intercept_entry*, return_entry*> entry)
{
  auto& site = sites[at];
  if(not site)
  {
    site = std::make_unique<code_site>(code_site{.run = this, .at = at, .hook = 0, .entries = {}});
    auto const err = uc_hook_add(
        engine, &site->hook, UC_HOOK_CODE, reinterpret_cast<void*>(&on_code_site), site.get(), at,
        at);
    if(err != UC_ERR_OK)
    {
      sites.erase(at);
      check(err, std::format("adding the code hook at {:#010x}", at));
    }
    check(
        uc_ctl_remove_cache(engine, at, std::uint64_t{at} + 4),
        std::format("removing translated code at {:#010x}", at));
  }
  site->entries.push_back(entry);
}

void machine::run_state::remove_hook(std::uint64_t id) noexcept
{
  auto const drop_from_site = [this](address at, auto* entry) {
    auto const found = sites.find(at);
    if(found == sites.end())
    {
      return;
    }
    auto&      site  = *found->second;
    auto const index = std::ranges::find_if(site.entries, [entry](auto const& held) {
      auto const* const same = std::get_if<decltype(entry)>(&held);
      return same != nullptr and *same == entry;
    });
    if(index == site.entries.end())
    {
      return;
    }
    if(resume_past
       and resume_past->first
       == at
       and static_cast<std::size_t>(index - site.entries.begin())
       < resume_past->second)
    {
      --resume_past->second;
    }
    site.entries.erase(index);
    if(site.entries.empty())
    {
      uc_hook_del(engine, site.hook);
      uc_ctl_remove_cache(engine, at, std::uint64_t{at} + 4);
      sites.erase(found);
      if(resume_past and resume_past->first == at)
      {
        resume_past.reset();
      }
    }
  };
  if(sinks.contains(id))
  {
    remove_sink(id);
    return;
  }
  if(auto const found = intercepts.find(id); found != intercepts.end())
  {
    drop_from_site(found->second->at, found->second.get());
    intercepts.erase(found);
    return;
  }
  if(auto const found = return_hooks.find(id); found != return_hooks.end())
  {
    std::erase_if(frames, [entry = found->second.get()](return_frame const& frame) {
      return frame.entry == entry;
    });
    refresh_return_watch();
    drop_from_site(found->second->at, found->second.get());
    return_hooks.erase(found);
    return;
  }
  if(auto const found = watches.find(id); found != watches.end())
  {
    for(auto const hook : found->second->hooks)
    {
      uc_hook_del(engine, hook);
    }
    watches.erase(found);
    release_exact_pc();
  }
}

auto machine::run_state::abi_at(address at) const -> arch::float_abi
{
  for(auto const& loaded : owner.images_)
  {
    auto const holds = std::ranges::any_of(loaded.segments, [at](image::segment const& seg) {
      return seg.start <= at and at < std::uint64_t{seg.start} + seg.memory_size;
    });
    if(holds)
    {
      return loaded.abi;
    }
  }
  return arch::float_abi::soft;
}

auto machine::run_state::stack_pointer() const -> std::uint64_t
{
  auto const registers = arch->registers();
  auto const sp = std::ranges::find(registers, std::string_view{"sp"}, &arch::cpu_register::name);
  if(sp == registers.end())
  {
    throw emulation_error("the arch module lists no register named sp");
  }
  std::uint64_t value = 0;
  check(uc_reg_read(engine, sp->engine_id, &value), "reading sp");
  return sp->bits > 32 ? value : value & 0xffff'ffff;
}

void machine::run_state::trace_call(address at)
{
  auto const registers = arch->registers();
  auto const r0 = std::ranges::find(registers, std::string_view{"r0"}, &arch::cpu_register::name);
  std::uint64_t value = 0;
  if(r0 != registers.end())
  {
    check(uc_reg_read(engine, r0->engine_id, &value), "reading r0");
  }
  auto const symbol = symbol_at(at);
  emit(
      {
          .kind    = trace_kind::call,
          .aux     = symbol ? trace_id(symbol->first) : trace_id_overflow,
          .payload = {at, static_cast<std::uint32_t>(value), 0}
  },
      at, true);
}

void machine::run_state::refresh_return_watch() noexcept
{
  return_watch = frames.empty() ? address{} : frames.back().ret;
}

void machine::run_state::on_return_block(address addr)
{
  auto const sp    = stack_pointer();
  auto const floor = call ? call->frames : std::size_t{0};
  while(frames.size() > floor and frames.back().sp < sp)
  {
    frames.pop_back();
  }
  while(frames.size() > floor and frames.back().ret == addr and frames.back().sp == sp)
  {
    auto frame = std::move(frames.back());
    frames.pop_back();
    refresh_return_watch();
    auto&                  entry = *frame.entry;
    arch::call_frame const call{
        .entry = entry.at, .returns = entry.returns, .abi = abi_at(entry.at)};
    returned_call const returned{
        .function     = entry.at,
        .occurrence   = entry.calls,
        .args         = std::move(frame.args),
        .value        = arch->call_result(call),
        .instructions = icount{owner.clock_.now().value - frame.entered.value},
    };
    ++entry.calls;
    handler_scope const scope{in_handler};
    entry.handler(returned);
  }
  refresh_return_watch();
}

void machine::run_state::on_code_site(
    uc_struct*    engine,
    std::uint64_t addr,
    std::uint32_t,
    void* user) noexcept
{
  auto&       site  = *static_cast<code_site*>(user);
  auto&       run   = *site.run;
  std::size_t first = 0;
  if(run.resume_past and run.resume_past->first == site.at)
  {
    first = std::exchange(run.resume_past, std::nullopt)->second;
  }
  auto const stops = run.guarded([&] {
    for(auto index = first; index < site.entries.size(); ++index)
    {
      auto const held = site.entries[index];
      if(auto* const hook = std::get_if<return_entry*>(&held))
      {
        auto&            entry = **hook;
        arch::call_frame frame{.entry = entry.at, .abi = run.abi_at(entry.at)};
        for(auto const kind : entry.args)
        {
          frame.args.push_back({.kind = kind, .bits = 0});
        }
        run.arch->read_args(frame);
        run.frames.push_back({
            .entry   = &entry,
            .ret     = run.arch->return_address(),
            .sp      = run.stack_pointer(),
            .args    = frame.args
                     | std::views::transform(&arch::call_arg::bits)
                     | std::ranges::to<std::vector>(),
            .entered = run.owner.clock_.now(),
        });
        run.refresh_return_watch();
        continue;
      }
      auto& entry = *std::get<intercept_entry*>(held);
      if(entry.spent)
      {
        continue;
      }
      run.trace_call(entry.at);
      intercept_result result = intercept_result::proceed();
      {
        handler_scope const  scope{run.in_handler};
        intercept_call const call{run.owner, entry.at, run.arch->return_address(), entry.calls};
        ++entry.calls;
        entry.spent = entry.mode == intercept_mode::once;
        result      = entry.handler(call);
      }
      switch(result.action())
      {
        case intercept_action::proceed:
          continue;
        case intercept_action::returns:
          run.arch->return_to_caller(result.value(), result.abi().value_or(run.abi_at(entry.at)));
          run.coverage.has_previous = false;
          return false;
        case intercept_action::stop:
          run.trap        = arch::trap_result::stop(*result.reason());
          run.resume_past = std::pair{static_cast<address>(addr), index + 1};
          run.stop_flag   = true;
          return true;
      }
    }
    return false;
  });
  if(stops)
  {
    uc_emu_stop(engine);
  }
}

auto machine::run_state::watch_spans(address at, std::uint64_t last) const
    -> std::vector<std::pair<std::uint64_t, std::uint64_t>>
{
  std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
  auto const add = [&spans](std::uint64_t first, std::uint64_t final) {
    if(first <= final and std::ranges::find(spans, std::pair{first, final}) == spans.end())
    {
      spans.emplace_back(first, final);
    }
  };
  for(auto const& region : owner.memory_)
  {
    auto const start = std::uint64_t{region->desc->start};
    auto const first = std::max<std::uint64_t>(at, start);
    auto const final = std::min<std::uint64_t>(last, start + region->desc->size - 1);
    if(first > final)
    {
      continue;
    }
    auto const low  = region->offset + (first - start);
    auto const high = region->offset + (final - start);
    for(auto const& mapping : region->buffer->mappings)
    {
      auto const from = std::max<std::uint64_t>(low, mapping.offset);
      auto const to   = std::min<std::uint64_t>(high, mapping.offset + mapping.size - 1);
      if(from <= to)
      {
        add(mapping.start + (from - mapping.offset), mapping.start + (to - mapping.offset));
      }
    }
  }
  for(auto const& window : windows)
  {
    auto const first = std::max<std::uint64_t>(at, window->base);
    auto const final =
        std::min<std::uint64_t>(last, std::uint64_t{window->base} + window->size - 1);
    if(first > final)
    {
      continue;
    }
    for(auto const& other : windows)
    {
      if(other->slot != window->slot)
      {
        continue;
      }
      auto const high = std::min<std::uint64_t>(final - window->base, other->size - 1);
      add(other->base + (first - window->base), other->base + high);
    }
  }
  return spans;
}

void machine::run_state::hit_watch(watch_entry& watch, watch_hit const& hit)
{
  watch_action action = watch_action::resume;
  {
    handler_scope const scope{in_handler};
    action = watch.handler(hit);
  }
  if(action == watch_action::stop)
  {
    owner.do_request_stop(
        stop_reason::watchpoint,
        std::format(
            "{} of {:#x} {} {:#010x} by pc {:#010x}, {}", hit.write ? "write" : "read", hit.value,
            hit.write ? "to" : "from", hit.addr, hit.pc, hit.by));
  }
}

void machine::run_state::note_watched(
    address       addr,
    std::uint8_t  size,
    std::uint32_t value,
    bool          write,
    initiator     by)
{
  if(watches.empty() or by == initiator::debugger)
  {
    return;
  }
  std::vector<watch_entry*> hit;
  for(auto const& [id, watch] : watches)
  {
    if(watch->covers(addr, size) and reports(watch->kind, write))
    {
      hit.push_back(watch.get());
    }
  }
  for(auto* const watch : hit)
  {
    hit_watch(
        *watch, {.pc    = arch->pc(),
                 .addr  = addr,
                 .size  = size,
                 .value = value & size_mask(size),
                 .write = write,
                 .by    = by});
  }
}

void machine::run_state::on_watch_access(
    uc_struct*,
    uc_mem_type   type,
    std::uint64_t addr,
    int           size,
    std::int64_t  value,
    void*         user) noexcept
{
  auto& watch = *static_cast<watch_entry*>(user);
  auto& run   = *watch.run;
  run.guarded([&] {
    auto const at    = static_cast<address>(addr);
    auto const bytes = static_cast<std::uint8_t>(size);
    if(not watch.covers(at, bytes))
    {
      return false;
    }
    auto const write = type == UC_MEM_WRITE;
    auto       seen  = static_cast<std::uint32_t>(value);
    if(not write)
    {
      auto const* const region = mapped_region::find(run.owner.memory_, at, bytes);
      seen                     = 0;
      if(region != nullptr)
      {
        for(auto const byte : std::span{region->host(at), bytes} | std::views::reverse)
        {
          seen = seen << 8 | std::to_integer<std::uint32_t>(byte);
        }
      }
    }
    run.hit_watch(
        watch, {.pc    = run.arch->pc(),
                .addr  = at,
                .size  = bytes,
                .value = seen & size_mask(bytes),
                .write = write,
                .by    = initiator::cpu});
    return false;
  });
}

auto machine::add_intercept(address at, intercept_handler handler, intercept_mode mode)
    -> intercept_handle
{
  require_stopped(*this);
  auto const id    = ++hooks_->last_id;
  auto       entry = std::make_unique<run_state::intercept_entry>(run_state::intercept_entry{
      .id = id, .at = at, .handler = std::move(handler), .mode = mode, .spent = false, .calls = 0});
  run_->add_to_site(at, entry.get());
  run_->intercepts.emplace(id, std::move(entry));
  return hook_handle{hooks_, id};
}

auto machine::add_return_hook(
    address                       at,
    std::vector<arch::value_kind> args,
    arch::value_kind              returns,
    return_handler                handler) -> intercept_handle
{
  require_stopped(*this);
  auto const id    = ++hooks_->last_id;
  auto       entry = std::make_unique<run_state::return_entry>(run_state::return_entry{
      .id      = id,
      .at      = at,
      .args    = std::move(args),
      .returns = returns,
      .handler = std::move(handler),
      .calls   = 0});
  run_->add_to_site(at, entry.get());
  run_->return_hooks.emplace(id, std::move(entry));
  return hook_handle{hooks_, id};
}

auto machine::add_watch(address at, std::uint32_t size, watch_kind kind, watch_handler handler)
    -> watch_handle
{
  require_stopped(*this);
  auto const id    = ++hooks_->last_id;
  auto       entry = std::make_unique<run_state::watch_entry>(run_state::watch_entry{
      .id      = id,
      .run     = run_.get(),
      .at      = at,
      .size    = size,
      .kind    = kind,
      .handler = std::move(handler),
      .hooks   = {}});
  entry->spans     = run_->watch_spans(at, std::uint64_t{at} + size - 1);
  if(entry->spans.empty())
  {
    throw config_error(
        description_.part, 0, std::format("watch at {:#010x}, {} bytes", at, size),
        "a range reaching memory or a peripheral window");
  }
  for(auto const& [first_byte, last_byte] : entry->spans)
  {
    auto const* const region = mapped_region::find(memory_, static_cast<address>(first_byte), 1);
    if(region == nullptr)
    {
      continue;
    }
    auto const lowest = std::max<std::uint64_t>(
        region->desc->start, first_byte - std::min<std::uint64_t>(first_byte, 3));
    uc_hook    hook = 0;
    auto const err  = uc_hook_add(
        engine_.get(), &hook, hook_types(kind),
        reinterpret_cast<void*>(&run_state::on_watch_access), entry.get(), lowest, last_byte);
    if(err != UC_ERR_OK)
    {
      for(auto const added : entry->hooks)
      {
        uc_hook_del(engine_.get(), added);
      }
      check(err, std::format("adding the watch hook at {:#010x}", first_byte));
    }
    entry->hooks.push_back(hook);
  }
  try
  {
    run_->acquire_exact_pc();
  }
  catch(...)
  {
    for(auto const added : entry->hooks)
    {
      uc_hook_del(engine_.get(), added);
    }
    throw;
  }
  run_->watches.emplace(id, std::move(entry));
  return hook_handle{hooks_, id};
}

auto intercept_call::args(
    std::span<arch::value_kind const> kinds,
    std::optional<arch::float_abi>    abi) const -> std::vector<std::uint64_t>
{
  auto&            run = *machine_->run_;
  arch::call_frame frame{.entry = function_, .abi = abi.value_or(run.abi_at(function_))};
  for(auto const kind : kinds)
  {
    frame.args.push_back({.kind = kind, .bits = 0});
  }
  run.arch->read_args(frame);
  return frame.args | std::views::transform(&arch::call_arg::bits) | std::ranges::to<std::vector>();
}
}
