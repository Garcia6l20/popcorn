#include "digest.hpp"

#include <reflex/hash/sha256.hpp>

#include <array>
#include <cstdint>

namespace popcorn::image::detail
{
namespace
{
auto encode_le32(std::uint32_t value) -> std::array<std::byte, 4>
{
  return {
      static_cast<std::byte>(value & 0xff),
      static_cast<std::byte>((value >> 8) & 0xff),
      static_cast<std::byte>((value >> 16) & 0xff),
      static_cast<std::byte>((value >> 24) & 0xff),
  };
}
}

auto digest_of(std::vector<segment> const& segments) -> std::string
{
  std::vector<std::byte> hashed;

  std::size_t i = 0;
  while(i < segments.size())
  {
    address     run_start = segments[i].start;
    std::size_t j         = i;
    std::size_t run_size  = 0;
    while(j < segments.size()
          and (j == i or segments[j].start == segments[j - 1].start + segments[j - 1].bytes.size()))
    {
      run_size += segments[j].bytes.size();
      ++j;
    }

    auto const start_le = encode_le32(run_start);
    auto const size_le  = encode_le32(static_cast<std::uint32_t>(run_size));
    hashed.insert(hashed.end(), start_le.begin(), start_le.end());
    hashed.insert(hashed.end(), size_le.begin(), size_le.end());

    for(std::size_t k = i; k < j; ++k)
    {
      hashed.insert(hashed.end(), segments[k].bytes.begin(), segments[k].bytes.end());
    }
    i = j;
  }

  auto const digest = reflex::sha256_of(hashed);
  auto const hex    = reflex::to_hex(digest);
  return std::string{hex.begin(), hex.end()};
}
}
