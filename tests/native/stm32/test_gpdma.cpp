#include "dma.hpp"
#include "dma_data.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
  while(f.field("GPDMA1", "GPDMA_C0CR", "EN") == 0
        or f.field("GPDMA1", "GPDMA_C0BR1", "BNDT") > 128)
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
  for(std::uint32_t index = 32; index < 96; ++index)
  {
    REQUIRE(f.target->mem().write(copy_from + index, 1, (index * 11u + 1u) & 0xffu));
  }
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
  CHECK(copied(f, 16));
  std::vector<std::uint32_t> second;
  std::vector<std::uint32_t> every_fifth;
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    auto const byte = f.target->mem().read(copy_to + 16 + index, 1);
    REQUIRE(byte.has_value());
    second.push_back(static_cast<std::uint32_t>(*byte));
    every_fifth.push_back(((16 + 5 * index) * 11u + 1u) & 0xffu);
  }
  CHECK(second == every_fifth);
  CHECK(f.field("GPDMA1", "GPDMA_C12TR3", "SAO") == 4);
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
  return source * bit("GPDMA_C0TR1", "SDW_LOG2")
       | destination * bit("GPDMA_C0TR1", "DDW_LOG2")
       | pam * bit("GPDMA_C0TR1", "PAM");
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
  std::uint8_t  destination_beats{};
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

namespace
{
struct event
{
  char          kind{};
  std::uint32_t moved{};

  friend auto operator==(event const&, event const&) -> bool = default;
};

auto operator<<(std::ostream& out, event const& seen) -> std::ostream&
{
  return out << seen.kind << seen.moved;
}

auto take_events(stm32_fixture const& f, std::string_view channel, std::uint32_t base)
    -> std::vector<event>
{
  auto const         sr  = std::format("GPDMA_C{}SR", channel);
  auto const         fcr = std::format("GPDMA_C{}FCR", channel);
  auto const         dar = std::format("GPDMA_C{}DAR", channel);
  std::vector<event> out;
  for(auto const& [kind, flag] : {
          std::pair{'H', "HTF"},
          {'T', "TCF"}
  })
  {
    if(f.field("GPDMA1", sr, flag) != 0)
    {
      write(f, fcr, bit(fcr, flag));
      out.push_back({kind, reg_value(f, dar) - base});
    }
  }
  return out;
}

auto events_until_idle(stm32_fixture const& f) -> std::vector<event>
{
  std::vector<event> out;
  for(int count = 0; count < 4000; ++count)
  {
    std::ranges::copy(take_events(f, "0", copy_to), std::back_inserter(out));
    if(f.field("GPDMA1", "GPDMA_C0CR", "EN") == 0)
    {
      break;
    }
    step(f);
  }
  return out;
}

auto three_items(std::uint32_t tcem) -> stm32_fixture
{
  auto       f     = long_source();
  auto const moves = llr("UB1") | llr("ULL");
  place(f, loop_a, std::array{std::uint32_t{64}, moves | (loop_b & 0xfffcu)});
  place(f, loop_b, std::array{std::uint32_t{64}, std::uint32_t{0}});
  start_copy(f, 64, moves | (loop_a & 0xfffcu), tcem);
  return f;
}
}

TEST_CASE("TCEM 2 raises a half transfer at the half of each item and a complete after each link")
{
  auto const f = three_items(2);
  CHECK(
      events_until_idle(f)
      == std::vector<event>{
          {'H', 32 },
          {'T', 64 },
          {'H', 96 },
          {'T', 128},
          {'H', 160},
          {'T', 192}
  });
}

TEST_CASE("TCEM 3 raises one half transfer, at the half of the last item, and one complete")
{
  auto const f = three_items(3);
  CHECK(
      events_until_idle(f)
      == std::vector<event>{
          {'H', 160},
          {'T', 192}
  });
}

TEST_CASE("a channel started with BNDT 0 raises a complete at the first load only under TCEM 2")
{
  for(std::uint32_t const tcem : {0u, 1u, 2u})
  {
    CAPTURE(tcem);
    auto       f     = long_source();
    auto const moves = llr("UB1") | llr("ULL");
    place(f, loop_a, std::array{std::uint32_t{8}, std::uint32_t{0}});
    start_copy(f, 0, moves | (loop_a & 0xfffcu), tcem);
    std::vector<event> expected{
        {'H', 4},
        {'T', 8}
    };
    if(tcem == 2)
    {
      expected.insert(expected.begin(), {'T', 0});
    }
    CHECK(events_until_idle(f) == expected);
  }
}

TEST_CASE("a channel at TCEM 1 whose first item loads TCEM 2 raises nothing at start")
{
  auto       f     = long_source();
  auto const moves = llr("UB1") | llr("ULL");
  auto const tr2   = bit("GPDMA_C0TR2", "SWREQ") | 2u * bit("GPDMA_C0TR2", "TCEM");
  place(f, loop_a, std::array{tr2, std::uint32_t{8}, moves | (loop_b & 0xfffcu)});
  place(f, loop_b, std::array{std::uint32_t{8}, std::uint32_t{0}});
  start_copy(f, 0, llr("UT2") | moves | (loop_a & 0xfffcu), 1);
  CHECK(
      events_until_idle(f)
      == std::vector<event>{
          {'H', 4 },
          {'T', 8 },
          {'H', 12},
          {'T', 16}
  });
}

TEST_CASE("a circular 1D list of ten requested words raises HT after five and TC after ten")
{
  constexpr std::uint32_t self_link = 0x3801'0000;
  auto                    f         = idle_machine();
  auto* const             controller =
      dynamic_cast<popcorn::periph::stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  place(
      f, loop_a,
      std::array{
          std::uint32_t{0x400A'0002}, std::uint32_t{0x8000'0000}, std::uint32_t{40}, copy_from,
          copy_to, self_link | (loop_b & 0xfffcu)});
  place(
      f, loop_b, std::array{std::uint32_t{40}, copy_from, copy_to, self_link | (loop_b & 0xfffcu)});
  write(f, "GPDMA_C1TR1", 0x400A'0002);
  write(f, "GPDMA_C1TR2", 0x4000'0000);
  write(f, "GPDMA_C1BR1", 0);
  write(f, "GPDMA_C1LBAR", loop_a & 0xffff'0000u);
  write(f, "GPDMA_C1LLR", 0xF801'0000 | (loop_a & 0xfffcu));
  write(f, "GPDMA_C1CR", bit("GPDMA_C1CR", "EN"));
  CHECK(take_events(f, "1", copy_to).empty());
  std::vector<event> seen;
  for(std::uint32_t round = 0; round < 3; ++round)
  {
    for(std::uint32_t index = 0; index < 10; ++index)
    {
      REQUIRE(f.target->mem().write(copy_from, 4, 0x100u * round + index));
      controller->on_request(0, true);
      idle_for(f, std::chrono::nanoseconds{1'500});
      controller->on_request(0, false);
      idle_for(f, std::chrono::microseconds{1});
      for(auto const found : take_events(f, "1", copy_to))
      {
        seen.push_back({found.kind, 10 * round + index + 1});
      }
    }
  }
  CHECK(
      seen
      == std::vector<event>{
          {'H', 5 },
          {'T', 10},
          {'H', 15},
          {'T', 20},
          {'H', 25},
          {'T', 30}
  });
  for(std::uint32_t index = 0; index < 10; ++index)
  {
    auto const word = f.target->mem().read(copy_to + 4 * index, 4);
    REQUIRE(word.has_value());
    CHECK(*word == 0x200u + index);
  }
  CHECK(f.field("GPDMA1", "GPDMA_C1CR", "EN") == 1);
}

TEST_CASE("a first item loading BNDT 0 and LLR 0 is a user error with no complete under TCEM 2")
{
  auto f = long_source();
  place(f, loop_a, std::array{std::uint32_t{0}, std::uint32_t{0}});
  start_copy(f, 0, llr("UB1") | llr("ULL") | (loop_a & 0xfffcu), 2);
  CHECK(f.field("GPDMA1", "GPDMA_C0SR", "USEF") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C0SR", "TCF") == 0);
  CHECK(f.field("GPDMA1", "GPDMA_C0CR", "EN") == 0);
}

TEST_CASE("an odd block raises its half transfer once the rounded-up half is moved")
{
  auto f = long_source();
  start_copy(f, 7, 0, 0);
  CHECK(
      events_until_idle(f)
      == std::vector<event>{
          {'H', 4},
          {'T', 7}
  });
}

namespace
{
constexpr popcorn::address stereo     = 0x2002'0000;
constexpr popcorn::address port       = 0x2002'1000;
constexpr popcorn::address list_head  = 0x2002'2000;
constexpr popcorn::address list_tail  = 0x2002'2020;
constexpr std::uint32_t    list_link  = 0x3801'0000;
constexpr std::uint32_t    list_frame = 1024;

struct two_d_registers
{
  std::uint32_t channel{};
  std::uint32_t request{};
  std::uint32_t tr1{};
  std::uint32_t tr2{};
  std::uint32_t br1{};
  std::uint32_t sar{};
  std::uint32_t dar{};
  std::uint32_t tr3{};
  std::uint32_t br2{};
};

constexpr two_d_registers tx_list{
    .channel = 12,
    .request = 36,
    .tr1     = 0x4002'0009,
    .tr2     = 0x8000'0424,
    .br1     = 0x41FF'0004,
    .sar     = stereo,
    .dar     = port,
    .tr3     = 0x0000'03FE,
    .br2     = 0x0000'07FE,
};

constexpr two_d_registers rx_list{
    .channel = 13,
    .request = 37,
    .tr1     = 0x4009'0002,
    .tr2     = 0x8000'0025,
    .br1     = 0x81FF'0008,
    .sar     = port,
    .dar     = stereo,
    .tr3     = 0x03FE'0000,
    .br2     = 0x07FE'0000,
};

auto name_of(std::uint32_t channel, std::string_view reg) -> std::string
{
  return std::format("GPDMA_C{}{}", channel, reg);
}

void put(stm32_fixture const& f, std::uint32_t channel, std::string_view reg, std::uint32_t value)
{
  write(f, name_of(channel, reg), value);
}

auto peek(stm32_fixture const& f, std::uint32_t channel, std::string_view reg) -> std::uint32_t
{
  return reg_value(f, name_of(channel, reg));
}

void start_list(stm32_fixture const& f, two_d_registers const& r, std::uint32_t channel_llr)
{
  place(
      f, list_head,
      std::array{
          r.tr1, r.tr2, r.br1, r.sar, r.dar, r.tr3, r.br2, list_link | (list_tail & 0xfffcu)});
  place(f, list_tail, std::array{r.br1, r.sar, r.dar, list_link | (list_tail & 0xfffcu)});
  put(f, r.channel, "TR2", 1u << 30);
  put(f, r.channel, "BR1", 0);
  put(f, r.channel, "LBAR", list_head & 0xffff'0000u);
  put(f, r.channel, "LLR", channel_llr | (list_head & 0xfffcu));
  put(f, r.channel, "CR", 0x0042'1F00);
  put(f, r.channel, "CR", 0x0042'1F01);
}

void request_pulse(stm32_fixture const& f, std::uint32_t request)
{
  auto* const controller =
      dynamic_cast<popcorn::periph::stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  controller->on_request(request, true);
  idle_for(f, std::chrono::nanoseconds{1'500});
  controller->on_request(request, false);
  idle_for(f, std::chrono::microseconds{1});
}

auto flags_at(stm32_fixture const& f, std::uint32_t channel, std::uint32_t at) -> std::vector<event>
{
  std::vector<event> out;
  for(auto const found : take_events(f, std::format("{}", channel), 0))
  {
    out.push_back({found.kind, at});
  }
  return out;
}

void fill_stereo(stm32_fixture const& f)
{
  for(std::uint32_t index = 0; index < 512; ++index)
  {
    REQUIRE(f.target->mem().write(stereo + 2 * index, 2, index));
    REQUIRE(f.target->mem().write(stereo + 1024 + 2 * index, 2, 0x8000u | index));
  }
}

struct tx_position
{
  std::uint32_t sar{};
  std::uint32_t repeat{};
  std::uint32_t bytes{};

  friend auto operator==(tx_position const&, tx_position const&) -> bool = default;
};

auto operator<<(std::ostream& out, tx_position const& at) -> std::ostream&
{
  return out << std::format("SAR {:#x} BRC {} BNDT {}", at.sar, at.repeat, at.bytes);
}

auto tx_expected(std::uint32_t done) -> tx_position
{
  auto const block = (done - 1) / 2;
  if((done - 1) % 2 == 0)
  {
    return {.sar = stereo + 2 * block + 1024, .repeat = 511 - block, .bytes = 2};
  }
  if(block < 511)
  {
    return {.sar = stereo + 2 * block + 2, .repeat = 510 - block, .bytes = 4};
  }
  return {.sar = stereo, .repeat = 511, .bytes = 4};
}

auto half_and_complete(std::uint32_t lists) -> std::vector<event>
{
  std::vector<event> out;
  for(std::uint32_t list = 0; list < lists; ++list)
  {
    out.push_back({'H', list * list_frame + list_frame / 2});
    out.push_back({'T', (list + 1) * list_frame});
  }
  return out;
}
}

TEST_CASE("a 2D circular list on channel 12 reads the two halves of a stereo buffer frame by frame")
{
  constexpr std::array<std::uint32_t, 13> watched{1,   2,   3,   4,   5,    6,   510,
                                                  511, 512, 513, 514, 1023, 1024};
  auto                                    f = idle_machine();
  fill_stereo(f);
  start_list(f, tx_list, 0xFE01'0000);
  CHECK(flags_at(f, 12, 0).empty());
  std::vector<event>         seen;
  std::vector<std::uint32_t> wrong_words;
  for(std::uint32_t pulse = 1; pulse <= 3 * list_frame; ++pulse)
  {
    request_pulse(f, tx_list.request);
    auto const done  = (pulse - 1) % list_frame + 1;
    auto const frame = (done - 1) / 2;
    auto const word  = f.target->mem().read(port, 4);
    REQUIRE(word.has_value());
    if(*word != ((done - 1) % 2 == 0 ? frame : 0x8000u | frame))
    {
      wrong_words.push_back(pulse);
    }
    if(std::ranges::contains(watched, done))
    {
      CAPTURE(pulse);
      CHECK(
          tx_position{
              .sar    = peek(f, 12, "SAR"),
              .repeat = f.field("GPDMA1", "GPDMA_C12BR1", "BRC"),
              .bytes  = f.field("GPDMA1", "GPDMA_C12BR1", "BNDT")}
          == tx_expected(done));
    }
    std::ranges::copy(flags_at(f, 12, pulse), std::back_inserter(seen));
  }
  CHECK(wrong_words.empty());
  CHECK(seen == half_and_complete(3));
  CHECK(peek(f, 12, "DAR") == port);
  CHECK(f.field("GPDMA1", "GPDMA_C12CR", "EN") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C12SR", "USEF") == 0);
}

namespace
{
auto rx_halves(stm32_fixture const& f)
    -> std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
{
  std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>> out;
  for(std::uint32_t index = 0; index < 512; ++index)
  {
    auto const left  = f.target->mem().read(stereo + 2 * index, 2);
    auto const right = f.target->mem().read(stereo + 1024 + 2 * index, 2);
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    out.first.push_back(static_cast<std::uint32_t>(*left));
    out.second.push_back(static_cast<std::uint32_t>(*right));
  }
  return out;
}

auto rx_round(std::uint32_t first)
    -> std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
{
  std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>> out;
  for(std::uint32_t index = 0; index < 512; ++index)
  {
    out.first.push_back((first + 2 * index) & 0xffffu);
    out.second.push_back((first + 2 * index + 1) & 0xffffu);
  }
  return out;
}

auto rx_pulses(stm32_fixture const& f, std::uint32_t from, std::uint32_t to) -> std::vector<event>
{
  std::vector<event> seen;
  for(std::uint32_t pulse = from; pulse < to; ++pulse)
  {
    REQUIRE(f.target->mem().write(port, 4, 0x0001'0000u | pulse));
    request_pulse(f, rx_list.request);
    std::ranges::copy(flags_at(f, 13, pulse + 1), std::back_inserter(seen));
  }
  return seen;
}
}

TEST_CASE("a 2D circular list on channel 13 writes each frame's two slots into two buffer halves")
{
  auto f = idle_machine();
  start_list(f, rx_list, 0xFE01'0000);
  CHECK(flags_at(f, 13, 0).empty());
  std::vector<event> seen;
  for(std::uint32_t list = 0; list < 3; ++list)
  {
    CAPTURE(list);
    std::ranges::copy(
        rx_pulses(f, list * list_frame, (list + 1) * list_frame), std::back_inserter(seen));
    CHECK(rx_halves(f) == rx_round(list * list_frame));
    CHECK(peek(f, 13, "DAR") == stereo);
    CHECK(f.field("GPDMA1", "GPDMA_C13BR1", "BRC") == 511);
  }
  CHECK(seen == half_and_complete(3));
  CHECK(f.field("GPDMA1", "GPDMA_C13CR", "EN") == 1);
}

namespace
{
struct two_d_copy
{
  std::uint32_t tr1{};
  std::uint32_t tcem{};
  std::uint32_t br1{};
  std::uint32_t tr3{};
  std::uint32_t br2{};
  std::uint32_t sar{copy_from};
  std::uint32_t dar{copy_to};
  std::uint32_t llr{};
};

auto halfwords() -> std::uint32_t
{
  return tr1(1, 1, 0) | bit("GPDMA_C12TR1", "SINC") | bit("GPDMA_C12TR1", "DINC");
}

auto repeat(std::uint32_t blocks) -> std::uint32_t
{
  return blocks * bit("GPDMA_C12BR1", "BRC");
}

void start_two_d(stm32_fixture const& f, two_d_copy const& copy)
{
  put(f, 12, "TR1", copy.tr1);
  put(f, 12, "TR2", bit("GPDMA_C12TR2", "SWREQ") | copy.tcem * bit("GPDMA_C12TR2", "TCEM"));
  put(f, 12, "BR1", copy.br1);
  put(f, 12, "TR3", copy.tr3);
  put(f, 12, "BR2", copy.br2);
  put(f, 12, "SAR", copy.sar);
  put(f, 12, "DAR", copy.dar);
  put(f, 12, "LBAR", node & 0xffff'0000u);
  put(f, 12, "LLR", copy.llr);
  put(f, 12, "CR", bit("GPDMA_C12CR", "EN"));
}

auto two_d_events(stm32_fixture const& f, std::size_t wanted) -> std::vector<event>
{
  std::vector<event> out;
  for(int count = 0; count < 4000 and out.size() < wanted; ++count)
  {
    std::ranges::copy(take_events(f, "12", copy_to), std::back_inserter(out));
    if(f.field("GPDMA1", "GPDMA_C12CR", "EN") == 0)
    {
      break;
    }
    step(f);
  }
  return out;
}

auto run_two_d(two_d_copy const& copy, std::size_t wanted) -> std::vector<event>
{
  auto f = long_source();
  start_two_d(f, copy);
  return two_d_events(f, wanted);
}

auto finish_two_d(two_d_copy const& copy) -> stm32_fixture
{
  auto f = long_source();
  start_two_d(f, copy);
  for(int count = 0; count < 4000 and f.field("GPDMA1", "GPDMA_C12CR", "EN") != 0; ++count)
  {
    step(f);
  }
  REQUIRE(f.field("GPDMA1", "GPDMA_C12CR", "EN") == 0);
  return f;
}

auto bytes_at(stm32_fixture const& f, popcorn::address at, std::uint32_t length)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < length; ++index)
  {
    auto const byte = f.target->mem().read(at + index, 1);
    REQUIRE(byte.has_value());
    out.push_back(static_cast<std::uint32_t>(*byte));
  }
  return out;
}

auto joined(std::initializer_list<std::vector<std::uint32_t>> parts) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const& part : parts)
  {
    std::ranges::copy(part, std::back_inserter(out));
  }
  return out;
}
}

TEST_CASE("channel 12 raises TC and HT per 2D block under TCEM 1 and per block under TCEM 0")
{
  auto const restore = loop_a & 0xfffcu;
  CHECK(
      run_two_d(
          {
              .tr1 = halfwords(), .tcem = 1, .br1 = 4 | repeat(7), .llr = restore
  },
          4)
      == std::vector<event>{{'H', 16}, {'T', 32}, {'H', 48}, {'T', 64}});
  CHECK(
      run_two_d(
          {
              .tr1 = halfwords(), .tcem = 0, .br1 = 4 | repeat(7), .llr = restore
  },
          8)
      == std::vector<event>{
          {'H', 2}, {'T', 4}, {'H', 6}, {'T', 8}, {'H', 10}, {'T', 12}, {'H', 14}, {'T', 16}});
  CHECK(
      run_two_d(
          {
              .tr1 = halfwords(), .tcem = 3, .br1 = 4 | repeat(7)
  },
          4)
      == std::vector<event>{{'H', 16}, {'T', 32}});
}

TEST_CASE("a 2D block of one repeat raises HT with TC at its end, BRC 2 raises HT after two blocks")
{
  CHECK(
      run_two_d(
          {
              .tr1 = halfwords(), .tcem = 1, .br1 = 4
  },
          4)
      == std::vector<event>{{'H', 4}, {'T', 4}});
  CHECK(
      run_two_d(
          {
              .tr1 = halfwords(), .tcem = 1, .br1 = 4 | repeat(2)
  },
          4)
      == std::vector<event>{{'H', 8}, {'T', 12}});
}

TEST_CASE("SAO follows each full source burst, a burst shortened by the block end gets none")
{
  auto const f = finish_two_d(
      {.tr1 = halfwords() | 3u * bit("GPDMA_C12TR1", "SBL_1"), .tcem = 0, .br1 = 20, .tr3 = 8});
  CHECK(peek(f, 12, "SAR") == copy_from + 36);
  CHECK(peek(f, 12, "DAR") == copy_to + 20);
  CHECK(bytes_at(f, copy_to, 20) == joined({source_bytes({0, 16}, 8), source_bytes({32}, 4)}));
}

TEST_CASE("DAO follows each destination burst of DBL_1 + 1 packed words")
{
  auto const f = finish_two_d(
      {.tr1  = tr1(1, 2, 2)
             | bit("GPDMA_C12TR1", "SINC")
             | bit("GPDMA_C12TR1", "DINC")
             | bit("GPDMA_C12TR1", "DBL_1"),
       .tcem = 0,
       .br1  = 16,
       .tr3  = 8u * bit("GPDMA_C12TR3", "DAO")});
  CHECK(peek(f, 12, "DAR") == copy_to + 32);
  CHECK(
      bytes_at(f, copy_to, 24)
      == joined({source_bytes({0}, 8), std::vector<std::uint32_t>(8, 0), source_bytes({8}, 8)}));
}

TEST_CASE("SDEC and BRDDEC subtract their offsets, the address arithmetic wraps unsigned")
{
  SUBCASE("SDEC")
  {
    auto const f = finish_two_d(
        {.tr1 = halfwords(),
         .br1 = 8 | bit("GPDMA_C12BR1", "SDEC"),
         .tr3 = 6,
         .sar = copy_from + 16});
    CHECK(peek(f, 12, "SAR") == copy_from);
    CHECK(bytes_at(f, copy_to, 8) == source_bytes({16, 12, 8, 4}, 2));
  }
  SUBCASE("BRDDEC")
  {
    auto const f = finish_two_d(
        {.tr1 = halfwords(),
         .br1 = 4 | repeat(1) | bit("GPDMA_C12BR1", "BRDDEC"),
         .br2 = 12u * bit("GPDMA_C12BR2", "BRDAO"),
         .dar = copy_to + 16});
    CHECK(peek(f, 12, "DAR") == copy_to);
    CHECK(bytes_at(f, copy_to + 8, 4) == source_bytes({4}, 4));
    CHECK(bytes_at(f, copy_to + 16, 4) == source_bytes({0}, 4));
  }
  SUBCASE("below the SRAM base")
  {
    constexpr popcorn::address low = 0x2000'8000;
    auto const                 f   = finish_two_d(
        {.tr1 = halfwords(),
         .br1 = 4 | bit("GPDMA_C12BR1", "BRDDEC"),
         .br2 = 0xFFF0u * bit("GPDMA_C12BR2", "BRDAO"),
         .dar = low});
    CHECK(peek(f, 12, "DAR") == low + 4 - 0xFFF0);
    CHECK(f.field("GPDMA1", "GPDMA_C12SR", "TCF") == 1);
    CHECK(f.field("GPDMA1", "GPDMA_C12SR", "DTEF") == 0);
  }
}

TEST_CASE("an offset not a multiple of its data width is a user error at enable, nothing moves")
{
  struct offending
  {
    std::string_view name;
    std::uint32_t    tr3{};
    std::uint32_t    br2{};
  };
  for(auto const& [name, tr3, br2] : {
          offending{"SAO 3",   3,        0       },
          offending{"DAO 2",   2u << 16, 0       },
          offending{"BRSAO 1", 0,        1       },
          offending{"BRDAO 6", 0,        6u << 16},
  })
  {
    CAPTURE(name);
    auto f = long_source();
    start_two_d(
        f, {.tr1 = tr1(1, 2, 0) | bit("GPDMA_C12TR1", "SINC") | bit("GPDMA_C12TR1", "DINC"),
            .br1 = 4,
            .tr3 = tr3,
            .br2 = br2});
    idle_for(f, std::chrono::microseconds{10});
    CHECK(f.field("GPDMA1", "GPDMA_C12SR", "USEF") == 1);
    CHECK(f.field("GPDMA1", "GPDMA_C12CR", "EN") == 0);
    CHECK(f.field("GPDMA1", "GPDMA_C12BR1", "BNDT") == 4);
    CHECK(peek(f, 12, "DAR") == copy_to);
  }
}

TEST_CASE("a node 0 loaded without UT3 and UB2 takes its TR3 word as LLR, TR3 and BR2 stay")
{
  auto f = idle_machine();
  put(f, 12, "TR3", 0x0004'0002);
  put(f, 12, "BR2", 0x0008'0006);
  start_list(f, tx_list, 0xF801'0000);
  CHECK(peek(f, 12, "TR1") == tx_list.tr1);
  CHECK(peek(f, 12, "TR2") == tx_list.tr2);
  CHECK(peek(f, 12, "BR1") == tx_list.br1);
  CHECK(peek(f, 12, "SAR") == tx_list.sar);
  CHECK(peek(f, 12, "DAR") == tx_list.dar);
  CHECK(peek(f, 12, "LLR") == tx_list.tr3);
  CHECK(peek(f, 12, "TR3") == 0x0004'0002);
  CHECK(peek(f, 12, "BR2") == 0x0008'0006);
  CHECK(f.field("GPDMA1", "GPDMA_C12CR", "EN") == 1);
  CHECK(f.field("GPDMA1", "GPDMA_C12SR", "USEF") == 0);
  CHECK(f.field("GPDMA1", "GPDMA_C12SR", "ULEF") == 0);
}

namespace
{
constexpr std::uint32_t mid_block_300 = 601;

struct rx_outcome
{
  std::vector<event>                                                seen;
  std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>> halves;
  std::uint32_t                                                     status{};
  std::uint32_t                                                     dar{};

  friend auto operator==(rx_outcome const&, rx_outcome const&) -> bool = default;
};

auto rx_rest(stm32_fixture const& f) -> rx_outcome
{
  rx_outcome out;
  out.seen   = rx_pulses(f, mid_block_300, list_frame);
  out.halves = rx_halves(f);
  out.status = peek(f, 13, "SR");
  out.dar    = peek(f, 13, "DAR");
  return out;
}

auto rx_at_block_300() -> stm32_fixture
{
  auto f = idle_machine();
  start_list(f, rx_list, 0xFE01'0000);
  auto const seen = rx_pulses(f, 0, mid_block_300);
  REQUIRE(
      seen
      == std::vector<event>{
          {'H', 512}
  });
  REQUIRE(f.field("GPDMA1", "GPDMA_C13BR1", "BRC") == 211);
  REQUIRE(f.field("GPDMA1", "GPDMA_C13BR1", "BNDT") == 4);
  REQUIRE(peek(f, 13, "DAR") == stereo + 1024 + 600);
  return f;
}

struct repeat_channel_state
{
  std::uint32_t generation{};
  std::uint32_t block_bytes{};
  std::uint16_t repeat{};
  std::uint32_t packed{};
  std::uint8_t  packed_bytes{};
  bool          half_reported{};
  bool          scheduled{};
};

struct repeat_gpdma_state
{
  std::array<repeat_channel_state, 16> channel{};
  std::array<std::uint8_t, 128>        request{};
};

auto without_beats(new_gpdma_state const& now) -> repeat_gpdma_state
{
  repeat_gpdma_state out{.request = now.request};
  for(auto const [index, c] : std::views::enumerate(now.channel))
  {
    out.channel[static_cast<std::size_t>(index)] = {
        .generation    = c.generation,
        .block_bytes   = c.block_bytes,
        .repeat        = c.repeat,
        .packed        = c.packed,
        .packed_bytes  = c.packed_bytes,
        .half_reported = c.half_reported,
        .scheduled     = c.scheduled,
    };
  }
  return out;
}
}

TEST_CASE(
    "a snapshot mid-block 300 of a 2D list resumes to the same buffer, in place and serialized")
{
  auto       f     = rx_at_block_300();
  auto const saved = f.target->snapshot();
  auto const whole = rx_rest(f);
  CHECK(whole.halves == rx_round(0));
  CHECK(
      whole.seen
      == std::vector<event>{
          {'T', 1024}
  });

  f.target->restore(saved);
  CHECK(rx_rest(f) == whole);

  auto other = idle_machine();
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(rx_rest(other) == whole);
}

TEST_CASE("a GPDMA state saved before the destination burst count loads and resumes a 2D block")
{
  auto       f     = rx_at_block_300();
  auto const saved = f.target->snapshot();
  auto const whole = rx_rest(f);

  std::vector<std::byte> current;
  f.target->restore(saved);
  f.target->model("GPDMA1")->save(current);
  auto const now = reflex::serde::bson::deserializer{
      std::span<std::byte const>{
          current}}.load<new_gpdma_state>();
  REQUIRE(now.channel[13].repeat == 511);
  REQUIRE(now.channel[13].block_bytes == 8);
  REQUIRE(now.channel[13].half_reported);
  REQUIRE(now.channel[13].destination_beats == 0);
  REQUIRE_FALSE(now.channel[13].scheduled);
  std::vector<std::byte> old;
  reflex::serde::bson::serializer{old}.dump(without_beats(now));

  auto other = idle_machine();
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.target->model("GPDMA1")->load(old);
  std::vector<std::byte> loaded_bytes;
  other.target->model("GPDMA1")->save(loaded_bytes);
  auto const loaded = reflex::serde::bson::deserializer{
      std::span<std::byte const>{
          loaded_bytes}}.load<new_gpdma_state>();
  CHECK(loaded.channel[13].destination_beats == 0);
  CHECK(loaded.channel[13].repeat == 511);
  CHECK(rx_rest(other) == whole);
}

TEST_CASE("a snapshot inside a destination burst restores its item count into a fresh machine")
{
  two_d_copy const packed{
      .tr1  = tr1(1, 2, 2)
            | bit("GPDMA_C12TR1", "SINC")
            | bit("GPDMA_C12TR1", "DINC")
            | bit("GPDMA_C12TR1", "DBL_1"),
      .tcem = 0,
      .br1  = 16,
      .tr3  = 8u * bit("GPDMA_C12TR3", "DAO")};
  auto f = long_source();
  start_two_d(f, packed);
  for(int count = 0; count < 4000 and peek(f, 12, "DAR") != copy_to + 4; ++count)
  {
    step(f);
  }
  REQUIRE(peek(f, 12, "DAR") == copy_to + 4);
  auto const saved = f.target->snapshot();
  auto       other = long_source();
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  for(int count = 0; count < 4000 and other.field("GPDMA1", "GPDMA_C12CR", "EN") != 0; ++count)
  {
    step(other);
  }
  CHECK(peek(other, 12, "DAR") == copy_to + 32);
  CHECK(
      bytes_at(other, copy_to, 24)
      == joined({source_bytes({0}, 8), std::vector<std::uint32_t>(8, 0), source_bytes({8}, 8)}));
}

TEST_CASE("a destination burst left partial by the block end gets no DAO, the next block restarts")
{
  auto const f = finish_two_d(
      {.tr1  = tr1(0, 2, 2)
             | bit("GPDMA_C12TR1", "SINC")
             | bit("GPDMA_C12TR1", "DINC")
             | bit("GPDMA_C12TR1", "DBL_1"),
       .tcem = 0,
       .br1  = 12 | repeat(1),
       .tr3  = 8u * bit("GPDMA_C12TR3", "DAO")});
  CHECK(peek(f, 12, "DAR") == copy_to + 40);
  CHECK(
      bytes_at(f, copy_to, 40)
      == joined(
          {source_bytes({0}, 8), std::vector<std::uint32_t>(8, 0), source_bytes({8}, 12),
           std::vector<std::uint32_t>(8, 0), source_bytes({20}, 4)}));
}

TEST_CASE("TCEM 3 on a 2D channel raises HT only at the 2D half of the last item")
{
  auto f = long_source();
  place(f, loop_a, std::array{4 | repeat(3), std::uint32_t{0}});
  start_two_d(
      f, {.tr1  = halfwords(),
          .tcem = 3,
          .br1  = 4 | repeat(3),
          .llr  = llr("UB1") | llr("ULL") | (loop_a & 0xfffcu)});
  CHECK(
      two_d_events(f, 4)
      == std::vector<event>{
          {'H', 24},
          {'T', 32}
  });
}

namespace
{
class requester_probe final : public popcorn::periph::model,
                              public popcorn::periph::stm32::dma_requester
{
public:
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }

  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  void transfer_completed(std::uint32_t request) override
  {
    completed.push_back(request);
  }

  std::vector<std::uint32_t> completed;
};

auto probed_machine() -> stm32_fixture
{
  auto options          = popcorn::testing::stm32_options();
  options.models["CRC"] = [](popcorn::periph::model_context&, popcorn::peripheral_desc const&)
      -> std::shared_ptr<popcorn::periph::model> { return std::make_shared<requester_probe>(); };
  stm32_fixture f{u575(), "rcc_ll", std::move(options)};
  f.set("pc", sym("rcc_ll", "idle"));
  return f;
}

auto probe(stm32_fixture const& f) -> requester_probe&
{
  auto* const found = dynamic_cast<requester_probe*>(f.target->model("CRC"));
  REQUIRE(found != nullptr);
  return *found;
}
}

TEST_CASE("a finished hardware-requested channel tells each requester its REQSEL, once")
{
  auto f = probed_machine();
  write(f, "GPDMA_C0TR1", bit("GPDMA_C0TR1", "SINC") | bit("GPDMA_C0TR1", "DINC"));
  write(f, "GPDMA_C0TR2", bit("GPDMA_C0TR2", "SWREQ"));
  write(f, "GPDMA_C0BR1", 8);
  write(f, "GPDMA_C0SAR", copy_from);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  idle_for(f, std::chrono::microseconds{50});
  REQUIRE(f.field("GPDMA1", "GPDMA_C0SR", "TCF") == 1);
  CHECK(probe(f).completed.empty());

  write(f, "GPDMA_C0FCR", bit("GPDMA_C0FCR", "TCF"));
  write(f, "GPDMA_C0TR2", paced_request * bit("GPDMA_C0TR2", "REQSEL"));
  write(f, "GPDMA_C0BR1", 8);
  write(f, "GPDMA_C0SAR", copy_from);
  write(f, "GPDMA_C0DAR", copy_to);
  write(f, "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  idle_for(f, std::chrono::microseconds{20});
  CHECK(probe(f).completed.empty());
  pace(f, true);
  idle_for(f, std::chrono::microseconds{50});
  REQUIRE(f.field("GPDMA1", "GPDMA_C0SR", "TCF") == 1);
  CHECK(probe(f).completed == std::vector<std::uint32_t>{paced_request});
  idle_for(f, std::chrono::microseconds{50});
  CHECK(probe(f).completed == std::vector<std::uint32_t>{paced_request});
}
