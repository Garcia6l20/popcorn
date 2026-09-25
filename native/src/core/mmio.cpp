#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>

namespace popcorn::core
{
namespace
{
constexpr auto size_mask(std::uint32_t size) noexcept -> std::uint32_t
{
  return size >= 4 ? 0xffff'ffffu : (std::uint32_t{1} << (8 * size)) - 1;
}

auto register_name(peripheral_desc const& unit, std::uint32_t word) -> std::string
{
  auto const found = std::ranges::find_if(unit.registers, [word](register_desc const& reg) {
    return reg.offset / 4 == word and not reg.alternate_of;
  });
  return found == unit.registers.end() ? std::string{} : found->name;
}

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
}

void machine::run_state::map_peripherals()
{
  std::uint32_t page = 0;
  check(uc_ctl_get_page_size(engine, &page), "reading the engine page size");
  auto const& units = owner.description_.peripherals;
  for(auto const& [index, unit] : std::views::enumerate(units))
  {
    auto serving = static_cast<std::size_t>(index);
    if(unit.alias_of)
    {
      serving = static_cast<std::size_t>(
          std::ranges::find(units, *unit.alias_of, &peripheral_desc::name) - units.begin());
    }
    auto const size = (unit.size + page - 1) / page * page;
    windows.push_back(
        std::make_unique<mmio_window>(mmio_window{
            .run    = this,
            .slot   = owner.peripherals_[serving].get(),
            .unit   = &unit,
            .base   = unit.base,
            .size   = size,
            .secure = unit.alias_of.has_value(),
        }));
  }
  std::ranges::sort(windows, {}, [](auto const& window) { return window->base; });
  for(auto const [before, after] : windows | std::views::pairwise)
  {
    if(std::uint64_t{before->base} + before->size > after->base)
    {
      throw config_error(
          owner.description_.part, 0, std::format("peripherals.{}", after->unit->name),
          std::format(
              "a window on its own engine pages of {} bytes, it shares one with {}", page,
              before->unit->name));
    }
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  for(auto const& window : windows)
  {
    check(
        uc_mmio_map(
            engine, window->base, window->size, &on_mmio_read, window.get(), &on_mmio_write,
            window.get()),
        std::format("mapping peripheral {}", window->unit->name));
    ranges.emplace_back(window->base, std::uint64_t{window->base} + window->size - 1);
  }
  if(not ranges.empty())
  {
    auto const first        = ranges.front().first;
    auto const last         = ranges.back().second;
    auto const holds_memory = std::ranges::any_of(owner.memory_, [&](auto const& region) {
      return region->desc->start
          <= last
         and first
           < std::uint64_t{region->desc->start}
           + region->desc->size;
    });
    if(not holds_memory)
    {
      ranges = {
          {first, last}
      };
    }
  }
  exact_pc.configure(engine, std::move(ranges));
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

auto machine::run_state::dispatch_read(
    mmio_window const& window,
    std::uint32_t      offset,
    std::uint8_t       size,
    initiator          by) -> std::expected<std::uint32_t, bus_error>
{
  auto&      slot = *window.slot;
  auto const word = offset / 4;
  if(word >= slot.layout.words())
  {
    warn_unregistered(window, offset, false, by);
    return 0u;
  }
  if(slot.claimed[word] != 0)
  {
    auto const                              hz = owner.clock_.instruction_hz();
    std::expected<std::uint32_t, bus_error> value{};
    {
      handler_scope const scope{in_handler};
      value = slot.model->on_read(
          slot.regs, offset, size,
          {.pc = arch->pc(), .now = owner.clock_.now(), .by = by, .secure = window.secure});
    }
    after_model(hz);
    if(value)
    {
      trace_mmio(slot, window.base + offset, offset, size, *value & size_mask(size), false, by);
      note_watched(window.base + offset, size, *value, false, by);
    }
    return value.transform([size](std::uint32_t read) { return read & size_mask(size); });
  }
  auto const* const rules = slot.layout.find(word);
  if(rules == nullptr)
  {
    warn_unregistered(window, offset, false, by);
    return 0u;
  }
  if(not slot.model and slot.logged[word] == 0)
  {
    note_unmodelled(window, offset, false, by);
  }
  auto const read = periph::layer_read(*rules, slot.regs, offset, size);
  trace_mmio(slot, window.base + offset, offset, size, read, false, by);
  note_watched(window.base + offset, size, read, false, by);
  return read;
}

auto machine::run_state::dispatch_write(
    mmio_window const& window,
    std::uint32_t      offset,
    std::uint8_t       size,
    std::uint32_t      value,
    initiator          by) -> std::expected<void, bus_error>
{
  auto&      slot = *window.slot;
  auto const word = offset / 4;
  if(word >= slot.layout.words())
  {
    warn_unregistered(window, offset, true, by);
    return {};
  }
  if(slot.claimed[word] != 0)
  {
    auto const old = (slot.regs.peek(word * 4) >> (8 * (offset % 4))) & size_mask(size);
    auto const hz  = owner.clock_.instruction_hz();
    std::expected<void, bus_error> done{};
    {
      handler_scope const scope{in_handler};
      done = slot.model->on_write(
          slot.regs, offset, size, old, value & size_mask(size),
          {.pc = arch->pc(), .now = owner.clock_.now(), .by = by, .secure = window.secure});
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
    return {};
  }
  if(not slot.model and slot.logged[word] == 0)
  {
    note_unmodelled(window, offset, true, by);
  }
  periph::layer_write(slot.layout, *rules, slot.regs, slot.layer, offset, size, value);
  trace_mmio(slot, window.base + offset, offset, size, value & size_mask(size), true, by);
  note_watched(window.base + offset, size, value, true, by);
  return {};
}

void machine::run_state::after_model(std::uint64_t instruction_hz)
{
  deadline = std::min(deadline, owner.scheduler_.earliest());
  if(owner.clock_.instruction_hz() != instruction_hz and in_engine)
  {
    stop_flag = true;
  }
}

void machine::run_state::record_bus_fault(address addr, arch::memory_op op) noexcept
{
  if(not bus_fault)
  {
    bus_fault = unmapped_access{.addr = addr, .op = op};
  }
  stop_flag = true;
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
  auto& logged = window.slot->logged[offset / 4];
  if(logged != 0)
  {
    return;
  }
  logged          = 1;
  auto const reg  = register_name(window.slot->desc, offset / 4);
  auto const name = window.unit->name;
  owner.hooks_->log(
      {.kind       = log_kind::unmodelled_access,
       .pc         = arch->pc(),
       .peripheral = name,
       .reg        = reg,
       .offset     = offset,
       .write      = write,
       .message    = std::format(
           "{} of {}.{} at offset {:#x}, no model", write ? "write" : "read", name, reg, offset)});
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
    void*         user) noexcept -> std::uint64_t
{
  auto const&   window = *static_cast<mmio_window const*>(user);
  auto&         run    = *window.run;
  std::uint32_t value  = 0;
  run.guarded([&] {
    auto const read = run.dispatch_read(
        window, static_cast<std::uint32_t>(offset), static_cast<std::uint8_t>(size),
        initiator::cpu);
    if(read)
    {
      value = *read;
    }
    else
    {
      run.record_bus_fault(window.base + static_cast<address>(offset), arch::memory_op::read);
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
    void*         user) noexcept
{
  auto const& window = *static_cast<mmio_window const*>(user);
  auto&       run    = *window.run;
  run.guarded([&] {
    auto const done = run.dispatch_write(
        window, static_cast<std::uint32_t>(offset), static_cast<std::uint8_t>(size),
        static_cast<std::uint32_t>(value), initiator::cpu);
    if(not done)
    {
      run.record_bus_fault(window.base + static_cast<address>(offset), arch::memory_op::write);
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
