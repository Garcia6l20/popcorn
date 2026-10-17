#include "machine_impl.hpp"

#include "engine.hpp"
#include "handler_scope.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace popcorn::core
{
namespace
{
constexpr auto size_mask(std::uint32_t size) noexcept -> std::uint32_t
{
  return size >= 4 ? 0xffff'ffffu : (std::uint32_t{1} << (8 * size)) - 1;
}

constexpr auto cpu_attrs(uc_mem_attrs attrs) noexcept -> bus_attrs
{
  return {.by = initiator::cpu, .secure = attrs.secure, .privileged = attrs.privileged};
}

auto register_name(peripheral_desc const& unit, std::uint32_t word) -> std::string
{
  auto const found = std::ranges::find_if(unit.registers, [word](register_desc const& reg) {
    return reg.offset / 4 == word and not reg.alternate_of;
  });
  return found == unit.registers.end() ? std::string{} : found->name;
}

}

void machine::run_state::map_peripherals()
{
  std::uint32_t page = 0;
  check(uc_ctl_get_page_size(engine, &page), "reading the engine page size");
  check(uc_ctl_exit_after_insn_enable(engine), "enabling exits after an instruction");
  auto const& units = owner.description_.peripherals;
  for(auto const& [index, unit] : std::views::enumerate(units))
  {
    auto serving = static_cast<std::size_t>(index);
    if(unit.alias_of)
    {
      serving = static_cast<std::size_t>(
          std::ranges::find(units, *unit.alias_of, &peripheral_desc::name) - units.begin());
    }
    windows.push_back(
        std::make_unique<mmio_window>(mmio_window{
            .run          = this,
            .slot         = owner.peripherals_[serving].get(),
            .unit         = &unit,
            .base         = unit.base,
            .secure_alias = unit.alias_of.has_value(),
            .serving      = serving,
        }));
  }
  std::ranges::sort(windows, {}, [](auto const& window) { return window->base; });
  auto const refuse = [&](mmio_window const& window, std::string expected) {
    throw config_error(
        owner.description_.part, 0, std::format("peripherals.{}", window.unit->name),
        std::move(expected));
  };
  auto const end_of = [page](mmio_window const& window) {
    auto const end = std::uint64_t{window.base} + window.unit->size;
    return (end + page - 1) / page * page;
  };
  for(auto const [before, after] : windows | std::views::pairwise)
  {
    if(std::uint64_t{before->base} + before->unit->size > after->base)
    {
      refuse(*after, std::format("a window that does not overlap {}", before->unit->name));
    }
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  for(std::size_t first = 0; first < windows.size();)
  {
    auto end  = end_of(*windows[first]);
    auto last = first + 1;
    for(; last < windows.size() and windows[last]->base / page * page < end; ++last)
    {
      end = std::max(end, end_of(*windows[last]));
    }
    auto const group = std::span{windows}.subspan(first, last - first);
    auto&      lead  = *group.front();
    if(lead.base % page != 0)
    {
      refuse(lead, std::format("a window starting on an engine page of {} bytes", page));
    }
    for(auto const [index, window] : std::views::enumerate(group))
    {
      auto const next = static_cast<std::size_t>(index) + 1;
      auto const stop = next < group.size() ? std::uint64_t{group[next]->base} : end;
      window->size    = static_cast<std::uint32_t>(stop - window->base);
    }
    if(group.size() == 1)
    {
      check(
          uc_mmio_map_attrs(
              engine, lead.base, lead.size, &on_mmio_read, &lead, &on_mmio_write, &lead),
          std::format("mapping peripheral {}", lead.unit->name));
    }
    else
    {
      for(auto const& window : group)
      {
        if(window->base % 4 != 0)
        {
          refuse(
              *window,
              std::format(
                  "a word-aligned window, it shares an engine page with {}", lead.unit->name));
        }
      }
      auto shared = std::make_unique<mmio_page>(mmio_page{
          .run     = this,
          .base    = lead.base,
          .members = group
                   | std::views::transform([](auto const& window) { return window.get(); })
                   | std::ranges::to<std::vector>(),
      });
      check(
          uc_mmio_map_attrs(
              engine, lead.base, end - lead.base, &on_shared_read, shared.get(), &on_shared_write,
              shared.get()),
          std::format("mapping peripherals {} to {}", lead.unit->name, group.back()->unit->name));
      shared_pages.push_back(std::move(shared));
    }
    ranges.emplace_back(lead.base, end - 1);
    first = last;
  }
  for(auto const [before, after] : windows | std::views::pairwise)
  {
    contract_assert(std::uint64_t{before->base} + before->size <= after->base);
  }
  ranges.append_range(map_raz_wi(page));
  std::ranges::sort(ranges);
  if(not ranges.empty())
  {
    auto const first        = ranges.front().first;
    auto const last         = ranges.back().second;
    auto const holds_memory = std::ranges::any_of(owner.memory_, [&](auto const& region) {
      return region->desc->start <= last
         and first < std::uint64_t{region->desc->start} + region->desc->size;
    });
    if(not holds_memory)
    {
      ranges = {
          {first, last}
      };
    }
  }
  exact_pc.configure(engine, std::move(ranges));
  std::vector<std::pair<std::uint64_t, std::uint64_t>> clock_ranges;
  for(auto const& window : windows)
  {
    if(window->slot->model and window->slot->model->drives_core_clock())
    {
      clock_ranges.emplace_back(window->base, std::uint64_t{window->base} + window->size - 1);
    }
  }
  if(not clock_ranges.empty())
  {
    clock_pc.configure(engine, std::move(clock_ranges), UC_HOOK_MEM_WRITE);
    clock_pc.acquire();
  }
}

auto machine::run_state::map_raz_wi(std::uint32_t page)
    -> std::vector<std::pair<std::uint64_t, std::uint64_t>>
{
  auto const& options     = owner.options_;
  auto const& description = owner.description_;
  auto const  overlaps    = [](std::uint64_t start, std::uint64_t end, address other,
                               std::uint64_t size) { return start < other + size and other < end; };
  std::vector<std::pair<std::uint64_t, std::uint64_t>> mapped;
  for(auto const& [index, range] : std::views::enumerate(options.raz_wi))
  {
    auto const refuse = [&](std::string expected) {
      throw config_error(
          "machine_options", 0, std::format("raz_wi[{}]", index), std::move(expected));
    };
    auto const start = std::uint64_t{range.start};
    auto const end   = start + range.size;
    if(range.size == 0 or start % page != 0 or range.size % page != 0)
    {
      refuse(std::format("a non-empty range, start and size multiples of {} bytes", page));
    }
    for(auto const& region : description.regions)
    {
      if(overlaps(start, end, region.start, region.size))
      {
        refuse(std::format("a range outside region {}", region.name));
      }
    }
    for(auto const& unit : description.peripherals)
    {
      if(overlaps(start, end, unit.base, unit.size))
      {
        refuse(std::format("a range outside peripheral {}", unit.name));
      }
    }
    if(options.scratch and overlaps(start, end, options.scratch->start, options.scratch->size))
    {
      refuse("a range outside the scratch region");
    }
    for(auto const& [other, before] :
        std::views::enumerate(options.raz_wi | std::views::take(index)))
    {
      if(overlaps(start, end, before.start, before.size))
      {
        refuse(std::format("a range outside raz_wi[{}]", other));
      }
    }
    auto& window = *raz_windows.emplace_back(
        std::make_unique<raz_window>(raz_window{.run = this, .range = &range}));
    check(
        uc_mmio_map_attrs(
            engine, range.start, range.size, &on_raz_read, &window, &on_raz_write, &window),
        std::format("mapping the read-as-zero range {}", range.name));
    mapped.emplace_back(start, end - 1);
  }
  return mapped;
}

void machine::run_state::note_raz(
    raz_window&   window,
    std::uint32_t offset,
    bool          write,
    initiator     by)
{
  auto const strict = owner.options_.strict_peripherals;
  if(by == initiator::debugger and strict)
  {
    return;
  }
  auto const& name = window.range->name;
  if(not std::exchange(window.warned, true))
  {
    owner.hooks_->log(
        {.kind       = log_kind::model_warning,
         .pc         = arch->pc(),
         .peripheral = name,
         .reg        = {},
         .offset     = offset,
         .write      = write,
         .message    = std::format(
             "{} of {} at offset {:#x}, no model: it reads as zero and ignores writes",
             write ? "write" : "read", name, offset)});
  }
  if(strict)
  {
    throw model_error(name, "", offset);
  }
}

auto machine::run_state::raz_at(address addr, std::uint8_t size) const noexcept -> raz_window*
{
  auto const found = std::ranges::find_if(raz_windows, [&](auto const& window) {
    auto const start = std::uint64_t{window->range->start};
    return addr >= start and std::uint64_t{addr} + size <= start + window->range->size;
  });
  return found == raz_windows.end() ? nullptr : found->get();
}

auto machine::run_state::on_raz_read(
    uc_struct*,
    std::uint64_t offset,
    unsigned,
    uc_mem_attrs,
    void* user) noexcept -> std::uint64_t
{
  auto& window = *static_cast<raz_window*>(user);
  window.run->guarded([&] {
    window.run->note_raz(window, static_cast<std::uint32_t>(offset), false, initiator::cpu);
    return false;
  });
  return 0;
}

void machine::run_state::on_raz_write(
    uc_struct*,
    std::uint64_t offset,
    unsigned,
    std::uint64_t,
    uc_mem_attrs,
    void* user) noexcept
{
  auto& window = *static_cast<raz_window*>(user);
  window.run->guarded([&] {
    window.run->note_raz(window, static_cast<std::uint32_t>(offset), true, initiator::cpu);
    return false;
  });
}

auto machine::run_state::window_at(address addr, std::uint8_t size) const noexcept -> mmio_window*
{
  auto const after =
      std::ranges::upper_bound(windows, addr, {}, [](auto const& window) { return window->base; });
  if(after == windows.begin())
  {
    return nullptr;
  }
  auto* const window = std::prev(after)->get();
  return std::uint64_t{addr} + size <= std::uint64_t{window->base} + window->size ? window
                                                                                  : nullptr;
}

auto machine::run_state::seen(bus_attrs attrs) const noexcept -> bus_attrs
{
  attrs.secure = attrs.secure and owner.options_.trustzone;
  return attrs;
}

auto machine::run_state::context_of(bus_attrs attrs, bool via_secure_alias) const
    -> periph::access_ctx
{
  return {
      .pc               = arch->pc(),
      .now              = owner.clock_.now(),
      .by               = attrs.by,
      .secure           = attrs.secure,
      .privileged       = attrs.privileged,
      .via_secure_alias = via_secure_alias,
  };
}

void machine::run_state::install_firewall(periph::bus_firewall& installed)
{
  if(building == nullptr)
  {
    throw model_error({}, "firewall", 0);
  }
  if(firewall != nullptr)
  {
    throw model_error(building->name, "firewall", 0);
  }
  firewall = &installed;
}

void machine::do_install_firewall(periph::bus_firewall& firewall)
{
  run_->install_firewall(firewall);
}

void machine::do_memory_policy_changed()
{
  if(run_->memory_filter)
  {
    check(uc_ctl_flush_tlb(engine_.get()), "flushing the TLB after a memory policy change");
  }
}

void machine::run_state::install_memory_filter()
{
  if(firewall == nullptr or not firewall->filters_memory())
  {
    return;
  }
  std::uint32_t page = 0;
  check(uc_ctl_get_page_size(engine, &page), "reading the engine page size");
  filter_page_bytes = page;
  check(uc_ctl_bus_filter(engine, &on_bus_page, &on_bus_access, this), "installing the bus filter");
  memory_filter = true;
}

auto machine::run_state::check_memory(
    address         addr,
    std::uint8_t    size,
    bus_attrs       attrs,
    arch::memory_op op) -> periph::bus_verdict
{
  if(not filters_host(attrs.by))
  {
    return periph::bus_verdict::allow;
  }
  handler_scope const scope{in_handler};
  return firewall->check_memory(addr, size, seen(attrs), op);
}

auto machine::run_state::on_bus_page(
    uc_struct*,
    std::uint64_t page,
    std::uint64_t addr,
    uc_mem_attrs  attrs,
    uc_mem_type   type,
    void*         user) noexcept -> unsigned
{
  auto&    run  = *static_cast<run_state*>(user);
  unsigned bits = 0;
  run.guarded([&] {
    auto const           op = type == UC_MEM_FETCH ? arch::memory_op::fetch
                            : type == UC_MEM_WRITE ? arch::memory_op::write
                                                   : arch::memory_op::read;
    periph::page_verdict verdict;
    {
      handler_scope const scope{run.in_handler};
      verdict = run.firewall->memory_page(
          static_cast<address>(page), run.filter_page_bytes, static_cast<address>(addr),
          cpu_attrs(attrs), op);
    }
    bits = (verdict.filter_data ? unsigned{UC_BUS_FILTER_DATA} : 0u)
         | (verdict.deny_fetch ? unsigned{UC_BUS_DENY_FETCH} : 0u)
         | (verdict.fetch_each ? unsigned{UC_BUS_FETCH_EACH} : 0u);
    return false;
  });
  return bits;
}

auto machine::run_state::on_bus_access(
    uc_struct*,
    std::uint64_t  addr,
    unsigned       size,
    uc_mem_attrs   attrs,
    uc_mem_type    type,
    std::uint64_t* value,
    void*          user) noexcept -> int
{
  auto& run     = *static_cast<run_state*>(user);
  auto  verdict = periph::bus_verdict::allow;
  run.guarded([&] {
    auto const op = type == UC_MEM_WRITE ? arch::memory_op::write : arch::memory_op::read;
    verdict       = run.raising_access([&] {
      handler_scope const scope{run.in_handler};
      return run.firewall->check_memory(
          static_cast<address>(addr), static_cast<std::uint8_t>(size), cpu_attrs(attrs), op);
    });
    return false;
  });
  if(verdict == periph::bus_verdict::read_zero_write_ignore and type != UC_MEM_WRITE)
  {
    *value = 0;
    run.guarded([&] {
      run.note_watched(
          static_cast<address>(addr), static_cast<std::uint8_t>(size), 0, false, initiator::cpu);
      return false;
    });
  }
  switch(verdict)
  {
    case periph::bus_verdict::allow:
      return UC_BUS_ALLOW;
    case periph::bus_verdict::read_zero_write_ignore:
      return UC_BUS_RAZ_WI;
    case periph::bus_verdict::bus_error:
      return UC_BUS_FAULT;
  }
  return UC_BUS_ALLOW;
}

auto machine::run_state::dispatch_read(
    mmio_window const& window,
    std::uint32_t      offset,
    std::uint8_t       size,
    bus_attrs          attrs) -> std::expected<std::uint32_t, bus_error>
{
  attrs           = seen(attrs);
  auto const by   = attrs.by;
  auto&      slot = *window.slot;
  auto const word = offset / 4;
  if(firewall != nullptr) [[unlikely]]
  {
    auto verdict = periph::bus_verdict::allow;
    {
      handler_scope const scope{in_handler};
      verdict = firewall->check_peripheral(window.serving, attrs, arch::memory_op::read);
    }
    if(verdict == periph::bus_verdict::bus_error)
    {
      firewall_refused = by == initiator::cpu;
      return std::unexpected(bus_error::denied);
    }
    if(verdict == periph::bus_verdict::read_zero_write_ignore)
    {
      if(word < slot.layout.words())
      {
        trace_mmio(slot, window.base + offset, offset, size, 0, false, by);
      }
      note_watched(window.base + offset, size, 0, false, by);
      return 0u;
    }
  }
  if(word >= slot.layout.words())
  {
    warn_unregistered(window, offset, false, by);
    note_watched(window.base + offset, size, 0, false, by);
    return 0u;
  }
  if(slot.claimed[word] != 0)
  {
    auto const                              hz = owner.clock_.instruction_hz();
    std::expected<std::uint32_t, bus_error> value{};
    {
      handler_scope const scope{in_handler};
      value = slot.model->on_read(slot.regs, offset, size, context_of(attrs, window.secure_alias));
    }
    after_model(hz);
    if(value)
    {
      trace_mmio(slot, window.base + offset, offset, size, *value & size_mask(size), false, by);
      if(spin_on and by == initiator::cpu)
      {
        spin_read(slot, window.base + offset, offset, *value & size_mask(size));
      }
      note_watched(window.base + offset, size, *value, false, by);
    }
    return value.transform([size](std::uint32_t read) { return read & size_mask(size); });
  }
  auto const* const rules = slot.layout.find(word);
  if(rules == nullptr)
  {
    warn_unregistered(window, offset, false, by);
    note_watched(window.base + offset, size, 0, false, by);
    return 0u;
  }
  if(not slot.model and (slot.logged[word] == 0 or owner.options_.strict_peripherals))
  {
    note_unmodelled(window, offset, false, by);
  }
  auto const read = periph::layer_read(*rules, slot.regs, offset, size);
  trace_mmio(slot, window.base + offset, offset, size, read, false, by);
  if(spin_on and by == initiator::cpu)
  {
    spin_read(slot, window.base + offset, offset, read);
  }
  note_watched(window.base + offset, size, read, false, by);
  return read;
}

auto machine::run_state::dispatch_write(
    mmio_window const& window,
    std::uint32_t      offset,
    std::uint8_t       size,
    std::uint32_t      value,
    bus_attrs          attrs) -> std::expected<void, bus_error>
{
  attrs           = seen(attrs);
  auto const by   = attrs.by;
  auto&      slot = *window.slot;
  auto const word = offset / 4;
  if(firewall != nullptr) [[unlikely]]
  {
    auto verdict = periph::bus_verdict::allow;
    {
      handler_scope const scope{in_handler};
      verdict = firewall->check_peripheral(window.serving, attrs, arch::memory_op::write);
    }
    if(verdict == periph::bus_verdict::bus_error)
    {
      firewall_refused = by == initiator::cpu;
      return std::unexpected(bus_error::denied);
    }
    if(verdict == periph::bus_verdict::read_zero_write_ignore)
    {
      if(word < slot.layout.words())
      {
        trace_mmio(slot, window.base + offset, offset, size, value & size_mask(size), true, by);
      }
      note_watched(window.base + offset, size, value, true, by);
      return {};
    }
  }
  if(word >= slot.layout.words())
  {
    warn_unregistered(window, offset, true, by);
    note_watched(window.base + offset, size, value, true, by);
    return {};
  }
  if(slot.claimed[word] != 0)
  {
    auto const old = (slot.regs.peek(word * 4) >> (8 * (offset % 4))) & size_mask(size);
    auto const hz  = owner.clock_.instruction_hz();
    std::expected<void, bus_error> done{};
    {
      handler_scope const scope{in_handler};
      exact_write = by == initiator::cpu and slot.model->drives_core_clock();
      done        = slot.model->on_write(
          slot.regs, offset, size, old, value & size_mask(size),
          context_of(attrs, window.secure_alias));
      exact_write = false;
    }
    after_model(hz);
    if(done)
    {
      trace_mmio(slot, window.base + offset, offset, size, value & size_mask(size), true, by);
      note_watched(window.base + offset, size, value, true, by);
    }
    return done;
  }
  auto const* const rules = slot.layout.find(word);
  if(rules == nullptr)
  {
    warn_unregistered(window, offset, true, by);
    note_watched(window.base + offset, size, value, true, by);
    return {};
  }
  if(not slot.model and (slot.logged[word] == 0 or owner.options_.strict_peripherals))
  {
    note_unmodelled(window, offset, true, by);
  }
  periph::layer_write(slot.layout, *rules, slot.regs, slot.layer, offset, size, value);
  trace_mmio(slot, window.base + offset, offset, size, value & size_mask(size), true, by);
  note_watched(window.base + offset, size, value, true, by);
  return {};
}

auto machine::run_state::dispatch_controller_write(
    mapped_region const& region,
    address              addr,
    std::uint8_t         size,
    std::uint64_t        value,
    bus_attrs            attrs) -> std::expected<void, bus_error>
{
  attrs           = seen(attrs);
  auto const by   = attrs.by;
  auto const hz   = owner.clock_.instruction_hz();
  auto const word = [&](address at, std::uint8_t bytes, std::uint32_t data) {
    handler_scope const scope{in_handler};
    return region.controller->on_memory_write(
        *region.desc, at, bytes, data, context_of(attrs, region.desc->secure));
  };
  auto done = size == 8 ? word(addr, 4, static_cast<std::uint32_t>(value))
                        : word(addr, size, static_cast<std::uint32_t>(value) & size_mask(size));
  if(done and size == 8)
  {
    done = word(addr + 4, 4, static_cast<std::uint32_t>(value >> 32));
  }
  after_model(hz);
  if(done and by != initiator::cpu)
  {
    note_watched(addr, size, static_cast<std::uint32_t>(value), true, by);
  }
  return done;
}

void machine::run_state::note_controller_read(
    mapped_region const& region,
    address              addr,
    std::uint8_t         size,
    bus_attrs            attrs)
{
  auto const  first      = region.offset + (addr - region.desc->start);
  auto* const controller = controller_read_hit(region.buffer.get(), first, first + size - 1);
  if(controller == nullptr)
  {
    return;
  }
  auto const hz = owner.clock_.instruction_hz();
  {
    handler_scope const scope{in_handler};
    controller->on_memory_read(
        *region.desc, addr, size, context_of(seen(attrs), region.desc->secure));
  }
  after_model(hz);
}

void machine::run_state::on_controller_read(
    uc_struct*,
    uc_mem_type,
    std::uint64_t addr,
    int           size,
    std::int64_t,
    void* user) noexcept
{
  auto const& hook  = *static_cast<controller_hook const*>(user);
  auto&       run   = *hook.owner;
  auto&       guard = run.controller_guard;
  if(guard.armed)
  {
    guard.armed = false;
    if(hook.serial == guard.serial and guard.addr == addr and guard.size == size)
    {
      return;
    }
  }
  run.guarded([&] {
    auto const  first = static_cast<std::size_t>(static_cast<std::int64_t>(addr) + hook.to_buffer);
    auto const  last  = first + static_cast<std::size_t>(size) - 1;
    auto* const controller = run.controller_read_hit(hook.buffer, first, last);
    if(controller == nullptr)
    {
      return false;
    }
    auto const* const mapped = hook.region;
    auto const* const buffer = hook.buffer;
    auto const&       region = *mapped->desc;
    auto const        at     = static_cast<address>(addr);
    auto const        bytes  = static_cast<std::uint8_t>(size);
    return run.raising_access([&] {
      auto const hz     = run.owner.clock_.instruction_hz();
      auto const before = run.controller_hook_serial;
      {
        handler_scope const scope{run.in_handler};
        controller->on_memory_read(
            region, at, bytes,
            run.context_of(
                run.seen({.by = initiator::cpu, .secure = region.secure}), region.secure));
      }
      if(run.controller_hook_serial != before)
      {
        run.arm_controller_guard(buffer, mapped, first, before, addr, size);
      }
      run.after_model(hz);
      return false;
    });
  });
}

void machine::run_state::after_model(std::uint64_t instruction_hz)
{
  deadline = std::min(deadline, owner.scheduler_.earliest());
  if(owner.clock_.instruction_hz() != instruction_hz and in_engine)
  {
    raise_stop();
  }
}

auto machine::run_state::charged_ahead() const -> std::uint64_t
{
  if(not exact_write or stepping)
  {
    return 0;
  }
  auto const before = arch->instructions_between(block_pc, arch->pc());
  if(not before or *before >= block_size)
  {
    return 0;
  }
  return block_size - *before - 1;
}

void machine::run_state::request_precise_exit()
{
  check(uc_ctl_exit_after_insn(engine), "asking the engine to stop after the instruction");
  precise_exit = true;
}

void machine::run_state::take_precise_exit()
{
  std::uint64_t taken = 0;
  check(
      uc_ctl_exit_after_insn_taken(engine, &taken),
      "reading the instruction the engine stopped after");
  if(taken == 0)
  {
    return;
  }
  auto const at     = static_cast<address>(taken);
  auto const inside = at - block_pc < max_insn_bytes * block_size;
  auto const before = inside ? arch->instructions_between(block_pc, at) : std::nullopt;
  if(not before or *before + 1 >= block_size)
  {
    coverage.open_insns = 0;
    return;
  }
  if(not stepping)
  {
    owner.clock_driver_.retract(icount{block_size - *before - 1});
  }
}

template <typename Access>
auto machine::run_state::raising_access(Access access) -> decltype(access())
{
  auto result = access();
  if(not precise_exit and arch->pending() and arch->deliverable_now())
  {
    request_precise_exit();
  }
  return result;
}

void machine::run_state::cpu_bus_error(address addr, arch::memory_op op)
{
  if(std::exchange(firewall_refused, false))
  {
    check(uc_ctl_refuse_mmio(engine), "refusing the engine access");
    return;
  }
  record_bus_fault(addr, op);
}

void machine::run_state::record_bus_fault(address addr, arch::memory_op op) noexcept
{
  if(not bus_fault)
  {
    bus_fault = unmapped_access{.addr = addr, .op = op};
  }
  raise_stop();
}

void machine::run_state::note_unmodelled(
    mmio_window const& window,
    std::uint32_t      offset,
    bool               write,
    initiator          by)
{
  if(by == initiator::debugger and owner.options_.strict_peripherals)
  {
    return;
  }
  auto&      logged = window.slot->logged[offset / 4];
  auto const reg    = register_name(window.slot->desc, offset / 4);
  auto const name   = window.unit->name;
  if(logged == 0)
  {
    logged = 1;
    owner.hooks_->log(
        {.kind       = log_kind::unmodelled_access,
         .pc         = arch->pc(),
         .peripheral = name,
         .reg        = reg,
         .offset     = offset,
         .write      = write,
         .message    = std::format(
             "{} of {}.{} at offset {:#x}, no model", write ? "write" : "read", name, reg,
             offset)});
  }
  if(owner.options_.strict_peripherals)
  {
    throw model_error(name, reg, offset);
  }
}

void machine::run_state::warn_unregistered(
    mmio_window const& window,
    std::uint32_t      offset,
    bool               write,
    initiator)
{
  if(not window.slot->warned.insert(offset).second)
  {
    return;
  }
  owner.hooks_->log(
      {.kind       = log_kind::model_warning,
       .pc         = arch->pc(),
       .peripheral = window.unit->name,
       .reg        = {},
       .offset     = offset,
       .write      = write,
       .message    = std::format(
           "{} of {} at offset {:#x}, no register there", write ? "write" : "read",
           window.unit->name, offset)});
}

auto machine::run_state::on_mmio_read(
    uc_struct*,
    std::uint64_t offset,
    unsigned      size,
    uc_mem_attrs  attrs,
    void*         user) noexcept -> std::uint64_t
{
  auto const& window  = *static_cast<mmio_window const*>(user);
  auto&       run     = *window.run;
  run.access          = attrs;
  std::uint32_t value = 0;
  run.guarded([&] {
    auto const read = run.raising_access([&] {
      return run.dispatch_read(
          window, static_cast<std::uint32_t>(offset), static_cast<std::uint8_t>(size),
          cpu_attrs(attrs));
    });
    if(read)
    {
      value = *read;
    }
    else
    {
      run.cpu_bus_error(window.base + static_cast<address>(offset), arch::memory_op::read);
    }
    return false;
  });
  return value;
}

void machine::run_state::on_mmio_write(
    uc_struct*,
    std::uint64_t offset,
    unsigned      size,
    std::uint64_t value,
    uc_mem_attrs  attrs,
    void*         user) noexcept
{
  auto const& window = *static_cast<mmio_window const*>(user);
  auto&       run    = *window.run;
  run.access         = attrs;
  run.guarded([&] {
    auto const done = run.raising_access([&] {
      return run.dispatch_write(
          window, static_cast<std::uint32_t>(offset), static_cast<std::uint8_t>(size),
          static_cast<std::uint32_t>(value), cpu_attrs(attrs));
    });
    if(not done)
    {
      run.cpu_bus_error(window.base + static_cast<address>(offset), arch::memory_op::write);
    }
    return false;
  });
}

auto machine::run_state::mmio_page::member_at(address addr) const noexcept -> mmio_window&
{
  auto const after = std::ranges::upper_bound(
      members, addr, {}, [](mmio_window const* window) { return window->base; });
  return **std::prev(after);
}

auto machine::run_state::on_shared_read(
    uc_struct*,
    std::uint64_t offset,
    unsigned      size,
    uc_mem_attrs  attrs,
    void*         user) noexcept -> std::uint64_t
{
  auto const& shared   = *static_cast<mmio_page const*>(user);
  auto&       run      = *shared.run;
  run.access           = attrs;
  auto const    addr   = static_cast<address>(shared.base + offset);
  auto const&   window = shared.member_at(addr);
  std::uint32_t value  = 0;
  run.guarded([&] {
    auto const read = run.raising_access([&] {
      return run.dispatch_read(
          window, addr - window.base, static_cast<std::uint8_t>(size), cpu_attrs(attrs));
    });
    if(read)
    {
      value = *read;
    }
    else
    {
      run.cpu_bus_error(addr, arch::memory_op::read);
    }
    return false;
  });
  return value;
}

void machine::run_state::on_shared_write(
    uc_struct*,
    std::uint64_t offset,
    unsigned      size,
    std::uint64_t value,
    uc_mem_attrs  attrs,
    void*         user) noexcept
{
  auto const& shared = *static_cast<mmio_page const*>(user);
  auto&       run    = *shared.run;
  run.access         = attrs;
  auto const  addr   = static_cast<address>(shared.base + offset);
  auto const& window = shared.member_at(addr);
  run.guarded([&] {
    auto const done = run.raising_access([&] {
      return run.dispatch_write(
          window, addr - window.base, static_cast<std::uint8_t>(size),
          static_cast<std::uint32_t>(value), cpu_attrs(attrs));
    });
    if(not done)
    {
      run.cpu_bus_error(addr, arch::memory_op::write);
    }
    return false;
  });
}

auto machine::add_log_sink(log_sink sink) -> hook_handle
{
  require_stopped(*this);
  auto const id = ++hooks_->last_id;
  hooks_->log_sinks.emplace(id, std::move(sink));
  return hook_handle{hooks_, id};
}
}
