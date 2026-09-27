#include "machine_impl.hpp"

#include "engine.hpp"
#include "engine_context.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/version.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::core
{
namespace
{
auto levels(auto const& lines) -> std::vector<std::uint8_t>
{
  return lines
       | std::views::transform(
             [](auto const& line) { return line.level() ? std::uint8_t{1} : std::uint8_t{0}; })
       | std::ranges::to<std::vector>();
}

template <typename Item>
auto named(std::vector<Item> const& items, std::string_view name, auto key) -> Item const*
{
  auto const found = std::ranges::find(items, name, key);
  return found == items.end() ? nullptr : &*found;
}

auto digests(std::span<popcorn::image::image const> images) -> std::vector<std::string>
{
  return images
       | std::views::transform(&popcorn::image::image::digest)
       | std::ranges::to<std::vector>();
}
}

auto machine::snapshot() const -> core::snapshot
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a call is in progress");
  }
  auto const& run = *run_;
  if(run.invalid)
  {
    throw emulation_error("the machine was left invalid by a failed restore");
  }
  contract_assert(run.stale_code.empty());
  contract_assert(run.failure == nullptr);
  contract_assert(not run.trap.has_value());
  contract_assert(not run.unmapped.has_value());
  contract_assert(not run.bus_fault.has_value());

  snapshot_data data{
      .format             = snapshot_format_version,
      .popcorn_version    = std::string{version()},
      .unicorn_commit     = POPCORN_UNICORN_COMMIT,
      .description_digest = run.description_digest,
      .image_digests      = digests(images_),
      .config_digest      = options_.config_digest,
      .generation         = generation_,
  };
  for(auto const& region : memory_)
  {
    if(region->desc->physical)
    {
      continue;
    }
    auto const* const first = region->host(region->desc->start);
    data.memory.push_back(
        {.region = region->desc->name,
         .start  = region->desc->start,
         .bytes  = std::vector<std::byte>(first, first + region->desc->size)});
  }
  auto const registers = arch_->registers();
  auto const values    = read_registers(engine_.get(), registers);
  for(auto const& [reg, value] : std::views::zip(registers, values))
  {
    data.registers.push_back({.name = std::string{reg.name}, .value = value});
  }
  data.arch.owner = "arch";
  arch_->save(data.arch.data);
  for(auto const& slot : peripherals_)
  {
    if(not slot)
    {
      continue;
    }
    peripheral_image image{
        .peripheral   = slot->desc.name,
        .words        = {},
        .irq_levels   = levels(slot->irqs),
        .dma_levels   = levels(slot->dma),
        .write_locked = slot->layer.write_locked,
    };
    for(std::uint32_t offset = 0; offset < slot->desc.size; offset += 4)
    {
      image.words.push_back(slot->regs.peek(offset));
    }
    data.peripherals.push_back(std::move(image));
    if(slot->model)
    {
      snapshot_part part{.owner = slot->desc.name, .data = {}};
      slot->model->save(part.data);
      data.models.push_back(std::move(part));
    }
  }
  auto const pending = scheduler_.pending();
  data.events        = {
      .pending  = std::vector<event>(pending.begin(), pending.end()),
      .next_seq = scheduler_.next_seq()};
  data.clock = clock_driver_.state();
  for(auto const& [id, entry] : run.intercepts)
  {
    data.intercepts.push_back({.at = entry->at, .calls = entry->calls, .spent = entry->spent});
  }
  for(auto const& [id, entry] : run.return_hooks)
  {
    return_hook_state state{.at = entry->at, .calls = entry->calls, .in_flight = {}};
    for(auto const& frame : run.frames)
    {
      if(frame.entry == entry.get())
      {
        state.in_flight.push_back(
            {.return_address = frame.ret,
             .stack_pointer  = static_cast<address>(frame.sp),
             .args           = frame.args,
             .entered        = frame.entered});
      }
    }
    data.return_hooks.push_back(std::move(state));
  }
  return core::snapshot{
      std::move(data), std::make_shared<engine_context const>(engine_.get()), id_};
}

void machine::restore(core::snapshot const& saved)
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a call is in progress");
  }
  auto const& data      = saved.data();
  auto const  own       = saved.machine_id() == id_;
  auto const  registers = arch_->registers();
  if(saved.machine_id() != 0 and not own)
  {
    throw emulation_error(
        std::format(
            "the snapshot was taken by machine {}, not by machine {}", saved.machine_id(), id_));
  }
  auto const refuse = [&](std::string key, std::string expected) {
    throw config_error("snapshot", 0, std::move(key), std::move(expected));
  };
  if(data.format != snapshot_format_version)
  {
    refuse("format", std::format("snapshot_format_version {}", snapshot_format_version));
  }
  if(not own)
  {
    if(data.description_digest != run_->description_digest)
    {
      refuse(
          "description_digest",
          std::format("the digest of {}, {}", description_.part, run_->description_digest));
    }
    auto const loaded = digests(images_);
    if(data.image_digests != loaded)
    {
      refuse("image_digests", std::format("the digests of the loaded images, {}", loaded));
    }
    if(data.config_digest != options_.config_digest)
    {
      refuse(
          "config_digest",
          std::format("the config digest of this machine, {}", options_.config_digest));
    }
  }

  auto const holds = [](memory_image const* image, region_desc const& region) {
    return image != nullptr and image->start == region.start and image->bytes.size() == region.size;
  };
  auto const lock_words = [](peripheral_slot const& slot) -> std::size_t {
    return slot.layout.has_write_once() ? (slot.layout.words() + 31) / 32 : 0;
  };
  auto const matches = [&](peripheral_image const* image, peripheral_slot const& slot) {
    if(image == nullptr)
    {
      return false;
    }
    auto const saved =
        std::array{image->words.size(), image->irq_levels.size(), image->dma_levels.size()};
    auto const live =
        std::array{std::size_t{(slot.desc.size + 3) / 4}, slot.irqs.size(), slot.dma.size()};
    auto const locks = image->write_locked.size();
    return saved == live and (locks == 0 or locks == lock_words(slot));
  };
  auto const& names   = data.clock.domain_names;
  auto const& domains = clock_driver_.state().domain_names;
  if(not std::ranges::equal(
         names | std::views::take(domains.size()), domains | std::views::take(names.size())))
  {
    refuse("clock.domain_names", std::format("the clock domains of this machine, {}", domains));
  }
  for(auto const& pending : data.events.pending)
  {
    auto const owner = pending.id.owner;
    auto const known =
        owner
        == arch::arch_event_owner
        or (owner < peripherals_.size() and peripherals_[owner] and peripherals_[owner]->model);
    if(not known)
    {
      refuse(
          std::format("events.{}", pending.seq),
          std::format("an event of the arch or of a modelled peripheral, not owner {}", owner));
    }
  }
  struct
  {
    std::vector<std::pair<mapped_region const*, memory_image const*>> memory;
    std::vector<std::uint64_t>                                        registers;
    std::vector<peripheral_image const*>                              peripherals;
    std::vector<snapshot_part const*>                                 models;
  } plan;
  for(auto const& region : memory_)
  {
    if(region->desc->physical)
    {
      continue;
    }
    auto const* const image = named(data.memory, region->desc->name, &memory_image::region);
    if(not holds(image, *region->desc))
    {
      refuse(
          std::format("memory.{}", region->desc->name),
          std::format("{} bytes at {:#010x}", region->desc->size, region->desc->start));
    }
    plan.memory.emplace_back(region.get(), image);
  }
  for(auto const& image : data.memory)
  {
    auto const mapped = std::ranges::any_of(memory_, [&](auto const& region) {
      return not region->desc->physical and region->desc->name == image.region;
    });
    if(not mapped)
    {
      refuse(
          std::format("memory.{}", image.region), "a RAM, flash or scratch region of this machine");
    }
  }
  if(not own or not saved.has_engine_context())
  {
    plan.registers = read_registers(engine_.get(), registers);
    for(auto const& value : data.registers)
    {
      auto const found = std::ranges::find(registers, value.name, &arch::cpu_register::name);
      if(found == registers.end())
      {
        refuse(std::format("registers.{}", value.name), "a register of this core");
      }
      plan.registers[static_cast<std::size_t>(found - registers.begin())] = value.value;
    }
  }
  for(auto const& slot : peripherals_)
  {
    if(not slot)
    {
      continue;
    }
    auto const* const image =
        named(data.peripherals, slot->desc.name, &peripheral_image::peripheral);
    if(not matches(image, *slot))
    {
      refuse(
          std::format("peripherals.{}", slot->desc.name),
          std::format("{} words and the lines of {}", slot->desc.size / 4, slot->desc.name));
    }
    plan.peripherals.push_back(image);
    snapshot_part const* part = nullptr;
    if(slot->model)
    {
      part = named(data.models, slot->desc.name, &snapshot_part::owner);
      if(part == nullptr)
      {
        refuse(std::format("models.{}", slot->desc.name), "the bytes of its model");
      }
    }
    plan.models.push_back(part);
  }

  auto& run   = *run_;
  run.invalid = true;
  run.restore_memory(plan.memory);
  if(plan.registers.empty())
  {
    check(uc_context_restore(engine_.get(), saved.context_->get()), "restoring the CPU context");
  }
  else
  {
    write_registers(engine_.get(), registers, std::move(plan.registers));
  }
  scheduler_.restore(data.events.pending, data.events.next_seq);
  clock_driver_.restore(data.clock);
  trace_.rebase(clock_.now());
  run.rebase_sinks(clock_.now());
  arch_->load(data.arch.data);
  std::ranges::fill(run.lines.active, 0u);
  auto next = plan.peripherals.begin();
  for(auto const& slot : peripherals_)
  {
    if(not slot)
    {
      continue;
    }
    auto const& image = **next++;
    for(auto const& [index, word] : std::views::enumerate(image.words))
    {
      slot->regs.poke(static_cast<std::uint32_t>(index * 4), word);
    }
    if(image.write_locked.empty())
    {
      slot->layer.write_locked.assign(lock_words(*slot), 0);
    }
    else
    {
      slot->layer.write_locked = image.write_locked;
    }
    for(auto const& [line, level] : std::views::zip(slot->irqs, image.irq_levels))
    {
      line.level_ = level != 0;
      if(line.level_)
      {
        if(line.source_ >= run.lines.active.size())
        {
          run.lines.active.resize(line.source_ + 1);
        }
        ++run.lines.active[line.source_];
      }
    }
    for(auto const& [line, level] : std::views::zip(slot->dma, image.dma_levels))
    {
      line.level_ = level != 0;
    }
  }
  auto part = plan.models.begin();
  for(auto const& slot : peripherals_)
  {
    if(not slot)
    {
      continue;
    }
    if(auto const* const bytes = *part++; bytes != nullptr)
    {
      slot->model->load(bytes->data);
    }
  }

  run.restore_hook_state(data.intercepts, data.return_hooks);
  run.frames.clear();
  run.coverage.has_previous = false;
  run.spin.clear();
  run.instant_events = 0;
  run.return_watch   = 0;
  run.resume_past.reset();
  run.requested.reset();
  run.reset_requested.reset();
  run.end_requested = false;
  run.stop_flag     = false;
  ++generation_;
  run.invalid = false;
}

void machine::run_state::restore_memory(
    std::span<std::pair<mapped_region const*, memory_image const*> const> plan)
{
  std::uint32_t page = 0;
  check(uc_ctl_get_page_size(engine, &page), "reading the engine page size");
  for(auto const& [region, image] : plan)
  {
    auto&       buffer = *region->buffer;
    auto* const live   = region->host(region->desc->start);
    auto const* stored = image->bytes.data();
    auto const  size   = image->bytes.size();
    if(not buffer.executable)
    {
      std::memcpy(live, stored, size);
      continue;
    }
    std::optional<std::size_t> run_start;
    auto const                 flush = [&](std::size_t end) {
      if(run_start)
      {
        remove_translated(engine, buffer, region->offset + *run_start, region->offset + end);
        run_start.reset();
      }
    };
    for(std::size_t at = 0; at < size; at += page)
    {
      auto const length = std::min<std::size_t>(page, size - at);
      if(std::memcmp(live + at, stored + at, length) == 0)
      {
        flush(at);
        continue;
      }
      std::memcpy(live + at, stored + at, length);
      if(not run_start)
      {
        run_start = at;
      }
    }
    flush(size);
  }
}

void machine::run_state::restore_hook_state(
    std::span<intercept_state const>   saved_intercepts,
    std::span<return_hook_state const> saved_returns)
{
  std::vector<bool> used(saved_intercepts.size());
  for(auto const& [id, entry] : intercepts)
  {
    entry->calls = 0;
    entry->spent = false;
    for(auto const& [index, state] : std::views::enumerate(saved_intercepts))
    {
      if(not used[static_cast<std::size_t>(index)] and state.at == entry->at)
      {
        used[static_cast<std::size_t>(index)] = true;
        entry->calls                          = state.calls;
        entry->spent                          = state.spent;
        break;
      }
    }
  }
  std::vector<bool> taken(saved_returns.size());
  for(auto const& [id, entry] : return_hooks)
  {
    entry->calls = 0;
    for(auto const& [index, state] : std::views::enumerate(saved_returns))
    {
      if(not taken[static_cast<std::size_t>(index)] and state.at == entry->at)
      {
        taken[static_cast<std::size_t>(index)] = true;
        entry->calls                           = state.calls;
        break;
      }
    }
  }
}
}
