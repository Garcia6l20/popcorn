#pragma once

#include "upper_case.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace popcorn::testing
{
/**
 * @brief Path of a test firmware ELF, from the `POPCORN_FW_<NAME>` variable the build script
 *        sets in the test environment.
 * @param name Firmware name, lower case, such as `basic`.
 */
inline auto firmware(std::string_view name) -> std::filesystem::path
{
  auto const        variable = "POPCORN_FW_" + upper_case(name);
  char const* const path     = std::getenv(variable.c_str());
  REQUIRE_MESSAGE(path != nullptr, variable);
  return path;
}
}
