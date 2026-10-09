#include "dwarf_macros.hpp"
#include "dwarf_units.hpp"
#include "elf_file.hpp"

#include <popcorn/image/image.hpp>

#include <filesystem>

namespace popcorn::image
{
auto read_macros(std::filesystem::path const& file) -> macro_index
{
  detail::elf_file const elf{file};
  auto const             macro = detail::debug_section_bytes(elf, ".debug_macro");
  auto const             info  = detail::debug_section_bytes(elf, ".debug_info");
  if(macro.empty() or info.empty())
  {
    return {};
  }
  detail::debug_sections const sections{
      .info        = info,
      .abbrev      = detail::debug_section_bytes(elf, ".debug_abbrev"),
      .line        = detail::debug_section_bytes(elf, ".debug_line"),
      .str         = detail::debug_section_bytes(elf, ".debug_str"),
      .line_str    = detail::debug_section_bytes(elf, ".debug_line_str"),
      .str_offsets = detail::debug_section_bytes(elf, ".debug_str_offsets"),
  };
  return detail::read_macros(sections, macro).index;
}
}
