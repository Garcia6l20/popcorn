#include "dwarf_lines.hpp"
#include "dwarf_units.hpp"
#include "synthetic_elf.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using popcorn::image::load_elf;
using popcorn::image::detail::compile_unit_ref;
using popcorn::image::detail::compile_units;
using popcorn::image::detail::debug_sections;
using popcorn::image::detail::decode_lines;
using popcorn::image::detail::file_ref;
using popcorn::image::detail::scan_compile_units;
using popcorn::image::detail::scan_functions;

namespace
{
auto env_path(char const* name) -> std::string
{
  char const* const value = std::getenv(name);
  REQUIRE(value != nullptr);
  return value;
}

struct byte_writer
{
  std::vector<std::byte> bytes;

  void u8(std::uint8_t v)
  {
    bytes.push_back(std::byte{v});
  }
  void u16(std::uint16_t v)
  {
    auto const* p = reinterpret_cast<std::byte const*>(&v);
    bytes.insert(bytes.end(), p, p + 2);
  }
  void u32(std::uint32_t v)
  {
    auto const* p = reinterpret_cast<std::byte const*>(&v);
    bytes.insert(bytes.end(), p, p + 4);
  }
  void u64(std::uint64_t v)
  {
    auto const* p = reinterpret_cast<std::byte const*>(&v);
    bytes.insert(bytes.end(), p, p + 8);
  }
  void uleb(std::uint64_t v)
  {
    do
    {
      std::uint8_t b = v & 0x7f;
      v >>= 7;
      if(v != 0)
      {
        b |= 0x80;
      }
      u8(b);
    } while(v != 0);
  }
  void sleb(std::int64_t v)
  {
    bool more = true;
    while(more)
    {
      std::uint8_t b = static_cast<std::uint8_t>(v) & 0x7f;
      v >>= 7;
      if((v == 0 && (b & 0x40) == 0) || (v == -1 && (b & 0x40) != 0))
      {
        more = false;
      }
      else
      {
        b |= 0x80;
      }
      u8(b);
    }
  }
  void cstr(std::string_view s)
  {
    for(char c : s)
    {
      u8(static_cast<std::uint8_t>(c));
    }
    u8(0);
  }
  auto size() const -> std::size_t
  {
    return bytes.size();
  }
};

/** @brief Builds a v4 `.debug_line` program: one directory-less file "a.c", a fixed header. */
struct v4_line_program_builder
{
  byte_writer               header_tail;
  byte_writer               program;
  std::vector<std::uint8_t> extra_std_opcode_lengths;
  std::uint8_t              max_ops = 1;

  void set_address(std::uint32_t addr)
  {
    program.u8(0);
    program.uleb(5);
    program.u8(2);
    program.u32(addr);
  }
  void copy()
  {
    program.u8(1);
  }
  void advance_pc(std::uint64_t n)
  {
    program.u8(2);
    program.uleb(n);
  }
  void advance_line(std::int64_t n)
  {
    program.u8(3);
    program.sleb(n);
  }
  void const_add_pc()
  {
    program.u8(8);
  }
  void fixed_advance_pc(std::uint16_t n)
  {
    program.u8(9);
    program.u16(n);
  }
  void end_sequence()
  {
    program.u8(0);
    program.uleb(1);
    program.u8(1);
  }
  void define_file(std::string_view name)
  {
    program.u8(0);
    byte_writer body;
    body.u8(3);
    body.cstr(name);
    body.uleb(0);
    body.uleb(0);
    body.uleb(0);
    program.uleb(body.size());
    program.bytes.insert(program.bytes.end(), body.bytes.begin(), body.bytes.end());
  }
  void unknown_extended(std::uint8_t sub, std::span<std::byte const> payload)
  {
    program.u8(0);
    program.uleb(1 + payload.size());
    program.u8(sub);
    program.bytes.insert(program.bytes.end(), payload.begin(), payload.end());
  }
  void unknown_standard(std::uint8_t opcode, std::uint64_t operand_a, std::uint64_t operand_b)
  {
    program.u8(opcode);
    program.uleb(operand_a);
    program.uleb(operand_b);
    extra_std_opcode_lengths.push_back(2);
  }

  auto build(bool dwarf64 = false) const -> std::vector<std::byte>
  {
    byte_writer header;
    header.u16(4);

    std::vector<std::uint8_t> std_lens{0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    std::uint8_t const        opcode_base =
        static_cast<std::uint8_t>(13 + extra_std_opcode_lengths.size());
    for(auto len : extra_std_opcode_lengths)
    {
      std_lens.push_back(len);
    }

    byte_writer body;
    body.u8(1);
    body.u8(max_ops);
    body.u8(1);
    body.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(-5)));
    body.u8(14);
    body.u8(opcode_base);
    for(auto len : std_lens)
    {
      body.u8(len);
    }
    body.u8(0);
    body.cstr("a.c");
    body.uleb(0);
    body.uleb(0);
    body.uleb(0);
    body.u8(0);

    if(dwarf64)
    {
      header.u64(body.size());
    }
    else
    {
      header.u32(static_cast<std::uint32_t>(body.size()));
    }
    header.bytes.insert(header.bytes.end(), body.bytes.begin(), body.bytes.end());
    header.bytes.insert(header.bytes.end(), program.bytes.begin(), program.bytes.end());

    byte_writer full;
    if(dwarf64)
    {
      full.u32(0xffff'ffffu);
      full.u64(header.size());
    }
    else
    {
      full.u32(static_cast<std::uint32_t>(header.size()));
    }
    full.bytes.insert(full.bytes.end(), header.bytes.begin(), header.bytes.end());
    return full.bytes;
  }
};

auto decode_single(std::vector<std::byte> const& line_bytes, std::string comp_dir = "")
    -> std::vector<popcorn::image::line_row>
{
  debug_sections const sections{
      .info = {}, .abbrev = {}, .line = line_bytes, .str = {}, .line_str = {}, .str_offsets = {}};
  std::vector<compile_unit_ref> units{
      compile_unit_ref{0, std::move(comp_dir)}
  };
  return decode_lines(sections, units).rows;
}
}

TEST_CASE("decode_lines resolves every row through the committed golden files")
{
  struct Firmware
  {
    char const* elf_env;
    char const* golden_name;
  };
  std::vector<Firmware> const firmwares{
      {"POPCORN_IMAGE_ELF",        "image"       },
      {"POPCORN_IMAGE_DW4_ELF",    "image-dw4"   },
      {"POPCORN_IMAGE_DW5_ELF",    "image-dw5"   },
      {"POPCORN_IMAGE_DW5GCC_ELF", "image-dw5gcc"},
  };

  auto const data_dir = env_path("POPCORN_IMAGE_DATA");

  for(auto const& fw : firmwares)
  {
    CAPTURE(fw.golden_name);
    auto const img = load_elf(env_path(fw.elf_env));
    REQUIRE_FALSE(img.lines.empty());

    std::ifstream golden{
        std::filesystem::path{data_dir} / std::format("locate_{}.txt", fw.golden_name)};
    REQUIRE(golden.is_open());

    std::string line;
    std::size_t checked = 0;
    while(std::getline(golden, line))
    {
      if(line.empty())
      {
        continue;
      }
      std::istringstream iss{line};
      std::string        addr_hex;
      std::string        loc;
      iss >> addr_hex >> loc;
      auto const addr  = static_cast<popcorn::address>(std::stoul(addr_hex, nullptr, 16));
      auto const colon = loc.rfind(':');
      REQUIRE(colon != std::string::npos);
      auto const expected_file = loc.substr(0, colon);
      auto const expected_line = static_cast<std::uint32_t>(std::stoul(loc.substr(colon + 1)));

      auto const resolved = img.locate(addr);
      if(expected_file == "??")
      {
        CHECK(resolved.line == 0);
      }
      else
      {
        CHECK(resolved.file == expected_file);
        CHECK(resolved.line == expected_line);
      }
      ++checked;
    }
    CHECK(checked > 0);
  }
}

TEST_CASE("the v3 and v5 line tables of the same source give the same rows")
{
  auto const v3 = load_elf(env_path("POPCORN_IMAGE_ELF"));
  auto const v5 = load_elf(env_path("POPCORN_IMAGE_DW5_ELF"));

  std::map<popcorn::address, std::pair<std::string, std::uint32_t>> v3_rows, v5_rows;
  for(auto const& r : v3.lines)
  {
    if(r.line != 0)
    {
      v3_rows[r.addr] = {v3.files[r.file], r.line};
    }
  }
  for(auto const& r : v5.lines)
  {
    if(r.line != 0)
    {
      v5_rows[r.addr] = {v5.files[r.file], r.line};
    }
  }
  CHECK(v3_rows == v5_rows);
}

TEST_CASE("decode_lines handles a 64-bit DWARF unit length")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  auto const rows = decode_single(b.build(true));
  REQUIRE(rows.size() == 2);
  CHECK(rows.front().addr == 0x0800'0000);
}

TEST_CASE("decode_lines skips DW_LNE_define_file and keeps decoding")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.define_file("extra.c");
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 2);
  CHECK(rows.front().addr == 0x0800'0000);
}

TEST_CASE("decode_lines skips an unknown extended opcode by its length")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  std::array<std::byte, 3> const payload{std::byte{1}, std::byte{2}, std::byte{3}};
  b.unknown_extended(0x80, payload);
  b.advance_pc(4);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 2);
  CHECK(rows.front().addr == 0x0800'0004);
}

TEST_CASE("decode_lines skips an unknown standard opcode by its operand count")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.unknown_standard(13, 7, 9);
  b.advance_pc(2);
  b.copy();
  b.advance_pc(2);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 2);
  CHECK(rows.front().addr == 0x0800'0002);
}

TEST_CASE("decode_lines advances op_index and wraps into the address with maximum_operations > 1")
{
  v4_line_program_builder b;
  b.max_ops = 3;
  b.set_address(0x0800'0000);
  b.advance_line(4);
  b.copy();
  b.advance_pc(2);
  b.copy();
  b.advance_pc(4);
  b.copy();
  b.advance_pc(3);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 4);
  CHECK(rows[0].addr == 0x0800'0000);
  CHECK(rows[0].line == 5);
  CHECK(rows[1].addr == 0x0800'0000);
  CHECK(rows[2].addr == 0x0800'0002);
}

TEST_CASE("decode_lines handles const_add_pc")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.const_add_pc();
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 2);
  CHECK(rows.front().addr == 0x0800'0000 + (255 - 13) / 14);
}

TEST_CASE("decode_lines handles fixed_advance_pc")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.fixed_advance_pc(0x10);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 2);
  CHECK(rows.front().addr == 0x0800'0010);
}

TEST_CASE("decode_lines rejects a unit length that runs past the section")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.copy();
  b.end_sequence();
  auto          bytes = b.build();
  std::uint32_t huge  = 0xffff'fff0u;
  std::memcpy(bytes.data(), &huge, 4);
  CHECK_THROWS_AS(decode_single(bytes), popcorn::image_error);
}

TEST_CASE("decode_lines rejects a header length that runs past the unit")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.copy();
  b.end_sequence();
  auto          bytes = b.build();
  std::uint32_t huge  = 0xffff'fff0u;
  std::memcpy(bytes.data() + 4 + 2, &huge, 4);
  CHECK_THROWS_AS(decode_single(bytes), popcorn::image_error);
}

TEST_CASE("decode_lines on an empty section gives no rows and does not throw")
{
  debug_sections const                sections{};
  std::vector<compile_unit_ref> const units{};
  auto const                          table = decode_lines(sections, units);
  CHECK(table.rows.empty());
  CHECK(table.files.empty());
}

TEST_CASE("decode_lines sort key keeps emission order at a shared address, line-0 first")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.advance_line(-1);
  b.copy();
  b.advance_line(1);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].addr == 0x0800'0000);
  CHECK(rows[0].line == 0);
  CHECK(rows[1].addr == 0x0800'0000);
  CHECK(rows[1].line == 1);
  CHECK(rows[2].addr == 0x0800'0004);
}

TEST_CASE("decode_lines sort key keeps emission order at a shared address, line-0 second")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.advance_line(4);
  b.copy();
  b.advance_line(-5);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].addr == 0x0800'0000);
  CHECK(rows[0].line == 5);
  CHECK(rows[1].addr == 0x0800'0000);
  CHECK(rows[1].line == 0);
  CHECK(rows[2].addr == 0x0800'0004);
}

TEST_CASE(
    "decode_lines drops a real row that is immediately superseded by its own end-of-sequence at "
    "the same address")
{
  v4_line_program_builder b;
  b.set_address(0x0800'2000);
  b.advance_line(4);
  b.copy();
  b.end_sequence();
  b.set_address(0x0800'2000);
  b.advance_line(-1);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();

  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 3);
  auto const at_2000 = std::ranges::count(rows, 0x0800'2000u, &popcorn::image::line_row::addr);
  REQUIRE(at_2000 == 2);
  CHECK(rows[0].line == 0);
  CHECK(rows[1].line == 0);
  CHECK(rows[2].addr == 0x0800'2004);
}

TEST_CASE(
    "decode_lines drops every trailing real row superseded by the same end-of-sequence address "
    "(Z1)")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0040);
  b.advance_line(5);
  b.copy();
  b.advance_line(1);
  b.copy();
  b.end_sequence();

  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].addr == 0x0800'0040);
  CHECK(rows[0].line == 0);
}

TEST_CASE(
    "decode_lines keeps a real row that starts a new sequence where the previous one's "
    "end-of-sequence lands")
{
  v4_line_program_builder b;
  b.set_address(0x0800'3000);
  b.advance_line(0);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();
  b.set_address(0x0800'3004);
  b.advance_line(9);
  b.copy();
  b.advance_pc(4);
  b.end_sequence();

  auto const rows = decode_single(b.build());
  REQUIRE(rows.size() == 4);
  auto const at_3004 = std::ranges::count(rows, 0x0800'3004u, &popcorn::image::line_row::addr);
  REQUIRE(at_3004 == 2);
  CHECK(rows[2].line == 10);
}

TEST_CASE(
    "compile_units resolves a DW_FORM_strx comp_dir when DW_AT_str_offsets_base comes after "
    "DW_AT_comp_dir in attribute order")
{
  constexpr std::uint64_t dw_tag_compile_unit    = 0x11;
  constexpr std::uint64_t dw_at_stmt_list        = 0x10;
  constexpr std::uint64_t dw_at_comp_dir         = 0x1b;
  constexpr std::uint64_t dw_at_str_offsets_base = 0x72;
  constexpr std::uint64_t dw_form_sec_offset     = 0x17;
  constexpr std::uint64_t dw_form_strx1          = 0x25;
  constexpr std::uint8_t  dw_ut_compile          = 0x01;

  byte_writer abbrev;
  abbrev.uleb(1);
  abbrev.uleb(dw_tag_compile_unit);
  abbrev.u8(0);
  abbrev.uleb(dw_at_stmt_list);
  abbrev.uleb(dw_form_sec_offset);
  abbrev.uleb(dw_at_comp_dir);
  abbrev.uleb(dw_form_strx1);
  abbrev.uleb(dw_at_str_offsets_base);
  abbrev.uleb(dw_form_sec_offset);
  abbrev.uleb(0);
  abbrev.uleb(0);
  abbrev.uleb(0);

  byte_writer str;
  auto const  dir_str_offset = str.size();
  str.cstr("synth/comp/dir");

  byte_writer str_offsets;
  str_offsets.u32(4);
  str_offsets.u16(5);
  str_offsets.u16(0);
  str_offsets.u32(static_cast<std::uint32_t>(dir_str_offset));

  byte_writer die;
  die.uleb(1);
  die.u32(0);
  die.u8(0);
  die.u32(8);

  byte_writer cu_body;
  cu_body.u16(5);
  cu_body.u8(dw_ut_compile);
  cu_body.u8(4);
  cu_body.u32(0);
  cu_body.bytes.insert(cu_body.bytes.end(), die.bytes.begin(), die.bytes.end());

  byte_writer info;
  info.u32(static_cast<std::uint32_t>(cu_body.size()));
  info.bytes.insert(info.bytes.end(), cu_body.bytes.begin(), cu_body.bytes.end());

  debug_sections const sections{
      .info        = info.bytes,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = str.bytes,
      .line_str    = {},
      .str_offsets = str_offsets.bytes};

  auto const units = compile_units(sections);
  REQUIRE(units.size() == 1);
  CHECK(units.front().comp_dir == "synth/comp/dir");
}

TEST_CASE("decode_lines rejects maximum_operations_per_instruction == 0 (D1)")
{
  v4_line_program_builder b;
  b.max_ops = 0;
  b.set_address(0x0800'0000);
  b.copy();
  b.end_sequence();
  CHECK_THROWS_AS(decode_single(b.build()), popcorn::image_error);
}

namespace
{
auto wrap_cu_v5(byte_writer const& die, std::uint32_t abbrev_offset = 0) -> std::vector<std::byte>
{
  byte_writer cu_body;
  cu_body.u16(5);
  cu_body.u8(1);
  cu_body.u8(4);
  cu_body.u32(abbrev_offset);
  cu_body.bytes.insert(cu_body.bytes.end(), die.bytes.begin(), die.bytes.end());

  byte_writer info;
  info.u32(static_cast<std::uint32_t>(cu_body.size()));
  info.bytes.insert(info.bytes.end(), cu_body.bytes.begin(), cu_body.bytes.end());
  return info.bytes;
}
}

TEST_CASE("compile_units rejects a DW_FORM_indirect that chains to another indirect form (D2)")
{
  constexpr std::uint64_t dw_at_stmt_list    = 0x10;
  constexpr std::uint64_t dw_form_sec_offset = 0x17;
  constexpr std::uint64_t dw_form_indirect   = 0x16;

  byte_writer abbrev;
  abbrev.uleb(1);
  abbrev.uleb(0x11);
  abbrev.u8(0);
  abbrev.uleb(dw_at_stmt_list);
  abbrev.uleb(dw_form_indirect);
  abbrev.uleb(0);
  abbrev.uleb(0);
  abbrev.uleb(0);

  byte_writer die;
  die.uleb(1);
  die.uleb(dw_form_indirect);
  die.uleb(dw_form_sec_offset);

  auto const           info = wrap_cu_v5(die);
  debug_sections const sections{
      .info        = info,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = {},
      .line_str    = {},
      .str_offsets = {}};
  CHECK_THROWS_AS(std::ignore = compile_units(sections), popcorn::image_error);
}

TEST_CASE(
    "compile_units rejects a .debug_info unit_length that wraps or runs past the section (D3)")
{
  byte_writer info;
  info.u32(0xFFFF'FFF0u);
  debug_sections const sections{
      .info = info.bytes, .abbrev = {}, .line = {}, .str = {}, .line_str = {}, .str_offsets = {}};
  CHECK_THROWS_AS(std::ignore = compile_units(sections), popcorn::image_error);
}

TEST_CASE("decode_lines rejects a .debug_line unit_length that wraps or runs past the section (D3)")
{
  byte_writer line;
  line.u32(0xFFFF'FFF0u);
  CHECK_THROWS_AS(decode_single(line.bytes), popcorn::image_error);
}

TEST_CASE("decode_lines rejects a header_length that runs past the unit (D3)")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.copy();
  b.end_sequence();
  auto          bytes = b.build();
  std::uint32_t huge  = 0xFFFF'FFF0u;
  std::memcpy(bytes.data() + 4 + 2, &huge, 4);
  CHECK_THROWS_AS(decode_single(bytes), popcorn::image_error);
}

TEST_CASE("decode_lines rejects an extended opcode length that wraps or runs past the unit (D4)")
{
  v4_line_program_builder b;
  b.set_address(0x0800'0000);
  b.program.u8(0);
  b.program.uleb(0xFFFF'FFFFULL);
  CHECK_THROWS_AS(decode_single(b.build()), popcorn::image_error);
}

TEST_CASE("decode_lines rejects a v5 directory table with zero formats and a nonzero count (D5)")
{
  byte_writer header_tail;
  header_tail.u8(1);
  header_tail.u8(1);
  header_tail.u8(1);
  header_tail.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(-5)));
  header_tail.u8(14);
  header_tail.u8(13);
  for(std::uint8_t len : {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1})
  {
    header_tail.u8(len);
  }
  header_tail.u8(0);
  header_tail.uleb(0xFFFF'FFFFULL);

  byte_writer full_header;
  full_header.u16(5);
  full_header.u8(4);
  full_header.u8(0);
  full_header.u32(static_cast<std::uint32_t>(header_tail.size()));
  full_header.bytes.insert(
      full_header.bytes.end(), header_tail.bytes.begin(), header_tail.bytes.end());

  byte_writer line;
  line.u32(static_cast<std::uint32_t>(full_header.size()));
  line.bytes.insert(line.bytes.end(), full_header.bytes.begin(), full_header.bytes.end());

  CHECK_THROWS_AS(decode_single(line.bytes), popcorn::image_error);
}

TEST_CASE("compile_units dedupes compile units that share a line_offset, not just cu_start (D6)")
{
  constexpr std::uint64_t dw_at_stmt_list    = 0x10;
  constexpr std::uint64_t dw_form_sec_offset = 0x17;

  byte_writer abbrev;
  abbrev.uleb(1);
  abbrev.uleb(0x11);
  abbrev.u8(0);
  abbrev.uleb(dw_at_stmt_list);
  abbrev.uleb(dw_form_sec_offset);
  abbrev.uleb(0);
  abbrev.uleb(0);
  abbrev.uleb(0);

  byte_writer die;
  die.uleb(1);
  die.u32(0);

  auto const             one_cu = wrap_cu_v5(die);
  std::vector<std::byte> info;
  info.insert(info.end(), one_cu.begin(), one_cu.end());
  info.insert(info.end(), one_cu.begin(), one_cu.end());

  debug_sections const sections{
      .info        = info,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = {},
      .line_str    = {},
      .str_offsets = {}};
  auto const units = compile_units(sections);
  CHECK(units.size() == 1);
}

namespace
{
auto stmt_list_abbrev_table() -> byte_writer
{
  constexpr std::uint64_t dw_at_stmt_list    = 0x10;
  constexpr std::uint64_t dw_form_sec_offset = 0x17;

  byte_writer abbrev;
  abbrev.uleb(1);
  abbrev.uleb(0x11);
  abbrev.u8(0);
  abbrev.uleb(dw_at_stmt_list);
  abbrev.uleb(dw_form_sec_offset);
  abbrev.uleb(0);
  abbrev.uleb(0);
  abbrev.uleb(0);
  return abbrev;
}

auto units_over(std::vector<std::uint32_t> const& abbrev_offsets) -> std::vector<std::byte>
{
  std::vector<std::byte> info;
  for(std::uint32_t index = 0; index < abbrev_offsets.size(); ++index)
  {
    byte_writer die;
    die.uleb(1);
    die.u32(index);
    auto const cu = wrap_cu_v5(die, abbrev_offsets[index]);
    info.insert(info.end(), cu.begin(), cu.end());
  }
  return info;
}
}

TEST_CASE("compile_units caches the abbrev table across many units sharing one abbrev_offset (A1)")
{
  auto const abbrev = stmt_list_abbrev_table();
  auto const info   = units_over(std::vector<std::uint32_t>(64, 0));

  debug_sections const sections{
      .info        = info,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = {},
      .line_str    = {},
      .str_offsets = {}};

  auto const scan = scan_compile_units(sections);
  REQUIRE(scan.units.size() == 64);
  CHECK(scan.abbrev_tables_parsed == 1);
  CHECK(compile_units(sections).size() == 64);
}

TEST_CASE("compile_units parses each distinct abbrev table once, units alternating between two")
{
  auto       abbrev = stmt_list_abbrev_table();
  auto const second = static_cast<std::uint32_t>(abbrev.size());
  auto const table  = stmt_list_abbrev_table();
  abbrev.bytes.insert(abbrev.bytes.end(), table.bytes.begin(), table.bytes.end());

  std::vector<std::uint32_t> offsets;
  for(int index = 0; index < 16; ++index)
  {
    offsets.push_back(index % 2 == 0 ? 0 : second);
  }
  auto const info = units_over(offsets);

  debug_sections const sections{
      .info        = info,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = {},
      .line_str    = {},
      .str_offsets = {}};

  auto const scan = scan_compile_units(sections);
  REQUIRE(scan.units.size() == 16);
  CHECK(scan.abbrev_tables_parsed == 2);
}

TEST_CASE("decode_lines resolves wanted file table entries, a file no row uses after the others")
{
  v4_line_program_builder used;
  used.set_address(0x0800'0000);
  used.copy();
  used.advance_pc(2);
  used.end_sequence();
  auto       line   = used.build();
  auto const second = line.size();
  auto const unused = v4_line_program_builder{}.build();
  line.insert(line.end(), unused.begin(), unused.end());

  std::vector<compile_unit_ref> const units{
      {.line_offset = 0,      .comp_dir = ""      },
      {.line_offset = second, .comp_dir = "/other"},
  };
  std::vector<file_ref> const wanted{
      {.line_offset = second, .index = 1},
      {.line_offset = 0,      .index = 1},
      {.line_offset = 0,      .index = 0},
      {.line_offset = 0,      .index = 7},
      {.line_offset = 12345,  .index = 1},
  };
  debug_sections const sections{
      .info = {}, .abbrev = {}, .line = line, .str = {}, .line_str = {}, .str_offsets = {}};
  auto const table = decode_lines(sections, units, wanted);
  CHECK(table.files == std::vector<std::string>{"a.c", "/other/a.c"});
  CHECK(
      table.wanted
      == std::vector<std::optional<std::uint32_t>>{1, 0, std::nullopt, std::nullopt, std::nullopt});
}

namespace
{
constexpr std::uint64_t dw_tag_subprogram_test = 0x2e;
constexpr std::uint64_t dw_tag_variable_test   = 0x34;

void attribute(byte_writer& abbrev, std::uint64_t attr, std::uint64_t form)
{
  abbrev.uleb(attr);
  abbrev.uleb(form);
}

void abbreviation(
    byte_writer&                                                   abbrev,
    std::uint64_t                                                  code,
    std::uint64_t                                                  tag,
    bool                                                           children,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> attrs)
{
  abbrev.uleb(code);
  abbrev.uleb(tag);
  abbrev.u8(children ? 1 : 0);
  for(auto const& [attr, form] : attrs)
  {
    attribute(abbrev, attr, form);
  }
  abbrev.uleb(0);
  abbrev.uleb(0);
}
}

TEST_CASE("scan_functions takes declarations through abstract origins and specifications")
{
  constexpr std::uint64_t stmt_list = 0x10, addr_base = 0x73, low_pc = 0x11, high_pc = 0x12,
                          origin = 0x31, decl_file = 0x3a, decl_line = 0x3b, specification = 0x47;
  constexpr std::uint64_t sec_offset = 0x17, data1 = 0x0b, data2 = 0x05, data4 = 0x06, addr = 0x01,
                          addrx = 0x1b, ref4 = 0x13, ref_addr = 0x10, implicit_const = 0x21;

  byte_writer abbrev;
  abbreviation(
      abbrev, 1, 0x11, true,
      {
          {stmt_list, sec_offset},
          {addr_base, sec_offset}
  });
  abbreviation(
      abbrev, 2, dw_tag_subprogram_test, false,
      {
          {decl_file, data1},
          {decl_line, data1}
  });
  abbreviation(
      abbrev, 3, dw_tag_subprogram_test, false,
      {
          {origin,  ref4 },
          {low_pc,  addrx},
          {high_pc, data4}
  });
  abbreviation(
      abbrev, 4, dw_tag_subprogram_test, false,
      {
          {specification, ref_addr},
          {decl_line,     data2   },
          {low_pc,        addr    },
          {high_pc,       addr    }
  });
  abbreviation(
      abbrev, 5, dw_tag_subprogram_test, false,
      {
          {low_pc,  addr },
          {high_pc, data4}
  });
  abbrev.uleb(6);
  abbrev.uleb(dw_tag_subprogram_test);
  abbrev.u8(0);
  attribute(abbrev, decl_file, implicit_const);
  abbrev.sleb(3);
  attribute(abbrev, decl_line, data1);
  attribute(abbrev, low_pc, addr);
  attribute(abbrev, high_pc, data4);
  abbrev.uleb(0);
  abbrev.uleb(0);
  abbreviation(
      abbrev, 7, dw_tag_subprogram_test, false,
      {
          {origin,  ref4 },
          {low_pc,  addr },
          {high_pc, data4}
  });
  for(auto const& [code, tag] : {
          std::pair{8, dw_tag_variable_test  },
          {9, dw_tag_subprogram_test}
  })
  {
    abbreviation(
        abbrev, static_cast<std::uint64_t>(code), tag, false,
        {
            {decl_file, data1},
            {decl_line, data1},
            {low_pc,    addr },
            {high_pc,   data4}
    });
  }
  abbreviation(
      abbrev, 10, dw_tag_subprogram_test, false,
      {
          {decl_file, data1},
          {decl_line, data1},
          {low_pc,    addr },
          {high_pc,   addrx}
  });
  abbreviation(
      abbrev, 11, dw_tag_subprogram_test, false,
      {
          {decl_file, data1},
          {decl_line, data1},
          {low_pc,    addrx},
          {high_pc,   data4}
  });
  abbreviation(
      abbrev, 12, dw_tag_subprogram_test, false,
      {
          {origin,    ref4 },
          {decl_file, data1},
          {low_pc,    addr },
          {high_pc,   data4}
  });
  abbrev.uleb(0);

  byte_writer die;
  die.uleb(1);
  die.u32(0);
  die.u32(8);
  auto const declared = 12 + die.size();
  die.uleb(2);
  die.u8(1);
  die.u8(30);
  die.uleb(3);
  die.u32(static_cast<std::uint32_t>(declared));
  die.uleb(1);
  die.u32(0x10);
  auto const specified = 12 + die.size();
  die.uleb(2);
  die.u8(2);
  die.u8(40);
  die.uleb(4);
  die.u32(static_cast<std::uint32_t>(specified));
  die.u16(44);
  die.u32(0x2000);
  die.u32(0x2020);
  die.uleb(5);
  die.u32(0x3000);
  die.u32(0x10);
  for(int copy = 0; copy < 2; ++copy)
  {
    die.uleb(6);
    die.u8(50);
    die.u32(0x4000);
    die.u32(8);
  }
  auto const looping = 12 + die.size();
  die.uleb(7);
  die.u32(static_cast<std::uint32_t>(looping));
  die.u32(0x5000);
  die.u32(4);
  die.uleb(8);
  die.u8(1);
  die.u8(70);
  die.u32(0x6000);
  die.u32(4);
  die.uleb(9);
  die.u8(1);
  die.u8(80);
  die.u32(0x7000);
  die.u32(0);
  die.uleb(9);
  die.u8(1);
  die.u8(0);
  die.u32(0x7800);
  die.u32(4);
  die.uleb(10);
  die.u8(5);
  die.u8(90);
  die.u32(0x8000);
  die.uleb(2);
  die.uleb(11);
  die.u8(1);
  die.u8(95);
  die.uleb(0x4000'0000'0000'0001);
  die.u32(4);
  die.uleb(11);
  die.u8(1);
  die.u8(96);
  die.uleb(3);
  die.u32(4);
  die.uleb(12);
  die.u32(static_cast<std::uint32_t>(declared));
  die.u8(6);
  die.u32(0xa000);
  die.u32(4);
  die.u8(0);

  byte_writer addresses;
  addresses.u32(16);
  addresses.u16(5);
  addresses.u8(4);
  addresses.u8(0);
  addresses.u32(0x1111'0000);
  addresses.u32(0x1000);
  addresses.u32(0x8010);

  auto const           info = wrap_cu_v5(die);
  debug_sections const sections{
      .info        = info,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = {},
      .line_str    = {},
      .str_offsets = {},
      .addr        = addresses.bytes};
  using row = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t>;
  auto const found = scan_functions(sections)
                   | std::views::transform([](auto const& held) {
                       return row{held.low, held.high, held.line_offset, held.file, held.line};
                     })
                   | std::ranges::to<std::vector>();
  CHECK(
      found
      == std::vector<row>{
          {0x1000, 0x1010, 0, 1, 30},
          {0x2000, 0x2020, 0, 2, 44},
          {0x4000, 0x4008, 0, 3, 50},
          {0x8000, 0x8010, 0, 5, 90},
          {0xa000, 0xa004, 0, 6, 30},
  });
}

TEST_CASE("scan_functions rejects a DIE with an unknown abbreviation code")
{
  byte_writer abbrev;
  abbreviation(abbrev, 1, 0x11, true, {});
  abbrev.uleb(0);
  byte_writer die;
  die.uleb(1);
  die.uleb(2);
  auto const           info = wrap_cu_v5(die);
  debug_sections const sections{
      .info        = info,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = {},
      .line_str    = {},
      .str_offsets = {}};
  CHECK_THROWS_AS(std::ignore = scan_functions(sections), popcorn::image_error);
}

TEST_CASE("load_elf: debug functions carry the declaring file and line in every DWARF version")
{
  for(auto const* variable :
      {"POPCORN_IMAGE_ELF", "POPCORN_IMAGE_DW4_ELF", "POPCORN_IMAGE_DW5_ELF",
       "POPCORN_IMAGE_DW5GCC_ELF"})
  {
    CAPTURE(variable);
    auto const loaded = load_elf(env_path(variable));
    std::vector<std::tuple<std::string, std::string, std::uint32_t, bool>> seen;
    for(auto const& function : loaded.functions)
    {
      auto const symbol = std::ranges::find_if(loaded.symbols, [&](auto const& held) {
        return held.value == function.low and held.kind == popcorn::image::symbol_kind::function;
      });
      REQUIRE(symbol != loaded.symbols.end());
      auto const file = loaded.files.at(function.file);
      seen.emplace_back(
          symbol->name, file.substr(file.rfind('/') + 1), function.line,
          function.high == symbol->value + symbol->size);
    }
    CHECK(
        seen
        == std::vector<std::tuple<std::string, std::string, std::uint32_t, bool>>{
            {"scale",            "main.c",   12, true},
            {"Reset_Handler",    "main.c",   17, true},
            {"scale",            "shapes.c", 5,  true},
            {"shapes_perimeter", "shapes.c", 12, true},
    });
  }
}

TEST_CASE("load_elf keeps the line table and no debug function when a DIE cannot be read")
{
  v4_line_program_builder program;
  program.set_address(0x0800'0000);
  program.copy();
  program.advance_pc(2);
  program.end_sequence();
  byte_writer abbrev;
  abbreviation(
      abbrev, 1, 0x11, true,
      {
          {0x10, 0x17}
  });
  abbrev.uleb(0);
  byte_writer die;
  die.uleb(1);
  die.u32(0);
  die.uleb(9);
  popcorn::testing::synthetic_elf builder;
  builder.add_section(".debug_info", SHT_PROGBITS, 0, wrap_cu_v5(die));
  builder.add_section(".debug_abbrev", SHT_PROGBITS, 0, abbrev.bytes);
  builder.add_section(".debug_line", SHT_PROGBITS, 0, program.build());
  auto const path =
      popcorn::testing::synthetic_elf::write(builder.build(), "debug-function-unreadable.elf");
  auto const loaded = load_elf(path);
  CHECK_FALSE(loaded.lines.empty());
  CHECK(loaded.functions.empty());
}

TEST_CASE("scan_functions leaves out an indexed address without DW_AT_addr_base")
{
  byte_writer abbrev;
  abbreviation(
      abbrev, 1, 0x11, true,
      {
          {0x10, 0x17}
  });
  abbreviation(
      abbrev, 2, dw_tag_subprogram_test, false,
      {
          {0x3a, 0x0b},
          {0x3b, 0x0b},
          {0x11, 0x1b},
          {0x12, 0x06}
  });
  abbrev.uleb(0);
  byte_writer die;
  die.uleb(1);
  die.u32(0);
  die.uleb(2);
  die.u8(1);
  die.u8(7);
  die.uleb(1);
  die.u32(4);
  die.u8(0);
  byte_writer addresses;
  addresses.u32(12);
  addresses.u16(5);
  addresses.u8(4);
  addresses.u8(0);
  addresses.u32(0x1111'0000);
  addresses.u32(0x1000);
  auto const           info = wrap_cu_v5(die);
  debug_sections const sections{
      .info        = info,
      .abbrev      = abbrev.bytes,
      .line        = {},
      .str         = {},
      .line_str    = {},
      .str_offsets = {},
      .addr        = addresses.bytes};
  CHECK(scan_functions(sections).empty());
}

TEST_CASE("load_elf clears the Thumb bit of debug functions and drops one past the address space")
{
  v4_line_program_builder program;
  program.set_address(0x0800'0000);
  program.copy();
  program.advance_pc(8);
  program.end_sequence();
  byte_writer abbrev;
  abbreviation(
      abbrev, 1, 0x11, true,
      {
          {0x10, 0x17}
  });
  abbreviation(
      abbrev, 2, dw_tag_subprogram_test, false,
      {
          {0x3a, 0x0b},
          {0x3b, 0x0b},
          {0x11, 0x01},
          {0x12, 0x06}
  });
  abbrev.uleb(0);
  byte_writer die;
  die.uleb(1);
  die.u32(0);
  die.uleb(2);
  die.u8(1);
  die.u8(3);
  die.u32(0x0800'0001);
  die.u32(8);
  die.uleb(2);
  die.u8(1);
  die.u8(4);
  die.u32(0xffff'fff0);
  die.u32(0x20);
  die.u8(0);
  popcorn::testing::synthetic_elf builder;
  builder.add_section(".debug_info", SHT_PROGBITS, 0, wrap_cu_v5(die));
  builder.add_section(".debug_abbrev", SHT_PROGBITS, 0, abbrev.bytes);
  builder.add_section(".debug_line", SHT_PROGBITS, 0, program.build());
  auto const path =
      popcorn::testing::synthetic_elf::write(builder.build(), "debug-function-thumb.elf");
  auto const loaded = load_elf(path);
  REQUIRE(loaded.files == std::vector<std::string>{"a.c"});
  CHECK(
      loaded.functions
      == std::vector<popcorn::image::debug_function>{
          {.low = 0x0800'0000, .high = 0x0800'0008, .file = 0, .line = 3}
  });
}
