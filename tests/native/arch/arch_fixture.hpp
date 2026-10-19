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
 * @brief The core test description with the Security Extension: 8 SAU regions, 8 Non-secure
 *        and @p secure_mpu Secure MPU regions.
 */
inline auto m33_trustzone(std::uint8_t secure_mpu = 8) -> part_description
{
  return m33_with([&](part_description& desc) {
    desc.core.trustzone          = true;
    desc.core.sau_regions        = 8;
    desc.core.mpu_regions_secure = secure_mpu;
  });
}

/** @brief Machine options with TrustZone on and an IDAU shaped as the STM32U5's, RM0456 2.2. */
inline auto trustzone_options() -> core::machine_options
{
  core::machine_options options;
  options.trustzone = true;
  options.idau      = {
      {.start = 0x0000'0000, .end = 0x0C00'0000, .nsc = false, .exempt = false},
      {.start = 0x0C00'0000, .end = 0x1000'0000, .nsc = true,  .exempt = false},
      {.start = 0x1000'0000, .end = 0x3000'0000, .nsc = false, .exempt = false},
      {.start = 0x3000'0000, .end = 0x4000'0000, .nsc = true,  .exempt = false},
      {.start = 0x4000'0000, .end = 0x5000'0000, .nsc = false, .exempt = false},
      {.start = 0x5000'0000, .end = 0x6000'0000, .nsc = true,  .exempt = false},
      {.start = 0x6000'0000, .end = 0xE000'0000, .nsc = false, .exempt = false},
  };
  return options;
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
