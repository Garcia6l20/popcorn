#include "dma.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

auto entered(std::string_view entry) -> stm32_fixture
{
  stm32_fixture f{u575(), "gpdma_ll"};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("gpdma_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("gpdma_ll", entry));
  return f;
}

auto run_log(std::string_view entry) -> std::vector<std::uint32_t>
{
  auto       f       = entered(entry);
  auto const stopped = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  return f.log();
}

auto destination(stm32_fixture const& f) -> std::vector<std::uint8_t>
{
  auto const                base = sym("gpdma_ll", "destination");
  std::vector<std::uint8_t> out;
  for(std::uint32_t index = 0; index < 256; ++index)
  {
    auto const byte = f.target->mem().read(base + index, 1);
    REQUIRE(byte.has_value());
    out.push_back(static_cast<std::uint8_t>(*byte));
  }
  return out;
}
}

TEST_CASE("a memory-to-memory copy completes once, exact, and disables its channel")
{
  auto const log = run_log("mem2mem");
  CHECK(log == std::vector<std::uint32_t>{1, 1, 0});
  CHECK(run_log("mem2mem") == log);
}

TEST_CASE("a 64-byte block raises one half and one complete transfer")
{
  CHECK(run_log("half") == std::vector<std::uint32_t>{1, 1, 1});
}

TEST_CASE("a read of unmapped memory is a data transfer error, not a CPU fault")
{
  CHECK(run_log("error") == std::vector<std::uint32_t>{1, 0, 1});
}

TEST_CASE("a suspended copy stops part way and completes after resume")
{
  CHECK(run_log("suspend") == std::vector<std::uint32_t>{0, 1});
}

TEST_CASE("a three-item linked list copies every item with one complete event each")
{
  auto f = entered("linked");
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{3, 1, 1, 1, 1});
  auto const out = sym("gpdma_ll", "items_out");
  for(auto const [item, offset, length] : {
          std::array{0u, 0u,   16u},
          {1u, 64u,  32u},
          {2u, 128u, 48u}
  })
  {
    for(std::uint32_t index = 0; index < length; ++index)
    {
      auto const byte = f.target->mem().read(out + 64 * item + index, 1);
      REQUIRE(byte.has_value());
      CHECK(*byte == (((offset + index) * 7u + 3u) & 0xffu));
    }
  }
}

TEST_CASE("the half transfer flag rises when exactly half the block is left")
{
  auto          f = entered("half");
  std::uint32_t left_at_half{};
  auto const    stopped = f.run_to_stop({.max_instructions = icount{10'000'000}, .stop_when = [&] {
                                        if(f.field("GPDMA1", "GPDMA_C0SR", "HTF") == 0)
                                        {
                                          return false;
                                        }
                                        left_at_half = f.field("GPDMA1", "GPDMA_C0BR1", "BNDT");
                                        return true;
                                         }});
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(left_at_half == 32);
}

TEST_CASE("a snapshot mid-copy restores to the same result, in place and serialized")
{
  auto f = entered("mem2mem");
  while(
      f.field("GPDMA1", "GPDMA_C0CR", "EN") == 0 or f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") > 128)
  {
    REQUIRE_FALSE(f.run({.max_instructions = icount{4}}).stopped.has_value());
  }
  REQUIRE(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") > 0);
  auto const saved = f.target->snapshot();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const log  = f.log();
  auto const copy = destination(f);
  CHECK(log == std::vector<std::uint32_t>{1, 1, 0});

  f.target->restore(saved);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.log() == log);
  CHECK(destination(f) == copy);

  auto other = entered("mem2mem");
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  REQUIRE(other.run_to_stop().reason == stop_reason::user);
  CHECK(other.log() == log);
  CHECK(destination(other) == copy);
}

namespace
{
constexpr popcorn::address copy_from = 0x2001'0000;
constexpr popcorn::address copy_to   = 0x2001'1000;
constexpr popcorn::address node      = 0x2001'2000;

auto idle_machine() -> stm32_fixture
{
  stm32_fixture f{u575(), "rcc_ll"};
  f.set("pc", sym("rcc_ll", "idle"));
  for(std::uint32_t index = 0; index < 32; ++index)
  {
    REQUIRE(f.target->mem().write(copy_from + index, 1, (index * 11u + 1u) & 0xffu));
    REQUIRE(f.target->mem().write(copy_to + index, 1, 0));
  }
  return f;
}

void idle_for(stm32_fixture const& f, std::chrono::nanoseconds span)
{
  auto const now = f.target->virtual_clock().time();
  REQUIRE(
      f.run_to_stop(
           {.max_instructions = icount{1'000'000'000},
            .until_time = popcorn::vtime{now.ps + static_cast<std::uint64_t>(span.count()) * 1000}})
          .reason
      == stop_reason::time);
}

auto bit(std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const& unit = *std::ranges::find(
      u575().peripherals, std::string_view{"GPDMA1"}, &popcorn::peripheral_desc::name);
  auto const found = std::ranges::find(unit.registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != unit.registers.end());
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << bits->lsb;
}

void write(stm32_fixture const& f, std::string_view reg, std::uint32_t value)
{
  auto&      regs = f.target->peripheral("GPDMA1");
  auto const found =
      std::ranges::find(regs.description().registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != regs.description().registers.end());
  REQUIRE(
      f.target->model("GPDMA1")
          ->on_write(
              regs, found->offset, 4, regs.peek(found->offset), value, {.pc = 0, .now = icount{0}})
          .has_value());
}

auto copied(stm32_fixture const& f, std::uint32_t length) -> bool
{
  for(std::uint32_t index = 0; index < length; ++index)
  {
    auto const byte = f.target->mem().read(copy_to + index, 1);
    REQUIRE(byte.has_value());
    if(*byte != ((index * 11u + 1u) & 0xffu))
    {
      return false;
    }
  }
  return true;
}
}

TEST_CASE("a linked-list item of channel 12 consumes its TR3 and BR2 words in RM order")
{
  auto       f       = idle_machine();
  auto const updates = bit("GPDMA_C12LLR", "UB1")
                     | bit("GPDMA_C12LLR", "USA")
                     | bit("GPDMA_C12LLR", "UDA")
                     | bit("GPDMA_C12LLR", "UT3")
                     | bit("GPDMA_C12LLR", "ULL");
  for(auto const [index, word] :
      std::views::enumerate(std::array<std::uint32_t, 5>{16, copy_from + 16, copy_to + 16, 4, 0}))
  {
    REQUIRE(f.target->mem().write(node + 4 * static_cast<std::uint32_t>(index), 4, word));
  }
  write(f, "GPDMA_C12TR1", bit("GPDMA_C12TR1", "SINC") | bit("GPDMA_C12TR1", "DINC"));
  write(f, "GPDMA_C12TR2", bit("GPDMA_C12TR2", "SWREQ"));
  write(f, "GPDMA_C12BR1", 16);
  write(f, "GPDMA_C12SAR", copy_from);
  write(f, "GPDMA_C12DAR", copy_to);
  write(f, "GPDMA_C12LBAR", node & 0xffff'0000u);
  write(f, "GPDMA_C12LLR", updates | (node & 0xfffcu));
  write(f, "GPDMA_C12CR", bit("GPDMA_C12CR", "EN"));
  idle_for(f, std::chrono::microseconds{100});
  CHECK(copied(f, 32));
  CHECK(f.field("GPDMA1", "GPDMA_C12SR", "ULEF") == 0);
  CHECK(f.field("GPDMA1", "GPDMA_C12SR", "USEF") == 0);
  CHECK(f.field("GPDMA1", "GPDMA_C12SR", "TCF") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C12SR", "IDLEF") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C12LLR", "LA") == 0);
  CHECK(f.field("GPDMA1", "GPDMA_C12LLR", "ULL") == 0);
}

TEST_CASE("a request held high across a machine reset starts nothing afterwards")
{
  auto        f = idle_machine();
  auto* const controller =
      dynamic_cast<popcorn::periph::stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  controller->on_request(25, true);
  f.target->reset();
  f.set("pc", sym("rcc_ll", "idle"));
  write(f, "GPDMA_C0TR1", bit("GPDMA_C0TR1", "SINC") | bit("GPDMA_C0TR1", "DINC"));
  write(f, "GPDMA_C0TR2", 25u * bit("GPDMA_C0TR2", "REQSEL") | bit("GPDMA_C0TR2", "DREQ"));
  write(f, "GPDMA_C0BR1", 16);
  write(f, "GPDMA_C0SAR", copy_from);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  idle_for(f, std::chrono::microseconds{50});
  CHECK(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") == 16);
  controller->on_request(25, true);
  idle_for(f, std::chrono::microseconds{50});
  CHECK(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") == 0);
}
