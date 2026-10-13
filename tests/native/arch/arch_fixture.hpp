#pragma once

#include "../support/machine_fixture.hpp"

#include "cortex_m.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace popcorn::testing
{
/** @brief Machine over the Cortex-M module, a firmware loaded and reset. */
using arch_fixture = machine_fixture<arch::armv8m::cortex_m>;

/**
 * @brief The core test description with fields changed in code.
 * @param edit Called with the description to change.
 */
inline auto m33_with(auto&& edit) -> part_description
{
  auto desc = m33();
  edit(desc);
  return desc;
}

/**
 * @brief Words of a firmware array read through the debugger view.
 * @param target Machine to read.
 * @param at First word.
 * @param count Words to read.
 */
inline auto words_at(core::machine& target, address at, std::size_t count)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::size_t index = 0; index < count; ++index)
  {
    auto const word = target.mem().read(static_cast<address>(at + 4 * index), 4);
    REQUIRE(word.has_value());
    out.push_back(*word);
  }
  return out;
}
}
