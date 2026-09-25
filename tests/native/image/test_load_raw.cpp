#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/image/raw.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using popcorn::image::load_bin;
using popcorn::image::load_elf;
using popcorn::image::load_hex;

namespace
{
auto env_path(char const* name) -> std::string
{
  char const* const value = std::getenv(name);
  REQUIRE(value != nullptr);
  return value;
}

auto scratch_dir() -> std::filesystem::path
{
  return env_path("POPCORN_IMAGE_SCRATCH");
}

auto write_text(std::string const& name, std::string const& text) -> std::filesystem::path
{
  std::filesystem::create_directories(scratch_dir());
  auto const    path = scratch_dir() / name;
  std::ofstream out{path, std::ios::binary};
  out << text;
  return path;
}

auto hex_record(
    std::uint8_t                     byte_count,
    std::uint16_t                    addr,
    std::uint8_t                     type,
    std::vector<std::uint8_t> const& data) -> std::string
{
  unsigned sum = byte_count + (addr >> 8) + (addr & 0xff) + type;
  for(auto b : data)
  {
    sum += b;
  }
  auto const  checksum = static_cast<std::uint8_t>(-static_cast<int>(sum) & 0xff);
  std::string out      = std::format(":{:02X}{:04X}{:02X}", byte_count, addr, type);
  for(auto b : data)
  {
    out += std::format("{:02X}", b);
  }
  out += std::format("{:02X}", checksum);
  return out;
}

auto data_record(std::uint16_t addr, std::vector<std::uint8_t> const& bytes) -> std::string
{
  return hex_record(static_cast<std::uint8_t>(bytes.size()), addr, 0x00, bytes);
}

auto eof_record() -> std::string
{
  return hex_record(0, 0, 0x01, {});
}

auto ext_linear_record(std::uint16_t upper) -> std::string
{
  return hex_record(
      2, 0, 0x04, {static_cast<std::uint8_t>(upper >> 8), static_cast<std::uint8_t>(upper & 0xff)});
}

auto ext_segment_record(std::uint16_t segment) -> std::string
{
  return hex_record(
      2, 0, 0x02,
      {static_cast<std::uint8_t>(segment >> 8), static_cast<std::uint8_t>(segment & 0xff)});
}

auto join_lines(std::vector<std::string> const& lines, std::string const& ending = "\n")
    -> std::string
{
  std::string out;
  for(auto const& line : lines)
  {
    out += line + ending;
  }
  return out;
}

auto load_by_address(popcorn::image::image const& img) -> std::map<popcorn::address, std::byte>
{
  std::map<popcorn::address, std::byte> out;
  for(auto const& seg : img.segments)
  {
    for(std::size_t i = 0; i < seg.bytes.size(); ++i)
    {
      out[seg.start + static_cast<popcorn::address>(i)] = seg.bytes[i];
    }
  }
  return out;
}
}

TEST_CASE("load_hex places bytes at the same addresses as load_elf")
{
  auto const from_elf = load_elf(env_path("POPCORN_IMAGE_ELF"));
  auto const from_hex = load_hex(env_path("POPCORN_IMAGE_HEX"));
  CHECK(load_by_address(from_elf) == load_by_address(from_hex));
  CHECK(from_elf.digest == from_hex.digest);
}

TEST_CASE("load_bin at the ELF's base address matches the ELF load over the union of ranges")
{
  auto const from_elf = load_elf(env_path("POPCORN_IMAGE_ELF"));
  auto const base     = from_elf.segments.front().start;
  auto const from_bin = load_bin(env_path("POPCORN_IMAGE_BIN"), base);

  auto const elf_map = load_by_address(from_elf);
  auto const bin_map = load_by_address(from_bin);
  for(auto const& [addr, value] : elf_map)
  {
    auto const it = bin_map.find(addr);
    REQUIRE(it != bin_map.end());
    CHECK(it->second == value);
  }
}

TEST_CASE("load_bin, load_hex and load_elf of the same firmware give the identical digest")
{
  auto const from_elf = load_elf(env_path("POPCORN_IMAGE_ELF"));
  auto const from_hex = load_hex(env_path("POPCORN_IMAGE_HEX"));
  auto const from_bin = load_bin(env_path("POPCORN_IMAGE_BIN"), from_elf.segments.front().start);
  CHECK(from_elf.digest == from_hex.digest);
  CHECK(from_elf.digest == from_bin.digest);
}

TEST_CASE("load_hex parses a start linear address record into entry")
{
  auto const img = load_hex(env_path("POPCORN_IMAGE_HEX"));
  REQUIRE(img.entry.has_value());
  CHECK((*img.entry & 1) != 0);
}

TEST_CASE("load_hex rejects a bad checksum")
{
  auto record     = data_record(0, {1, 2, 3, 4});
  record.back()   = record.back() == 'F' ? 'E' : 'F';
  auto const path = write_text("hex-bad-checksum.hex", join_lines({record, eof_record()}));
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a bad hex digit")
{
  auto record     = data_record(0, {1, 2, 3, 4});
  record[1]       = 'Z';
  auto const path = write_text("hex-bad-digit.hex", join_lines({record, eof_record()}));
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a short record")
{
  auto const path = write_text("hex-short.hex", join_lines({std::string{":020000"}, eof_record()}));
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex accepts both extended segment and extended linear address records")
{
  auto const text = join_lines({
      ext_segment_record(0x0000),
      ext_linear_record(0x0800),
      data_record(0, {0xAA}),
      eof_record(),
  });
  auto const path = write_text("hex-both-extended.hex", text);
  auto const img  = load_hex(path);
  REQUIRE(img.segments.size() == 1);
  CHECK(img.segments.front().start == 0x0800'0000);
}

TEST_CASE("load_hex rejects a data record after end of file")
{
  auto const text = join_lines({data_record(0, {1}), eof_record(), data_record(0, {2})});
  auto const path = write_text("hex-data-after-eof.hex", text);
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a file missing the end of file record")
{
  auto const path = write_text("hex-no-eof.hex", join_lines({data_record(0, {1})}));
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects an empty file")
{
  auto const path = write_text("hex-empty.hex", "");
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex accepts CRLF line endings")
{
  auto const text = join_lines({data_record(0, {1, 2}), eof_record()}, "\r\n");
  auto const path = write_text("hex-crlf.hex", text);
  auto const img  = load_hex(path);
  REQUIRE(img.segments.size() == 1);
  CHECK(img.segments.front().bytes.size() == 2);
}

TEST_CASE("load_hex keeps two runs separated by a gap as two segments")
{
  auto const text = join_lines({data_record(0, {1, 2}), data_record(0x100, {3, 4}), eof_record()});
  auto const path = write_text("hex-gap.hex", text);
  auto const img  = load_hex(path);
  REQUIRE(img.segments.size() == 2);
  CHECK(img.segments[0].start == 0);
  CHECK(img.segments[1].start == 0x100);
}

TEST_CASE("load_hex rejects overlapping runs")
{
  auto const text =
      join_lines({data_record(0, {1, 2, 3, 4}), data_record(2, {5, 6}), eof_record()});
  auto const path = write_text("hex-overlap.hex", text);
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a data record whose extended-linear run passes 4 GiB")
{
  auto const text = join_lines({
      ext_linear_record(0xFFFF),
      data_record(0xFFF8, std::vector<std::uint8_t>(16, 0)),
      eof_record(),
  });
  auto const path = write_text("hex-past-4gib.hex", text);
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a data record that wraps its 64 KiB segment")
{
  auto const text = join_lines({
      ext_segment_record(0x0000),
      data_record(0xFFF8, std::vector<std::uint8_t>(16, 0)),
      eof_record(),
  });
  auto const path = write_text("hex-segment-wrap.hex", text);
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a malformed start segment address record")
{
  auto const bad_record = hex_record(2, 0, 0x03, {0x00, 0x00});
  auto const path       = write_text("hex-bad-03.hex", join_lines({bad_record, eof_record()}));
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a malformed start linear address record")
{
  auto const bad_record = hex_record(2, 0, 0x05, {0x00, 0x00});
  auto const path       = write_text("hex-bad-05.hex", join_lines({bad_record, eof_record()}));
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_hex rejects a record longer than its declared byte count")
{
  auto record = data_record(0, {1, 2});
  record.insert(record.size() - 2, "AB");
  auto const path = write_text("hex-too-long.hex", join_lines({record, eof_record()}));
  CHECK_THROWS_AS(load_hex(path), popcorn::image_error);
}

TEST_CASE("load_bin rejects an empty file")
{
  auto const path = write_text("bin-empty.bin", "");
  CHECK_THROWS_AS(load_bin(path, 0x0800'0000), popcorn::image_error);
}

TEST_CASE("load_bin rejects a missing file")
{
  CHECK_THROWS_AS(load_bin("does-not-exist.bin", 0x0800'0000), popcorn::image_error);
}

TEST_CASE("load_bin rejects a start+size that does not fit below 4 GiB")
{
  auto const path = write_text("bin-overflow.bin", std::string(17, '\0'));
  CHECK_THROWS_AS(load_bin(path, 0xFFFF'FFF0u), popcorn::image_error);
}
