#include "errors.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/register_view.hpp>
#include <popcorn/periph/register_file.hpp>

#include <nanobind/nanobind.h>

#include <cstddef>
#include <format>
#include <ranges>
#include <span>
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
}

auto machine_handle::read_memory(address at, std::uint32_t size) const -> reflex::py::nb::bytes
{
  require_in_space(at, size);
  std::vector<std::byte> bytes(size);
  if(auto const read = machine_->mem().read_block(at, bytes); not read)
  {
    bus_failure(read.error());
  }
  return reflex::py::nb::bytes(bytes.data(), bytes.size());
}

auto machine_handle::register_names() const -> std::vector<std::string>
{
  return machine_->regs().list()
       | std::views::transform([](arch::cpu_register const& reg) { return std::string{reg.name}; })
       | std::ranges::to<std::vector>();
}

auto machine_handle::read_register(std::uint32_t index) const -> std::uint64_t
{
  return machine_->regs().read(register_at(*machine_, index));
}

void machine_handle::write_register(std::uint32_t index, std::uint64_t value)
{
  auto const reg = register_at(*machine_, index);
  if(not core::fits_register(value, reg.bits))
  {
    throw config_error(
        "registers", 0, std::string{reg.name}, std::format("a {}-bit value", reg.bits));
  }
  machine_->regs().write(reg, value);
}

auto machine_handle::peek(std::string const& peripheral, std::uint32_t offset, std::uint8_t width)
    -> std::uint32_t
{
  auto const at = slot_of(*machine_, peripheral, offset, width);
  return (at.file->peek(at.word) >> at.shift) & at.mask;
}

void machine_handle::poke(
    std::string const& peripheral,
    std::uint32_t      offset,
    std::uint8_t       width,
    std::uint32_t      value)
{
  auto const at = slot_of(*machine_, peripheral, offset, width);
  if((value & ~at.mask) != 0)
  {
    throw config_error(
        peripheral, 0, std::format("{:#x}", offset), std::format("a {}-bit value", width));
  }
  auto const word = at.file->peek(at.word);
  at.file->poke(at.word, (word & ~(at.mask << at.shift)) | (value << at.shift));
}
}
