#include "digest.hpp"
#include "elf_file.hpp"

#include <popcorn/image/raw.hpp>

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace popcorn::image
{
namespace
{
using detail::read_file;

struct hex_chunk
{
  address                addr{};
  std::vector<std::byte> bytes;
};

auto hex_byte(
    std::string_view   text,
    std::size_t        pos,
    std::string const& name,
    std::size_t        line,
    address            addr = 0) -> std::uint8_t
{
  if(pos + 2 > text.size())
  {
    throw image_error{name, addr, std::format("line {}: short record", line)};
  }
  std::uint8_t value{};
  auto const [ptr, ec] = std::from_chars(text.data() + pos, text.data() + pos + 2, value, 16);
  if(ec != std::errc{} || ptr != text.data() + pos + 2)
  {
    throw image_error{name, addr, std::format("line {}: bad hex digit", line)};
  }
  return value;
}

auto strip_line_ending(std::string_view line) -> std::string_view
{
  while(!line.empty() && (line.back() == '\r' || line.back() == '\n'))
  {
    line.remove_suffix(1);
  }
  return line;
}

auto build_segments(std::vector<hex_chunk> chunks, std::string const& name) -> std::vector<segment>
{
  std::ranges::sort(chunks, {}, &hex_chunk::addr);

  std::vector<segment> segments;
  for(std::size_t i = 1; i < chunks.size(); ++i)
  {
    auto const& prev = chunks[i - 1];
    auto const& curr = chunks[i];
    if(std::uint64_t{curr.addr} < std::uint64_t{prev.addr} + prev.bytes.size())
    {
      throw image_error{name, curr.addr, std::format("overlaps run at {:#010x}", prev.addr)};
    }
  }

  for(auto& chunk : chunks)
  {
    auto const memory_size = static_cast<std::uint32_t>(chunk.bytes.size());
    segments.push_back(
        segment{
            .name        = std::format("{}@{:#010x}", name, chunk.addr),
            .start       = chunk.addr,
            .bytes       = std::move(chunk.bytes),
            .memory_size = memory_size,
            .executable  = true,
            .writable    = false,
        });
  }
  return segments;
}
}

auto load_bin(std::filesystem::path const& file, address start) -> image
{
  auto bytes = read_file(file);
  if(bytes.empty())
  {
    throw image_error{file.filename().string(), start, "empty file"};
  }
  if(std::uint64_t{start} + bytes.size() > std::uint64_t{1} << 32)
  {
    throw image_error{file.filename().string(), start, "does not fit below 4 GiB"};
  }

  image      img;
  auto const memory_size = static_cast<std::uint32_t>(bytes.size());
  img.segments.push_back(
      segment{
          .name        = file.filename().string(),
          .start       = start,
          .bytes       = std::move(bytes),
          .memory_size = memory_size,
          .executable  = true,
          .writable    = false,
      });
  img.digest = detail::digest_of(img.segments);
  return img;
}

auto load_hex(std::filesystem::path const& file) -> image
{
  std::ifstream stream{file, std::ios::binary};
  if(!stream)
  {
    throw image_error{file.filename().string(), 0, "cannot open " + file.string()};
  }
  auto const name = file.filename().string();

  enum class base_kind
  {
    none,
    segment,
    linear
  };

  std::vector<hex_chunk> chunks;
  std::optional<address> entry;
  std::uint32_t          extended_base = 0;
  base_kind              base          = base_kind::none;
  bool                   seen_eof      = false;
  std::size_t            line_no       = 0;

  std::string raw_line;
  while(std::getline(stream, raw_line))
  {
    ++line_no;
    auto const line = strip_line_ending(raw_line);
    if(line.empty())
    {
      continue;
    }
    if(seen_eof)
    {
      throw image_error{name, 0, std::format("line {}: data after end of file record", line_no)};
    }
    if(line.front() != ':')
    {
      throw image_error{name, 0, std::format("line {}: record does not start with ':'", line_no)};
    }

    auto const body           = line.substr(1);
    auto const byte_count     = hex_byte(body, 0, name, line_no);
    auto const addr_hi        = hex_byte(body, 2, name, line_no);
    auto const addr_lo        = hex_byte(body, 4, name, line_no);
    auto const record_type    = hex_byte(body, 6, name, line_no);
    auto const record_addr    = static_cast<std::uint16_t>((addr_hi << 8) | addr_lo);
    auto const record_address = static_cast<address>(std::uint64_t{extended_base} + record_addr);

    std::size_t const data_start   = 8;
    std::size_t const data_end     = data_start + std::size_t{byte_count} * 2;
    std::size_t const checksum_pos = data_end;
    if(checksum_pos + 2 != body.size())
    {
      throw image_error{
          name, record_address,
          std::format("line {}: record length does not match byte count", line_no)};
    }

    std::vector<std::uint8_t> data(byte_count);
    for(std::size_t i = 0; i < byte_count; ++i)
    {
      data[i] = hex_byte(body, data_start + i * 2, name, line_no, record_address);
    }
    auto const checksum = hex_byte(body, checksum_pos, name, line_no, record_address);

    unsigned sum = byte_count + addr_hi + addr_lo + record_type + checksum;
    for(auto const b : data)
    {
      sum += b;
    }
    if((sum & 0xff) != 0)
    {
      throw image_error{name, record_address, std::format("line {}: bad checksum", line_no)};
    }

    switch(record_type)
    {
      case 0x00:
      {
        if(base == base_kind::segment && std::uint32_t{record_addr} + byte_count > 0x1'0000)
        {
          throw image_error{
              name, record_address,
              std::format("line {}: record wraps its 64 KiB segment", line_no)};
        }
        if(std::uint64_t{extended_base} + record_addr + byte_count > std::uint64_t{1} << 32)
        {
          throw image_error{
              name, record_address, std::format("line {}: run runs past 4 GiB", line_no)};
        }

        auto const             full_addr = extended_base + record_addr;
        std::vector<std::byte> payload(data.size());
        std::ranges::transform(data, payload.begin(), [](std::uint8_t b) { return std::byte{b}; });
        if(!chunks.empty()
           and std::uint64_t{chunks.back().addr} + chunks.back().bytes.size()
                   == std::uint64_t{full_addr})
        {
          auto& back = chunks.back();
          back.bytes.insert(back.bytes.end(), payload.begin(), payload.end());
        }
        else
        {
          chunks.push_back(hex_chunk{static_cast<address>(full_addr), std::move(payload)});
        }
        break;
      }
      case 0x01:
        if(byte_count != 0 || record_addr != 0)
        {
          throw image_error{
              name, record_address, std::format("line {}: malformed end of file record", line_no)};
        }
        seen_eof = true;
        break;
      case 0x02:
        if(byte_count != 2)
        {
          throw image_error{
              name, record_address,
              std::format("line {}: malformed extended segment address record", line_no)};
        }
        extended_base = (static_cast<std::uint32_t>(data[0]) << 8 | data[1]) << 4;
        base          = base_kind::segment;
        break;
      case 0x03:
        if(byte_count != 4)
        {
          throw image_error{
              name, record_address,
              std::format("line {}: malformed start segment address record", line_no)};
        }
        break;
      case 0x04:
        if(byte_count != 2)
        {
          throw image_error{
              name, record_address,
              std::format("line {}: malformed extended linear address record", line_no)};
        }
        extended_base = (static_cast<std::uint32_t>(data[0]) << 8 | data[1]) << 16;
        base          = base_kind::linear;
        break;
      case 0x05:
        if(byte_count != 4)
        {
          throw image_error{
              name, record_address,
              std::format("line {}: malformed start linear address record", line_no)};
        }
        entry = static_cast<address>(
            (static_cast<std::uint32_t>(data[0]) << 24)
            | (static_cast<std::uint32_t>(data[1]) << 16)
            | (static_cast<std::uint32_t>(data[2]) << 8)
            | data[3]);
        break;
      default:
        throw image_error{
            name, record_address,
            std::format("line {}: unknown record type {}", line_no, record_type)};
    }
  }

  if(!seen_eof)
  {
    throw image_error{name, 0, std::format("line {}: missing end of file record", line_no)};
  }

  image img;
  img.segments = build_segments(std::move(chunks), name);
  img.entry    = entry;
  img.digest   = detail::digest_of(img.segments);
  return img;
}
}
