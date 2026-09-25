#include "dwarf_reader.hpp"

#include "elf_file.hpp"

#include <popcorn/core/errors.hpp>

#include <cstring>
#include <format>

namespace popcorn::image::detail
{
dwarf_cursor::dwarf_cursor(std::span<std::byte const> data, std::string_view section_name)
    : data_{data}, section_{section_name}
{}

void dwarf_cursor::check(std::size_t count) const
{
  if(count > data_.size() - pos_)
  {
    throw image_error{std::string{section_}, 0, std::format("truncated at offset {}", pos_)};
  }
}

auto dwarf_cursor::u8() -> std::uint8_t
{
  check(1);
  return static_cast<std::uint8_t>(data_[pos_++]);
}

namespace
{
template <class T> auto read_le(std::span<std::byte const> data, std::size_t pos) -> T
{
  T v;
  std::memcpy(&v, data.data() + pos, sizeof(T));
  return v;
}
}

auto dwarf_cursor::u16() -> std::uint16_t
{
  check(2);
  auto const v = read_le<std::uint16_t>(data_, pos_);
  pos_ += 2;
  return v;
}

auto dwarf_cursor::u32() -> std::uint32_t
{
  check(4);
  auto const v = read_le<std::uint32_t>(data_, pos_);
  pos_ += 4;
  return v;
}

auto dwarf_cursor::u64() -> std::uint64_t
{
  check(8);
  auto const v = read_le<std::uint64_t>(data_, pos_);
  pos_ += 8;
  return v;
}

auto dwarf_cursor::uleb() -> std::uint64_t
{
  std::uint64_t result = 0;
  unsigned      shift  = 0;
  for(;;)
  {
    auto const byte = u8();
    if(shift < 64)
    {
      result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    }
    if((byte & 0x80) == 0)
    {
      break;
    }
    shift += 7;
    if(shift >= 64)
    {
      throw image_error{std::string{section_}, 0, "malformed ULEB128"};
    }
  }
  return result;
}

auto dwarf_cursor::sleb() -> std::int64_t
{
  std::int64_t result = 0;
  unsigned     shift  = 0;
  std::uint8_t byte{};
  do
  {
    byte = u8();
    if(shift < 64)
    {
      result |= static_cast<std::int64_t>(byte & 0x7f) << shift;
    }
    shift += 7;
    if(shift > 64)
    {
      throw image_error{std::string{section_}, 0, "malformed SLEB128"};
    }
  } while(byte & 0x80);
  if(shift < 64 && (byte & 0x40) != 0)
  {
    result |= -(static_cast<std::int64_t>(1) << shift);
  }
  return result;
}

auto dwarf_cursor::cstr() -> std::string_view
{
  return read_ntbs(data_, pos_);
}

void dwarf_cursor::skip(std::uint64_t count)
{
  check(count);
  pos_ += count;
}

void dwarf_cursor::seek(std::size_t offset)
{
  if(offset > data_.size())
  {
    throw image_error{std::string{section_}, 0, std::format("seek past end, offset {}", offset)};
  }
  pos_ = offset;
}

auto dwarf_cursor::pos() const -> std::size_t
{
  return pos_;
}

auto dwarf_cursor::size() const -> std::size_t
{
  return data_.size();
}

auto dwarf_cursor::remaining() const -> std::size_t
{
  return data_.size() - pos_;
}

auto dwarf_cursor::at_end() const -> bool
{
  return pos_ >= data_.size();
}

auto dwarf_cursor::data() const -> std::span<std::byte const>
{
  return data_;
}

auto read_initial_length(dwarf_cursor& cursor, bool& is64) -> std::uint64_t
{
  auto const first = cursor.u32();
  if(first == 0xffff'ffffu)
  {
    is64 = true;
    return cursor.u64();
  }
  is64 = false;
  return first;
}

auto read_offset(dwarf_cursor& cursor, bool is64) -> std::uint64_t
{
  return is64 ? cursor.u64() : cursor.u32();
}

auto read_cstr_at(
    std::span<std::byte const> data,
    std::uint64_t              offset,
    std::string_view           section_name) -> std::string_view
{
  if(offset > data.size())
  {
    throw image_error{
        std::string{section_name}, 0, std::format("offset {} past end of section", offset)};
  }
  auto pos = static_cast<std::size_t>(offset);
  return read_ntbs(data, pos);
}

auto checked_offset(
    std::size_t      start,
    std::uint64_t    length,
    std::size_t      bound,
    std::string_view section) -> std::size_t
{
  if(start > bound || length > bound - start)
  {
    throw image_error{
        std::string{section}, 0, std::format("length {} runs past offset {}", length, bound)};
  }
  return start + length;
}

auto checked_unit_end(
    std::size_t      start,
    std::uint64_t    length,
    std::size_t      size,
    std::string_view section) -> std::size_t
{
  auto const end = checked_offset(start, length, size, section);
  if(end <= start)
  {
    throw image_error{std::string{section}, 0, "unit length does not advance"};
  }
  return end;
}

namespace
{
enum class dw_form : std::uint64_t
{
  addr           = 0x01,
  block2         = 0x03,
  block4         = 0x04,
  data2          = 0x05,
  data4          = 0x06,
  data8          = 0x07,
  string_        = 0x08,
  block          = 0x09,
  block1         = 0x0a,
  data1          = 0x0b,
  flag           = 0x0c,
  sdata          = 0x0d,
  strp           = 0x0e,
  udata          = 0x0f,
  ref_addr       = 0x10,
  ref1           = 0x11,
  ref2           = 0x12,
  ref4           = 0x13,
  ref8           = 0x14,
  ref_udata      = 0x15,
  indirect       = 0x16,
  sec_offset     = 0x17,
  exprloc        = 0x18,
  flag_present   = 0x19,
  strx           = 0x1a,
  addrx          = 0x1b,
  ref_sup4       = 0x1c,
  strp_sup       = 0x1d,
  data16         = 0x1e,
  line_strp      = 0x1f,
  ref_sig8       = 0x20,
  implicit_const = 0x21,
  loclistx       = 0x22,
  rnglistx       = 0x23,
  ref_sup8       = 0x24,
  strx1          = 0x25,
  strx2          = 0x26,
  strx3          = 0x27,
  strx4          = 0x28,
  addrx1         = 0x29,
  addrx2         = 0x2a,
  addrx3         = 0x2b,
  addrx4         = 0x2c,
};

auto read_u24(dwarf_cursor& cursor) -> std::uint64_t
{
  std::uint64_t v = cursor.u8();
  v |= static_cast<std::uint64_t>(cursor.u8()) << 8;
  v |= static_cast<std::uint64_t>(cursor.u8()) << 16;
  return v;
}
}

auto read_form(
    dwarf_cursor&              cursor,
    std::uint64_t              form_code,
    std::span<std::byte const> str,
    std::span<std::byte const> line_str,
    bool                       is64,
    std::int64_t               implicit_const) -> form_value
{
  form_value v;
  switch(static_cast<dw_form>(form_code))
  {
    case dw_form::data1:
    case dw_form::ref1:
    case dw_form::flag:
    case dw_form::addrx1:
      v.u = cursor.u8();
      break;
    case dw_form::strx1:
      v.u       = cursor.u8();
      v.is_strx = true;
      break;
    case dw_form::data2:
    case dw_form::ref2:
    case dw_form::addrx2:
      v.u = cursor.u16();
      break;
    case dw_form::strx2:
      v.u       = cursor.u16();
      v.is_strx = true;
      break;
    case dw_form::addrx3:
      v.u = read_u24(cursor);
      break;
    case dw_form::strx3:
      v.u       = read_u24(cursor);
      v.is_strx = true;
      break;
    case dw_form::addr:
    case dw_form::data4:
    case dw_form::ref4:
    case dw_form::addrx4:
    case dw_form::ref_sup4:
      v.u = cursor.u32();
      break;
    case dw_form::strx4:
      v.u       = cursor.u32();
      v.is_strx = true;
      break;
    case dw_form::data8:
    case dw_form::ref8:
    case dw_form::ref_sig8:
    case dw_form::ref_sup8:
      v.u = cursor.u64();
      break;
    case dw_form::data16:
      cursor.skip(16);
      break;
    case dw_form::ref_udata:
    case dw_form::udata:
    case dw_form::addrx:
    case dw_form::loclistx:
    case dw_form::rnglistx:
      v.u = cursor.uleb();
      break;
    case dw_form::strx:
      v.u       = cursor.uleb();
      v.is_strx = true;
      break;
    case dw_form::sdata:
      v.u = static_cast<std::uint64_t>(cursor.sleb());
      break;
    case dw_form::flag_present:
      v.u = 1;
      break;
    case dw_form::implicit_const:
      v.u = static_cast<std::uint64_t>(implicit_const);
      break;
    case dw_form::string_:
      v.s         = cursor.cstr();
      v.is_string = true;
      break;
    case dw_form::strp:
      v.s         = read_cstr_at(str, read_offset(cursor, is64), ".debug_str");
      v.is_string = true;
      break;
    case dw_form::line_strp:
      v.s         = read_cstr_at(line_str, read_offset(cursor, is64), ".debug_line_str");
      v.is_string = true;
      break;
    case dw_form::sec_offset:
    case dw_form::ref_addr:
    case dw_form::strp_sup:
      v.u = read_offset(cursor, is64);
      break;
    case dw_form::exprloc:
    case dw_form::block:
      cursor.skip(cursor.uleb());
      break;
    case dw_form::block1:
      cursor.skip(cursor.u8());
      break;
    case dw_form::block2:
      cursor.skip(cursor.u16());
      break;
    case dw_form::block4:
      cursor.skip(cursor.u32());
      break;
    case dw_form::indirect:
    {
      auto const real_form = cursor.uleb();
      if(static_cast<dw_form>(real_form) == dw_form::indirect)
      {
        throw image_error{".debug_info", 0, "DW_FORM_indirect chains to another indirect form"};
      }
      return read_form(cursor, real_form, str, line_str, is64, implicit_const);
    }
    default:
      throw image_error{".debug_info", 0, std::format("unhandled DWARF form {:#x}", form_code)};
  }
  return v;
}
}
