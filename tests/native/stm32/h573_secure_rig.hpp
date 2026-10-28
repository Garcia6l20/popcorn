#pragma once

#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/flash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace popcorn::testing
{
/** @brief An STM32H573 at TZEN=1 on a firmware parked in an SRAM loop, the SAU at ALLNS, with
 *         Secure accesses through the 0x5xxx alias and Non-secure ones through the 0x4xxx
 *         address, both privileged debug accesses of that state. */
struct h573_secure_rig
{
  static constexpr std::uint32_t parked_at = 0x2000'0100;

  stm32_fixture f;

  explicit h573_secure_rig(
      std::string_view                   firmware = "h573_pwr_ll",
      periph::stm32::option_bytes const& wanted   = {.tzen = true})
      : f{h573(), firmware, options(wanted)}
  {
    REQUIRE(f.target->mem().write(parked_at, 4, 0xE7FD'BF30).has_value());
    f.set("pc", parked_at | 1u);
    REQUIRE(f.target->mem(true).write(0xe000'edd0, 4, 0x2).has_value());
  }

  static auto options(periph::stm32::option_bytes const& wanted) -> core::machine_options
  {
    auto out = stm32_options();
    REQUIRE(periph::stm32::configure_option_bytes(h573(), wanted, out).has_value());
    return out;
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view name) const
      -> popcorn::address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const* found = popcorn::find_register(desc, name);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto
      bit(std::string_view peripheral, std::string_view reg, std::string_view field) const
      -> std::uint32_t
  {
    auto const* found = popcorn::find_register(f.target->peripheral(peripheral).description(), reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    return ((std::uint32_t{1} << bits->width) - 1) << bits->lsb;
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg, bool secure) const
      -> std::uint32_t
  {
    auto const value = f.target->mem(secure).read(alias(peripheral, reg, secure), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value, bool secure)
      const
  {
    REQUIRE(f.target->mem(secure).write(alias(peripheral, reg, secure), 4, value).has_value());
  }

  [[nodiscard]] auto alias(std::string_view peripheral, std::string_view reg, bool secure) const
      -> popcorn::address
  {
    return at(peripheral, reg) + (secure ? 0x1000'0000u : 0u);
  }

  [[nodiscard]] auto peek(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const* found = popcorn::find_register(f.target->peripheral(peripheral).description(), reg);
    REQUIRE(found != nullptr);
    return f.target->peripheral(peripheral).peek(found->offset & ~3u);
  }
};
}
