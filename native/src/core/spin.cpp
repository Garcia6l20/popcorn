#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/description.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::core
{
namespace
{
constexpr std::array<std::string_view, 6> subset_names{"sp", "r0", "r1", "r2", "r3", "lr"};

auto pointers_into(std::vector<std::uint64_t>& values) -> std::vector<void*>
{
  return values
       | std::views::transform([](std::uint64_t& value) -> void* { return &value; })
       | std::ranges::to<std::vector>();
}

auto backward_step(address previous, address addr) noexcept -> bool
{
  return previous != 0 and addr <= previous and previous - addr < spin_state::loop_window;
}

auto in_loop(spin_state::loop_state const& loop, address addr) noexcept -> bool
{
  return loop.head != 0 and loop.head <= addr and addr - loop.head < spin_state::loop_window;
}
}

auto machine::run_state::skip_self_branch(uc_struct* engine, icount now) noexcept -> bool
{
  if(stepping or spin_on or coverage_on or block_records or block_watch or sites.contains(block_pc))
  {
    return false;
  }
  auto const target = std::min(deadline, owner.scheduler_.earliest());
  if(target.value == std::numeric_limits<std::uint64_t>::max()
     or not guarded([this] { return arch->self_branch(block_pc); })
     or failure)
  {
    return false;
  }
  owner.clock_driver_.advance(icount{target.value - now.value});
  ++counters.self_branch_skips;
  stop_at_block(engine, engine_end::event);
  return true;
}

void machine::run_state::spin_block(address addr)
{
  auto&      state       = spin;
  auto&      loop        = state.loop();
  auto const jumped_back = backward_step(state.previous, addr);
  auto const inside      = loop.head != 0 and addr != loop.head and in_loop(loop, addr);
  if(addr == loop.head)
  {
    loop.candidate      = {};
    loop.candidate_hits = 0;
  }
  auto adopt = false;
  if(jumped_back and inside)
  {
    loop.candidate_hits = addr == loop.candidate ? loop.candidate_hits + 1 : 1;
    loop.candidate      = addr;
    adopt               = loop.candidate_hits >= 2;
  }
  auto const backward = (jumped_back and (not inside or adopt))
                     or (loop.head != 0 and addr == loop.head and state.previous == 0);
  state.previous      = addr;
  if(not backward)
  {
    if(in_loop(loop, addr))
    {
      loop.last = std::max(loop.last, addr);
    }
    return;
  }
  if(state.register_ids.empty())
  {
    for(auto const& reg : arch->registers())
    {
      if(reg.name == "pc")
      {
        continue;
      }
      if(std::ranges::contains(subset_names, reg.name))
      {
        state.subset.push_back(state.register_ids.size());
        state.subset_ids.push_back(reg.engine_id);
      }
      state.register_ids.push_back(reg.engine_id);
    }
    state.scratch.resize(state.register_ids.size());
    state.pointers = pointers_into(state.scratch);
    state.subset_scratch.resize(state.subset_ids.size());
    state.subset_pointers = pointers_into(state.subset_scratch);
  }
  auto const read_all = [&] {
    check(
        uc_reg_read_batch(
            engine, state.register_ids.data(), state.pointers.data(),
            static_cast<int>(state.register_ids.size())),
        "reading the registers of a loop");
  };
  if(addr != loop.head or not loop.comparable or loop.reading != loop.reads)
  {
    loop.progress();
    loop.known = false;
    if(addr != loop.head)
    {
      loop.head = addr;
      loop.last = addr;
    }
  }
  else if(not loop.known)
  {
    read_all();
    loop.registers = state.scratch;
    loop.known     = true;
    loop.progress();
  }
  else
  {
    check(
        uc_reg_read_batch(
            engine, state.subset_ids.data(), state.subset_pointers.data(),
            static_cast<int>(state.subset_ids.size())),
        "reading the registers of a loop");
    auto const subset_same = std::ranges::equal(
        state.subset_scratch,
        state.subset | std::views::transform([&](std::size_t at) { return loop.registers[at]; }));
    if(not subset_same)
    {
      loop.progress();
      loop.known = false;
    }
    else
    {
      read_all();
      if(state.scratch == loop.registers)
      {
        ++loop.count;
      }
      else
      {
        loop.progress();
        loop.registers = state.scratch;
      }
    }
  }
  std::swap(loop.reads, loop.reading);
  loop.reading.clear();
  loop.comparable = true;
  if(loop.count >= owner.options_.spin_threshold and not loop.reported)
  {
    if(model_due())
    {
      loop.progress();
      return;
    }
    loop.reported = true;
    owner.do_request_stop(stop_reason::spin, spin_detail());
  }
}

auto machine::run_state::model_due() const -> bool
{
  auto const pending = owner.scheduler_.pending();
  return std::ranges::any_of(spin.loop().reads, [&](spin_state::mmio_read const& read) {
    return std::ranges::any_of(
        pending, [&](event const& due) { return due.id.owner == read.peripheral; });
  });
}

void machine::run_state::spin_read(
    peripheral_slot const& slot,
    address                addr,
    std::uint32_t          offset,
    std::uint32_t          value)
{
  auto& loop = spin.loop();
  if(loop.head == 0 or not loop.comparable)
  {
    return;
  }
  if(loop.reading.size() == spin_state::max_reads)
  {
    loop.reading.clear();
    loop.comparable = false;
    return;
  }
  loop.reading.push_back(
      {.addr  = addr,
       .value = value,
       .reg   = slot.trace_id_at(offset),
       .peripheral =
           static_cast<std::uint32_t>(&slot.desc - owner.description_.peripherals.data())});
}

auto machine::run_state::spin_detail() const -> std::string
{
  auto const& loop = spin.loop();
  auto        text = std::format("loop {:#010x}-{:#010x}", loop.head, loop.last);
  if(auto const found = symbol_at(loop.head))
  {
    text += std::format(" in {}", symbols[found->first]->name);
  }
  if(loop.reads.empty())
  {
    return text;
  }
  auto const& read = loop.reads.back();
  auto const  ref  = register_at(owner.description_, read.reg);
  if(read.reg == trace_id_overflow or not ref)
  {
    return text + std::format(", polling {:#010x} = {:#010x}", read.addr, read.value);
  }
  auto const& unit = owner.description_.peripherals[ref->peripheral];
  auto const& reg  = unit.registers[ref->reg];
  text += std::format(", polling {}.{} = {:#010x}", unit.name, reg.name, read.value);
  if(not reg.fields.empty())
  {
    text += ", fields ";
    for(auto const& [index, field] : std::views::enumerate(reg.fields))
    {
      text += index == 0 ? field.name : ", " + field.name;
    }
  }
  return text;
}
}
