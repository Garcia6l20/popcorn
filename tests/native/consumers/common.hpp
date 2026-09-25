#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>

#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <cstdlib>

namespace popcorn::testing
{
/** @brief The toy description of `tests/data/desc/toy.json`, path from `POPCORN_TOY_DESC`. */
inline auto toy_description() -> mcu_description
{
  char const* const path = std::getenv("POPCORN_TOY_DESC");
  REQUIRE(path != nullptr);
  return load_description(path);
}
}
