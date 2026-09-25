#include "elf_image.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/hash/sha256.hpp>

#include <elf.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace popcorn::testing
{
namespace
{
template <typename T> auto read_at(std::vector<char> const& file, std::size_t offset) -> T
{
  if(offset + sizeof(T) > file.size())
  {
    throw image_error{"header", 0, std::format("truncated ELF at offset {:#x}", offset)};
  }
  T value;
  std::memcpy(&value, file.data() + offset, sizeof(T));
  return value;
}

auto bytes_at(std::vector<char> const& file, std::size_t offset, std::size_t size, std::string name)
    -> std::vector<std::byte>
{
  if(offset + size > file.size())
  {
    throw image_error{std::move(name), 0, std::format("truncated ELF at offset {:#x}", offset)};
  }
  auto const* first = reinterpret_cast<std::byte const*>(file.data() + offset);
  return {first, first + size};
}

auto string_at(std::vector<char> const& file, std::size_t table, std::uint32_t index) -> std::string
{
  if(table + index >= file.size())
  {
    throw image_error{"strtab", 0, std::format("string {} past the end of the file", index)};
  }
  return std::string{file.data() + table + index};
}

void read_symbols(std::vector<char> const& file, Elf32_Ehdr const& header, image::image& out)
{
  for(std::size_t i = 0; i < header.e_shnum; ++i)
  {
    auto const section = read_at<Elf32_Shdr>(file, header.e_shoff + i * header.e_shentsize);
    if(section.sh_type != SHT_SYMTAB)
    {
      continue;
    }
    auto const strings =
        read_at<Elf32_Shdr>(file, header.e_shoff + section.sh_link * header.e_shentsize);
    for(std::size_t at = 0; at + sizeof(Elf32_Sym) <= section.sh_size; at += sizeof(Elf32_Sym))
    {
      auto const sym  = read_at<Elf32_Sym>(file, section.sh_offset + at);
      auto const type = ELF32_ST_TYPE(sym.st_info);
      if(type != STT_FUNC and type != STT_OBJECT)
      {
        continue;
      }
      auto const function = type == STT_FUNC;
      out.symbols.push_back({
          .name   = string_at(file, strings.sh_offset, sym.st_name),
          .value  = function ? sym.st_value & ~Elf32_Addr{1} : sym.st_value,
          .size   = sym.st_size,
          .kind   = function ? image::symbol_kind::function : image::symbol_kind::object,
          .global = ELF32_ST_BIND(sym.st_info) == STB_GLOBAL,
      });
    }
  }
  std::ranges::stable_sort(out.symbols, {}, &image::symbol::value);
}
}

auto read_elf(std::filesystem::path const& path) -> image::image
{
  std::ifstream     stream{path, std::ios::binary};
  std::vector<char> file{std::istreambuf_iterator<char>{stream}, {}};
  auto const        header = read_at<Elf32_Ehdr>(file, 0);
  auto const        is_elf = std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0;
  auto const is_arm32      = header.e_ident[EI_CLASS] == ELFCLASS32 and header.e_machine == EM_ARM;
  if(not is_elf or not is_arm32)
  {
    throw image_error{"header", 0, "not an Arm ELF32 file: " + path.string()};
  }

  image::image out;
  for(std::size_t i = 0; i < header.e_phnum; ++i)
  {
    auto const program = read_at<Elf32_Phdr>(file, header.e_phoff + i * header.e_phentsize);
    if(program.p_type != PT_LOAD or program.p_memsz == 0)
    {
      continue;
    }
    auto name = std::to_string(i);
    out.segments.push_back({
        .name        = name,
        .start       = program.p_paddr,
        .bytes       = bytes_at(file, program.p_offset, program.p_filesz, name),
        .memory_size = program.p_paddr == program.p_vaddr ? program.p_memsz : program.p_filesz,
        .executable  = (program.p_flags & PF_X) != 0,
        .writable    = (program.p_flags & PF_W) != 0,
    });
  }
  std::ranges::sort(out.segments, {}, &image::segment::start);
  read_symbols(file, header, out);
  out.entry = header.e_entry;

  reflex::sha256 digest;
  for(auto const& seg : out.segments)
  {
    digest.update(std::span<std::byte const>{seg.bytes});
  }
  auto const hex = reflex::to_hex(digest.finish());
  out.digest     = std::string{hex.begin(), hex.end()};
  return out;
}

auto find_symbol(image::image const& loaded, std::string_view name) -> address
{
  auto const named  = [&](image::symbol const& sym) { return sym.name == name; };
  auto const global = std::ranges::find_if(
      loaded.symbols, [&](auto const& sym) { return named(sym) and sym.global; });
  if(global != loaded.symbols.end())
  {
    return global->value;
  }
  auto const any = std::ranges::find_if(loaded.symbols, named);
  if(any == loaded.symbols.end())
  {
    throw symbol_error{std::string{name}, {}};
  }
  return any->value;
}
}
