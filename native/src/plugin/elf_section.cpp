#include "elf_section.hpp"

#include <elf.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace popcorn::plugin
{
namespace
{
constexpr std::string_view section_name = ".popcorn_abi";
constexpr std::uint64_t    max_names    = std::uint64_t{1} << 20;
constexpr std::uint64_t    max_tag      = 4096;
constexpr std::uint64_t    max_table    = std::uint64_t{16} << 20;

struct registry_prefix
{
  std::string_view mangled;
  std::string_view name;
};

constexpr std::array registry_functions{
    registry_prefix{"_ZN7popcorn6periph15registered_glob", "popcorn::periph::registered_glob"},
    registry_prefix{"_ZN7popcorn6periph14register_model",  "popcorn::periph::register_model" },
};

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
auto section_table(reader& file) -> std::optional<std::vector<Elf64_Shdr>>
{
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
  return sections;
}

auto contents(reader& file, Elf64_Shdr const& section, std::uint64_t cap)
    -> std::optional<std::vector<char>>
{
  if(section.sh_type == SHT_NOBITS or section.sh_size > cap)
  {
    return std::nullopt;
  }
  std::vector<char> bytes(section.sh_size);
  if(not file.read(section.sh_offset, std::as_writable_bytes(std::span{bytes})))
  {
    return std::nullopt;
  }
  return bytes;
}

auto bytes_at(std::vector<char> const& table, std::uint64_t offset, std::uint64_t length)
    -> std::string_view
{
  if(offset >= table.size())
  {
    return {};
  }
  return std::string_view{table.data(), table.size()}.substr(offset, length);
}

auto string_within(std::vector<char> const& table, std::uint64_t offset, std::uint64_t limit)
    -> std::optional<std::string_view>
{
  auto const rest = bytes_at(table, offset, limit);
  auto const end  = rest.find('\0');
  if(end == std::string_view::npos)
  {
    return std::nullopt;
  }
  return rest.substr(0, end);
}

constexpr auto longest_prefix =
    std::ranges::max(registry_functions | std::views::transform([](registry_prefix const& p) {
                       return p.mangled.size();
                     }));

template <typename Entry>
auto entries_of(reader& file, std::vector<Elf64_Shdr> const& sections, std::uint32_t type)
    -> std::optional<std::pair<std::vector<Entry>, std::vector<char>>>
{
  auto const found = std::ranges::find(sections, type, &Elf64_Shdr::sh_type);
  if(found == sections.end()
     or found->sh_link >= sections.size()
     or sections[found->sh_link].sh_type != SHT_STRTAB)
  {
    return std::nullopt;
  }
  auto raw     = contents(file, *found, max_table);
  auto strings = contents(file, sections[found->sh_link], max_table);
  if(not raw or not strings)
  {
    return std::nullopt;
  }
  std::vector<Entry> entries(raw->size() / sizeof(Entry));
  std::memcpy(entries.data(), raw->data(), entries.size() * sizeof(Entry));
  return std::pair{std::move(entries), std::move(*strings)};
}

auto registry_function(std::string_view symbol) -> std::optional<std::string_view>
{
  for(auto const& [prefix, name] : registry_functions)
  {
    if(symbol.starts_with(prefix)
       and symbol.size() > prefix.size()
       and (symbol[prefix.size()] == 'E' or symbol[prefix.size()] == 'B'))
    {
      return name;
    }
  }
  return std::nullopt;
}
}

auto read_abi_section(std::filesystem::path const& path) -> std::optional<std::string>
{
  reader     file{path};
  auto const sections = section_table(file);
  if(not sections)
  {
    return std::nullopt;
  }
  Elf64_Ehdr header{};
  file.read(0, header);
  auto const& names_header = (*sections)[header.e_shstrndx];
  if(names_header.sh_type != SHT_STRTAB)
  {
    return std::nullopt;
  }
  auto const names = contents(file, names_header, max_names);
  if(not names)
  {
    return std::nullopt;
  }
  auto const named = [&names](Elf64_Shdr const& section) {
    auto const name = bytes_at(*names, section.sh_name, section_name.size() + 1);
    return name.size() == section_name.size() + 1
       and name.starts_with(section_name)
       and name.back() == '\0';
  };
  auto const found = std::ranges::find_if(*sections, named);
  if(found == sections->end() or found->sh_type == SHT_NOBITS)
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

auto read_dynamic_links(std::filesystem::path const& path) -> std::optional<dynamic_links>
{
  reader     file{path};
  auto const sections = section_table(file);
  if(not sections)
  {
    return std::nullopt;
  }
  auto const dynamic = entries_of<Elf64_Dyn>(file, *sections, SHT_DYNAMIC);
  if(not dynamic)
  {
    return std::nullopt;
  }
  dynamic_links links;
  auto          budget = std::uint64_t{dynamic->second.size()};
  for(auto const& entry : dynamic->first | std::views::take_while([](Elf64_Dyn const& e) {
                            return e.d_tag != DT_NULL;
                          }))
  {
    if(entry.d_tag != DT_NEEDED)
    {
      continue;
    }
    auto const name = string_within(dynamic->second, entry.d_un.d_val, budget);
    if(not name)
    {
      return std::nullopt;
    }
    budget -= name->size() + 1;
    links.needed.emplace_back(*name);
  }
  if(auto const symbols = entries_of<Elf64_Sym>(file, *sections, SHT_DYNSYM))
  {
    for(auto const& symbol : symbols->first)
    {
      if(symbol.st_shndx == SHN_UNDEF)
      {
        continue;
      }
      if(auto const function =
             registry_function(bytes_at(symbols->second, symbol.st_name, longest_prefix + 1)))
      {
        links.registry_definitions.emplace_back(*function);
      }
    }
  }
  return links;
}
}
