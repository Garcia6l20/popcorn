#include "../support/inert_model.hpp"
#include "dma.hpp"
#include "dma_rows.hpp"
#include "dma_signal.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/firewall.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::address;
using popcorn::bus_attrs;
using popcorn::icount;
using popcorn::part_description;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::arch::memory_op;
using popcorn::testing::l152rb;
using popcorn::testing::l152rc;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware   = std::string_view{"l152xe_l1_dma_ll"};
constexpr auto          firmware_b = std::string_view{"l152xb_l1_dma_ll"};
constexpr address       dma1       = 0x4002'6000;
constexpr address       dma2       = 0x4002'6400;
constexpr address       isr        = 0x00;
constexpr address       ifcr       = 0x04;
constexpr address       ahbrstr    = 0x4002'3800 + 0x10;
constexpr std::uint32_t dma1rst    = 1u << 24;
constexpr address       from       = 0x2000'4000;
constexpr address       to         = 0x2000'5000;
constexpr std::uint32_t en         = 1u << 0;
constexpr std::uint32_t tcie       = 1u << 1;
constexpr std::uint32_t htie       = 1u << 2;
constexpr std::uint32_t teie       = 1u << 3;
constexpr std::uint32_t circ       = 1u << 5;
constexpr std::uint32_t pinc       = 1u << 6;
constexpr std::uint32_t minc       = 1u << 7;
constexpr std::uint32_t psize_word = 2u << 8;
constexpr std::uint32_t msize_word = 2u << 10;
constexpr std::uint32_t mem2mem    = 1u << 14;
constexpr std::uint32_t word_copy  = mem2mem | msize_word | psize_word | minc | pinc;
constexpr std::uint64_t latency_ps = 4'000'000'000'000 / 2'097'152;

constexpr auto ccr(std::uint32_t ch) -> address
{
  return 0x08 + 20 * (ch - 1);
}
constexpr auto cndtr(std::uint32_t ch) -> address
{
  return 0x0C + 20 * (ch - 1);
}
constexpr auto cpar(std::uint32_t ch) -> address
{
  return 0x10 + 20 * (ch - 1);
}
constexpr auto cmar(std::uint32_t ch) -> address
{
  return 0x14 + 20 * (ch - 1);
}
constexpr auto flags(std::uint32_t word, std::uint32_t ch) -> std::uint32_t
{
  return word >> (4 * (ch - 1)) & 0xFu;
}
constexpr std::uint32_t gif  = 1u << 0;
constexpr std::uint32_t tcif = 1u << 1;
constexpr std::uint32_t htif = 1u << 2;
constexpr std::uint32_t teif = 1u << 3;

auto parked(
    part_description const& desc    = l152re(),
    std::string_view        name    = firmware,
    core::machine_options   options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{desc, name, std::move(options)};
  f.set("pc", sym(name, "idle"));
  return f;
}

auto read(stm32_fixture const& f, address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void write(stm32_fixture const& f, address at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}

auto source_word(std::uint32_t index) -> std::uint32_t
{
  return (0x1122'3344u * (index + 1u)) ^ 0xA5A5'A5A5u;
}

void fill(stm32_fixture const& f, std::uint32_t words)
{
  for(std::uint32_t index = 0; index < words; ++index)
  {
    write(f, from + 4 * index, source_word(index));
    write(f, to + 4 * index, 0);
  }
}

void program(
    stm32_fixture const& f,
    address              dma,
    std::uint32_t        ch,
    std::uint32_t        control,
    address              peripheral,
    address              memory,
    std::uint32_t        items)
{
  write(f, dma + cpar(ch), static_cast<std::uint32_t>(peripheral));
  write(f, dma + cmar(ch), static_cast<std::uint32_t>(memory));
  write(f, dma + cndtr(ch), items);
  write(f, dma + ccr(ch), control);
}

auto now_ps(stm32_fixture const& f) -> std::uint64_t
{
  return f.target->virtual_clock().time().ps;
}

void idle_to(stm32_fixture const& f, std::uint64_t ps)
{
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
      == stop_reason::time);
}

auto copied(stm32_fixture const& f, std::uint32_t words) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < words; ++index)
  {
    out.push_back(read(f, to + 4 * index));
  }
  return out;
}

auto expected_log() -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  auto const                 s    = [](std::uint32_t index) { return source_word(index); };
  auto const                 copy = [&](std::uint32_t d0, std::uint32_t d1, std::uint32_t last) {
    out.insert(out.end(), {gif | tcif | htif, 0, 1, d0, d1, last});
  };
  copy(s(0), s(1), s(15));
  auto const byte = [&](std::uint32_t at) { return s(at / 4) >> (8 * (at % 4)) & 0xFFu; };
  copy(byte(0) | byte(1) << 16, byte(2) | byte(3) << 16, 0xFFFF'FFFFu);
  copy(
      (s(0) & 0xFFu) | (s(1) & 0xFFu) << 8 | (s(2) & 0xFFu) << 16 | (s(3) & 0xFFu) << 24,
      0xFFFF'FFFFu, 0xFFFF'FFFFu);
  copy(s(0) & 0xFFFFu, s(0) >> 16, s(1) >> 16);
  out.insert(out.end(), {gif | htif, 0, 0x7C00, 1, 1, 1, gif | teif, 0, 4, 0});
  return out;
}

class counting_firewall final : public popcorn::testing::inert_model, public periph::bus_firewall
{
public:
  explicit counting_firewall(periph::model_context& ctx)
  {
    ctx.install_firewall(*this);
  }

  auto check_peripheral(std::size_t, bus_attrs, memory_op) -> periph::bus_verdict override
  {
    return periph::bus_verdict::allow;
  }
  [[nodiscard]] auto filters_memory() const -> bool override
  {
    return true;
  }
  auto check_memory(address, std::uint8_t, bus_attrs attrs, memory_op)
      -> periph::bus_verdict override
  {
    if(attrs.by == popcorn::initiator::dma)
    {
      ++checks;
    }
    return periph::bus_verdict::allow;
  }
  auto allowed_extent(address, std::uint8_t, bus_attrs, memory_op) -> periph::memory_extent override
  {
    return {.first = 0, .last = ~address{}};
  }

  std::size_t checks{};
};

struct log_capture
{
  std::vector<core::log_record> records;
  core::hook_handle             sink;

  explicit log_capture(stm32_fixture const& f)
      : sink{f.target->add_log_sink(
            [this](core::log_record const& record) { records.push_back(record); })}
  {}
};
}

TEST_CASE("RM0038 11.3: the LL firmware copies, packs per Table 53, flags HT TC TE and clears them")
{
  for(auto const& [desc, name] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(name);
    stm32_fixture f{*desc, name};
    REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
    CHECK(f.log() == expected_log());
  }
}

TEST_CASE("RM0038 11.3.2: the arbiter serves PL first, then the lowest channel, after every item")
{
  auto const f = parked();
  fill(f, 16);
  program(f, dma1, 2, word_copy, from, to, 8);
  program(f, dma1, 3, word_copy | (3u << 12), from + 32, to + 32, 8);
  auto const t0 = now_ps(f);
  write(f, dma1 + ccr(2), word_copy | en);
  write(f, dma1 + ccr(3), word_copy | (3u << 12) | en);
  idle_to(f, t0 + 8 * latency_ps + latency_ps / 2);
  CHECK(flags(read(f, dma1 + isr), 3) == (gif | tcif | htif));
  CHECK(read(f, dma1 + cndtr(2)) == 8);
  idle_to(f, t0 + 16 * latency_ps + latency_ps / 2);
  CHECK(flags(read(f, dma1 + isr), 2) == (gif | tcif | htif));
  CHECK(
      copied(f, 16)
      == std::vector<std::uint32_t>{
          source_word(0), source_word(1), source_word(2), source_word(3), source_word(4),
          source_word(5), source_word(6), source_word(7), source_word(8), source_word(9),
          source_word(10), source_word(11), source_word(12), source_word(13), source_word(14),
          source_word(15)});

  write(f, dma1 + ifcr, 0x0FFF'FFFFu);
  program(f, dma1, 5, word_copy | (1u << 12), from, to, 4);
  program(f, dma1, 4, word_copy | (1u << 12), from, to, 4);
  auto const t1 = now_ps(f);
  write(f, dma1 + ccr(5), word_copy | (1u << 12) | en);
  write(f, dma1 + ccr(4), word_copy | (1u << 12) | en);
  idle_to(f, t1 + 4 * latency_ps + latency_ps / 2);
  CHECK(flags(read(f, dma1 + isr), 4) == (gif | tcif | htif));
  CHECK(read(f, dma1 + cndtr(5)) == 4);
}

TEST_CASE("RM0038 11.3.2: a higher priority channel enabled mid-copy takes the next item")
{
  auto const f = parked();
  fill(f, 16);
  program(f, dma1, 1, word_copy, from, to, 8);
  auto const t0 = now_ps(f);
  write(f, dma1 + ccr(1), word_copy | en);
  idle_to(f, t0 + 2 * latency_ps + latency_ps / 2);
  REQUIRE(read(f, dma1 + cndtr(1)) == 6);
  program(f, dma1, 7, word_copy | (2u << 12) | en, from + 32, to + 32, 4);
  idle_to(f, t0 + 7 * latency_ps + latency_ps / 2);
  CHECK(read(f, dma1 + cndtr(7)) == 0);
  CHECK(read(f, dma1 + cndtr(1)) == 5);
}

TEST_CASE(
    "RM0038 11.3.5: a read of an unmapped address or, inferred, of a bit-band alias "
    "sets TEIF, clears EN")
{
  for(address const source : {address{0x6000'0000}, address{0x2200'0000}})
  {
    CAPTURE(source);
    auto const f = parked();
    program(f, dma1, 1, word_copy | en, source, to, 4);
    idle_to(f, now_ps(f) + 2 * latency_ps);
    CHECK(flags(read(f, dma1 + isr), 1) == (gif | teif));
    CHECK((read(f, dma1 + ccr(1)) & en) == 0);
    CHECK(read(f, dma1 + cndtr(1)) == 4);
  }
}

TEST_CASE("RM0038 11.3.5: a write to an unmapped address sets TEIF, clears EN, keeps NDT")
{
  auto const f = parked();
  fill(f, 4);
  program(f, dma1, 1, word_copy | en, from, 0x6000'0000, 4);
  idle_to(f, now_ps(f) + 2 * latency_ps);
  CHECK(flags(read(f, dma1 + isr), 1) == (gif | teif));
  CHECK((read(f, dma1 + ccr(1)) & en) == 0);
  CHECK(read(f, dma1 + cndtr(1)) == 4);
}

TEST_CASE("RM0038 11.4.5 and 11.4.6: an odd CMAR of a halfword copy is aligned down")
{
  constexpr std::uint32_t half_copy = mem2mem | (1u << 10) | (1u << 8) | minc | pinc;
  auto const              f         = parked();
  write(f, from, 0xBEEF'CAFEu);
  write(f, to, 0);
  program(f, dma1, 1, half_copy | en, from, to + 1, 1);
  idle_to(f, now_ps(f) + 2 * latency_ps);
  CHECK(read(f, dma1 + cndtr(1)) == 0);
  CHECK(read(f, to) == 0x0000'CAFEu);
}

TEST_CASE(
    "RM0038 11.4.4: CNDTR ignores writes while EN is set, CPAR and CMAR reach the next enable")
{
  auto const f = parked();
  fill(f, 8);
  program(f, dma1, 1, word_copy | en, from, to, 8);
  write(f, dma1 + cndtr(1), 2);
  write(f, dma1 + cpar(1), static_cast<std::uint32_t>(from + 16));
  CHECK(read(f, dma1 + cndtr(1)) == 8);
  CHECK(read(f, dma1 + cpar(1)) == from + 16);
  idle_to(f, now_ps(f) + 9 * latency_ps);
  CHECK(read(f, to) == source_word(0));
  CHECK(read(f, to + 28) == source_word(7));
  CHECK((read(f, dma1 + ccr(1)) & en) == en);
  write(f, dma1 + ccr(1), word_copy);
  write(f, dma1 + cndtr(1), 2);
  write(f, dma1 + cmar(1), static_cast<std::uint32_t>(to + 64));
  write(f, dma1 + ccr(1), word_copy | en);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  CHECK(read(f, to + 64) == source_word(4));
  CHECK(read(f, to + 68) == source_word(5));
}

TEST_CASE("RM0038 11.3.3: HTIF sets once half the items are left, TCIF once none is")
{
  auto const f = parked();
  fill(f, 4);
  auto const t0 = now_ps(f);
  program(f, dma1, 1, word_copy | en, from, to, 4);
  idle_to(f, t0 + latency_ps + latency_ps / 2);
  CHECK(flags(read(f, dma1 + isr), 1) == 0);
  idle_to(f, t0 + 2 * latency_ps + latency_ps / 2);
  CHECK(flags(read(f, dma1 + isr), 1) == (gif | htif));
  idle_to(f, t0 + 4 * latency_ps + latency_ps / 2);
  CHECK(flags(read(f, dma1 + isr), 1) == (gif | htif | tcif));
}

TEST_CASE("RM0038 11.3.3: TC with TCIE raises DMA1_Channel1_IRQn, a write of CGIF1 lowers it")
{
  constexpr address       ispr0 = 0xE000'E200;
  constexpr std::uint32_t irq   = 11;
  auto const              f     = parked();
  fill(f, 2);
  program(f, dma1, 1, word_copy | tcie | en, from, to, 2);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  CHECK((read(f, ispr0) >> irq & 1u) == 1);
  write(f, dma1 + ifcr, gif);
  write(f, 0xE000'E280, 1u << irq);
  CHECK(flags(read(f, dma1 + isr), 1) == 0);
  CHECK((read(f, ispr0) >> irq & 1u) == 0);
}

TEST_CASE("RM0038 Table 54: HT with HTIE alone, and TE with TEIE alone, raise DMA1_Channel1_IRQn")
{
  constexpr address       ispr0 = 0xE000'E200;
  constexpr std::uint32_t irq   = 11;
  {
    auto const f  = parked();
    auto const t0 = now_ps(f);
    fill(f, 4);
    program(f, dma1, 1, word_copy | htie | en, from, to, 4);
    idle_to(f, t0 + latency_ps + latency_ps / 2);
    CHECK((read(f, ispr0) >> irq & 1u) == 0);
    idle_to(f, t0 + 2 * latency_ps + latency_ps / 2);
    CHECK(read(f, dma1 + cndtr(1)) == 2);
    CHECK(flags(read(f, dma1 + isr), 1) == (gif | htif));
    CHECK((read(f, ispr0) >> irq & 1u) == 1);
  }
  auto const f = parked();
  program(f, dma1, 1, word_copy | teie | en, 0x6000'0000, to, 4);
  idle_to(f, now_ps(f) + 2 * latency_ps);
  CHECK(flags(read(f, dma1 + isr), 1) == (gif | teif));
  CHECK((read(f, ispr0) >> irq & 1u) == 1);
}

TEST_CASE("a DMA snapshot mid-copy restores to the same result, in place and serialized")
{
  auto       f  = parked();
  auto const t0 = now_ps(f);
  fill(f, 16);
  program(f, dma1, 1, word_copy, from, to, 16);
  write(f, dma1 + ccr(1), word_copy | en);
  idle_to(f, t0 + 5 * latency_ps + latency_ps / 2);
  REQUIRE(read(f, dma1 + cndtr(1)) == 11);
  auto const saved = f.target->snapshot();
  auto const end   = t0 + 17 * latency_ps;
  idle_to(f, end);
  auto const result = copied(f, 16);
  auto const flags1 = read(f, dma1 + isr);
  REQUIRE(read(f, dma1 + cndtr(1)) == 0);

  f.target->restore(saved);
  idle_to(f, end);
  CHECK(copied(f, 16) == result);
  CHECK(read(f, dma1 + isr) == flags1);

  auto other = parked();
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  idle_to(other, end);
  CHECK(copied(other, 16) == result);
  CHECK(read(other, dma1 + isr) == flags1);
  CHECK(read(other, dma1 + cndtr(1)) == 0);
}

TEST_CASE("checklist 31: a DMA1RST pulse drops the pending transfer, a new copy keeps its own time")
{
  auto const f = parked();
  fill(f, 8);
  auto const t0 = now_ps(f);
  program(f, dma1, 1, word_copy | en, from, to, 8);
  idle_to(f, t0 + 2 * latency_ps + latency_ps / 2);
  REQUIRE(read(f, dma1 + cndtr(1)) == 6);
  write(f, ahbrstr, dma1rst);
  write(f, ahbrstr, 0);
  CHECK(read(f, dma1 + ccr(1)) == 0);
  CHECK(read(f, dma1 + cndtr(1)) == 0);
  CHECK(read(f, dma1 + isr) == 0);
  auto const t1 = now_ps(f);
  program(f, dma1, 1, word_copy | en, from + 16, to + 64, 4);
  idle_to(f, t0 + 3 * latency_ps + latency_ps / 4);
  CHECK(read(f, dma1 + cndtr(1)) == 4);
  idle_to(f, t1 + latency_ps + latency_ps / 4);
  CHECK(read(f, dma1 + cndtr(1)) == 3);
  CHECK(read(f, to + 64) == source_word(4));
}

TEST_CASE("a CIRC memory-to-memory channel copies once and warns, a model_error under strict")
{
  {
    auto const        f = parked();
    log_capture const logs{f};
    fill(f, 4);
    program(f, dma1, 1, word_copy | circ | en, from, to, 4);
    idle_to(f, now_ps(f) + 10 * latency_ps);
    CHECK(read(f, dma1 + cndtr(1)) == 0);
    CHECK(read(f, to + 12) == source_word(3));
    CHECK(logs.records.size() == 1);
    CHECK(logs.records.front().reg == "CCR1");
  }
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const f               = parked(l152re(), firmware, std::move(options));
  program(f, dma1, 1, word_copy | circ, from, to, 4);
  CHECK_THROWS_AS(
      static_cast<void>(f.target->mem().write(dma1 + ccr(1), 4, word_copy | circ | en)),
      popcorn::model_error);
}

TEST_CASE("a reserved PSIZE of 3 transfers words and warns once, a model_error at EN under strict")
{
  auto const        f = parked();
  log_capture const logs{f};
  fill(f, 4);
  program(f, dma1, 1, (word_copy | (3u << 8)) | en, from, to, 4);
  idle_to(f, now_ps(f) + 5 * latency_ps);
  CHECK(
      copied(f, 4)
      == std::vector<std::uint32_t>{
          source_word(0), source_word(1), source_word(2), source_word(3)});
  CHECK(logs.records.size() == 1);
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const strict          = parked(l152re(), firmware, std::move(options));
  program(strict, dma1, 1, word_copy | (3u << 8), from, to, 4);
  CHECK_THROWS_AS(
      static_cast<void>(strict.target->mem().write(dma1 + ccr(1), 4, word_copy | (3u << 8) | en)),
      popcorn::model_error);
}

TEST_CASE("a DMA copy asks the bus firewall once per grant and side, not once per item")
{
  std::shared_ptr<counting_firewall> firewall;
  auto                               options = popcorn::testing::stm32_options();
  options.models.emplace(
      "FLASH",
      [&firewall](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> {
        firewall = std::make_shared<counting_firewall>(ctx);
        return firewall;
      });
  auto const f = parked(l152re(), firmware, std::move(options));
  REQUIRE(firewall != nullptr);
  fill(f, 16);
  program(f, dma1, 1, word_copy | en, from, to, 16);
  idle_to(f, now_ps(f) + 17 * latency_ps);
  CHECK(read(f, dma1 + cndtr(1)) == 0);
  CHECK(firewall->checks == 2);
}

TEST_CASE("every L1 part with DMA2 gets 5 channels, Cat.1 has DMA1 only")
{
  for(auto const* desc : {&l152re(), &l152rc()})
  {
    auto const f = parked(*desc, firmware);
    CHECK(f.target->model("DMA1") != nullptr);
    CHECK(f.target->model("DMA2") != nullptr);
    fill(f, 4);
    program(f, dma2, 5, word_copy | en, from, to, 4);
    idle_to(f, now_ps(f) + 5 * latency_ps);
    CHECK(flags(read(f, dma2 + isr), 5) == (gif | tcif | htif));
  }
  auto const b = parked(l152rb(), firmware_b);
  CHECK(b.target->model("DMA1") != nullptr);
  CHECK(std::ranges::none_of(l152rb().peripherals, [](popcorn::peripheral_desc const& unit) {
    return unit.name == "DMA2";
  }));
}

TEST_CASE("the DMA2 model refuses a description with channel 6 registers or channel 6 flags")
{
  auto const& base = l152re();
  auto const unit = [](part_description& desc, std::string_view name) -> popcorn::peripheral_desc& {
    auto const found = std::ranges::find(desc.peripherals, name, &popcorn::peripheral_desc::name);
    REQUIRE(found != desc.peripherals.end());
    return *found;
  };
  {
    auto        desc = base;
    auto const& one  = unit(desc, "DMA1");
    auto&       two  = unit(desc, "DMA2");
    for(auto const& reg : one.registers)
    {
      if(reg.name.ends_with('6'))
      {
        two.registers.push_back(reg);
      }
    }
    CHECK_THROWS_AS(stm32_fixture(desc, firmware), popcorn::config_error);
  }
  auto        desc = base;
  auto const& one  = unit(desc, "DMA1");
  auto&       two  = unit(desc, "DMA2");
  auto const  isr1 =
      std::ranges::find(one.registers, std::string_view{"ISR"}, &popcorn::register_desc::name);
  auto const isr2 =
      std::ranges::find(two.registers, std::string_view{"ISR"}, &popcorn::register_desc::name);
  REQUIRE(isr1 != one.registers.end());
  REQUIRE(isr2 != two.registers.end());
  *isr2 = *isr1;
  CHECK_THROWS_AS(stm32_fixture(desc, firmware), popcorn::config_error);
}

TEST_CASE("the DMA model refuses a variant past its request slots or naming a channel it lacks")
{
  auto const refused = [](stm32::dma_classic_variant const& variant) {
    auto options = popcorn::testing::stm32_options();
    options.models.insert_or_assign("DMA1", stm32::dma_classic_factory(variant));
    CHECK_THROWS_AS(parked(l152re(), firmware, std::move(options)), popcorn::config_error);
  };
  std::array<stm32::dma_classic_request, 65> many{};
  std::ranges::fill(many, stm32::dma_classic_request{.signal = "usart1_tx_dma", .channel = 4});
  refused({.channels = 7, .requests = many});
  refused({.channels = 3, .requests = std::span{many}.first(64)});
  constexpr std::array<stm32::dma_classic_request, 1> zero{
      {{.signal = "usart1_tx_dma", .channel = 0}}};
  refused({.channels = 7, .requests = zero});
  auto options = popcorn::testing::stm32_options();
  options.models.insert_or_assign(
      "DMA1", stm32::dma_classic_factory({.channels = 7, .requests = std::span{many}.first(64)}));
  CHECK_NOTHROW(parked(l152re(), firmware, std::move(options)));
}

namespace
{
class requester final : public popcorn::testing::inert_model, public stm32::dma_requester
{
public:
  requester(
      periph::model_context&   ctx,
      std::vector<std::string> names,
      std::vector<std::string> claimed)
      : names_{std::move(names)}, claimed_{std::move(claimed)}
  {
    for(auto const& name : names_)
    {
      signals_.emplace_back(ctx.peers, name);
    }
  }

  struct bus_write
  {
    std::uint32_t offset;
    std::uint8_t  size;
    std::uint32_t value;

    auto operator==(bus_write const&) const -> bool = default;
  };

  std::vector<std::string> acks;
  std::vector<std::string> completions;
  std::vector<std::string> last_but_ones;
  std::vector<std::string> count_ends;
  std::vector<bus_write>   writes;
  bool                     drop_on_ack = true;
  std::uint32_t            next_data{};

  void set(std::string_view name, bool level)
  {
    signal(name).set(level);
  }

  void acknowledged(stm32::dma_controller const& by, std::uint32_t request) override
  {
    for(std::size_t index = 0; index < signals_.size(); ++index)
    {
      if(signals_[index].carries(by, request))
      {
        acks.push_back(names_[index]);
        if(drop_on_ack)
        {
          signals_[index].set(false);
        }
      }
    }
  }

  void transfer_completed(stm32::dma_controller const& by, std::uint32_t request) override
  {
    for(std::size_t index = 0; index < signals_.size(); ++index)
    {
      if(signals_[index].carries(by, request))
      {
        completions.push_back(names_[index]);
      }
    }
  }

  void last_but_one(stm32::dma_controller const& by, std::uint32_t request) override
  {
    for(std::size_t index = 0; index < signals_.size(); ++index)
    {
      if(signals_[index].carries(by, request))
      {
        last_but_ones.push_back(names_[index]);
      }
    }
  }

  void count_ended(stm32::dma_controller const& by, std::uint32_t request) override
  {
    for(std::size_t index = 0; index < signals_.size(); ++index)
    {
      if(signals_[index].carries(by, request))
      {
        count_ends.push_back(names_[index]);
      }
    }
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return claimed_;
  }
  void reset(periph::register_file&, reset_kind) override
  {
    for(auto& one : signals_)
    {
      one.resolve();
    }
  }
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return next_data++;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t offset,
      std::uint8_t  size,
      std::uint32_t,
      std::uint32_t value,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    writes.push_back({.offset = offset, .size = size, .value = value});
    return {};
  }

private:
  auto signal(std::string_view name) -> stm32::dma_signal&
  {
    auto const found = std::ranges::find(names_, name);
    REQUIRE(found != names_.end());
    return signals_[static_cast<std::size_t>(found - names_.begin())];
  }

  std::vector<std::string>       names_;
  std::vector<stm32::dma_signal> signals_;
  std::vector<std::string>       claimed_;
};

struct table_row
{
  std::string_view instance;
  std::uint32_t    channel;
  std::string_view signal;
};

constexpr std::array<table_row, 48> rm0038_requests{
    {
     {"DMA1", 1, "adc_dma"},       {"DMA1", 1, "tim2_cc3_dma"},  {"DMA1", 1, "tim4_cc1_dma"},
     {"DMA1", 2, "spi1_rx_dma"},   {"DMA1", 2, "usart3_tx_dma"}, {"DMA1", 2, "tim2_upd_dma"},
     {"DMA1", 2, "tim3_cc3_dma"},  {"DMA1", 2, "tim6_upd_dma"},  {"DMA1", 2, "dac_ch1_dma"},
     {"DMA1", 3, "spi1_tx_dma"},   {"DMA1", 3, "usart3_rx_dma"}, {"DMA1", 3, "tim3_cc4_dma"},
     {"DMA1", 3, "tim3_upd_dma"},  {"DMA1", 3, "tim7_upd_dma"},  {"DMA1", 3, "dac_ch2_dma"},
     {"DMA1", 4, "spi2_rx_dma"},   {"DMA1", 4, "usart1_tx_dma"}, {"DMA1", 4, "i2c2_tx_dma"},
     {"DMA1", 4, "tim4_cc2_dma"},  {"DMA1", 5, "spi2_tx_dma"},   {"DMA1", 5, "usart1_rx_dma"},
     {"DMA1", 5, "i2c2_rx_dma"},   {"DMA1", 5, "tim2_cc1_dma"},  {"DMA1", 5, "tim4_cc3_dma"},
     {"DMA1", 6, "usart2_rx_dma"}, {"DMA1", 6, "i2c1_tx_dma"},   {"DMA1", 6, "tim3_cc1_dma"},
     {"DMA1", 6, "tim3_trg_dma"},  {"DMA1", 7, "usart2_tx_dma"}, {"DMA1", 7, "i2c1_rx_dma"},
     {"DMA1", 7, "tim2_cc2_dma"},  {"DMA1", 7, "tim2_cc4_dma"},  {"DMA1", 7, "tim4_upd_dma"},
     {"DMA2", 1, "spi3_rx_dma"},   {"DMA2", 1, "uart5_tx_dma"},  {"DMA2", 1, "tim5_cc4_dma"},
     {"DMA2", 1, "tim5_trg_dma"},  {"DMA2", 2, "spi3_tx_dma"},   {"DMA2", 2, "uart5_rx_dma"},
     {"DMA2", 2, "tim5_cc3_dma"},  {"DMA2", 2, "tim5_upd_dma"},  {"DMA2", 3, "uart4_rx_dma"},
     {"DMA2", 3, "aes_out_dma"},   {"DMA2", 4, "tim5_cc2_dma"},  {"DMA2", 4, "sdio_dma"},
     {"DMA2", 5, "uart4_tx_dma"},  {"DMA2", 5, "tim5_cc1_dma"},  {"DMA2", 5, "aes_in_dma"},
     }
};

auto instance_of(std::string_view signal) -> std::string
{
  std::string out{signal.substr(0, signal.find('_'))};
  std::ranges::transform(out, out.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  return out;
}

auto has_unit(part_description const& desc, std::string_view name) -> bool
{
  return std::ranges::any_of(
      desc.peripherals, [name](popcorn::peripheral_desc const& unit) { return unit.name == name; });
}

using names_by_unit = std::map<std::string, std::vector<std::string>>;

auto with_requesters(
    part_description const& desc,
    names_by_unit const&    by_unit,
    names_by_unit const&    claims = {}) -> core::machine_options
{
  auto options = popcorn::testing::stm32_options();
  for(auto const& [unit, names] : by_unit)
  {
    if(has_unit(desc, unit))
    {
      auto const claimed = claims.find(unit);
      options.models.emplace(
          unit,
          [names,
           registers = claimed == claims.end() ? std::vector<std::string>{} : claimed->second](
              periph::model_context& ctx,
              popcorn::peripheral_desc const&) -> std::shared_ptr<periph::model> {
            return std::make_shared<requester>(ctx, names, registers);
          });
    }
  }
  return options;
}

auto peer(stm32_fixture const& f, std::string_view unit) -> requester&
{
  auto* const found = f.model_of<requester>(unit);
  REQUIRE(found != nullptr);
  return *found;
}

constexpr std::uint32_t word_sizes = psize_word | msize_word;
}

TEST_CASE("RM0038 Tables 55 and 56: the DMA1 and DMA2 variants hold every request on its channel")
{
  auto const listed = [](stm32::dma_classic_variant const& variant, std::string_view instance) {
    std::vector<std::pair<std::string_view, std::uint32_t>> out;
    for(auto const& request : variant.requests)
    {
      CAPTURE(request.signal);
      out.emplace_back(request.signal, request.channel);
    }
    std::vector<std::pair<std::string_view, std::uint32_t>> expected;
    for(auto const& row : rm0038_requests)
    {
      if(row.instance == instance)
      {
        expected.emplace_back(row.signal, row.channel);
      }
    }
    std::ranges::sort(out);
    std::ranges::sort(expected);
    CHECK(out == expected);
  };
  listed(stm32::l1_dma1, "DMA1");
  listed(stm32::l1_dma2, "DMA2");
}

TEST_CASE("RM0038 11.3.7: every request of a peripheral the part has moves one item on its channel")
{
  constexpr address source      = 0x2000'2000;
  constexpr address destination = 0x2000'2400;
  struct leg
  {
    part_description const&       desc;
    std::string_view              name;
    std::vector<std::string_view> unrouted;
  };
  std::vector<std::string_view> dma2_rows;
  for(auto const& row : rm0038_requests)
  {
    if(row.instance == "DMA2")
    {
      dma2_rows.push_back(row.signal);
    }
  }
  for(auto const& [desc, name, unrouted] : {
          leg{l152re(), firmware,   {"aes_out_dma", "sdio_dma", "aes_in_dma"}},
          leg{l152rb(), firmware_b, dma2_rows                                },
          leg{l152rc(),
              firmware,             {"uart5_tx_dma", "uart5_rx_dma", "uart4_rx_dma", "aes_out_dma", "sdio_dma",
               "uart4_tx_dma", "aes_in_dma"}          }
  })
  {
    CAPTURE(name);
    names_by_unit by_unit;
    for(auto const& row : rm0038_requests)
    {
      by_unit[instance_of(row.signal)].emplace_back(row.signal);
    }
    auto const                    f = parked(desc, name, with_requesters(desc, by_unit));
    std::vector<std::string_view> skipped;
    for(auto const& row : rm0038_requests)
    {
      auto const unit = instance_of(row.signal);
      if(not has_unit(desc, row.instance) or not has_unit(desc, unit))
      {
        skipped.push_back(row.signal);
        continue;
      }
      CAPTURE(row.signal);
      std::vector<std::string> sharing;
      for(auto const& other : rm0038_requests)
      {
        if(other.instance == row.instance
           and other.channel == row.channel
           and instance_of(other.signal) == unit)
        {
          sharing.emplace_back(other.signal);
        }
      }
      auto const dma = row.instance == "DMA1" ? dma1 : dma2;
      auto&      p   = peer(f, unit);
      p.acks.clear();
      p.completions.clear();
      write(f, source, 0x5A00'0000u + row.channel);
      write(f, destination, 0);
      write(f, dma + ifcr, 0x0FFF'FFFFu);
      program(f, dma, row.channel, word_sizes | en, source, destination, 1);
      idle_to(f, now_ps(f) + 2 * latency_ps);
      REQUIRE(read(f, dma + cndtr(row.channel)) == 1);
      p.set(row.signal, true);
      idle_to(f, now_ps(f) + 2 * latency_ps);
      CHECK(read(f, dma + cndtr(row.channel)) == 0);
      CHECK(read(f, destination) == 0x5A00'0000u + row.channel);
      CHECK(flags(read(f, dma + isr), row.channel) == (gif | tcif | htif));
      CHECK(p.acks == std::vector<std::string>{std::string{row.signal}});
      CHECK(p.completions == sharing);
      CHECK(p.last_but_ones.empty());
      write(f, dma + ccr(row.channel), 0);
    }
    CHECK(skipped == unrouted);
  }
}

TEST_CASE(
    "RM0038 11.3.7: two requests ORed into DMA1 channel 4, each peer takes only its own "
    "acknowledge, completion and EOT_1, a memory-to-memory copy tells none")
{
  names_by_unit const by_unit{
      {"USART1", {"usart1_tx_dma"}},
      {"SPI2",   {"spi2_rx_dma"}  },
  };
  auto const f     = parked(l152re(), firmware, with_requesters(l152re(), by_unit));
  auto&      usart = peer(f, "USART1");
  auto&      spi   = peer(f, "SPI2");
  fill(f, 2);
  program(f, dma1, 4, word_sizes | minc | pinc | en, from, to, 2);
  usart.set("usart1_tx_dma", true);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  CHECK(read(f, dma1 + cndtr(4)) == 1);
  CHECK(usart.acks == std::vector<std::string>{"usart1_tx_dma"});
  CHECK(spi.acks.empty());
  CHECK(usart.last_but_ones == std::vector<std::string>{"usart1_tx_dma"});
  CHECK(spi.last_but_ones == std::vector<std::string>{"spi2_rx_dma"});
  spi.set("spi2_rx_dma", true);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  CHECK(read(f, dma1 + cndtr(4)) == 0);
  CHECK(copied(f, 2) == std::vector<std::uint32_t>{source_word(0), source_word(1)});
  CHECK(usart.acks == std::vector<std::string>{"usart1_tx_dma"});
  CHECK(spi.acks == std::vector<std::string>{"spi2_rx_dma"});
  CHECK(usart.completions == std::vector<std::string>{"usart1_tx_dma"});
  CHECK(spi.completions == std::vector<std::string>{"spi2_rx_dma"});
  CHECK(usart.last_but_ones.size() == 1);
  CHECK(spi.last_but_ones.size() == 1);
  usart.set("usart1_tx_dma", false);
  spi.set("spi2_rx_dma", false);
  fill(f, 2);
  write(f, dma1 + ccr(4), 0);
  program(f, dma1, 4, word_copy | en, from, to, 2);
  idle_to(f, now_ps(f) + 4 * latency_ps);
  CHECK(read(f, dma1 + cndtr(4)) == 0);
  CHECK(copied(f, 2) == std::vector<std::uint32_t>{source_word(0), source_word(1)});
  CHECK(usart.last_but_ones.size() == 1);
  CHECK(spi.last_but_ones.size() == 1);
}

TEST_CASE("RM0038 11.3.7: every requester among the last 40 peers is told")
{
  std::set<std::string, std::less<>> const core_units{
      "DMA1", "DMA2", "EXTI", "RCC", "PWR", "SYSCFG", "RI", "DBGMCU", "USB", "USB_SRAM", "FLASH"};
  names_by_unit by_unit;
  for(auto const& unit : std::span{l152re().peripherals}.last(40))
  {
    if(not core_units.contains(unit.name))
    {
      by_unit[unit.name] = {"usart1_tx_dma"};
    }
  }
  auto const f = parked(l152re(), firmware, with_requesters(l152re(), by_unit));
  fill(f, 2);
  program(f, dma1, 4, word_sizes | minc | pinc | en, from, to, 2);
  std::vector<requester*> probes;
  for(auto const& unit : std::span{l152re().peripherals}.last(40))
  {
    if(auto* const found = f.model_of<requester>(unit.name))
    {
      found->set("usart1_tx_dma", true);
      probes.push_back(found);
    }
  }
  REQUIRE(probes.size() > 1);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  for(auto* const found : probes)
  {
    CHECK(found->acks == std::vector<std::string>{"usart1_tx_dma"});
  }
}

TEST_CASE("RM0038 11.3.7: a requester in the first peer slot is told")
{
  auto const& first = l152re().peripherals.front().name;
  REQUIRE(first != "DMA1");
  REQUIRE(first != "DMA2");
  names_by_unit by_unit;
  by_unit[first] = {"usart1_tx_dma"};
  auto const f   = parked(l152re(), firmware, with_requesters(l152re(), by_unit));
  fill(f, 2);
  program(f, dma1, 4, word_sizes | minc | pinc | en, from, to, 2);
  auto& probe = peer(f, first);
  probe.set("usart1_tx_dma", true);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  CHECK(probe.acks == std::vector<std::string>{"usart1_tx_dma"});
}

TEST_CASE(
    "RM0038 11.3.3: a circular peripheral-to-memory channel reloads, raises HT and TC and tells "
    "EOT_1 and the count end each pass, a memory-to-memory copy tells neither")
{
  constexpr address       usart1_dr = 0x4001'3804;
  constexpr std::uint32_t irq       = 15;
  constexpr address       ispr0     = 0xE000'E200;
  names_by_unit const     by_unit{
      {"USART1", {"usart1_rx_dma"}}
  };
  auto const f = parked(
      l152re(), firmware,
      with_requesters(
          l152re(), by_unit,
          {
              {"USART1", {"DR"}}
  }));
  auto& p       = peer(f, "USART1");
  p.drop_on_ack = false;
  write(f, to, 0);
  write(f, to + 4, 0);
  auto const t0 = now_ps(f);
  program(f, dma1, 5, circ | minc | htie | tcie | en, usart1_dr, to, 4);
  p.set("usart1_rx_dma", true);
  idle_to(f, t0 + 4 * latency_ps + latency_ps / 2);
  CHECK(read(f, dma1 + cndtr(5)) == 4);
  CHECK(flags(read(f, dma1 + isr), 5) == (gif | tcif | htif));
  CHECK((read(f, ispr0) >> irq & 1u) == 1);
  CHECK(read(f, to) == 0x0302'0100u);
  CHECK(p.last_but_ones.size() == 1);
  write(f, dma1 + ifcr, gif << 16);
  write(f, 0xE000'E280, 1u << irq);
  idle_to(f, t0 + 6 * latency_ps + latency_ps / 2);
  CHECK(read(f, dma1 + cndtr(5)) == 2);
  CHECK(flags(read(f, dma1 + isr), 5) == (gif | htif));
  CHECK((read(f, ispr0) >> irq & 1u) == 1);
  CHECK(read(f, to) == 0x0302'0504u);
  idle_to(f, t0 + 8 * latency_ps + latency_ps / 2);
  CHECK(read(f, dma1 + cndtr(5)) == 4);
  CHECK(flags(read(f, dma1 + isr), 5) == (gif | tcif | htif));
  CHECK(read(f, to) == 0x0706'0504u);
  CHECK(read(f, to + 4) == 0);
  CHECK(p.completions.empty());
  CHECK(p.acks.size() == 8);
  CHECK(p.last_but_ones == std::vector<std::string>{"usart1_rx_dma", "usart1_rx_dma"});
  CHECK(p.count_ends == std::vector<std::string>{"usart1_rx_dma", "usart1_rx_dma"});
  write(f, dma1 + ccr(5), 0);
  p.set("usart1_rx_dma", false);
  write(f, dma1 + ifcr, 0x0FFF'FFFFu);
  write(f, from, 0x1234'5678u);
  program(f, dma1, 5, mem2mem | minc | pinc | en, from, to + 8, 2);
  idle_to(f, now_ps(f) + 4 * latency_ps);
  CHECK(flags(read(f, dma1 + isr), 5) == (gif | tcif | htif));
  CHECK(read(f, to + 8) == 0x5678u);
  CHECK(p.count_ends.size() == 2);
  CHECK(p.last_but_ones.size() == 2);
}

TEST_CASE(
    "RM0038 11.3.4: a byte or halfword DMA write into APB is a word write with the item on "
    "every lane, into AHB it stays narrow")
{
  using write_seen                  = requester::bus_write;
  constexpr address       tim5_arr  = 0x4000'0C2C;
  constexpr address       rtc_bkp0r = 0x4000'2850;
  constexpr address       gpioa_odr = 0x4002'0014;
  constexpr std::uint32_t to_periph = mem2mem | (1u << 4);
  constexpr std::uint32_t halves    = (1u << 10) | (1u << 8);
  auto const              f         = parked(
      l152re(), firmware,
      with_requesters(
          l152re(),
          {
              {"TIM5", {}},
              {"RTC",  {}}
  },
          {{"TIM5", {"ARR"}}, {"RTC", {"BKP0R"}}}));
  auto&      tim5 = peer(f, "TIM5");
  auto&      rtc  = peer(f, "RTC");
  auto const copy = [&](std::uint32_t sizes, address at, std::uint32_t item) {
    write(f, from, item);
    write(f, dma1 + ifcr, 0x0FFF'FFFFu);
    program(f, dma1, 1, to_periph | sizes | en, at, from, 1);
    idle_to(f, now_ps(f) + 2 * latency_ps);
    CHECK(flags(read(f, dma1 + isr), 1) == (gif | tcif | htif));
    write(f, dma1 + ccr(1), 0);
  };
  copy(halves, tim5_arr, 0xABCD);
  copy(halves, tim5_arr + 2, 0x1234);
  CHECK(
      tim5.writes
      == std::vector<write_seen>{
          {.offset = 0x2C, .size = 4, .value = 0xABCD'ABCDu},
          {.offset = 0x2C, .size = 4, .value = 0x1234'1234u}
  });
  copy(0, rtc_bkp0r + 1, 0xB0);
  copy(0, rtc_bkp0r + 3, 0x5C);
  CHECK(
      rtc.writes
      == std::vector<write_seen>{
          {.offset = 0x50, .size = 4, .value = 0xB0B0'B0B0u},
          {.offset = 0x50, .size = 4, .value = 0x5C5C'5C5Cu}
  });
  write(f, gpioa_odr, 0);
  copy(0, gpioa_odr + 1, 0xB0);
  CHECK(read(f, gpioa_odr) == 0x0000'B000u);
}

TEST_CASE("an L1 DMA takes a firmware byte write to CNDTR1 with no warning, under strict too")
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const        f        = parked(l152re(), firmware, std::move(options));
  log_capture const logs{f};
  CHECK(f.target->model("DMA1")
            ->on_write(
                f.target->peripheral("DMA1"), cndtr(1), 1, 0, 0x12, {.pc = 0, .now = icount{0}})
            .has_value());
  CHECK(read(f, dma1 + cndtr(1)) == 0x12u);
  CHECK(logs.records.empty());
}
