#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

auto options() -> core::machine_options
{
  auto options = popcorn::testing::stm32_options();
  return options;
}

auto idle() -> stm32_fixture
{
  stm32_fixture f{u575(), "rcc_ll", options()};
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  return f;
}

void idle_for(stm32_fixture const& f, std::chrono::nanoseconds span)
{
  auto const now = f.target->virtual_clock().time();
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000},
                     .until_time = vtime{now.ps + static_cast<std::uint64_t>(span.count()) * 1000}})
          .reason
      == stop_reason::time);
}

auto offset_of(std::string_view peripheral, std::string_view reg) -> std::uint32_t
{
  auto const& unit =
      *std::ranges::find(u575().peripherals, peripheral, &popcorn::peripheral_desc::name);
  auto const found = std::ranges::find(unit.registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != unit.registers.end());
  return found->offset;
}

auto bit_of(std::string_view peripheral, std::string_view reg, std::string_view field)
    -> std::uint32_t
{
  auto const& unit =
      *std::ranges::find(u575().peripherals, peripheral, &popcorn::peripheral_desc::name);
  auto const found = std::ranges::find(unit.registers, reg, &popcorn::register_desc::name);
  auto const bits  = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << bits->lsb;
}

void write(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::uint32_t        value)
{
  auto&      regs = f.target->peripheral(peripheral);
  auto const at   = offset_of(peripheral, reg);
  REQUIRE(f.target->model(peripheral)
              ->on_write(regs, at, 4, regs.peek(at), value, {.pc = 0, .now = icount{0}})
              .has_value());
}

auto pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  auto const word = f.target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
  REQUIRE(word.has_value());
  return (*word >> (irq % 32) & 1u) != 0;
}

auto flag(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::string_view     field) -> bool
{
  return (f.target->peripheral(peripheral).peek(offset_of(peripheral, reg))
          & bit_of(peripheral, reg, field))
      != 0;
}
}

TEST_CASE("both caches invalidate after reset, BUSYF then BSYENDF after the maintenance time")
{
  auto f = idle();
  for(std::string_view const unit : {"ICache", "DCACHE"})
  {
    auto const reg =
        unit == "ICache" ? std::string_view{"ICACHE_SR"} : std::string_view{"DCACHE_SR"};
    CAPTURE(unit);
    CHECK(flag(f, unit, reg, "BUSYF"));
    CHECK_FALSE(flag(f, unit, reg, "BSYENDF"));
  }
  idle_for(f, std::chrono::microseconds{20});
  CHECK_FALSE(flag(f, "ICache", "ICACHE_SR", "BUSYF"));
  CHECK(flag(f, "ICache", "ICACHE_SR", "BSYENDF"));
  CHECK_FALSE(flag(f, "DCACHE", "DCACHE_SR", "BUSYF"));
  CHECK(flag(f, "DCACHE", "DCACHE_SR", "BSYENDF"));
}

TEST_CASE("ICACHE invalidate raises its interrupt when enabled and FCR clears the flag")
{
  auto f = idle();
  idle_for(f, std::chrono::microseconds{20});
  write(f, "ICache", "ICACHE_FCR", bit_of("ICache", "ICACHE_FCR", "CBSYENDF"));
  CHECK_FALSE(flag(f, "ICache", "ICACHE_SR", "BSYENDF"));
  CHECK(f.target->peripheral("ICache").peek(offset_of("ICache", "ICACHE_FCR")) == 0);
  write(f, "ICache", "ICACHE_IER", bit_of("ICache", "ICACHE_IER", "BSYENDIE"));
  write(f, "ICache", "ICACHE_CR", bit_of("ICache", "ICACHE_CR", "CACHEINV"));
  CHECK(flag(f, "ICache", "ICACHE_SR", "BUSYF"));
  CHECK_FALSE(flag(f, "ICache", "ICACHE_CR", "CACHEINV"));
  CHECK_FALSE(pending(f, 107));
  idle_for(f, std::chrono::microseconds{20});
  CHECK(flag(f, "ICache", "ICACHE_SR", "BSYENDF"));
  CHECK(pending(f, 107));
}

TEST_CASE("disabling ICACHE starts an invalidate")
{
  auto f = idle();
  write(f, "ICache", "ICACHE_CR", bit_of("ICache", "ICACHE_CR", "EN"));
  idle_for(f, std::chrono::microseconds{20});
  write(f, "ICache", "ICACHE_FCR", bit_of("ICache", "ICACHE_FCR", "CBSYENDF"));
  write(f, "ICache", "ICACHE_CR", 0);
  CHECK(flag(f, "ICache", "ICACHE_SR", "BUSYF"));
}

TEST_CASE("a DCACHE range command runs only on an enabled idle cache and ends with CMDENDF")
{
  auto       f       = idle();
  auto const start   = bit_of("DCACHE", "DCACHE_CR", "STARTCMD");
  auto const command = 2u * bit_of("DCACHE", "DCACHE_CR", "CACHECMD");
  idle_for(f, std::chrono::microseconds{20});
  write(f, "DCACHE", "DCACHE_CR", command | start);
  CHECK_FALSE(flag(f, "DCACHE", "DCACHE_SR", "BUSYCMDF"));
  auto const enable = bit_of("DCACHE", "DCACHE_CR", "EN");
  write(f, "DCACHE", "DCACHE_IER", bit_of("DCACHE", "DCACHE_IER", "CMDENDIE"));
  write(f, "DCACHE", "DCACHE_CR", enable | command | start);
  CHECK(flag(f, "DCACHE", "DCACHE_SR", "BUSYCMDF"));
  CHECK_FALSE(flag(f, "DCACHE", "DCACHE_CR", "STARTCMD"));
  idle_for(f, std::chrono::microseconds{20});
  CHECK_FALSE(flag(f, "DCACHE", "DCACHE_SR", "BUSYCMDF"));
  CHECK(flag(f, "DCACHE", "DCACHE_SR", "CMDENDF"));
  CHECK(pending(f, 111));
  write(f, "DCACHE", "DCACHE_FCR", bit_of("DCACHE", "DCACHE_FCR", "CCMDENDF"));
  CHECK_FALSE(flag(f, "DCACHE", "DCACHE_SR", "CMDENDF"));
}

TEST_CASE("a snapshot taken while the reset invalidate is pending ends it at the same count")
{
  auto       f     = idle();
  auto const saved = f.target->snapshot();
  auto const ended = [&] {
    auto const stopped = f.run_to_stop(
        {.max_instructions = icount{1'000'000},
         .until_time       = vtime{f.target->virtual_clock().time().ps + 100'000'000},
         .stop_when        = [&] { return flag(f, "ICache", "ICACHE_SR", "BSYENDF"); }});
    CHECK(stopped.reason == stop_reason::user);
    return f.target->virtual_clock().now();
  };
  REQUIRE_FALSE(flag(f, "ICache", "ICACHE_SR", "BSYENDF"));
  auto const first = ended();
  f.target->restore(saved);
  REQUIRE_FALSE(flag(f, "ICache", "ICACHE_SR", "BSYENDF"));
  CHECK(ended() == first);
}

TEST_CASE("an invalidate pending across a core clock change ends at its virtual time")
{
  auto f = idle();
  write(f, "ICache", "ICACHE_CR", bit_of("ICache", "ICACHE_CR", "CACHEINV"));
  REQUIRE(flag(f, "ICache", "ICACHE_SR", "BUSYF"));
  auto const start = f.target->virtual_clock().time().ps;
  idle_for(f, std::chrono::microseconds{1});
  auto const icscr1 = f.target->peripheral("RCC").peek(offset_of("RCC", "RCC_ICSCR1"));
  write(
      f, "RCC", "RCC_ICSCR1",
      (icscr1 & ~(0xFu << 28)) | (1u << 28) | bit_of("RCC", "RCC_ICSCR1", "MSIRGSEL"));
  REQUIRE(f.target->virtual_clock().core_hz() == 24'000'000);
  auto const stopped = f.run_to_stop({.max_instructions = icount{100'000'000}, .stop_when = [&] {
                                        return flag(f, "ICache", "ICACHE_SR", "BSYENDF");
                                      }});
  REQUIRE(stopped.reason == stop_reason::user);
  auto const took = f.target->virtual_clock().time().ps - start;
  CHECK(took >= 10'000'000);
  CHECK(took < 10'000'000 + 1'000'000 / 24);
}
