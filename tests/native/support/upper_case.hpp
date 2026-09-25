#pragma once

#include <string>
#include <string_view>

namespace popcorn::testing
{
/**
 * @brief ASCII upper-case copy of a name, for enumerator and environment variable names.
 * @param name Text to convert, only `a` to `z` change.
 * @return The converted text.
 */
constexpr auto upper_case(std::string_view name) -> std::string
{
  std::string out;
  out.reserve(name.size());
  for(char const c : name)
  {
    out.push_back(c >= 'a' and c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  }
  return out;
}
}
