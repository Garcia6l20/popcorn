#include "errors.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/register_view.hpp>
#include <popcorn/periph/register_file.hpp>

#include <nanobind/nanobind.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::bind
{
namespace
{
void require_in_space(address at, std::size_t size)
{
  if(std::uint64_t{at} + size > std::uint64_t{1} << 32)
  {
    throw config_error(
        "memory", 0, std::format("{:#010x}", at),
        std::format("{} bytes within the 32-bit address space", size));
  }
}

[[noreturn]] void bus_failure(core::block_error const& failed)
{
  throw detail::bus_access_error(
      std::format(
          "bus error at {:#010x}: {}", failed.at,
          failed.error == bus_error::unmapped ? "unmapped" : "denied"),
      failed.at);
}

auto register_at(core::machine& target, std::uint32_t index) -> arch::cpu_register
{
  auto const registers = target.regs().list();
  if(index >= registers.size())
  {
    throw config_error(
        "registers", 0, std::format("{}", index),
        std::format("a register index below {}", registers.size()));
  }
  return registers[index];
}

struct slot
{
  periph::register_file* file;
  std::uint32_t          word;
  std::uint32_t          shift;
  std::uint32_t          mask;
};

auto slot_of(
    core::machine&     target,
    std::string const& name,
    std::uint32_t      offset,
    std::uint8_t       width) -> slot
{
  auto& file = target.peripheral(name);
  if((width != 8 and width != 16 and width != 32)
     or offset % (width / 8) != 0
     or std::uint64_t{offset} + width / 8 > file.description().size)
  {
    throw config_error(
        name, 0, std::format("{:#x}", offset),
        std::format(
            "an aligned {}-bit register inside the {} bytes of {}", width, file.description().size,
            name));
  }
  return {
      .file  = &file,
      .word  = offset & ~3u,
      .shift = (offset % 4) * 8,
      .mask  = width == 32 ? 0xffff'ffffu : (1u << width) - 1u,
  };
}

auto view_of(core::machine& target, memory_space space) -> core::memory_view
{
  switch(space)
  {
    case memory_space::secure:
      return target.mem(true);
    case memory_space::nonsecure:
      return target.mem(false);
    case memory_space::debugger:
      break;
  }
  return target.mem();
}

auto read_memory_body(address at, std::uint32_t size, memory_space space)
{
  require_in_space(at, size);
  return [at, size, space](core::machine& target) {
    std::vector<std::byte> bytes(size);
    if(auto const read = view_of(target, space).read_block(at, bytes); not read)
    {
      bus_failure(read.error());
    }
    return bytes;
  };
}

auto write_memory_body(address at, std::vector<std::byte> data, memory_space space)
{
  require_in_space(at, data.size());
  return [at, data = std::move(data), space](core::machine& target) {
    if(auto const written = view_of(target, space).write_block(at, data); not written)
    {
      bus_failure(written.error());
    }
  };
}

auto memory_masked_body(
    address                at,
    std::vector<std::byte> mask,
    std::vector<std::byte> value,
    memory_space           space)
{
  if(mask.size() != value.size())
  {
    throw config_error(
        "memory", 0, std::format("{:#010x}", at), "a mask and a value of the same length");
  }
  require_in_space(at, mask.size());
  return [at, mask = std::move(mask), value = std::move(value), space](core::machine& target) {
    auto const             view = view_of(target, space);
    std::vector<std::byte> word(mask.size());
    if(auto const read = view.read_block(at, word); not read)
    {
      bus_failure(read.error());
    }
    for(auto const [index, byte] : std::views::enumerate(word))
    {
      byte = (byte & ~mask[static_cast<std::size_t>(index)])
           | (value[static_cast<std::size_t>(index)] & mask[static_cast<std::size_t>(index)]);
    }
    if(auto const written = view.write_block(at, word); not written)
    {
      bus_failure(written.error());
    }
  };
}

constexpr std::uint32_t cstring_block = 64;

auto cstring_body(address at, std::uint32_t limit, memory_space space)
{
  return [at, limit, space](core::machine& target) {
    auto const             view = view_of(target, space);
    std::vector<std::byte> text;
    auto const             nul = std::byte{0};
    while(text.size() < limit)
    {
      auto const next = std::uint64_t{at} + text.size();
      auto const size =
          std::min<std::uint64_t>(cstring_block - next % cstring_block, limit - text.size());
      if(next + size > std::uint64_t{1} << 32)
      {
        throw config_error(
            "memory", 0, std::format("{:#010x}", next),
            "a string ending within the 32-bit address space");
      }
      auto const             from = static_cast<address>(next);
      std::vector<std::byte> chunk(size);
      if(not view.read_block(from, chunk))
      {
        chunk.clear();
        for(address byte_at = from; byte_at < from + size; ++byte_at)
        {
          std::array<std::byte, 1> byte{};
          if(auto const read = view.read_block(byte_at, byte); not read)
          {
            bus_failure(read.error());
          }
          chunk.push_back(byte[0]);
          if(byte[0] == nul)
          {
            break;
          }
        }
      }
      if(auto const end = std::ranges::find(chunk, nul); end != chunk.end())
      {
        text.insert(text.end(), chunk.begin(), end);
        break;
      }
      text.insert(text.end(), chunk.begin(), chunk.end());
    }
    return text;
  };
}

auto read_register_body(core::machine& engine, std::uint32_t index)
{
  auto const reg = register_at(engine, index);
  return [reg](core::machine& target) { return target.regs().read(reg); };
}

auto write_register_body(core::machine& engine, std::uint32_t index, std::uint64_t value)
{
  auto const reg = register_at(engine, index);
  if(not core::fits_register(value, reg.bits))
  {
    throw config_error(
        "registers", 0, std::string{reg.name}, std::format("a {}-bit value", reg.bits));
  }
  return [reg, value](core::machine& target) { target.regs().write(reg, value); };
}

auto peek_body(
    core::machine&     engine,
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width)
{
  auto const at = slot_of(engine, peripheral, offset, width);
  return [at](core::machine&) { return (at.file->peek(at.word) >> at.shift) & at.mask; };
}

auto poke_masked_body(
    core::machine&     engine,
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width,
    std::uint32_t      mask,
    std::uint32_t      value)
{
  auto const at = slot_of(engine, peripheral, offset, width);
  if((value & ~at.mask) != 0 or (mask & ~at.mask) != 0)
  {
    throw config_error(
        peripheral, 0, std::format("{:#x}", offset), std::format("a {}-bit value", width));
  }
  return [at, mask, value](core::machine&) {
    auto const word = at.file->peek(at.word);
    at.file->poke(at.word, (word & ~(mask << at.shift)) | ((value & mask) << at.shift));
  };
}

struct neutral_merge
{
  std::uint32_t stored{};
  std::uint32_t ones{};
  std::string   reg;
  std::string   effectful;
};

auto neutral_of(slot const& at, std::uint32_t offset, std::uint8_t width, std::uint32_t mask)
    -> neutral_merge
{
  neutral_merge merge{.stored = at.mask & ~mask, .ones = 0, .reg = {}, .effectful = {}};
  for(auto const& described : at.file->description().registers)
  {
    if(described.offset != offset or described.width != width)
    {
      continue;
    }
    merge.reg = described.name;
    for(auto const& field : described.fields)
    {
      auto const bits =
          (field.width >= 32 ? 0xffff'ffffu : (1u << field.width) - 1u) << field.lsb & at.mask;
      if((bits & mask) != 0)
      {
        continue;
      }
      if(field.acc == access::write_only or field.acc == access::write_once)
      {
        merge.stored &= ~bits;
      }
      switch(field.on_write)
      {
        case write_effect::one_to_clear:
        case write_effect::one_to_set:
        case write_effect::one_to_toggle:
          merge.stored &= ~bits;
          break;
        case write_effect::zero_to_clear:
        case write_effect::zero_to_set:
        case write_effect::zero_to_toggle:
          merge.stored &= ~bits;
          merge.ones |= bits;
          break;
        case write_effect::clear:
        case write_effect::set:
          if(merge.effectful.empty())
          {
            merge.effectful = field.name;
          }
          break;
        case write_effect::modify:
          break;
      }
    }
  }
  return merge;
}

auto write_field_body(
    core::machine&                                                                       engine,
    std::string const&                                                                   peripheral,
    std::uint32_t                                                                        offset,
    std::uint8_t                                                                         width,
    std::uint32_t                                                                        mask,
    std::uint32_t                                                                        value,
    std::map<std::pair<std::string, std::uint32_t>, std::shared_ptr<std::atomic<bool>>>& logged)
{
  auto const at = slot_of(engine, peripheral, offset, width);
  if((value & ~at.mask) != 0 or (mask & ~at.mask) != 0)
  {
    throw config_error(
        peripheral, 0, std::format("{:#x}", offset), std::format("a {}-bit value", width));
  }
  auto merge = neutral_of(at, offset, width, mask);
  auto once  = std::shared_ptr<std::atomic<bool>>{};
  if(not merge.effectful.empty())
  {
    auto& slot = logged[{peripheral, offset}];
    if(not slot)
    {
      slot = std::make_shared<std::atomic<bool>>(false);
    }
    once = slot;
  }
  auto const base = at.file->description().base;
  return [at, base, offset, width, mask, value, once = std::move(once), merge = std::move(merge),
          peripheral](core::machine& target) {
    auto const stored = (at.file->peek(at.word) >> at.shift) & at.mask;
    auto const merged = (stored & merge.stored) | merge.ones | (value & mask);
    if(auto const written = target.mem().write(base + offset, width / 8u, merged); not written)
    {
      bus_failure({.error = written.error(), .at = base + offset});
    }
    if(once and not once->exchange(true))
    {
      target.log(
          {.kind       = core::log_kind::model_warning,
           .peripheral = peripheral,
           .reg        = merge.reg,
           .offset     = offset,
           .write      = true,
           .message    = std::format(
               "field write to {} also writes {}, whose every write has an effect", merge.reg,
               merge.effectful)});
    }
  };
}

auto bytes_of(reflex::py::nb::bytes const& data) -> std::vector<std::byte>
{
  auto const* const first = static_cast<std::byte const*>(data.data());
  return {first, first + data.size()};
}

auto python_bytes(std::vector<std::byte> const& data) -> reflex::py::nb::bytes
{
  return reflex::py::nb::bytes(data.data(), data.size());
}
}

auto machine_handle::read_memory(address at, std::uint32_t size, memory_space space) const
    -> reflex::py::nb::bytes
{
  return python_bytes(access(read_memory_body(at, size, space)));
}

void machine_handle::write_memory(
    address                      at,
    reflex::py::nb::bytes const& bytes,
    memory_space                 space)
{
  access(write_memory_body(at, bytes_of(bytes), space));
}

void machine_handle::write_memory_masked(
    address                      at,
    reflex::py::nb::bytes const& mask,
    reflex::py::nb::bytes const& value,
    memory_space                 space)
{
  access(memory_masked_body(at, bytes_of(mask), bytes_of(value), space));
}

auto machine_handle::cstring(address at, std::uint32_t limit, memory_space space) const
    -> reflex::py::nb::bytes
{
  return python_bytes(access(cstring_body(at, limit, space)));
}

auto machine_handle::register_names() const -> std::vector<std::string>
{
  return engine().regs().list()
       | std::views::transform([](arch::cpu_register const& reg) { return std::string{reg.name}; })
       | std::ranges::to<std::vector>();
}

auto machine_handle::read_register(std::uint32_t index) const -> std::uint64_t
{
  return access([&](core::machine& target) { return read_register_body(target, index)(target); });
}

void machine_handle::write_register(std::uint32_t index, std::uint64_t value)
{
  access([&](core::machine& target) { write_register_body(target, index, value)(target); });
}

auto machine_handle::peek(std::string const& peripheral, std::uint32_t offset, std::uint8_t width)
    -> std::uint32_t
{
  return access(
      [&](core::machine& target) { return peek_body(target, peripheral, offset, width)(target); });
}

void machine_handle::poke(
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width,
    std::uint32_t      value)
{
  auto const all = width >= 32 ? 0xffff'ffffu : (1u << width) - 1u;
  access([&](core::machine& target) {
    poke_masked_body(target, peripheral, offset, width, all, value)(target);
  });
}

void machine_handle::poke_masked(
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width,
    std::uint32_t      mask,
    std::uint32_t      value)
{
  access([&](core::machine& target) {
    poke_masked_body(target, peripheral, offset, width, mask, value)(target);
  });
}

void machine_handle::write_field(
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width,
    std::uint32_t      mask,
    std::uint32_t      value)
{
  auto body =
      write_field_body(engine(), peripheral, offset, width, mask, value, field_merge_logged_);
  access([&](core::machine& target) { body(target); });
}

auto machine_handle::op_read_memory(address at, std::uint32_t size, memory_space space) const
    -> access_op
{
  return detail::machine_op(read_memory_body(at, size, space));
}

auto machine_handle::op_write_memory(
    address                      at,
    reflex::py::nb::bytes const& bytes,
    memory_space                 space) const -> access_op
{
  return detail::machine_op(write_memory_body(at, bytes_of(bytes), space));
}

auto machine_handle::op_memory_masked(
    address                      at,
    reflex::py::nb::bytes const& mask,
    reflex::py::nb::bytes const& value,
    memory_space                 space) const -> access_op
{
  return detail::machine_op(memory_masked_body(at, bytes_of(mask), bytes_of(value), space));
}

auto machine_handle::op_cstring(address at, std::uint32_t limit, memory_space space) const
    -> access_op
{
  return detail::machine_op(cstring_body(at, limit, space));
}

auto machine_handle::op_read_register(std::uint32_t index) const -> access_op
{
  return detail::machine_op(read_register_body(engine(), index));
}

auto machine_handle::op_write_register(std::uint32_t index, std::uint64_t value) const -> access_op
{
  return detail::machine_op(write_register_body(engine(), index, value));
}

auto machine_handle::op_peek(
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width) const -> access_op
{
  return detail::machine_op(peek_body(engine(), peripheral, offset, width));
}

auto machine_handle::op_poke_masked(
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width,
    std::uint32_t      mask,
    std::uint32_t      value) const -> access_op
{
  return detail::machine_op(poke_masked_body(engine(), peripheral, offset, width, mask, value));
}

auto machine_handle::op_write_field(
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width,
    std::uint32_t      mask,
    std::uint32_t      value) const -> access_op
{
  return detail::machine_op(
      write_field_body(engine(), peripheral, offset, width, mask, value, field_merge_logged_));
}

auto machine_handle::op_pc() const -> access_op
{
  return detail::machine_op([](core::machine& target) { return target.regs().pc(); });
}

auto machine_handle::op_instructions() const -> access_op
{
  return detail::machine_op(
      [](core::machine& target) { return target.virtual_clock().now().value; });
}

auto machine_handle::op_pending_events() const -> access_op
{
  return detail::machine_op([](core::machine& target) { return target.pending_events(); });
}
}
