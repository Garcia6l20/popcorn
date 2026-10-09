#pragma once

#include "dwarf_units.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::testing
{
/** @brief Little-endian bytes of synthetic DWARF sections. */
struct dwarf_bytes
{
  std::vector<std::byte> bytes;

  auto size() const -> std::uint64_t
  {
    return bytes.size();
  }
  void u8(std::uint8_t v)
  {
    bytes.push_back(std::byte{v});
  }
  void u16(std::uint16_t v)
  {
    u8(static_cast<std::uint8_t>(v));
    u8(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v)
  {
    u16(static_cast<std::uint16_t>(v));
    u16(static_cast<std::uint16_t>(v >> 16));
  }
  void u64(std::uint64_t v)
  {
    u32(static_cast<std::uint32_t>(v));
    u32(static_cast<std::uint32_t>(v >> 32));
  }
  void uleb(std::uint64_t v)
  {
    do
    {
      auto b = static_cast<std::uint8_t>(v & 0x7f);
      v >>= 7;
      u8(v != 0 ? b | 0x80 : b);
    } while(v != 0);
  }
  void cstr(std::string_view s)
  {
    for(char const c : s)
    {
      u8(static_cast<std::uint8_t>(c));
    }
    u8(0);
  }
};

/** @brief A compile unit of the synthetic .debug_info. */
struct synthetic_unit
{
  std::string                  name;
  std::optional<std::uint32_t> macros{};
  bool                         partial{};
  std::optional<std::uint32_t> name_strx{};
  std::optional<std::uint32_t> str_offsets_base{};
  bool                         gnu_macros{};
  bool                         is64{};
};

/** @brief A .debug_macro section built unit by unit. */
class macro_section
{
public:
  /** @brief Starts a macro unit, returning its offset. */
  auto unit(std::uint16_t version = 5, bool is64 = false, bool with_line = false) -> std::uint32_t
  {
    auto const at = static_cast<std::uint32_t>(out_.size());
    is64_         = is64;
    out_.u16(version);
    out_.u8(static_cast<std::uint8_t>((is64 ? 1 : 0) | (with_line ? 2 : 0)));
    if(with_line)
    {
      offset(0);
    }
    return at;
  }

  /** @brief Starts a macro unit whose operands table lists @p opcode with @p forms. */
  auto unit_with_operands(std::uint8_t opcode, std::vector<std::uint8_t> const& forms)
      -> std::uint32_t
  {
    auto const at = static_cast<std::uint32_t>(out_.size());
    is64_         = false;
    out_.u16(5);
    out_.u8(4);
    out_.u8(1);
    out_.u8(opcode);
    out_.uleb(forms.size());
    for(auto const form : forms)
    {
      out_.u8(form);
    }
    return at;
  }

  void define(std::string_view text, std::uint8_t opcode = 0x01)
  {
    out_.u8(opcode);
    out_.uleb(1);
    out_.cstr(text);
  }
  void undef(std::string_view text)
  {
    define(text, 0x02);
  }
  void strp(std::uint64_t string, std::uint8_t opcode = 0x05)
  {
    out_.u8(opcode);
    out_.uleb(1);
    offset(string);
  }
  void strx(std::uint64_t index, std::uint8_t opcode = 0x0b)
  {
    out_.u8(opcode);
    out_.uleb(1);
    out_.uleb(index);
  }
  void import(std::uint64_t at)
  {
    out_.u8(0x07);
    offset(at);
  }
  void start_file()
  {
    out_.u8(0x03);
    out_.uleb(0);
    out_.uleb(1);
  }
  void end_file()
  {
    out_.u8(0x04);
  }
  void raw(std::uint8_t byte)
  {
    out_.u8(byte);
  }
  void end()
  {
    out_.u8(0);
  }
  auto bytes() const -> std::vector<std::byte> const&
  {
    return out_.bytes;
  }

private:
  void offset(std::uint64_t value)
  {
    if(is64_)
    {
      out_.u64(value);
    }
    else
    {
      out_.u32(static_cast<std::uint32_t>(value));
    }
  }

  dwarf_bytes out_;
  bool        is64_{};
};

/** @brief .debug_info and .debug_abbrev holding @p units, version 5. */
struct synthetic_info
{
  std::vector<std::byte> info;
  std::vector<std::byte> abbrev;

  explicit synthetic_info(std::vector<synthetic_unit> const& units)
  {
    constexpr std::uint64_t dw_at_name             = 0x03;
    constexpr std::uint64_t dw_at_str_offsets_base = 0x72;
    constexpr std::uint64_t dw_at_macros           = 0x79;
    constexpr std::uint64_t dw_at_gnu_macros       = 0x2119;
    constexpr std::uint64_t dw_form_string         = 0x08;
    constexpr std::uint64_t dw_form_sec_offset     = 0x17;
    constexpr std::uint64_t dw_form_strx4          = 0x28;

    dwarf_bytes   abbrevs;
    dwarf_bytes   out;
    std::uint64_t code = 0;
    for(auto const& unit : units)
    {
      ++code;
      abbrevs.uleb(code);
      abbrevs.uleb(unit.partial ? 0x3c : 0x11);
      abbrevs.u8(0);
      abbrevs.uleb(dw_at_name);
      abbrevs.uleb(unit.name_strx ? dw_form_strx4 : dw_form_string);
      if(unit.str_offsets_base)
      {
        abbrevs.uleb(dw_at_str_offsets_base);
        abbrevs.uleb(dw_form_sec_offset);
      }
      if(unit.macros)
      {
        abbrevs.uleb(unit.gnu_macros ? dw_at_gnu_macros : dw_at_macros);
        abbrevs.uleb(dw_form_sec_offset);
      }
      abbrevs.uleb(0);
      abbrevs.uleb(0);

      dwarf_bytes die;
      die.uleb(code);
      if(unit.name_strx)
      {
        die.u32(*unit.name_strx);
      }
      else
      {
        die.cstr(unit.name);
      }
      auto const offset = [&](std::uint32_t value) {
        if(unit.is64)
        {
          die.u64(value);
        }
        else
        {
          die.u32(value);
        }
      };
      if(unit.str_offsets_base)
      {
        offset(*unit.str_offsets_base);
      }
      if(unit.macros)
      {
        offset(*unit.macros);
      }
      if(unit.is64)
      {
        out.u32(0xFFFF'FFFFu);
        out.u64(12 + die.size());
      }
      else
      {
        out.u32(static_cast<std::uint32_t>(8 + die.size()));
      }
      out.u16(5);
      out.u8(unit.partial ? 0x03 : 0x01);
      out.u8(4);
      if(unit.is64)
      {
        out.u64(0);
      }
      else
      {
        out.u32(0);
      }
      out.bytes.insert(out.bytes.end(), die.bytes.begin(), die.bytes.end());
    }
    abbrevs.uleb(0);
    info   = std::move(out.bytes);
    abbrev = std::move(abbrevs.bytes);
  }

  auto sections(
      std::vector<std::byte> const& str         = {},
      std::vector<std::byte> const& str_offsets = {}) const -> image::detail::debug_sections
  {
    return {
        .info        = info,
        .abbrev      = abbrev,
        .line        = {},
        .str         = str,
        .line_str    = {},
        .str_offsets = str_offsets};
  }
};
}
