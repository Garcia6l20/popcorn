#include <popcorn/core/smoke.hpp>

#include <popcorn/core/errors.hpp>

#include <unicorn/unicorn.h>

#include <elf.h>

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace popcorn::core
{
namespace
{
constexpr std::uint64_t flash_base = 0x0800'0000;
constexpr std::uint64_t flash_size = 2048 * 1024;
constexpr std::uint64_t sram_base  = 0x2000'0000;
constexpr std::uint64_t sram_size  = 768 * 1024;
constexpr std::uint32_t excp_bkpt  = 7;
constexpr std::size_t   max_steps  = 1'000'000;

void check(uc_err err, char const* what)
{
  if(err != UC_ERR_OK)
  {
    throw emulation_error{std::string{what} + ": " + uc_strerror(err)};
  }
}

template <typename T> auto read_at(std::vector<char> const& image, std::size_t offset) -> T
{
  if(offset + sizeof(T) > image.size())
  {
    throw image_error{"header", 0, std::format("truncated ELF at offset {:#x}", offset)};
  }
  T value;
  std::memcpy(&value, image.data() + offset, sizeof(T));
  return value;
}

void on_interrupt(uc_engine* engine, std::uint32_t number, void* user)
{
  *static_cast<std::uint32_t*>(user) = number;
  uc_emu_stop(engine);
}
}

auto run_to_bkpt(engine_handle const& engine, std::filesystem::path const& elf) -> std::uint32_t
{
  std::ifstream     file{elf, std::ios::binary};
  std::vector<char> image{std::istreambuf_iterator<char>{file}, {}};
  auto const        header   = read_at<Elf32_Ehdr>(image, 0);
  auto const        is_elf   = std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0;
  auto const        is_arm32 = header.e_ident[EI_CLASS] == ELFCLASS32 && header.e_machine == EM_ARM;
  if(!is_elf || !is_arm32)
  {
    throw image_error{"header", 0, "not an ARM ELF32 file: " + elf.string()};
  }

  auto* uc = engine.get();
  check(uc_mem_map(uc, flash_base, flash_size, UC_PROT_READ | UC_PROT_EXEC), "map flash");
  check(uc_mem_map(uc, sram_base, sram_size, UC_PROT_ALL), "map sram");
  for(std::size_t i = 0; i < header.e_phnum; ++i)
  {
    auto const segment = read_at<Elf32_Phdr>(image, header.e_phoff + i * header.e_phentsize);
    if(segment.p_type != PT_LOAD || segment.p_filesz == 0)
    {
      continue;
    }
    if(segment.p_offset + segment.p_filesz > image.size())
    {
      throw image_error{
          std::to_string(i), static_cast<address>(segment.p_paddr), "truncated ELF segment"};
    }
    check(
        uc_mem_write(uc, segment.p_paddr, image.data() + segment.p_offset, segment.p_filesz),
        "load segment");
  }

  std::uint32_t vectors[2] = {};
  check(uc_mem_read(uc, flash_base, vectors, sizeof vectors), "read vectors");
  check(uc_reg_write(uc, UC_ARM_REG_SP, &vectors[0]), "write sp");

  std::uint32_t interrupt = 0;
  uc_hook       hook      = 0;
  check(
      uc_hook_add(
          uc, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_interrupt), &interrupt, 1, 0),
      "hook interrupts");
  check(uc_emu_start(uc, vectors[1] | 1, 0, 0, max_steps), "run");
  check(uc_hook_del(uc, hook), "unhook interrupts");
  if(interrupt != excp_bkpt)
  {
    throw emulation_error{"firmware stopped without reaching bkpt"};
  }

  std::uint32_t r0 = 0;
  check(uc_reg_read(uc, UC_ARM_REG_R0, &r0), "read r0");
  return r0;
}
}
