#pragma once

#include "stm32_fixture.hpp"
#include <popcorn/core/types.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>

namespace popcorn::testing
{
/**
 * @brief Checks the main flash boot alias of an STM32L1 or STM32L4 machine at a power-on reset:
 *        VTOR reads 0, the core boots from the vector table read through the alias, a CPU load
 *        and a fetch at the alias reach the flash, a store to it is a bus fault, and with VTOR 0
 *        a SysTick exception takes its vector through the alias.
 * @param f Machine with its firmware loaded and every fault stopping the run.
 * @param flash_base Address of the flash, 0x08000000.
 * @param spare Flash address of an erased page the firmware does not use.
 * @param sram An SRAM address with 64 free bytes and room for a stack below it.
 */
inline void
    check_flash_boot_alias(stm32_fixture const& f, address flash_base, address spare, address sram)
{
  constexpr address vtor = 0xe000'ed08;
  constexpr address syst = 0xe000'e010;
  auto              host = f.target->mem();
  auto const        word = [&host](address at) {
    auto const value = host.read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  };
  auto const reg = [&f](std::string_view name) {
    return static_cast<std::uint32_t>(f.target->regs().read(*f.target->regs().find(name)));
  };
  f.target->reset(reset_kind::power_on);
  CHECK(word(vtor) == 0u);
  CHECK(reg("sp") == (word(flash_base) & ~3u));
  CHECK(reg("pc") == (word(flash_base + 4) & ~1u));
  CHECK(word(0) == word(flash_base));
  CHECK(word(spare - flash_base) == word(spare));

  REQUIRE(host.write(sram, 2, 0x6808u).has_value());
  REQUIRE(host.write(sram + 2, 2, 0xbe00u).has_value());
  f.set("r1", 4);
  f.set("r0", 0);
  f.set("pc", sram | 1u);
  REQUIRE(f.run_to_stop({.max_instructions = icount{4}}).reason == stop_reason::user);
  CHECK(reg("r0") == word(flash_base + 4));

  REQUIRE(host.write(spare, 2, 0x202au).has_value());
  REQUIRE(host.write(spare + 2, 2, 0xbe00u).has_value());
  f.set("r0", 0);
  f.set("pc", (spare - flash_base) | 1u);
  REQUIRE(f.run_to_stop({.max_instructions = icount{4}}).reason == stop_reason::user);
  CHECK(reg("r0") == 42u);
  CHECK(reg("pc") == spare - flash_base + 4);

  REQUIRE(host.write(sram + 4, 2, 0x6008u).has_value());
  REQUIRE(host.write(sram + 6, 2, 0xbe00u).has_value());
  f.set("r1", 0x100);
  f.set("r0", 0x1234);
  f.set("pc", (sram + 4) | 1u);
  auto const stored = f.run_to_stop({.max_instructions = icount{4}});
  REQUIRE(stored.reason == stop_reason::fault);
  REQUIRE(stored.fault.has_value());
  CHECK(stored.fault->kind == fault_kind::bus);
  CHECK(word(flash_base + 0x100) != 0x1234u);

  f.target->reset(reset_kind::power_on);
  REQUIRE(word(vtor) == 0u);
  auto const handler = sram + 0x20;
  REQUIRE(host.write(handler, 2, 0xbe00u).has_value());
  REQUIRE(host.write(sram + 0x10, 2, 0xe7feu).has_value());
  REQUIRE(host.write(flash_base + 15 * 4, 4, handler | 1u).has_value());
  REQUIRE(host.write(syst + 4, 4, 99u).has_value());
  REQUIRE(host.write(syst + 8, 4, 0u).has_value());
  REQUIRE(host.write(syst, 4, 7u).has_value());
  f.set("sp", sram + 0x1000);
  f.set("pc", (sram + 0x10) | 1u);
  REQUIRE(f.run_to_stop({.max_instructions = icount{1000}}).reason == stop_reason::user);
  CHECK(reg("pc") == handler + 2);
  CHECK((reg("xpsr") & 0x1ffu) == 15u);
}
}
