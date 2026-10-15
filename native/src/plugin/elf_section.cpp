#include "elf_section.hpp"

#include <elf.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace popcorn::plugin
{
namespace
{
constexpr std::string_view section_name = ".popcorn_abi";
constexpr std::uint64_t    max_names    = std::uint64_t{1} << 20;
constexpr std::uint64_t    max_tag      = 4096;

class reader
{
public:
  explicit reader(std::filesystem::path const& path) : in_(path, std::ios::binary)
  {
    std::error_code error;
    size_ = std::filesystem::file_size(path, error);
    if(error)
    {
      size_ = 0;
    }
  }

  auto read(std::uint64_t offset, std::span<std::byte> into) -> bool
  {
    if(not in_ or offset > size_ or into.size() > size_ - offset)
    {
      return false;
    }
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size()));
    return static_cast<bool>(in_);
  }

  template <typename T> auto read(std::uint64_t offset, T& value) -> bool
  {
    return read(offset, std::as_writable_bytes(std::span{&value, 1}));
  }

private:
  std::ifstream in_;
  std::uint64_t size_ = 0;
};

auto is_host_elf64(Elf64_Ehdr const& header) -> bool
{
  auto const data = std::endian::native == std::endian::little ? ELFDATA2LSB : ELFDATA2MSB;
  return std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0
     and header.e_ident[EI_CLASS] == ELFCLASS64
     and header.e_ident[EI_DATA] == data
     and header.e_shentsize == sizeof(Elf64_Shdr)
     and header.e_shnum != 0
     and header.e_shstrndx < header.e_shnum;
}
}

auto read_abi_section(std::filesystem::path const& path) -> std::optional<std::string>
{
  reader     file{path};
  Elf64_Ehdr header{};
  if(not file.read(0, header) or not is_host_elf64(header))
  {
    return std::nullopt;
  }
  std::vector<Elf64_Shdr> sections(header.e_shnum);
  if(not file.read(header.e_shoff, std::as_writable_bytes(std::span{sections})))
  {
    return std::nullopt;
  }
  auto const& names_header = sections[header.e_shstrndx];
  if(names_header.sh_type != SHT_STRTAB or names_header.sh_size > max_names)
  {
    return std::nullopt;
  }
  std::vector<char> names(names_header.sh_size);
  if(not file.read(names_header.sh_offset, std::as_writable_bytes(std::span{names})))
  {
    return std::nullopt;
  }
  auto const named = [&names](Elf64_Shdr const& section) {
    if(section.sh_name >= names.size())
    {
      return false;
    }
    auto const rest = std::string_view{names.data(), names.size()}.substr(section.sh_name);
    return rest.substr(0, rest.find('\0')) == section_name;
  };
  auto const found = std::ranges::find_if(sections, named);
  if(found == sections.end() or found->sh_type == SHT_NOBITS)
  {
    return std::nullopt;
  }
  std::vector<char> bytes(std::min(found->sh_size, max_tag));
  if(not file.read(found->sh_offset, std::as_writable_bytes(std::span{bytes})))
  {
    return std::nullopt;
  }
  auto const end = std::ranges::find(bytes, '\0');
  if(end == bytes.end())
  {
    return std::nullopt;
  }
  return std::string{bytes.begin(), end};
}
}
