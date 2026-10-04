#include "dma.hpp"
#include "dma_data.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <span>
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

TEST_CASE("the data handling functions follow RM0456 Table 141 for one item")
{
  using namespace popcorn::periph::stm32;
  constexpr std::uint32_t word = 0x0483'0281;
  CHECK(resize(0x81, 1, 2, 0) == 0x0081);
  CHECK(exchange_destination(resize(0x81, 1, 2, 0), 2, true, false) == 0x8100);
  CHECK(resize(0x81, 1, 2, 1) == 0xFF81);
  CHECK(resize(0x81, 1, 4, 0) == 0x0000'0081);
  CHECK(exchange_destination(0x0000'0081, 4, true, false) == 0x0000'8100);
  CHECK(exchange_destination(0x0000'0081, 4, false, true) == 0x0081'0000);
  CHECK(exchange_destination(0x0000'0081, 4, true, true) == 0x8100'0000);
  CHECK(resize(0x81, 1, 4, 1) == 0xFFFF'FF81);
  CHECK(exchange_destination(0xFFFF'FF81, 4, true, false) == 0xFFFF'81FF);
  CHECK(exchange_destination(0xFFFF'FF81, 4, false, true) == 0xFF81'FFFF);
  CHECK(exchange_destination(0xFFFF'FF81, 4, true, true) == 0x81FF'FFFF);
  CHECK(resize(0x0281, 2, 1, 0) == 0x81);
  CHECK(resize(0x0281, 2, 1, 1) == 0x02);
  CHECK(resize(0x0281, 2, 4, 0) == 0x0000'0281);
  CHECK(exchange_destination(0x0000'0281, 4, true, false) == 0x0000'8102);
  CHECK(exchange_destination(0x0000'0281, 4, false, true) == 0x0281'0000);
  CHECK(exchange_destination(0x0000'0281, 4, true, true) == 0x8102'0000);
  CHECK(resize(0x8302, 2, 4, 1) == 0xFFFF'8302);
  CHECK(resize(0x0302, 2, 4, 1) == 0x0000'0302);
  CHECK(resize(word, 4, 1, 0) == 0x81);
  CHECK(resize(word, 4, 1, 1) == 0x04);
  CHECK(resize(word, 4, 2, 0) == 0x0281);
  CHECK(exchange_destination(0x0281, 2, true, false) == 0x8102);
  CHECK(exchange_destination(0x0281, 2, false, true) == 0x0281);
  CHECK(resize(word, 4, 2, 1) == 0x0483);
  CHECK(resize(word, 4, 4, 1) == word);
  CHECK(resize(0x81, 1, 1, 1) == 0x81);
  CHECK(exchange_source(word, 4, true) == 0x0402'8381);
  CHECK(exchange_source(word, 4, false) == word);
  CHECK(exchange_source(0x0281, 2, true) == 0x0281);
  CHECK(exchange_destination(word, 4, true, false) == 0x8304'8102);
  CHECK(exchange_destination(word, 4, false, true) == 0x0281'0483);
  CHECK(exchange_destination(0x81, 1, true, true) == 0x81);
}

namespace
{
constexpr std::array<std::uint8_t, 8> handled_bytes{0x81, 0x02, 0x83, 0x04, 0x85, 0x06, 0x87, 0x08};

void load_source(stm32_fixture const& f)
{
  for(auto const [index, byte] : std::views::enumerate(handled_bytes))
  {
    REQUIRE(f.target->mem().write(copy_from + static_cast<std::uint32_t>(index), 1, byte));
  }
}

auto copy_with(std::uint32_t tr1, std::uint32_t bytes) -> stm32_fixture
{
  auto f = idle_machine();
  load_source(f);
  write(f, "GPDMA_C0TR1", tr1 | bit("GPDMA_C0TR1", "SINC") | bit("GPDMA_C0TR1", "DINC"));
  write(f, "GPDMA_C0TR2", bit("GPDMA_C0TR2", "SWREQ"));
  write(f, "GPDMA_C0BR1", bytes);
  write(f, "GPDMA_C0SAR", copy_from);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  idle_for(f, std::chrono::microseconds{50});
  return f;
}

auto copied_bytes(stm32_fixture const& f, std::uint32_t length) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  for(std::uint32_t index = 0; index < length; ++index)
  {
    auto const byte = f.target->mem().read(copy_to + index, 1);
    REQUIRE(byte.has_value());
    out.push_back(static_cast<std::uint8_t>(*byte));
  }
  return out;
}

auto tr1(std::uint32_t source, std::uint32_t destination, std::uint32_t pam) -> std::uint32_t
{
  return source
       * bit("GPDMA_C0TR1", "SDW_LOG2")
       | destination
       * bit("GPDMA_C0TR1", "DDW_LOG2")
       | pam
       * bit("GPDMA_C0TR1", "PAM");
}
}

TEST_CASE("bytes packed into words keep their order, DHX exchanges the half-words")
{
  auto const plain = copy_with(tr1(0, 2, 2), 8);
  CHECK(
      copied_bytes(plain, 8)
      == std::vector<std::uint8_t>(handled_bytes.begin(), handled_bytes.end()));
  CHECK(plain.field("GPDMA1", "GPDMA_C0SR", "TCF") == 1);
  auto const halves = copy_with(tr1(0, 2, 2) | bit("GPDMA_C0TR1", "DHX"), 8);
  CHECK(
      copied_bytes(halves, 8)
      == std::vector<std::uint8_t>{0x83, 0x04, 0x81, 0x02, 0x87, 0x08, 0x85, 0x06});
}

TEST_CASE("words unpacked into bytes go LSB first, after SBX")
{
  auto const plain = copy_with(tr1(2, 0, 2), 8);
  CHECK(
      copied_bytes(plain, 8)
      == std::vector<std::uint8_t>(handled_bytes.begin(), handled_bytes.end()));
  auto const exchanged = copy_with(tr1(2, 0, 2) | bit("GPDMA_C0TR1", "SBX"), 8);
  CHECK(
      copied_bytes(exchanged, 8)
      == std::vector<std::uint8_t>{0x81, 0x83, 0x02, 0x04, 0x85, 0x87, 0x06, 0x08});
}

TEST_CASE("half-words widened to words are 0-padded or sign-extended, narrowed keep low or high")
{
  CHECK(
      copied_bytes(copy_with(tr1(1, 2, 0), 4), 8)
      == std::vector<std::uint8_t>{0x81, 0x02, 0x00, 0x00, 0x83, 0x04, 0x00, 0x00});
  CHECK(
      copied_bytes(copy_with(tr1(1, 2, 1), 4), 8)
      == std::vector<std::uint8_t>{0x81, 0x02, 0x00, 0x00, 0x83, 0x04, 0x00, 0x00});
  CHECK(
      copied_bytes(copy_with(tr1(0, 2, 1), 2), 8)
      == std::vector<std::uint8_t>{0x81, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x00});
  CHECK(copied_bytes(copy_with(tr1(1, 0, 0), 4), 2) == std::vector<std::uint8_t>{0x81, 0x83});
  CHECK(copied_bytes(copy_with(tr1(1, 0, 1), 4), 2) == std::vector<std::uint8_t>{0x02, 0x04});
}

TEST_CASE("packing a block that is not a whole number of destination items is a user error")
{
  auto const f = copy_with(tr1(0, 2, 2), 6);
  CHECK(f.field("GPDMA1", "GPDMA_C0SR", "USEF") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") == 6);
}

TEST_CASE("a packing channel stopped by a data error starts its next block with nothing queued")
{
  constexpr popcorn::address sram_end = 0x200C'0000;
  auto                       f        = idle_machine();
  load_source(f);
  REQUIRE(f.target->mem().write(sram_end - 2, 2, 0x5A5A));
  write(f, "GPDMA_C0TR1", tr1(0, 2, 2) | bit("GPDMA_C0TR1", "SINC") | bit("GPDMA_C0TR1", "DINC"));
  write(f, "GPDMA_C0TR2", bit("GPDMA_C0TR2", "SWREQ"));
  write(f, "GPDMA_C0BR1", 4);
  write(f, "GPDMA_C0SAR", sram_end - 2);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  idle_for(f, std::chrono::microseconds{50});
  REQUIRE(f.field("GPDMA1", "GPDMA_C0SR", "DTEF") == 1);
  write(f, "GPDMA_C0FCR", bit("GPDMA_C0FCR", "DTEF"));
  write(f, "GPDMA_C0BR1", 4);
  write(f, "GPDMA_C0SAR", copy_from);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  idle_for(f, std::chrono::microseconds{50});
  CHECK(copied_bytes(f, 4) == std::vector<std::uint8_t>{0x81, 0x02, 0x83, 0x04});
}

namespace
{
constexpr std::uint32_t paced_request = 25;

auto paced_pack() -> stm32_fixture
{
  auto f = idle_machine();
  load_source(f);
  write(f, "GPDMA_C0TR1", tr1(0, 2, 2) | bit("GPDMA_C0TR1", "SINC") | bit("GPDMA_C0TR1", "DINC"));
  write(f, "GPDMA_C0TR2", paced_request * bit("GPDMA_C0TR2", "REQSEL"));
  write(f, "GPDMA_C0BR1", 8);
  write(f, "GPDMA_C0SAR", copy_from);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  return f;
}

void pace(stm32_fixture const& f, bool level)
{
  auto* const controller =
      dynamic_cast<popcorn::periph::stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  controller->on_request(paced_request, level);
}

auto finish_paced(stm32_fixture const& f) -> std::vector<std::uint64_t>
{
  pace(f, true);
  std::vector<std::uint64_t> seen;
  for(int step = 0; step < 200 and f.field("GPDMA1", "GPDMA_C0SR", "TCF") == 0; ++step)
  {
    auto const before = f.field("GPDMA1", "GPDMA_C0BR1", "BNDT");
    idle_for(f, std::chrono::nanoseconds{250});
    if(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") != before)
    {
      seen.push_back(f.target->virtual_clock().time().ps);
    }
  }
  return seen;
}
}

TEST_CASE("a snapshot with packed bytes queued restores into a fresh machine to the same words")
{
  auto f = paced_pack();
  pace(f, true);
  for(int step = 0; step < 200 and f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") > 6; ++step)
  {
    idle_for(f, std::chrono::nanoseconds{250});
  }
  pace(f, false);
  REQUIRE(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") == 6);
  CHECK(copied_bytes(f, 4) == std::vector<std::uint8_t>{0, 0, 0, 0});
  auto const saved = f.target->snapshot();
  auto const times = finish_paced(f);
  CHECK(times.size() == 6);
  auto const words = copied_bytes(f, 8);
  CHECK(words == std::vector<std::uint8_t>(handled_bytes.begin(), handled_bytes.end()));
  auto other = idle_machine();
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(finish_paced(other) == times);
  CHECK(copied_bytes(other, 8) == words);
}

namespace
{
constexpr popcorn::address loop_a = node;
constexpr popcorn::address loop_b = node + 0x20;

auto llr(std::string_view field) -> std::uint32_t
{
  return bit("GPDMA_C0LLR", field);
}

auto reg_value(stm32_fixture const& f, std::string_view reg) -> std::uint32_t
{
  auto const& regs = f.target->peripheral("GPDMA1");
  auto const  found =
      std::ranges::find(regs.description().registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != regs.description().registers.end());
  return regs.peek(found->offset);
}

auto source_byte(std::uint32_t index) -> std::uint32_t
{
  return (index * 11u + 1u) & 0xffu;
}

auto long_source() -> stm32_fixture
{
  auto f = idle_machine();
  for(std::uint32_t index = 32; index < 64; ++index)
  {
    REQUIRE(f.target->mem().write(copy_from + index, 1, source_byte(index)));
  }
  return f;
}

void place(stm32_fixture const& f, popcorn::address at, std::span<std::uint32_t const> words)
{
  for(auto const [index, word] : std::views::enumerate(words))
  {
    REQUIRE(f.target->mem().write(at + 4 * static_cast<std::uint32_t>(index), 4, word));
  }
}

void start_copy(stm32_fixture const& f, std::uint32_t bytes, std::uint32_t link, std::uint32_t tcem)
{
  write(f, "GPDMA_C0TR1", bit("GPDMA_C0TR1", "SINC") | bit("GPDMA_C0TR1", "DINC"));
  write(f, "GPDMA_C0TR2", bit("GPDMA_C0TR2", "SWREQ") | tcem * bit("GPDMA_C0TR2", "TCEM"));
  write(f, "GPDMA_C0BR1", bytes);
  write(f, "GPDMA_C0SAR", copy_from);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0LBAR", node & 0xffff'0000u);
  write(f, "GPDMA_C0LLR", link);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
}

void suspend(stm32_fixture const& f)
{
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN") | bit("GPDMA_C0CR", "SUSP"));
}

void step(stm32_fixture const& f)
{
  idle_for(f, std::chrono::nanoseconds{250});
}

auto bndt_at_block_ends(stm32_fixture const& f, popcorn::address end) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> seen;
  auto                       last = reg_value(f, "GPDMA_C0DAR");
  for(int count = 0; count < 2000 and last < end; ++count)
  {
    step(f);
    auto const now = reg_value(f, "GPDMA_C0DAR");
    if(now != last and (now - copy_to) % 16 == 0)
    {
      seen.push_back(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT"));
    }
    last = now;
  }
  return seen;
}

auto destination_bytes(stm32_fixture const& f, std::uint32_t length) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < length; ++index)
  {
    auto const byte = f.target->mem().read(copy_to + index, 1);
    REQUIRE(byte.has_value());
    out.push_back(static_cast<std::uint32_t>(*byte));
  }
  return out;
}

auto source_bytes(std::initializer_list<std::uint32_t> blocks, std::uint32_t length)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const first : blocks)
  {
    for(std::uint32_t index = 0; index < length; ++index)
    {
      out.push_back(source_byte(first + index));
    }
  }
  return out;
}

auto count_completions(stm32_fixture const& f, int wanted, auto&& on_complete) -> int
{
  int completed = 0;
  for(int count = 0; count < 2000 and completed < wanted; ++count)
  {
    step(f);
    if(f.field("GPDMA1", "GPDMA_C0SR", "TCF") != 0)
    {
      write(f, "GPDMA_C0FCR", bit("GPDMA_C0FCR", "TCF"));
      ++completed;
      on_complete(completed);
    }
  }
  return completed;
}

auto restore_loop() -> stm32_fixture
{
  auto       f       = long_source();
  auto const updates = llr("USA") | llr("ULL");
  place(f, loop_a, std::array{copy_from, updates | (loop_b & 0xfffcu)});
  place(f, loop_b, std::array{copy_from + 16, updates | (loop_a & 0xfffcu)});
  start_copy(f, 16, updates | (loop_b & 0xfffcu), 0);
  return f;
}
}

TEST_CASE("an item with UB1 clear and another update bit set restores BNDT on each link")
{
  auto f = restore_loop();
  CHECK(bndt_at_block_ends(f, copy_to + 64) == std::vector<std::uint32_t>{16, 16, 16, 16});
  suspend(f);
  CHECK(destination_bytes(f, 64) == source_bytes({0, 16, 0, 16}, 16));
  CHECK(f.field("GPDMA1", "GPDMA_C0SR", "USEF") == 0);
  CHECK(f.field("GPDMA1", "GPDMA_C0CR", "EN") == 1);
}

TEST_CASE("a last item with every update bit clear and LA set runs again, as long as enabled")
{
  auto const link = loop_a & 0xfffcu;
  SUBCASE("TCEM 0 raises a transfer complete per block")
  {
    auto f = long_source();
    start_copy(f, 8, link, 0);
    CHECK(count_completions(f, 5, [](int) {}) == 5);
    suspend(f);
    CHECK(reg_value(f, "GPDMA_C0SAR") == copy_from + 40);
    CHECK(reg_value(f, "GPDMA_C0DAR") == copy_to + 40);
    CHECK(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") == 8);
    CHECK(reg_value(f, "GPDMA_C0LLR") == link);
    CHECK(f.field("GPDMA1", "GPDMA_C0SR", "USEF") == 0);
    CHECK(f.field("GPDMA1", "GPDMA_C0SR", "ULEF") == 0);
    CHECK(destination_bytes(f, 40) == source_bytes({0}, 40));
  }
  SUBCASE("TCEM 3 raises no event, the channel transfer never ends")
  {
    auto f = long_source();
    start_copy(f, 8, link, 3);
    for(int count = 0; count < 2000 and reg_value(f, "GPDMA_C0SAR") < copy_from + 40; ++count)
    {
      step(f);
    }
    suspend(f);
    CHECK(reg_value(f, "GPDMA_C0SAR") == copy_from + 40);
    CHECK(f.field("GPDMA1", "GPDMA_C0SR", "TCF") == 0);
    CHECK(f.field("GPDMA1", "GPDMA_C0SR", "HTF") == 0);
    CHECK(f.field("GPDMA1", "GPDMA_C0SR", "USEF") == 0);
    CHECK(destination_bytes(f, 40) == source_bytes({0}, 40));
  }
}

TEST_CASE("an item with ULL clear is recursive: each link reads the same item again")
{
  auto       f    = long_source();
  auto const link = llr("UB1") | llr("USA") | (loop_a & 0xfffcu);
  place(f, loop_a, std::array{std::uint32_t{8}, copy_from});
  start_copy(f, 8, link, 0);
  auto const completed = count_completions(f, 4, [&](int done) {
    if(done == 1)
    {
      place(f, loop_a, std::array{std::uint32_t{8}, copy_from + 16});
    }
  });
  suspend(f);
  CHECK(completed == 4);
  CHECK(reg_value(f, "GPDMA_C0LLR") == link);
  CHECK(f.field("GPDMA1", "GPDMA_C0CR", "EN") == 1);
  CHECK(destination_bytes(f, 32) == source_bytes({0, 0, 16, 16}, 8));
}

TEST_CASE("a snapshot one and a half blocks into a restoring list resumes in a fresh machine")
{
  auto f = restore_loop();
  for(int count = 0; count < 2000 and reg_value(f, "GPDMA_C0DAR") < copy_to + 24; ++count)
  {
    step(f);
  }
  REQUIRE(reg_value(f, "GPDMA_C0DAR") == copy_to + 24);
  auto const saved = f.target->snapshot();
  auto const ends  = bndt_at_block_ends(f, copy_to + 64);
  suspend(f);
  auto const copy   = destination_bytes(f, 64);
  auto const status = reg_value(f, "GPDMA_C0SR");
  CHECK(ends == std::vector<std::uint32_t>{16, 16, 16});
  CHECK(copy == source_bytes({0, 16, 0, 16}, 16));

  auto other = idle_machine();
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(bndt_at_block_ends(other, copy_to + 64) == ends);
  suspend(other);
  CHECK(destination_bytes(other, 64) == copy);
  CHECK(reg_value(other, "GPDMA_C0SR") == status);
}

namespace
{
struct old_channel_state
{
  std::uint32_t generation{};
  std::uint32_t block_bytes{};
  std::uint32_t packed{};
  std::uint8_t  packed_bytes{};
  bool          half_reported{};
  bool          scheduled{};
};

struct old_gpdma_state
{
  std::array<old_channel_state, 16> channel{};
  std::array<std::uint8_t, 128>     request{};
};

struct new_channel_state
{
  std::uint32_t generation{};
  std::uint32_t block_bytes{};
  std::uint16_t repeat{};
  std::uint32_t packed{};
  std::uint8_t  packed_bytes{};
  bool          half_reported{};
  bool          scheduled{};
};

struct new_gpdma_state
{
  std::array<new_channel_state, 16> channel{};
  std::array<std::uint8_t, 128>     request{};
};
}

TEST_CASE("a GPDMA state saved before the block repeat counter loads and resumes mid-block")
{
  auto f = paced_pack();
  pace(f, true);
  for(int count = 0; count < 200 and f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") > 6; ++count)
  {
    idle_for(f, std::chrono::nanoseconds{250});
  }
  pace(f, false);
  idle_for(f, std::chrono::microseconds{2});
  REQUIRE(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") == 6);
  auto const saved = f.target->snapshot();
  auto const times = finish_paced(f);
  auto const words = copied_bytes(f, 8);
  REQUIRE(words == std::vector<std::uint8_t>(handled_bytes.begin(), handled_bytes.end()));

  old_gpdma_state old{};
  old.channel[0] = {
      .generation    = 1,
      .block_bytes   = 8,
      .packed        = 0x0281,
      .packed_bytes  = 2,
      .half_reported = false,
      .scheduled     = false,
  };
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(old);

  auto other = idle_machine();
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.target->model("GPDMA1")->load(bytes);
  CHECK(finish_paced(other) == times);
  CHECK(copied_bytes(other, 8) == words);

  std::vector<std::byte> now;
  other.target->model("GPDMA1")->save(now);
  auto const loaded =
      reflex::serde::bson::deserializer{std::span<std::byte const>{now}}.load<new_gpdma_state>();
  CHECK(loaded.channel[0].block_bytes == 8);
  CHECK(std::ranges::all_of(loaded.channel, [](auto const& c) { return c.repeat == 0; }));
}

TEST_CASE("a UB1 clear link restores the BNDT the last loaded item started with")
{
  auto       f     = long_source();
  auto const moves = llr("USA") | llr("ULL");
  place(f, loop_a, std::array{std::uint32_t{8}, copy_from + 16, moves | (loop_b & 0xfffcu)});
  place(f, loop_b, std::array{copy_from + 32, moves | (loop_b & 0xfffcu)});
  start_copy(f, 16, llr("UB1") | moves | (loop_a & 0xfffcu), 0);
  std::vector<std::uint32_t> ends;
  for(int count = 0; count < 2000 and ends.size() < 4; ++count)
  {
    auto const before = reg_value(f, "GPDMA_C0DAR");
    step(f);
    auto const now = reg_value(f, "GPDMA_C0DAR");
    if(now != before and now >= copy_to + 16 and (now - copy_to) % 8 == 0)
    {
      ends.push_back(f.field("GPDMA1", "GPDMA_C0BR1", "BNDT"));
    }
  }
  suspend(f);
  CHECK(ends == std::vector<std::uint32_t>{8, 8, 8, 8});
  CHECK(f.field("GPDMA1", "GPDMA_C0SR", "USEF") == 0);
  auto expected = source_bytes({0}, 16);
  std::ranges::copy(source_bytes({16, 32, 32}, 8), std::back_inserter(expected));
  CHECK(destination_bytes(f, 40) == expected);
}
