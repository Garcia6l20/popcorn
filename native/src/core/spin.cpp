#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/description.hpp>

#include <algorithm>
#include <array>
#include <format>
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

auto in_loop(spin_state const& state, address addr) noexcept -> bool
{
  return state.head != 0 and state.head <= addr and addr - state.head < spin_state::loop_window;
}
}

void machine::run_state::spin_block(address addr)
{
  auto&      state       = spin;
  auto const jumped_back = backward_step(state.previous, addr);
  auto const inside      = state.head != 0 and addr != state.head and in_loop(state, addr);
  auto const backward    = (jumped_back and not inside)
                        or (state.head != 0 and addr == state.head and state.previous == 0);
  state.previous         = addr;
  if(not backward)
  {
    if(in_loop(state, addr))
    {
      state.last = std::max(state.last, addr);
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
  if(addr != state.head or not state.comparable or state.reading != state.reads)
  {
    state.progress();
    state.known = false;
    if(addr != state.head)
    {
      state.head       = addr;
      state.head_depth = state.depth;
      state.last       = addr;
    }
  }
  else if(not state.known)
  {
    read_all();
    state.registers = state.scratch;
    state.known     = true;
    state.progress();
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
        state.subset | std::views::transform([&](std::size_t at) { return state.registers[at]; }));
    if(not subset_same)
    {
      state.progress();
      state.known = false;
    }
    else
    {
      read_all();
      if(state.scratch == state.registers)
      {
        ++state.count;
      }
      else
      {
        state.progress();
        state.registers = state.scratch;
      }
    }
  }
  std::swap(state.reads, state.reading);
  state.reading.clear();
  state.comparable = true;
  if(state.count >= owner.options_.spin_threshold and not state.reported)
  {
    if(model_due())
    {
      state.progress();
      return;
    }
    state.reported = true;
    owner.do_request_stop(stop_reason::spin, spin_detail());
  }
}

auto machine::run_state::model_due() const -> bool
{
  auto const pending = owner.scheduler_.pending();
  return std::ranges::any_of(spin.reads, [&](spin_state::mmio_read const& read) {
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
  auto& state = spin;
  if(state.head == 0 or state.depth != state.head_depth or not state.comparable)
  {
    return;
  }
  if(state.reading.size() == spin_state::max_reads)
  {
    state.reading.clear();
    state.comparable = false;
    return;
  }
  state.reading.push_back(
      {.addr  = addr,
       .value = value,
       .reg   = slot.trace_id_at(offset),
       .peripheral =
           static_cast<std::uint32_t>(&slot.desc - owner.description_.peripherals.data())});
}

auto machine::run_state::spin_detail() const -> std::string
{
  auto text = std::format("loop {:#010x}-{:#010x}", spin.head, spin.last);
  if(auto const found = symbol_at(spin.head))
  {
    text += std::format(" in {}", symbols[found->first]->name);
  }
  if(spin.reads.empty())
  {
    return text;
  }
  auto const& read = spin.reads.back();
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
