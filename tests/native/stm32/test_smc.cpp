#include "stm32_fixture.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace
{
using popcorn::address;
using popcorn::icount;

constexpr std::uint16_t movs_r0_1 = 0x2001;
constexpr std::uint16_t movs_r0_2 = 0x2002;

constexpr std::array<std::uint16_t, 4> within_block{0x800a, 0xbf00, movs_r0_1, 0xe7fe};
constexpr std::uint32_t                within_target = 4;
constexpr std::uint32_t                within_done   = 6;

constexpr std::array<std::uint16_t, 10> next_block{0x3301, 0xe000, 0xbf00, movs_r0_1, 0x2b02,
                                                   0xd002, 0xe7ff, 0x800a, 0xe7f6,    0xe7fe};
constexpr std::uint32_t                 next_target = 6;
constexpr std::uint32_t                 next_done   = 18;

constexpr std::uint32_t              code_offset = 0x2'0000;
constexpr std::array<address, 4>     windows{0x0a00'0000, 0x0e00'0000, 0x2000'0000, 0x3000'0000};
constexpr std::array<char const*, 4> window_names{"RAM_C_NS", "RAM_C_S", "SRAM_NS", "SRAM_S"};

struct outcome
{
  std::uint64_t r0{};
  std::uint64_t r3{};
  std::uint64_t pc{};
  std::uint64_t executed{};
  std::uint64_t stored{};
};

auto run_code(
    std::span<std::uint16_t const> code,
    address                        run_base,
    address                        store_base,
    std::uint32_t                  target,
    std::uint32_t                  done) -> outcome
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  popcorn::testing::stm32_fixture f{popcorn::testing::u575(), "boot_ll", options};
  auto const                      booted = f.run_to_stop(
      {.max_instructions = icount{100'000'000}, .until = popcorn::testing::sym("boot_ll", "main")});
  REQUIRE(booted.reason == popcorn::stop_reason::address);
  for(std::size_t index = 0; index < code.size(); ++index)
  {
    REQUIRE(f.target->mem().write(run_base + 2 * index, 2, code[index]));
  }
  f.set("r0", 0);
  f.set("r1", store_base + target);
  f.set("r2", movs_r0_2);
  f.set("r3", 0);
  f.set("pc", run_base);
  auto const result = f.run(
      {.max_instructions = icount{10'000},
       .wall_budget      = std::chrono::seconds{60},
       .until            = run_base + done});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == popcorn::stop_reason::address);
  auto const& regs = f.target->regs();
  return {
      .r0       = regs.read(*regs.find("r0")),
      .r3       = regs.read(*regs.find("r3")),
      .pc       = regs.read(*regs.find("pc")),
      .executed = result.executed.value,
      .stored   = f.target->mem().read(run_base + target, 2).value_or(0)};
}
}

TEST_CASE("a store into its own translated block leaves the rest of the block running old code")
{
  for(std::size_t run = 0; run < windows.size(); ++run)
  {
    for(std::size_t store = 0; store < windows.size(); ++store)
    {
      CAPTURE(window_names[run]);
      CAPTURE(window_names[store]);
      auto const base = windows[run] + code_offset;
      auto const o =
          run_code(within_block, base, windows[store] + code_offset, within_target, within_done);
      CHECK(o.r0 == 1);
      CHECK(o.executed == 3);
      CHECK(o.pc == base + within_done);
      CHECK(o.stored == movs_r0_2);
    }
  }
}

TEST_CASE("a store into a chained block, through any window, is seen at its next execution")
{
  for(std::size_t run = 0; run < windows.size(); ++run)
  {
    for(std::size_t store = 0; store < windows.size(); ++store)
    {
      CAPTURE(window_names[run]);
      CAPTURE(window_names[store]);
      auto const base = windows[run] + code_offset;
      auto const o =
          run_code(next_block, base, windows[store] + code_offset, next_target, next_done);
      CHECK(o.r0 == 2);
      CHECK(o.r3 == 2);
      CHECK(o.executed == 13);
      CHECK(o.pc == base + next_done);
      CHECK(o.stored == movs_r0_2);
    }
  }
}
