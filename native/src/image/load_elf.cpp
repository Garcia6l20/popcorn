#include "arm_attributes.hpp"
#include "digest.hpp"
#include "dwarf_lines.hpp"
#include "dwarf_units.hpp"
#include "elf_file.hpp"

#include <popcorn/image/image.hpp>

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace popcorn::image
{
namespace
{
using detail::checked_span;
using detail::elf_file;
using detail::read;
using detail::read_ntbs;

auto segment_name(elf_file const& elf, std::size_t index, Elf32_Phdr const& ph) -> std::string
{
  std::string name = std::to_string(index);
  for(auto const& sh : elf.sections())
  {
    if((sh.sh_flags & SHF_ALLOC) == 0 || sh.sh_size == 0)
    {
      continue;
    }
    bool const within =
        sh.sh_offset >= ph.p_offset
        and std::size_t{sh.sh_offset} + sh.sh_size <= std::size_t{ph.p_offset} + ph.p_filesz;
    if(within)
    {
      name += ' ';
      name += elf.section_name(sh);
    }
  }
  return name;
}

auto is_mapping_symbol(std::string_view name) -> bool
{
  if(name.empty() || name[0] != '$')
  {
    return false;
  }
  if(name.size() == 1)
  {
    return false;
  }
  char const kind = name[1];
  if(kind != 'a' && kind != 'd' && kind != 't' && kind != 'x')
  {
    return false;
  }
  return name.size() == 2 || name[2] == '.';
}

auto is_iar_label(std::string_view name, unsigned char info) -> bool
{
  return !name.empty()
     and name[0] == '?'
     and ELF32_ST_TYPE(info) == STT_NOTYPE
     and ELF32_ST_BIND(info) == STB_LOCAL;
}

auto load_segments(elf_file const& elf) -> std::vector<segment>
{
  std::vector<segment> segments;
  auto const           bytes = elf.bytes();

  for(auto const [index, ph] : std::views::enumerate(elf.program_headers()))
  {
    if(ph.p_type != PT_LOAD || ph.p_filesz == 0)
    {
      continue;
    }
    if(ph.p_memsz < ph.p_filesz)
    {
      throw image_error{
          std::to_string(index), static_cast<address>(ph.p_paddr), "memory size below file size"};
    }

    auto const file_bytes = checked_span(bytes, ph.p_offset, ph.p_filesz, "PT_LOAD segment");
    std::vector<std::byte> data(file_bytes.begin(), file_bytes.end());

    auto const memory_size = ph.p_paddr == ph.p_vaddr ? static_cast<std::uint32_t>(ph.p_memsz)
                                                      : static_cast<std::uint32_t>(ph.p_filesz);
    if(std::uint64_t{ph.p_paddr} + memory_size > std::uint64_t{1} << 32)
    {
      throw image_error{
          std::to_string(index), static_cast<address>(ph.p_paddr),
          "segment runs past the top of the address space"};
    }

    segments.push_back(
        segment{
            .name        = segment_name(elf, static_cast<std::size_t>(index), ph),
            .start       = static_cast<address>(ph.p_paddr),
            .bytes       = std::move(data),
            .memory_size = memory_size,
            .executable  = (ph.p_flags & PF_X) != 0,
            .writable    = (ph.p_flags & PF_W) != 0,
        });
  }

  std::ranges::sort(segments, {}, &segment::start);

  for(std::size_t i = 1; i < segments.size(); ++i)
  {
    auto const& prev = segments[i - 1];
    auto const& curr = segments[i];
    if(std::uint64_t{curr.start} < std::uint64_t{prev.start} + prev.memory_size)
    {
      throw image_error{curr.name, curr.start, std::format("overlaps segment \"{}\"", prev.name)};
    }
  }

  return segments;
}

auto load_symbols(elf_file const& elf) -> std::vector<symbol>
{
  std::vector<symbol> symbols;

  Elf32_Shdr const* symtab_sh = nullptr;
  for(auto const& sh : elf.sections())
  {
    if(sh.sh_type == SHT_SYMTAB)
    {
      symtab_sh = &sh;
      break;
    }
  }
  if(symtab_sh == nullptr)
  {
    return symbols;
  }

  auto const symtab_bytes = elf.section_bytes(*symtab_sh);
  if(symtab_bytes.empty() || symtab_sh->sh_link >= elf.sections().size())
  {
    return symbols;
  }
  auto const& strtab_sh    = elf.sections()[symtab_sh->sh_link];
  auto const  strtab_bytes = elf.section_bytes(strtab_sh);
  if(strtab_bytes.empty())
  {
    return symbols;
  }

  auto const count = symtab_bytes.size() / sizeof(Elf32_Sym);
  for(std::size_t i = 0; i < count; ++i)
  {
    auto const sym = read<Elf32_Sym>(symtab_bytes, i * sizeof(Elf32_Sym), "symbol table entry");
    if(sym.st_shndx == SHN_UNDEF)
    {
      continue;
    }
    auto const type = ELF32_ST_TYPE(sym.st_info);
    if(type == STT_SECTION || type == STT_FILE)
    {
      continue;
    }
    if(sym.st_name >= strtab_bytes.size())
    {
      continue;
    }
    std::size_t            name_pos = sym.st_name;
    std::string_view const name     = read_ntbs(strtab_bytes, name_pos);
    if(name.empty() || is_mapping_symbol(name) || is_iar_label(name, sym.st_info))
    {
      continue;
    }

    auto const  bind  = ELF32_ST_BIND(sym.st_info);
    symbol_kind kind  = symbol_kind::other;
    address     value = sym.st_value;
    if(type == STT_FUNC)
    {
      kind = symbol_kind::function;
      value &= ~address{1};
    }
    else if(type == STT_OBJECT)
    {
      kind = symbol_kind::object;
    }

    symbols.push_back(
        symbol{
            .name   = std::string{name},
            .value  = value,
            .size   = static_cast<std::uint32_t>(sym.st_size),
            .kind   = kind,
            .global = bind == STB_GLOBAL || bind == STB_WEAK,
        });
  }

  std::ranges::sort(symbols, [](symbol const& lhs, symbol const& rhs) {
    return lhs.value != rhs.value ? lhs.value < rhs.value : lhs.name < rhs.name;
  });
  return symbols;
}

auto load_abi(elf_file const& elf) -> arch::float_abi
{
  if((elf.header().e_flags & EF_ARM_ABI_FLOAT_HARD) == 0)
  {
    return arch::float_abi::soft;
  }
  auto const attributes = elf.section(".ARM.attributes");
  if(!attributes)
  {
    return arch::float_abi::hard_double;
  }
  return detail::hard_float_abi(*attributes);
}

constexpr std::uint64_t address_end = std::uint64_t{1} << 32;

auto debug_section_bytes(elf_file const& elf, std::string_view name) -> std::span<std::byte const>
{
  auto const* sh = elf.section_header(name);
  if(sh == nullptr)
  {
    return {};
  }
  if((sh->sh_flags & SHF_COMPRESSED) != 0)
  {
    throw image_error{std::string{name}, 0, "compressed debug sections are not supported"};
  }
  return elf.section_bytes(*sh);
}

auto debug_functions(detail::debug_sections const& sections) -> std::vector<detail::function_die>
{
  try
  {
    return detail::scan_functions(sections);
  }
  catch(image_error const&)
  {
    return {};
  }
}

void load_lines(elf_file const& elf, image& img)
{
  auto const info = debug_section_bytes(elf, ".debug_info");
  auto const line = debug_section_bytes(elf, ".debug_line");
  if(info.empty() || line.empty())
  {
    return;
  }

  detail::debug_sections const sections{
      .info        = info,
      .abbrev      = debug_section_bytes(elf, ".debug_abbrev"),
      .line        = line,
      .str         = debug_section_bytes(elf, ".debug_str"),
      .line_str    = debug_section_bytes(elf, ".debug_line_str"),
      .str_offsets = debug_section_bytes(elf, ".debug_str_offsets"),
      .addr        = debug_section_bytes(elf, ".debug_addr"),
  };

  auto const                    units     = detail::compile_units(sections);
  auto const                    functions = debug_functions(sections);
  std::vector<detail::file_ref> wanted;
  for(auto const& die : functions)
  {
    wanted.push_back({.line_offset = die.line_offset, .index = die.file});
  }
  auto table = detail::decode_lines(sections, units, wanted);
  img.files  = std::move(table.files);
  img.lines  = std::move(table.rows);
  for(auto const& [die, file] : std::views::zip(functions, table.wanted))
  {
    if(file and die.high < address_end)
    {
      img.functions.push_back(
          {.low  = static_cast<address>(die.low) & ~address{1},
           .high = static_cast<address>(die.high) & ~address{1},
           .file = *file,
           .line = die.line});
    }
  }
  auto const key = [](debug_function const& held) {
    return std::tuple{held.low, held.high, held.file, held.line};
  };
  std::ranges::sort(img.functions, {}, key);
  auto const repeats = std::ranges::unique(img.functions, {}, key);
  img.functions.erase(repeats.begin(), repeats.end());
}
}

auto load_elf(std::filesystem::path const& file) -> image
{
  detail::elf_file const elf{file};

  image img;
  img.segments = load_segments(elf);
  img.symbols  = load_symbols(elf);
  img.entry    = elf.header().e_entry == 0 ? std::optional<address>{} : elf.header().e_entry;
  img.abi      = load_abi(elf);
  load_lines(elf, img);
  img.digest = detail::digest_of(img.segments);
  return img;
}
}
