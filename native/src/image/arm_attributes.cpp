#include "arm_attributes.hpp"
#include "elf_file.hpp"

#include <popcorn/core/errors.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace popcorn::image::detail
{
namespace
{
constexpr std::uint64_t tag_file          = 1;
constexpr std::uint64_t tag_abi_vfp_args  = 28;
constexpr std::uint64_t tag_compatibility = 32;
constexpr std::size_t   min_scope_len     = 5;

auto read_u32(std::span<std::byte const> data, std::size_t pos) -> std::uint32_t
{
  std::uint32_t value = 0;
  std::memcpy(&value, data.data() + pos, sizeof value);
  return value;
}

auto read_uleb(std::span<std::byte const> data, std::size_t& pos) -> std::uint64_t
{
  std::uint64_t result = 0;
  unsigned      shift  = 0;
  while(pos < data.size())
  {
    auto const byte = static_cast<std::uint8_t>(data[pos++]);
    if(shift >= 64)
    {
      throw image_error{"header", 0, "malformed ULEB128 in .ARM.attributes"};
    }
    result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    if((byte & 0x80) == 0)
    {
      break;
    }
    shift += 7;
  }
  return result;
}

struct file_tags
{
  std::optional<std::uint64_t> vfp_args;
};

auto parse_file_scope(std::span<std::byte const> data, std::size_t pos, std::size_t scope_end)
    -> file_tags
{
  file_tags tags;
  while(pos < scope_end)
  {
    auto const pos_before = pos;
    auto const tag        = read_uleb(data, pos);
    if(tag == tag_compatibility)
    {
      read_uleb(data, pos);
      read_ntbs(data, pos);
    }
    else if(tag == 4 || tag == 5 || (tag > 32 && tag % 2 == 1))
    {
      read_ntbs(data, pos);
    }
    else
    {
      auto const value = read_uleb(data, pos);
      if(tag == tag_abi_vfp_args)
      {
        tags.vfp_args = value;
      }
    }
    if(pos == pos_before)
    {
      break;
    }
  }
  return tags;
}
}

auto hard_float_abi(std::span<std::byte const> attributes) -> arch::float_abi
{
  if(attributes.empty() || attributes[0] != std::byte{'A'})
  {
    return arch::float_abi::hard_double;
  }

  std::size_t pos = 1;
  while(pos + 4 <= attributes.size())
  {
    auto const subsection_start = pos;
    auto const length           = read_u32(attributes, pos);
    if(length < 4)
    {
      break;
    }
    auto const subsection_end = subsection_start + length;
    if(subsection_end > attributes.size())
    {
      break;
    }
    pos += 4;
    auto const vendor = read_ntbs(attributes, pos);

    if(vendor != "aeabi")
    {
      pos = subsection_end;
      continue;
    }

    while(pos < subsection_end)
    {
      if(pos + min_scope_len > subsection_end)
      {
        break;
      }
      auto const scope_field = pos;
      auto const scope_tag   = static_cast<std::uint8_t>(attributes[pos]);
      pos += 1;
      auto const scope_len = read_u32(attributes, pos);
      pos += 4;
      if(scope_len < min_scope_len || scope_field + scope_len > subsection_end)
      {
        break;
      }
      auto const scope_end = scope_field + scope_len;
      if(scope_tag != tag_file)
      {
        pos = scope_end;
        continue;
      }
      auto const tags = parse_file_scope(attributes.first(scope_end), pos, scope_end);

      if(tags.vfp_args == 0)
      {
        return arch::float_abi::soft;
      }
      return arch::float_abi::hard_double;
    }
    pos = subsection_end;
  }
  return arch::float_abi::hard_double;
}
}
