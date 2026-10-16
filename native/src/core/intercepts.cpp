#include "machine_impl.hpp"

#include "engine.hpp"
#include "handler_scope.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace popcorn::core
{
namespace
{
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

auto machine::run_state::site_at(address at) -> code_site&
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
  return *site;
}

void machine::run_state::add_to_site(
    address                                       at,
    std::variant<intercept_entry*, return_entry*> entry)
{
  site_at(at).entries.push_back(entry);
}

void machine::run_state::add_probe_to_site(address at, probe_entry entry)
{
  site_at(at).probes.push_back(std::move(entry));
}

auto machine::run_state::arrival_time(address addr) const -> vtime
{
  auto const now = owner.clock_driver_.exact_now();
  if(stepping)
  {
    return owner.clock_driver_.time_at(now);
  }
  auto const inside = addr - block_pc < max_insn_bytes * block_size;
  auto const before = inside ? arch->instructions_between(block_pc, addr) : std::nullopt;
  auto const ahead  = before and *before < block_size ? block_size - *before : 0;
  return owner.clock_driver_.time_at(icount{now.value - std::min<std::uint64_t>(ahead, now.value)});
}

void machine::run_state::remove_hook(std::uint64_t id) noexcept
{
  auto const drop_site_when_empty = [this](auto found) {
    auto const at = found->first;
    if(found->second->entries.empty() and found->second->probes.empty())
    {
      uc_hook_del(engine, found->second->hook);
      uc_ctl_remove_cache(engine, at, std::uint64_t{at} + 4);
      sites.erase(found);
      if(resume_past and resume_past->first == at)
      {
        resume_past.reset();
      }
    }
  };
  auto const drop_from_site = [this, &drop_site_when_empty](address at, auto* entry) {
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
       and resume_past->first == at
       and static_cast<std::size_t>(index - site.entries.begin()) < resume_past->second)
    {
      --resume_past->second;
    }
    site.entries.erase(index);
    drop_site_when_empty(found);
  };
  if(auto const found = probes.find(id); found != probes.end())
  {
    if(auto const site = sites.find(found->second); site != sites.end())
    {
      std::erase_if(site->second->probes, [id](probe_entry const& held) { return held.id == id; });
      drop_site_when_empty(site);
    }
    probes.erase(found);
    return;
  }
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
  rewatch();
}

void machine::run_state::on_return_block(address addr)
{
  complete_returns([addr](address ret) { return ret == addr; });
}

void machine::run_state::on_exception_return(std::uint32_t intno)
{
  complete_returns([this, intno](address ret) { return arch->interrupt_returns(intno, ret); });
}

void machine::run_state::complete_returns(std::function_ref<bool(address) const> returns)
{
  auto const sp       = stack_pointer();
  auto const floor    = call ? call->frames : std::size_t{0};
  auto const returned = [&] {
    return frames.size() > floor and frames.back().sp == sp and returns(frames.back().ret);
  };
  while(frames.size() > floor and frames.back().sp < sp)
  {
    frames.pop_back();
  }
  while(returned())
  {
    auto frame = std::move(frames.back());
    frames.pop_back();
    refresh_return_watch();
    auto&                  entry = *frame.entry;
    arch::call_frame const call{
        .entry = entry.at, .returns = entry.returns, .abi = abi_at(entry.at)};
    returned_call const done{
        .function     = entry.at,
        .occurrence   = entry.calls,
        .args         = std::move(frame.args),
        .value        = arch->call_result(call),
        .instructions = icount{owner.clock_driver_.exact_now().value - frame.entered.value},
    };
    ++entry.calls;
    try
    {
      handler_scope const scope{in_handler};
      entry.handler(done);
    }
    catch(...)
    {
      while(returned())
      {
        frames.pop_back();
      }
      refresh_return_watch();
      throw;
    }
  }
  refresh_return_watch();
}

void machine::run_state::on_code_site(
    uc_struct*    engine,
    std::uint64_t addr,
    std::uint32_t,
    void* user) noexcept
{
  auto& site = *static_cast<code_site*>(user);
  auto& run  = *site.run;
  if(run.stepping and run.step_ends(run.owner.clock_driver_.exact_now()))
  {
    return;
  }
  auto const        resumed = run.resume_past and run.resume_past->first == site.at;
  std::size_t const first   = resumed ? std::exchange(run.resume_past, std::nullopt)->second : 0;
  std::optional<std::uint32_t> exit;
  auto const                   stops = run.guarded([&] {
    if(not resumed and not site.probes.empty())
    {
      auto const at = run.arrival_time(static_cast<address>(addr));
      for(auto const& probe : site.probes)
      {
        probe.hits->record(at);
      }
    }
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
            .entered = run.owner.clock_driver_.exact_now(),
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
          if(run.coverage_on)
          {
            run.coverage.leave(*run.arch, static_cast<address>(addr), false);
          }
          run.arch->return_to_caller(result.value(), result.abi().value_or(run.abi_at(entry.at)));
          run.coverage.has_previous = false;
          exit                      = run.arch->return_interrupt();
          return false;
        case intercept_action::stop:
          run.trap        = arch::trap_result::stop(*result.reason());
          run.resume_past = std::pair{static_cast<address>(addr), index + 1};
          run.raise_stop();
          return true;
      }
    }
    return false;
  });
  if(stops)
  {
    run.end_cause = engine_end::other;
    uc_emu_stop(engine);
  }
  else if(exit)
  {
    on_interrupt(engine, *exit, &run);
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

auto machine::add_probe(address at) -> probe_handle
{
  require_stopped(*this);
  auto const id   = ++hooks_->last_id;
  auto       hits = std::make_shared<probe_hits>();
  run_->add_probe_to_site(at, {.id = id, .hits = hits});
  run_->probes.emplace(id, at);
  return {
      .hook = hook_handle{hooks_, id},
        .hits = std::move(hits)
  };
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

void machine::do_watch_reads(address at, std::uint32_t size, bool on)
{
  auto const* const region = mapped_region::find(memory_, at, size);
  if(region == nullptr or region->controller == nullptr)
  {
    throw config_error(
        description_.part, 0, std::format("watch_reads at {:#010x}, {} bytes", at, size),
        "a range inside one region with a controller model");
  }
  auto& run = *run_;
  if(run.controller_page_bytes == 0)
  {
    std::uint32_t page = 0;
    check(uc_ctl_get_page_size(engine_.get(), &page), "reading the engine page size");
    run.controller_page_bytes = page;
  }
  auto const* const buffer = region->buffer.get();
  auto const        first  = region->offset + (at - region->desc->start);
  auto const        key    = std::tuple{buffer, first, first + size - 1};
  if(on)
  {
    auto const [entry, added] = run.controller_ranges.emplace(key, region->controller);
    if(not added)
    {
      return;
    }
    try
    {
      run.sync_controller_runs(buffer);
    }
    catch(...)
    {
      run.controller_ranges.erase(entry);
      throw;
    }
    return;
  }
  auto const entry = run.controller_ranges.find(key);
  if(entry == run.controller_ranges.end())
  {
    return;
  }
  auto const controller = entry->second;
  run.controller_ranges.erase(entry);
  try
  {
    run.sync_controller_runs(buffer);
  }
  catch(...)
  {
    run.controller_ranges.emplace(key, controller);
    throw;
  }
}

auto machine::controller_watches() const noexcept -> std::size_t
{
  return run_->controller_ranges.size();
}

auto machine::controller_watch_hooks() const noexcept -> std::size_t
{
  return std::ranges::fold_left(
      run_->controller_pages, std::size_t{0},
      [](std::size_t sum, auto const& entry) { return sum + entry.second.hooks.size(); });
}

void machine::run_state::sync_controller_runs(host_buffer const* buffer)
{
  std::map<std::pair<host_buffer const*, std::size_t>, controller_run> runs;
  std::set<std::size_t>                                                pages;
  controller_run*                                                      open = nullptr;
  for(auto entry =
          controller_ranges.lower_bound(std::tuple{buffer, std::size_t{0}, std::size_t{0}});
      entry != controller_ranges.end() and std::get<0>(entry->first) == buffer; ++entry)
  {
    auto const [_, from, to] = entry->first;
    auto const lead          = from - std::min(from, controller_lead);
    if(open == nullptr or lead > open->last)
    {
      open =
          &runs.emplace(
                   std::pair{buffer, lead}, controller_run{.last = to, .controller = entry->second})
               .first->second;
    }
    open->last = std::max(open->last, to);
    open->ranges.emplace_back(from, to);
    open->reach.push_back(std::max(open->reach.empty() ? to : open->reach.back(), to));
    for(auto page = lead / controller_page_bytes; page <= to / controller_page_bytes; ++page)
    {
      pages.insert(page);
    }
  }
  std::vector<std::pair<std::size_t, controller_page>> fresh;
  try
  {
    for(auto const page : pages)
    {
      if(not controller_pages.contains({buffer, page}))
      {
        fresh.emplace_back(page, new_controller_page(*buffer, page));
      }
    }
  }
  catch(...)
  {
    for(auto& [page, made] : fresh)
    {
      drop_controller_hooks(made);
    }
    throw;
  }
  for(auto entry = controller_pages.lower_bound({buffer, std::size_t{0}});
      entry != controller_pages.end() and entry->first.first == buffer;)
  {
    if(pages.contains(entry->first.second))
    {
      ++entry;
      continue;
    }
    drop_controller_hooks(entry->second);
    entry = controller_pages.erase(entry);
  }
  for(auto& [page, made] : fresh)
  {
    controller_pages.emplace(std::pair{buffer, page}, std::move(made));
  }
  std::erase_if(
      controller_runs, [buffer](auto const& entry) { return entry.first.first == buffer; });
  controller_runs.merge(runs);
}

auto machine::run_state::new_controller_page(host_buffer const& buffer, std::size_t page)
    -> controller_page
{
  controller_page made;
  auto const      low  = page * controller_page_bytes;
  auto const      high = std::min(low + controller_page_bytes, buffer.size) - 1;
  try
  {
    for(auto const& mapping : buffer.mappings)
    {
      auto const from = std::max(low, mapping.offset);
      auto const to   = std::min(high, mapping.offset + mapping.size - 1);
      if(from > to)
      {
        continue;
      }
      auto const        guest  = std::uint64_t{mapping.start} + (from - mapping.offset);
      auto const* const region = mapped_region::find(owner.memory_, static_cast<address>(guest), 1);
      if(region == nullptr)
      {
        continue;
      }
      auto hook = std::make_unique<controller_hook>(controller_hook{
          .owner     = this,
          .buffer    = &buffer,
          .region    = region,
          .to_buffer = static_cast<std::int64_t>(from) - static_cast<std::int64_t>(guest),
          .serial    = controller_hook_serial + 1,
          .hook      = 0});
      check(
          uc_hook_add(
              engine, &hook->hook, UC_HOOK_MEM_READ, reinterpret_cast<void*>(&on_controller_read),
              hook.get(), guest, guest + (to - from)),
          std::format("adding the controller read hook at {:#010x}", guest));
      ++controller_hook_serial;
      made.hooks.push_back(std::move(hook));
    }
  }
  catch(...)
  {
    drop_controller_hooks(made);
    throw;
  }
  return made;
}

void machine::run_state::drop_controller_hooks(controller_page& page) noexcept
{
  for(auto const& hook : page.hooks)
  {
    uc_hook_del(engine, hook->hook);
  }
  page.hooks.clear();
}

void machine::run_state::arm_controller_guard(
    host_buffer const*   buffer,
    mapped_region const* region,
    std::size_t          first,
    std::uint64_t        before,
    std::uint64_t        addr,
    int                  size) noexcept
{
  auto const page = controller_pages.find({buffer, first / controller_page_bytes});
  if(page == controller_pages.end())
  {
    return;
  }
  auto const fresh = std::ranges::find_if(page->second.hooks, [&](auto const& hook) {
    return hook->region == region and hook->serial > before;
  });
  if(fresh != page->second.hooks.end())
  {
    controller_guard = {.addr = addr, .size = size, .serial = (*fresh)->serial, .armed = true};
  }
}

auto machine::run_state::controller_read_hit(
    host_buffer const* buffer,
    std::size_t        first,
    std::size_t        last) const noexcept -> periph::model*
{
  auto const after = controller_runs.upper_bound({buffer, first});
  if(after == controller_runs.begin())
  {
    return nullptr;
  }
  auto const& [key, run] = *std::prev(after);
  return key.first == buffer and first <= run.last and run.overlaps(first, last) ? run.controller
                                                                                 : nullptr;
}

auto machine::run_state::controller_block_hit(
    host_buffer const* buffer,
    std::size_t        first,
    std::size_t        last) const noexcept -> bool
{
  auto const next = controller_runs.lower_bound({buffer, first});
  if(next != controller_runs.end() and next->first.first == buffer and next->first.second <= last)
  {
    return true;
  }
  if(next == controller_runs.begin())
  {
    return false;
  }
  auto const& [key, run] = *std::prev(next);
  return key.first == buffer and run.last >= first;
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
