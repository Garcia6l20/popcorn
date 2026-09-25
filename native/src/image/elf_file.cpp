#include "elf_file.hpp"

#include <popcorn/core/errors.hpp>

#include <cstring>
#include <fstream>

namespace popcorn::image::detail
{
auto read_file(std::filesystem::path const& file) -> std::vector<std::byte>
{
  std::ifstream stream{file, std::ios::binary | std::ios::ate};
  if(!stream)
  {
    throw image_error{file.filename().string(), 0, "cannot open " + file.string()};
  }
  auto const size = stream.tellg();
  if(size < 0)
  {
    throw image_error{file.filename().string(), 0, "cannot size " + file.string()};
  }
  std::vector<std::byte> out(static_cast<std::size_t>(size));
  stream.seekg(0);
  if(!out.empty())
  {
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  }
  if(!stream)
  {
    throw image_error{file.filename().string(), 0, "failed reading " + file.string()};
  }
  return out;
}

elf_file::elf_file(std::filesystem::path const& file) : bytes_{read_file(file)}
{
  std::span<std::byte const> const view{bytes_};
  header_ = read<Elf32_Ehdr>(view, 0, "ELF header");

  bool const is_elf = std::memcmp(header_.e_ident, ELFMAG, SELFMAG) == 0;
  if(!is_elf || header_.e_ident[EI_CLASS] != ELFCLASS32 || header_.e_ident[EI_DATA] != ELFDATA2LSB)
  {
    throw image_error{"header", 0, "not a little-endian ELF32 file: " + file.string()};
  }
  if(header_.e_machine != EM_ARM)
  {
    throw image_error{"header", 0, "not an ARM ELF file: " + file.string()};
  }
  if(header_.e_type != ET_EXEC)
  {
    throw image_error{"header", 0, "not an executable ELF file: " + file.string()};
  }

  if(header_.e_phnum != 0)
  {
    if(header_.e_phentsize != sizeof(Elf32_Phdr))
    {
      throw image_error{"header", 0, "unexpected program header entry size"};
    }
    if(header_.e_phoff % alignof(Elf32_Phdr) != 0)
    {
      throw image_error{"header", 0, "misaligned program header table"};
    }
    program_headers_.reserve(header_.e_phnum);
    for(std::uint16_t i = 0; i < header_.e_phnum; ++i)
    {
      auto const offset = std::size_t{header_.e_phoff} + std::size_t{i} * sizeof(Elf32_Phdr);
      program_headers_.push_back(read<Elf32_Phdr>(view, offset, "program headers"));
    }
  }

  if(header_.e_shnum != 0)
  {
    if(header_.e_shentsize != sizeof(Elf32_Shdr))
    {
      throw image_error{"header", 0, "unexpected section header entry size"};
    }
    if(header_.e_shoff % alignof(Elf32_Shdr) != 0)
    {
      throw image_error{"header", 0, "misaligned section header table"};
    }
    if(header_.e_shstrndx == SHN_XINDEX || header_.e_shstrndx >= header_.e_shnum)
    {
      throw image_error{"header", 0, "invalid section header string table index"};
    }
    sections_.reserve(header_.e_shnum);
    for(std::uint16_t i = 0; i < header_.e_shnum; ++i)
    {
      auto const offset = std::size_t{header_.e_shoff} + std::size_t{i} * sizeof(Elf32_Shdr);
      sections_.push_back(read<Elf32_Shdr>(view, offset, "section headers"));
    }

    auto const& strtab = sections_[header_.e_shstrndx];
    auto const  strtab_bytes =
        checked_span(view, strtab.sh_offset, strtab.sh_size, "section string table");
    shstrtab_ =
        std::string_view{reinterpret_cast<char const*>(strtab_bytes.data()), strtab_bytes.size()};
  }
}

auto elf_file::header() const -> Elf32_Ehdr const&
{
  return header_;
}

auto elf_file::program_headers() const -> std::span<Elf32_Phdr const>
{
  return program_headers_;
}

auto elf_file::sections() const -> std::span<Elf32_Shdr const>
{
  return sections_;
}

auto elf_file::section_name(Elf32_Shdr const& section) const -> std::string_view
{
  if(section.sh_name >= shstrtab_.size())
  {
    return {};
  }
  auto const rest = shstrtab_.substr(section.sh_name);
  auto const nul  = rest.find('\0');
  if(nul == std::string_view::npos)
  {
    throw image_error{"header", 0, "unterminated section name"};
  }
  return rest.substr(0, nul);
}

auto elf_file::section_bytes(Elf32_Shdr const& section) const -> std::span<std::byte const>
{
  if(section.sh_type == SHT_NOBITS || section.sh_size == 0)
  {
    return {};
  }
  return checked_span(bytes_, section.sh_offset, section.sh_size, "section");
}

auto elf_file::section_header(std::string_view name) const -> Elf32_Shdr const*
{
  for(auto const& sh : sections_)
  {
    if(section_name(sh) == name)
    {
      return &sh;
    }
  }
  return nullptr;
}

auto elf_file::section(std::string_view name) const -> std::optional<std::span<std::byte const>>
{
  auto const* sh = section_header(name);
  if(sh == nullptr)
  {
    return std::nullopt;
  }
  return section_bytes(*sh);
}

auto elf_file::bytes() const -> std::span<std::byte const>
{
  return bytes_;
}
}
