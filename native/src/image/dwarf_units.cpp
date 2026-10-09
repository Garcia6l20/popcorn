#include "dwarf_units.hpp"

#include "dwarf_reader.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace popcorn::image::detail
{
namespace
{
constexpr std::uint64_t dw_tag_compile_unit = 0x11;
constexpr std::uint64_t dw_tag_partial_unit = 0x3c;
constexpr std::uint8_t  dw_ut_compile       = 0x01;
constexpr std::uint8_t  dw_ut_partial       = 0x03;

constexpr std::uint64_t dw_at_stmt_list        = 0x10;
constexpr std::uint64_t dw_at_name             = 0x03;
constexpr std::uint64_t dw_at_comp_dir         = 0x1b;
constexpr std::uint64_t dw_at_str_offsets_base = 0x72;
constexpr std::uint64_t dw_at_macros           = 0x79;
constexpr std::uint64_t dw_at_gnu_macros       = 0x2119;
constexpr std::uint64_t dw_form_implicit_const = 0x21;

struct attr_spec
{
  std::uint64_t attr{};
  std::uint64_t form{};
  std::int64_t  implicit_const{};
};

struct abbrev_decl
{
  std::uint64_t          tag{};
  bool                   has_children{};
  std::vector<attr_spec> attrs;
};

auto parse_abbrev_table(std::span<std::byte const> abbrev, std::uint64_t start)
    -> std::unordered_map<std::uint64_t, abbrev_decl>
{
  std::unordered_map<std::uint64_t, abbrev_decl> table;
  dwarf_cursor                                   cursor{abbrev, ".debug_abbrev"};
  cursor.seek(static_cast<std::size_t>(start));
  for(;;)
  {
    auto const code = cursor.uleb();
    if(code == 0)
    {
      break;
    }
    abbrev_decl decl;
    decl.tag          = cursor.uleb();
    decl.has_children = cursor.u8() != 0;
    for(;;)
    {
      auto const   attr           = cursor.uleb();
      auto const   form           = cursor.uleb();
      std::int64_t implicit_const = 0;
      if(form == dw_form_implicit_const)
      {
        implicit_const = cursor.sleb();
      }
      if(attr == 0 && form == 0)
      {
        break;
      }
      decl.attrs.push_back({attr, form, implicit_const});
    }
    table[code] = std::move(decl);
  }
  return table;
}

struct deferred_string
{
  std::string                  direct{};
  std::optional<std::uint64_t> strx{};

  void read(form_value const& value)
  {
    if(value.is_strx)
    {
      strx = value.u;
    }
    else if(value.is_string)
    {
      direct = std::string{value.s};
      strx.reset();
    }
  }

  [[nodiscard]] auto resolved(
      std::optional<std::uint64_t> str_offsets_base,
      debug_sections const&        sections,
      bool                         is64) const -> std::string
  {
    if(strx)
    {
      return std::string{
          resolve_strx(*strx, str_offsets_base, sections.str_offsets, sections.str, is64)};
    }
    return direct;
  }
};

struct first_die
{
  std::uint64_t                tag{};
  std::optional<std::uint64_t> stmt_list{};
  std::string                  comp_dir{};
  deferred_string              name{};
  std::optional<std::uint64_t> macros{};
  std::optional<std::uint64_t> str_offsets_base{};
  bool                         is64{};
};

struct first_dies
{
  std::vector<first_die> dies;
  std::size_t            abbrev_tables_parsed{};
};

auto read_first_dies(debug_sections const& sections) -> first_dies
{
  first_dies                                                                        scan;
  std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, abbrev_decl>> abbrev_cache;

  dwarf_cursor cursor{sections.info, ".debug_info"};
  while(!cursor.at_end())
  {
    bool       is64        = false;
    auto const unit_length = read_initial_length(cursor, is64);
    auto const next_cu =
        checked_unit_end(cursor.pos(), unit_length, sections.info.size(), ".debug_info");

    auto const    version = cursor.u16();
    std::uint64_t abbrev_offset{};
    if(version < 2 || version > 5)
    {
      throw image_error{".debug_info", 0, "unsupported compile unit version"};
    }
    if(version >= 5)
    {
      auto const unit_type = cursor.u8();
      std::ignore          = cursor.u8();
      abbrev_offset        = read_offset(cursor, is64);
      if(unit_type != dw_ut_compile && unit_type != dw_ut_partial)
      {
        cursor.seek(next_cu);
        continue;
      }
    }
    else
    {
      abbrev_offset = read_offset(cursor, is64);
      std::ignore   = cursor.u8();
    }

    auto const [cache_it, inserted] = abbrev_cache.try_emplace(abbrev_offset);
    if(inserted)
    {
      cache_it->second = parse_abbrev_table(sections.abbrev, abbrev_offset);
      ++scan.abbrev_tables_parsed;
    }
    auto const& table = cache_it->second;
    auto const  code  = cursor.uleb();
    auto const  it    = table.find(code);

    first_die die{.is64 = is64};
    if(it != table.end()
       and (it->second.tag == dw_tag_compile_unit or it->second.tag == dw_tag_partial_unit))
    {
      die.tag = it->second.tag;
      deferred_string comp_dir;
      for(auto const& spec : it->second.attrs)
      {
        auto const value = read_form(
            cursor, spec.form, sections.str, sections.line_str, is64, spec.implicit_const);
        switch(spec.attr)
        {
          case dw_at_stmt_list:
            die.stmt_list = value.u;
            break;
          case dw_at_comp_dir:
            comp_dir.read(value);
            break;
          case dw_at_name:
            die.name.read(value);
            break;
          case dw_at_str_offsets_base:
            die.str_offsets_base = value.u;
            break;
          case dw_at_macros:
          case dw_at_gnu_macros:
            die.macros = value.u;
            break;
          default:
            break;
        }
      }
      die.comp_dir = comp_dir.resolved(die.str_offsets_base, sections, is64);
    }
    scan.dies.push_back(std::move(die));
    cursor.seek(next_cu);
  }
  return scan;
}
}

auto resolve_strx(
    std::uint64_t                index,
    std::optional<std::uint64_t> str_offsets_base,
    std::span<std::byte const>   str_offsets,
    std::span<std::byte const>   str,
    bool                         is64) -> std::string_view
{
  auto const base       = static_cast<std::size_t>(str_offsets_base.value_or(is64 ? 16 : 8));
  auto const entry_size = static_cast<std::size_t>(is64 ? 8 : 4);
  if(index > str_offsets.size() / entry_size)
  {
    throw image_error{".debug_str_offsets", 0, "strx index runs past the section"};
  }
  auto const pos = checked_offset(
      base, static_cast<std::uint64_t>(index) * entry_size, str_offsets.size(),
      ".debug_str_offsets");
  dwarf_cursor cursor{str_offsets, ".debug_str_offsets"};
  cursor.seek(pos);
  auto const str_off = is64 ? cursor.u64() : cursor.u32();
  return read_cstr_at(str, str_off, ".debug_str");
}

auto unit_heads(debug_sections const& sections) -> std::vector<unit_head>
{
  std::vector<unit_head> heads;
  for(auto& die : read_first_dies(sections).dies)
  {
    if(die.tag == dw_tag_compile_unit)
    {
      heads.push_back(
          {.name             = die.name.resolved(die.str_offsets_base, sections, die.is64),
           .macros           = die.macros,
           .str_offsets_base = die.str_offsets_base,
           .is64             = die.is64});
    }
  }
  return heads;
}

auto scan_compile_units(debug_sections const& sections) -> compile_unit_scan
{
  auto              dies = read_first_dies(sections);
  compile_unit_scan scan{.units = {}, .abbrev_tables_parsed = dies.abbrev_tables_parsed};
  std::unordered_set<std::uint64_t> seen_line_offsets;
  for(auto& die : dies.dies)
  {
    if(die.stmt_list and seen_line_offsets.insert(*die.stmt_list).second)
    {
      scan.units.push_back({.line_offset = *die.stmt_list, .comp_dir = std::move(die.comp_dir)});
    }
  }
  return scan;
}

auto compile_units(debug_sections const& sections) -> std::vector<compile_unit_ref>
{
  return scan_compile_units(sections).units;
}

namespace
{
constexpr std::uint64_t dw_tag_subprogram     = 0x2e;
constexpr std::uint64_t dw_at_low_pc          = 0x11;
constexpr std::uint64_t dw_at_high_pc         = 0x12;
constexpr std::uint64_t dw_at_abstract_origin = 0x31;
constexpr std::uint64_t dw_at_decl_file       = 0x3a;
constexpr std::uint64_t dw_at_decl_line       = 0x3b;
constexpr std::uint64_t dw_at_specification   = 0x47;
constexpr std::uint64_t dw_at_addr_base       = 0x73;
constexpr std::uint64_t dw_form_addr          = 0x01;
constexpr std::uint64_t dw_form_ref_addr      = 0x10;
constexpr std::uint64_t dw_form_ref1          = 0x11;
constexpr std::uint64_t dw_form_ref_udata     = 0x15;
constexpr std::uint64_t dw_form_addrx         = 0x1b;
constexpr std::uint64_t dw_form_addrx1        = 0x29;
constexpr std::uint64_t dw_form_addrx4        = 0x2c;
constexpr std::size_t   max_origin_depth      = 8;
constexpr std::uint64_t no_line_program       = ~std::uint64_t{0};

auto is_addrx(std::uint64_t form) noexcept -> bool
{
  return form == dw_form_addrx or (form >= dw_form_addrx1 and form <= dw_form_addrx4);
}

struct declaring_die
{
  std::uint64_t                line_offset{};
  std::optional<std::uint32_t> file{};
  std::optional<std::uint32_t> line{};
  std::optional<std::uint64_t> origin{};
};

struct code_die
{
  std::uint64_t offset{};
  std::uint64_t low{};
  std::uint64_t high{};
};

auto indexed_address(
    std::span<std::byte const>   addr,
    std::optional<std::uint64_t> base,
    std::uint64_t                index) -> std::optional<std::uint64_t>
{
  constexpr std::uint64_t entry = 4;
  if(not base or index > addr.size() / entry)
  {
    return std::nullopt;
  }
  auto const at = *base + index * entry;
  if(at + entry > addr.size())
  {
    return std::nullopt;
  }
  dwarf_cursor cursor{addr, ".debug_addr"};
  cursor.seek(static_cast<std::size_t>(at));
  return cursor.u32();
}
}

auto scan_functions(debug_sections const& sections) -> std::vector<function_die>
{
  std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, abbrev_decl>> abbrev_cache;
  std::unordered_map<std::uint64_t, declaring_die>                                  declaring;
  std::vector<code_die>                                                             code;

  dwarf_cursor cursor{sections.info, ".debug_info"};
  while(!cursor.at_end())
  {
    auto const unit_start  = cursor.pos();
    bool       is64        = false;
    auto const unit_length = read_initial_length(cursor, is64);
    auto const next_cu =
        checked_unit_end(cursor.pos(), unit_length, sections.info.size(), ".debug_info");
    auto const version = cursor.u16();
    if(version < 2 || version > 5)
    {
      throw image_error{".debug_info", 0, "unsupported compile unit version"};
    }
    std::uint64_t abbrev_offset{};
    if(version >= 5)
    {
      auto const unit_type = cursor.u8();
      std::ignore          = cursor.u8();
      abbrev_offset        = read_offset(cursor, is64);
      if(unit_type != dw_ut_compile && unit_type != dw_ut_partial)
      {
        cursor.seek(next_cu);
        continue;
      }
    }
    else
    {
      abbrev_offset = read_offset(cursor, is64);
      std::ignore   = cursor.u8();
    }
    auto const [cache_it, inserted] = abbrev_cache.try_emplace(abbrev_offset);
    if(inserted)
    {
      cache_it->second = parse_abbrev_table(sections.abbrev, abbrev_offset);
    }
    auto const& table = cache_it->second;

    std::uint64_t                line_offset = no_line_program;
    std::optional<std::uint64_t> addr_base;
    while(cursor.pos() < next_cu)
    {
      auto const offset = cursor.pos();
      auto const number = cursor.uleb();
      if(number == 0)
      {
        continue;
      }
      auto const found = table.find(number);
      if(found == table.end())
      {
        throw image_error{".debug_info", 0, "DIE with an unknown abbreviation code"};
      }
      auto const&                  decl = found->second;
      declaring_die                held{.line_offset = line_offset};
      std::optional<std::uint64_t> low;
      std::optional<std::uint64_t> high;
      bool                         low_indexed  = false;
      bool                         high_address = false;
      for(auto const& spec : decl.attrs)
      {
        auto const value = read_form(
            cursor, spec.form, sections.str, sections.line_str, is64, spec.implicit_const);
        switch(spec.attr)
        {
          case dw_at_stmt_list:
            line_offset = value.u;
            break;
          case dw_at_addr_base:
            addr_base = value.u;
            break;
          case dw_at_low_pc:
            low         = value.u;
            low_indexed = is_addrx(spec.form);
            break;
          case dw_at_high_pc:
            high         = value.u;
            high_address = spec.form == dw_form_addr or is_addrx(spec.form);
            if(is_addrx(spec.form))
            {
              high = indexed_address(sections.addr, addr_base, value.u);
            }
            break;
          case dw_at_decl_file:
            held.file = static_cast<std::uint32_t>(value.u);
            break;
          case dw_at_decl_line:
            held.line = static_cast<std::uint32_t>(value.u);
            break;
          case dw_at_abstract_origin:
          case dw_at_specification:
            if(spec.form == dw_form_ref_addr)
            {
              held.origin = value.u;
            }
            else if(spec.form >= dw_form_ref1 and spec.form <= dw_form_ref_udata)
            {
              held.origin = unit_start + value.u;
            }
            break;
          default:
            break;
        }
      }
      if(decl.tag != dw_tag_subprogram)
      {
        continue;
      }
      held.line_offset = line_offset;
      declaring.insert_or_assign(offset, held);
      if(low and low_indexed)
      {
        low = indexed_address(sections.addr, addr_base, *low);
      }
      if(low and high)
      {
        auto const end = high_address ? *high : *low + *high;
        if(end > *low)
        {
          code.push_back({.offset = offset, .low = *low, .high = end});
        }
      }
    }
    cursor.seek(next_cu);
  }

  std::vector<function_die> functions;
  for(auto const& held : code)
  {
    std::optional<std::pair<std::uint64_t, std::uint32_t>> file;
    std::optional<std::uint32_t>                           line;
    auto                                                   at = held.offset;
    for(std::size_t depth = 0; depth < max_origin_depth and (not file or not line); ++depth)
    {
      auto const found = declaring.find(at);
      if(found == declaring.end())
      {
        break;
      }
      auto const& die = found->second;
      if(not file and die.file)
      {
        file = std::pair{die.line_offset, *die.file};
      }
      if(not line and die.line)
      {
        line = die.line;
      }
      if(not die.origin)
      {
        break;
      }
      at = *die.origin;
    }
    if(file and line and *line != 0)
    {
      functions.push_back(
          {.low         = held.low,
           .high        = held.high,
           .line_offset = file->first,
           .file        = file->second,
           .line        = *line});
    }
  }
  auto const key = [](function_die const& die) {
    return std::tuple{die.low, die.high, die.line_offset, die.file, die.line};
  };
  std::ranges::sort(functions, {}, key);
  auto const repeats = std::ranges::unique(functions, {}, key);
  functions.erase(repeats.begin(), repeats.end());
  return functions;
}
}
