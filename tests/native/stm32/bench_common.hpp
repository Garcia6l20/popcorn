#pragma once

#include <popcorn/core/description.hpp>

namespace popcorn::testing::bench
{
inline auto u575() -> popcorn::part_description const&
{
  static auto const desc = popcorn::load_description(POPCORN_U575_DESC);
  return desc;
}
}
