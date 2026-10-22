#include "dwarf_macros.hpp"

#include "dwarf_reader.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace popcorn::image::detail
{
namespace
{
constexpr std::string_view section_name = ".debug_macro";

enum class op_kind : std::uint8_t
{
  define,
  undef,
  define_strx,
  undef_strx,
  import,
};

constexpr std::uint8_t dw_macro_define      = 0x01;
constexpr std::uint8_t dw_macro_undef       = 0x02;
constexpr std::uint8_t dw_macro_start_file  = 0x03;
constexpr std::uint8_t dw_macro_end_file    = 0x04;
constexpr std::uint8_t dw_macro_define_strp = 0x05;
constexpr std::uint8_t dw_macro_undef_strp  = 0x06;
constexpr std::uint8_t dw_macro_import      = 0x07;
constexpr std::uint8_t dw_macro_define_sup  = 0x08;
constexpr std::uint8_t dw_macro_import_sup  = 0x0a;
constexpr std::uint8_t dw_macro_define_strx = 0x0b;
constexpr std::uint8_t dw_macro_undef_strx  = 0x0c;
constexpr std::uint8_t dw_macro_lo_user     = 0xe0;
constexpr std::uint8_t flag_offset_size_64  = 0x01;
constexpr std::uint8_t flag_debug_line      = 0x02;
constexpr std::uint8_t flag_opcode_operands = 0x04;

struct macro_op
{
  op_kind          kind{};
  std::string_view text{};
  std::uint64_t    value{};
};

using macro_unit = std::vector<macro_op>;

struct definition
{
  std::string_view text{};
  bool             function_like{};
};

struct entry_key
{
  std::string_view name{};
  std::string_view text{};
  bool             function_like{};

  friend auto operator==(entry_key const&, entry_key const&) -> bool = default;
};

struct entry_key_hash
{
  auto operator()(entry_key const& key) const noexcept -> std::size_t
  {
    auto const hash = std::hash<std::string_view>{};
    return hash(key.name) ^ (hash(key.text) * 31) ^ static_cast<std::size_t>(key.function_like);
  }
};

auto decode_unit(
    debug_sections const&      sections,
    std::span<std::byte const> macro,
    std::uint64_t              offset,
    bool                       zero_is_root) -> macro_unit
{
  if(offset >= macro.size())
  {
    throw image_error{std::string{section_name}, 0, "macro unit offset runs past the section"};
  }
  dwarf_cursor cursor{macro, section_name};
  cursor.seek(static_cast<std::size_t>(offset));
  auto const version = cursor.u16();
  if(version != 4 and version != 5)
  {
    throw image_error{std::string{section_name}, 0, "unsupported macro unit version"};
  }
  auto const flags = cursor.u8();
  auto const is64  = (flags & flag_offset_size_64) != 0;
  if((flags & flag_debug_line) != 0)
  {
    std::ignore = read_offset(cursor, is64);
  }
  std::unordered_map<std::uint8_t, std::vector<std::uint8_t>> operands;
  if((flags & flag_opcode_operands) != 0)
  {
    auto const count = cursor.u8();
    for(std::uint8_t i = 0; i < count; ++i)
    {
      auto const opcode = cursor.u8();
      auto const forms  = cursor.uleb();
      if(forms > cursor.remaining())
      {
        throw image_error{std::string{section_name}, 0, "operands table runs past the section"};
      }
      auto& listed = operands[opcode];
      listed.clear();
      for(std::uint64_t form = 0; form < forms; ++form)
      {
        listed.push_back(cursor.u8());
      }
    }
  }

  macro_unit ops;
  for(;;)
  {
    auto const opcode = cursor.u8();
    switch(opcode)
    {
      case 0:
        return ops;
      case dw_macro_define:
      case dw_macro_undef:
        std::ignore = cursor.uleb();
        ops.push_back(
            {.kind = opcode == dw_macro_define ? op_kind::define : op_kind::undef,
             .text = cursor.cstr()});
        break;
      case dw_macro_start_file:
        std::ignore = cursor.uleb();
        std::ignore = cursor.uleb();
        break;
      case dw_macro_end_file:
        break;
      case dw_macro_define_strp:
      case dw_macro_undef_strp:
      {
        std::ignore       = cursor.uleb();
        auto const string = read_offset(cursor, is64);
        ops.push_back(
            {.kind = opcode == dw_macro_define_strp ? op_kind::define : op_kind::undef,
             .text = read_cstr_at(sections.str, string, ".debug_str")});
        break;
      }
      case dw_macro_import:
      {
        auto const target = read_offset(cursor, is64);
        if(target != 0 or not zero_is_root)
        {
          ops.push_back({.kind = op_kind::import, .value = target});
        }
        break;
      }
      case dw_macro_define_strx:
      case dw_macro_undef_strx:
        std::ignore = cursor.uleb();
        ops.push_back(
            {.kind  = opcode == dw_macro_define_strx ? op_kind::define_strx : op_kind::undef_strx,
             .value = cursor.uleb()});
        break;
      default:
        if(opcode >= dw_macro_define_sup and opcode <= dw_macro_import_sup)
        {
          throw image_error{
              std::string{section_name}, 0, "supplementary object files are not supported"};
        }
        auto const listed = operands.find(opcode);
        if(opcode < dw_macro_lo_user or listed == operands.end())
        {
          throw image_error{std::string{section_name}, 0, "unknown macro opcode"};
        }
        for(auto const form : listed->second)
        {
          std::ignore = read_form(cursor, form, sections.str, sections.line_str, is64, 0);
        }
        break;
    }
  }
}

auto name_end(std::string_view text) -> std::size_t
{
  auto const end = text.find_first_of(" (");
  return end == std::string_view::npos ? text.size() : end;
}

class unit_runner
{
public:
  unit_runner(debug_sections const& sections, std::span<std::byte const> macro, bool zero_is_root)
      : sections_{sections}, macro_{macro}, zero_is_root_{zero_is_root}
  {}

  [[nodiscard]] auto decoded() const -> std::size_t
  {
    return decodes_;
  }

  auto count(unit_head const& head) -> std::uint64_t
  {
    importing_.clear();
    auto const operations = expanded(*head.macros);
    if(operations > max_macro_operations)
    {
      throw image_error{
          std::string{section_name}, 0, "more than 2^24 macro operations for one unit"};
    }
    return operations;
  }

  auto run(unit_head const& head) -> std::unordered_map<std::string_view, definition>
  {
    defined_.clear();
    importing_.clear();
    head_ = &head;
    execute(*head.macros);
    return std::move(defined_);
  }

private:
  auto unit(std::uint64_t offset) -> macro_unit const&
  {
    auto const found = decoded_.find(offset);
    if(found != decoded_.end())
    {
      return found->second;
    }
    ++decodes_;
    return decoded_.emplace(offset, decode_unit(sections_, macro_, offset, zero_is_root_))
        .first->second;
  }

  void enter(std::uint64_t offset)
  {
    if(std::ranges::contains(importing_, offset))
    {
      throw image_error{std::string{section_name}, 0, "macro import cycle"};
    }
    if(importing_.size() > max_macro_import_depth)
    {
      throw image_error{std::string{section_name}, 0, "more than 64 nested macro imports"};
    }
    importing_.push_back(offset);
  }

  auto expanded(std::uint64_t offset) -> std::uint64_t
  {
    auto const known = expanded_.find(offset);
    if(known != expanded_.end())
    {
      return known->second;
    }
    enter(offset);
    std::uint64_t total = 0;
    for(auto const& op : unit(offset))
    {
      auto const inner = op.kind == op_kind::import ? expanded(op.value) : 0;
      total            = std::min(total + 1 + inner, max_macro_operations + 1);
    }
    importing_.pop_back();
    expanded_.emplace(offset, total);
    return total;
  }

  void execute(std::uint64_t offset)
  {
    enter(offset);
    for(auto const& op : unit(offset))
    {
      switch(op.kind)
      {
        case op_kind::define:
          define(op.text);
          break;
        case op_kind::undef:
          undefine(op.text);
          break;
        case op_kind::define_strx:
          define(string_at(op.value));
          break;
        case op_kind::undef_strx:
          undefine(string_at(op.value));
          break;
        case op_kind::import:
          execute(op.value);
          break;
      }
    }
    importing_.pop_back();
  }

  auto string_at(std::uint64_t index) const -> std::string_view
  {
    return resolve_strx(
        index, head_->str_offsets_base, sections_.str_offsets, sections_.str, head_->is64);
  }

  void define(std::string_view text)
  {
    auto const end = name_end(text);
    if(end == 0)
    {
      throw image_error{std::string{section_name}, 0, "macro definition without a name"};
    }
    auto const function_like = end < text.size() and text[end] == '(';
    auto       body          = text.substr(end);
    if(not function_like and not body.empty())
    {
      body.remove_prefix(1);
    }
    defined_.insert_or_assign(text.substr(0, end), definition{body, function_like});
  }

  void undefine(std::string_view text)
  {
    defined_.erase(text.substr(0, name_end(text)));
  }

  debug_sections const&                            sections_;
  std::span<std::byte const>                       macro_;
  bool                                             zero_is_root_{};
  std::unordered_map<std::uint64_t, macro_unit>    decoded_;
  std::unordered_map<std::string_view, definition> defined_;
  std::unordered_map<std::uint64_t, std::uint64_t> expanded_;
  std::vector<std::uint64_t>                       importing_;
  unit_head const*                                 head_{};
  std::size_t                                      decodes_{};
};
}

auto read_macros(debug_sections const& sections, std::span<std::byte const> macro) -> macro_scan
{
  macro_index                                                               index;
  std::unordered_map<entry_key, std::vector<std::uint32_t>, entry_key_hash> merged;
  auto heads = unit_heads(sections);
  std::erase_if(heads, [](unit_head const& head) { return not head.macros; });
  auto const zero_is_root = std::ranges::contains(
      heads, std::optional<std::uint64_t>{0}, [](unit_head const& head) { return head.macros; });
  unit_runner   runner{sections, macro, zero_is_root};
  std::uint64_t operations = 0;
  for(auto const& head : heads)
  {
    operations += runner.count(head);
    if(operations > max_macro_file_operations)
    {
      throw image_error{
          std::string{section_name}, 0, "more than 2^26 macro operations for one file"};
    }
  }
  for(auto const& head : heads)
  {
    auto const unit = static_cast<std::uint32_t>(index.units.size());
    index.units.push_back(head.name);
    for(auto const& [name, defined] : runner.run(head))
    {
      merged[{name, defined.text, defined.function_like}].push_back(unit);
    }
  }
  index.entries.reserve(merged.size());
  for(auto& [key, units] : merged)
  {
    index.entries.push_back(
        {.name          = std::string{key.name},
         .text          = std::string{key.text},
         .function_like = key.function_like,
         .units         = std::move(units)});
  }
  std::ranges::sort(index.entries, {}, [](macro_entry const& entry) {
    return std::tie(entry.name, entry.text, entry.function_like);
  });
  return {.index = std::move(index), .units_decoded = runner.decoded()};
}
}
