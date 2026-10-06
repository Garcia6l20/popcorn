#include "dwarf_lines.hpp"

#include "dwarf_reader.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace popcorn::image::detail
{
namespace
{
enum class dw_lnct : std::uint64_t
{
  path            = 1,
  directory_index = 2,
  timestamp       = 3,
  size            = 4,
  md5             = 5,
};

enum class dw_lns : std::uint8_t
{
  copy               = 1,
  advance_pc         = 2,
  advance_line       = 3,
  set_file           = 4,
  set_column         = 5,
  negate_stmt        = 6,
  set_basic_block    = 7,
  const_add_pc       = 8,
  fixed_advance_pc   = 9,
  set_prologue_end   = 10,
  set_epilogue_begin = 11,
  set_isa            = 12,
};

enum class dw_lne : std::uint8_t
{
  end_sequence      = 1,
  set_address       = 2,
  define_file       = 3,
  set_discriminator = 4,
};

struct raw_row
{
  address       addr{};
  std::uint32_t file{};
  std::uint32_t line{};
  bool          is_stmt{};
  bool          end_sequence{};
};

auto normalize_path(std::filesystem::path const& p) -> std::string
{
  return p.lexically_normal().generic_string();
}

void decode_one_program(
    debug_sections const&                           sections,
    compile_unit_ref const&                         unit,
    std::vector<raw_row>&                           out,
    std::unordered_map<std::string, std::uint32_t>& global_file_ids,
    std::vector<std::string>&                       global_files,
    std::span<file_ref const>                       wanted,
    std::vector<std::optional<std::string>>&        wanted_paths)
{
  dwarf_cursor cursor{sections.line, ".debug_line"};
  cursor.seek(static_cast<std::size_t>(unit.line_offset));

  bool       is64        = false;
  auto const unit_length = read_initial_length(cursor, is64);
  auto const unit_end =
      checked_unit_end(cursor.pos(), unit_length, sections.line.size(), ".debug_line");

  auto const version = cursor.u16();
  if(version < 2 || version > 5)
  {
    throw image_error{".debug_line", 0, "unsupported line program version"};
  }
  if(version >= 5)
  {
    std::ignore = cursor.u8();
    std::ignore = cursor.u8();
  }
  auto const header_length = read_offset(cursor, is64);
  auto const program_start = checked_offset(cursor.pos(), header_length, unit_end, ".debug_line");

  auto const min_insn_len = cursor.u8();
  auto const max_ops      = version >= 4 ? cursor.u8() : std::uint8_t{1};
  if(max_ops == 0)
  {
    throw image_error{".debug_line", 0, "maximum_operations_per_instruction is 0"};
  }
  auto const default_is_stmt = cursor.u8() != 0;
  auto const line_base       = static_cast<std::int8_t>(cursor.u8());
  auto const line_range      = cursor.u8();
  if(line_range == 0)
  {
    throw image_error{".debug_line", 0, "line_range is 0"};
  }
  auto const                opcode_base = cursor.u8();
  std::vector<std::uint8_t> std_opcode_lengths(opcode_base > 0 ? opcode_base - 1 : 0);
  for(auto& len : std_opcode_lengths)
  {
    len = cursor.u8();
  }

  std::vector<std::string>                           dirs;
  std::vector<std::pair<std::string, std::uint32_t>> files;

  if(version <= 4)
  {
    dirs.push_back(unit.comp_dir);
    for(;;)
    {
      auto const s = cursor.cstr();
      if(s.empty())
      {
        break;
      }
      dirs.emplace_back(s);
    }
    files.emplace_back("", 0);
    for(;;)
    {
      auto const s = cursor.cstr();
      if(s.empty())
      {
        break;
      }
      auto const dir_idx = static_cast<std::uint32_t>(cursor.uleb());
      std::ignore        = cursor.uleb();
      std::ignore        = cursor.uleb();
      files.emplace_back(std::string{s}, dir_idx);
    }
  }
  else
  {
    constexpr std::uint64_t dw_form_flag_present   = 0x19;
    constexpr std::uint64_t dw_form_implicit_const = 0x21;
    auto read_formats = [&](std::vector<std::pair<std::uint64_t, std::uint64_t>>& formats) {
      auto const count = cursor.u8();
      formats.resize(count);
      for(auto& [content, form] : formats)
      {
        content = cursor.uleb();
        form    = cursor.uleb();
        if(form == dw_form_flag_present || form == dw_form_implicit_const)
        {
          throw image_error{".debug_line", 0, "zero-byte form in line table entry format"};
        }
      }
    };
    auto read_table = [&](std::vector<std::pair<std::uint64_t, std::uint64_t>> const& formats,
                          auto&&                                                      push_entry) {
      auto const count = cursor.uleb();
      if(formats.empty() && count > 0)
      {
        throw image_error{".debug_line", 0, "entry count with no formats to read them"};
      }
      if(count > cursor.remaining())
      {
        throw image_error{".debug_line", 0, "entry count runs past the section"};
      }
      for(std::uint64_t i = 0; i < count; ++i)
      {
        push_entry(formats);
      }
    };

    std::vector<std::pair<std::uint64_t, std::uint64_t>> dir_formats;
    read_formats(dir_formats);
    read_table(dir_formats, [&](auto const& formats) {
      std::string name;
      for(auto const& [content, form] : formats)
      {
        auto const v = read_form(cursor, form, sections.str, sections.line_str, is64, 0);
        if(static_cast<dw_lnct>(content) == dw_lnct::path && v.is_string)
        {
          name = std::string{v.s};
        }
      }
      dirs.push_back(std::move(name));
    });

    std::vector<std::pair<std::uint64_t, std::uint64_t>> file_formats;
    read_formats(file_formats);
    read_table(file_formats, [&](auto const& formats) {
      std::string   name;
      std::uint32_t dir_idx{};
      for(auto const& [content, form] : formats)
      {
        auto const v  = read_form(cursor, form, sections.str, sections.line_str, is64, 0);
        auto const ct = static_cast<dw_lnct>(content);
        if(ct == dw_lnct::path && v.is_string)
        {
          name = std::string{v.s};
        }
        else if(ct == dw_lnct::directory_index)
        {
          dir_idx = static_cast<std::uint32_t>(v.u);
        }
      }
      files.emplace_back(std::move(name), dir_idx);
    });
  }

  bool const file_zero_based = version >= 5;
  auto       resolve_file    = [&](std::uint32_t file_idx) -> std::string {
    if(file_idx >= files.size())
    {
      return "<invalid>";
    }
    auto const& [name, dir_idx] = files[file_idx];
    if(name.empty())
    {
      return "";
    }
    std::filesystem::path const np{name};
    if(np.is_absolute())
    {
      return normalize_path(np);
    }
    std::string const           dir = dir_idx < dirs.size() ? dirs[dir_idx] : unit.comp_dir;
    std::filesystem::path const dp{dir};
    if(dp.is_absolute())
    {
      return normalize_path(dp / name);
    }
    if(file_zero_based && dir_idx == 0)
    {
      return normalize_path(dp / name);
    }
    return normalize_path(std::filesystem::path{unit.comp_dir} / dir / name);
  };

  for(auto const& [ref, path] : std::views::zip(wanted, wanted_paths))
  {
    if(ref.line_offset == unit.line_offset
       and ref.index < files.size()
       and not files[ref.index].first.empty())
    {
      path = resolve_file(ref.index);
    }
  }

  std::unordered_map<std::uint32_t, std::uint32_t> local_to_global;
  auto resolve_file_id = [&](std::uint32_t file_idx) -> std::uint32_t {
    auto const [it, inserted] = local_to_global.try_emplace(file_idx, 0);
    if(inserted)
    {
      auto path = resolve_file(file_idx);
      auto const [git, global_new] =
          global_file_ids.try_emplace(path, static_cast<std::uint32_t>(global_files.size()));
      if(global_new)
      {
        global_files.push_back(std::move(path));
      }
      it->second = git->second;
    }
    return it->second;
  };

  cursor.seek(program_start);
  std::uint64_t addr_reg  = 0;
  std::uint32_t op_index  = 0;
  std::uint32_t file_reg  = 1;
  std::uint32_t line_reg  = 1;
  bool          is_stmt   = default_is_stmt;
  bool          end_seq   = false;
  std::size_t   seq_start = out.size();

  auto emit = [&]() {
    out.push_back(
        raw_row{
            static_cast<popcorn::address>(addr_reg), resolve_file_id(file_reg), line_reg, is_stmt,
            end_seq});
  };
  auto emit_real = [&]() { emit(); };
  auto reset     = [&]() {
    addr_reg  = 0;
    op_index  = 0;
    file_reg  = 1;
    line_reg  = 1;
    is_stmt   = default_is_stmt;
    end_seq   = false;
    seq_start = out.size();
  };
  auto advance_addr = [&](std::uint64_t op_advance) {
    auto const new_addr_adv = (op_index + op_advance) / max_ops;
    addr_reg += min_insn_len * new_addr_adv;
    op_index = static_cast<std::uint32_t>((op_index + op_advance) % max_ops);
  };

  while(cursor.pos() < unit_end)
  {
    auto const opcode = cursor.u8();
    if(opcode == 0)
    {
      auto const len = cursor.uleb();
      if(len < 1 || len > unit_end - cursor.pos())
      {
        throw image_error{".debug_line", 0, "extended opcode length runs past the unit"};
      }
      auto const ext_end = cursor.pos() + len;
      auto const sub     = static_cast<dw_lne>(cursor.u8());
      switch(sub)
      {
        case dw_lne::end_sequence:
          while(out.size() > seq_start
                and out.back().addr == static_cast<popcorn::address>(addr_reg)
                and not out.back().end_sequence)
          {
            out.pop_back();
          }
          end_seq  = true;
          line_reg = 0;
          emit();
          reset();
          break;
        case dw_lne::set_address:
          addr_reg = cursor.u32();
          op_index = 0;
          break;
        case dw_lne::define_file:
        {
          auto const s       = cursor.cstr();
          auto const dir_idx = static_cast<std::uint32_t>(cursor.uleb());
          std::ignore        = cursor.uleb();
          std::ignore        = cursor.uleb();
          files.emplace_back(std::string{s}, dir_idx);
          break;
        }
        case dw_lne::set_discriminator:
          std::ignore = cursor.uleb();
          break;
        default:
          break;
      }
      cursor.seek(ext_end);
    }
    else if(opcode < opcode_base)
    {
      switch(static_cast<dw_lns>(opcode))
      {
        case dw_lns::copy:
          emit_real();
          break;
        case dw_lns::advance_pc:
          advance_addr(cursor.uleb());
          break;
        case dw_lns::advance_line:
          line_reg += static_cast<std::uint32_t>(cursor.sleb());
          break;
        case dw_lns::set_file:
          file_reg = static_cast<std::uint32_t>(cursor.uleb());
          break;
        case dw_lns::set_column:
          std::ignore = cursor.uleb();
          break;
        case dw_lns::negate_stmt:
          is_stmt = !is_stmt;
          break;
        case dw_lns::set_basic_block:
          break;
        case dw_lns::const_add_pc:
        {
          auto const adjusted = static_cast<std::uint8_t>(255 - opcode_base);
          advance_addr(adjusted / line_range);
          break;
        }
        case dw_lns::fixed_advance_pc:
          addr_reg += cursor.u16();
          op_index = 0;
          break;
        case dw_lns::set_prologue_end:
          break;
        case dw_lns::set_epilogue_begin:
          break;
        case dw_lns::set_isa:
          std::ignore = cursor.uleb();
          break;
        default:
          for(std::uint8_t i = 0; i < std_opcode_lengths[opcode - 1]; ++i)
          {
            std::ignore = cursor.uleb();
          }
          break;
      }
    }
    else
    {
      auto const adjusted = static_cast<std::uint8_t>(opcode - opcode_base);
      advance_addr(adjusted / line_range);
      line_reg += static_cast<std::uint32_t>(line_base + static_cast<int>(adjusted % line_range));
      emit_real();
    }
  }
}
}

auto decode_lines(
    debug_sections const&             sections,
    std::span<compile_unit_ref const> units,
    std::span<file_ref const>         wanted) -> line_table
{
  std::vector<raw_row>                           rows;
  line_table                                     table;
  std::unordered_map<std::string, std::uint32_t> global_file_ids;
  std::vector<std::optional<std::string>>        wanted_paths(wanted.size());
  for(auto const& unit : units)
  {
    decode_one_program(sections, unit, rows, global_file_ids, table.files, wanted, wanted_paths);
  }
  table.wanted.reserve(wanted.size());
  for(auto& path : wanted_paths)
  {
    if(not path)
    {
      table.wanted.emplace_back();
      continue;
    }
    auto const [held, fresh] =
        global_file_ids.try_emplace(*path, static_cast<std::uint32_t>(table.files.size()));
    if(fresh)
    {
      table.files.push_back(std::move(*path));
    }
    table.wanted.emplace_back(held->second);
  }

  std::ranges::stable_sort(rows, [](raw_row const& a, raw_row const& b) {
    if(a.addr != b.addr)
    {
      return a.addr < b.addr;
    }
    return a.end_sequence && !b.end_sequence;
  });

  table.rows.reserve(rows.size());
  for(auto const& r : rows)
  {
    table.rows.push_back(
        line_row{.addr = r.addr, .file = r.file, .line = r.line, .is_stmt = r.is_stmt});
  }
  return table;
}
}
