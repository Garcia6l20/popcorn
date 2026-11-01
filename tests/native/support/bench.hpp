#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::testing::bench
{
inline auto percentile(std::vector<double> samples, double fraction) -> double
{
  std::ranges::sort(samples);
  auto const index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
  return samples[index];
}

inline auto median(std::vector<double> values) -> double
{
  std::ranges::sort(values);
  auto const n = values.size();
  return n % 2 != 0 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

inline auto option(std::span<char*> args, std::string_view name) -> std::optional<std::string_view>
{
  for(std::size_t i = 1; i + 1 < args.size(); ++i)
  {
    if(std::string_view{args[i]} == name)
    {
      return std::string_view{args[i + 1]};
    }
  }
  return std::nullopt;
}

inline auto first_line(char const* path) -> std::string
{
  std::ifstream file{path};
  std::string   line;
  std::getline(file, line);
  return line;
}

struct fnv
{
  static constexpr std::uint64_t basis = 0xcbf2'9ce4'8422'2325u;
  static constexpr std::uint64_t prime = 0x100'0000'01b3u;

  std::uint64_t value = basis;

  void mix(std::uint64_t word) noexcept
  {
    for(int shift = 0; shift < 64; shift += 8)
    {
      value = (value ^ ((word >> shift) & 0xffu)) * prime;
    }
  }
};
}
