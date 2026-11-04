#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/rng.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::periph::stm32::rng_word_at;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;

constexpr auto             firmware = std::string_view{"l476_l4_smoke"};
constexpr address          stub     = 0x2000'8000;
constexpr std::uint32_t    rng_base = 0x5006'0800;
constexpr std::uint32_t    rng_irq  = 80;
constexpr std::uint32_t    cr       = 0x00;
constexpr std::uint32_t    sr       = 0x04;
constexpr std::uint32_t    dr       = 0x08;
constexpr std::uint32_t    ced      = 1u << 5;
constexpr std::uint32_t    ie       = 1u << 3;
constexpr std::uint32_t    rngen    = 1u << 2;
constexpr std::uint32_t    drdy     = 1u << 0;
constexpr std::uint32_t    cecs     = 1u << 1;
constexpr std::uint32_t    secs     = 1u << 2;
constexpr std::uint32_t    ceis     = 1u << 5;
constexpr std::uint32_t    seis     = 1u << 6;
constexpr std::uint64_t    us       = 1'000'000;
constexpr std::uint64_t    first_ps = 11'500'000;
constexpr std::uint64_t    word_ps  = 10'500'000;
constexpr std::uint32_t    msi      = 3;
constexpr std::uint32_t    pll_msi  = 1;
constexpr std::uint32_t    pll_r_en = 1u << 24;
constexpr std::uint32_t    pll_sw   = 3;
constexpr std::string_view config_fields[]{"CONFIGLOCK",  "CONDRST", "RNG_CONFIG1", "CLKDIV",
                                           "RNG_CONFIG2", "NISTC",   "RNG_CONFIG3", "ARDIS"};

auto options(bool strict) -> core::machine_options
{
  auto out               = popcorn::testing::stm32_options();
  out.strict_peripherals = strict;
  return out;
}

auto rng_of(part_description& desc) -> popcorn::peripheral_desc&
{
  auto found =
      std::ranges::find(desc.peripherals, std::string_view{"RNG"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  return *found;
}

auto cr_of(popcorn::peripheral_desc& rng) -> popcorn::register_desc&
{
  auto found =
      std::ranges::find(rng.registers, std::string_view{"CR"}, &popcorn::register_desc::name);
  REQUIRE(found != rng.registers.end());
  return *found;
}

struct rig
{
  explicit rig(bool strict = false, part_description desc = l476rc())
      : f{std::move(desc), firmware, options(strict)}
  {
    REQUIRE(f.target->mem().write(stub, 2, 0xE7FE).has_value());
    f.set("pc", stub | 1u);
  }

  [[nodiscard]] auto read(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(rng_base + offset, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::uint32_t offset, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(rng_base + offset, 4, value).has_value());
  }

  [[nodiscard]] auto cpu_dr() const -> std::uint32_t
  {
    auto* const model = f.target->model("RNG");
    REQUIRE(model != nullptr);
    auto const value = model->on_read(
        f.target->peripheral("RNG"), dr, 4,
        {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  void
      set(std::string_view peripheral,
          std::string_view reg,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = ((std::uint32_t{1} << bits->width) - 1u) << bits->lsb;
    auto const old  = f.target->mem().read(desc.base + found->offset, 4);
    REQUIRE(old.has_value());
    auto const word = (static_cast<std::uint32_t>(*old) & ~mask) | ((value << bits->lsb) & mask);
    REQUIRE(f.target->mem().write(desc.base + found->offset, 4, word).has_value());
  }

  void unpend() const
  {
    REQUIRE(f.target->mem()
                .write(0xE000'E280u + 4 * (rng_irq / 32), 4, 1u << (rng_irq % 32))
                .has_value());
    REQUIRE_FALSE(f.pending(rng_irq));
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.now().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(ps >= now());
    if(ps > now())
    {
      REQUIRE(
          f.run_to_stop({.max_instructions = icount{10'000'000}, .until_time = vtime{ps}}).reason
          == stop_reason::time);
    }
  }

  [[nodiscard]] auto due(std::uint64_t ps) const -> bool
  {
    auto const pending = f.target->snapshot().data().events.pending;
    return std::ranges::contains(pending, ps, &core::event::due_ps);
  }

  void clocks() const
  {
    set("RCC", "CCIPR", "CLK48SEL", msi);
    set("RCC", "AHB2ENR", "RNGEN", 1);
  }

  void hclk_from_pll(std::uint32_t n) const
  {
    set("FLASH", "ACR", "LATENCY", 4);
    REQUIRE(
        f.target->mem()
            .write(
                f.target->peripheral("RCC").description().base
                    + popcorn::find_register(f.target->peripheral("RCC").description(), "PLLCFGR")
                          ->offset,
                4, pll_msi | n << 8 | pll_r_en)
            .has_value());
    set("RCC", "CR", "PLLON", 1);
    run_until(now() + 200 * us);
    REQUIRE(f.field("RCC", "CR", "PLLRDY") == 1);
    set("RCC", "CFGR", "SW", pll_sw);
    REQUIRE(f.target->virtual_clock().core_hz() == n * 2'000'000);
  }

  [[nodiscard]] auto enabled(std::uint32_t extra = 0) const -> std::uint64_t
  {
    auto const at = now();
    write(cr, rngen | extra);
    return at;
  }

  [[nodiscard]] auto source() const -> popcorn::periph::stm32::entropy_source&
  {
    auto* const found =
        dynamic_cast<popcorn::periph::stm32::entropy_source*>(f.target->model("RNG"));
    REQUIRE(found != nullptr);
    return *found;
  }

  stm32_fixture f;
};
}

TEST_CASE("RM0351 27.7.4: the L4 RNG resets CR SR and DR to 0 and CR keeps only IE RNGEN and CED")
{
  rig const r{true};
  CHECK(r.read(cr) == 0);
  CHECK(r.read(sr) == 0);
  CHECK(r.read(dr) == 0);
  r.write(cr, 0xFFFF'FFFFu & ~rngen);
  CHECK(r.read(cr) == (ie | ced));
  r.write(cr, 0);
  r.write(cr, 0xFFFF'FFFFu);
  CHECK(r.read(cr) == (ie | rngen | ced));
}

TEST_CASE(
    "RM0351 27.5: the first word comes 46 RNG clock cycles after RNGEN 1, "
    "the next 42 after the read")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled();
  CHECK(r.due(start + first_ps));
  r.run_until(start + first_ps - 300'000);
  CHECK((r.read(sr) & drdy) == 0);
  CHECK(r.read(dr) == 0);
  r.run_until(start + first_ps + 300'000);
  CHECK(r.read(sr) == drdy);
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  CHECK(r.read(sr) == 0);
  auto const read_at = r.now();
  CHECK(r.due(read_at + word_ps));
  r.run_until(read_at + word_ps + 300'000);
  CHECK(r.read(sr) == drdy);
  CHECK(r.cpu_dr() == rng_word_at(0, 1));
}

TEST_CASE("RM0351 27.3.3: DR holds one word and the next starts only once DR is read")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled();
  r.run_until(start + first_ps + 20 * word_ps);
  CHECK(r.read(sr) == drdy);
  CHECK(r.source().drawn() == 1);
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  CHECK(r.read(sr) == 0);
  CHECK(r.cpu_dr() == 0);
  r.run_until(r.now() + word_ps + 300'000);
  CHECK(r.cpu_dr() == rng_word_at(0, 1));
}

TEST_CASE("RM0351 27.4: the RNG line on IRQ 80 follows IE RNGEN and DRDY")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled(ie);
  r.run_until(start + first_ps - 300'000);
  CHECK_FALSE(r.f.pending(rng_irq));
  r.run_until(start + first_ps + 300'000);
  CHECK(r.f.pending(rng_irq));
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  r.unpend();
  r.run_until(r.now() + word_ps + 300'000);
  CHECK(r.read(sr) == drdy);
  CHECK(r.f.pending(rng_irq));
  r.write(cr, rngen);
  r.unpend();
  r.run_until(r.now() + 300'000);
  CHECK_FALSE(r.f.pending(rng_irq));
  r.write(cr, ie | rngen);
  CHECK(r.f.pending(rng_irq));
  r.write(cr, ie);
  r.unpend();
  r.run_until(r.now() + 300'000);
  CHECK_FALSE(r.f.pending(rng_irq));
  CHECK(r.read(sr) == drdy);
}

TEST_CASE(
    "RM0351 27.3.7 as corrected by ES0250 2.15.1: at HCLK 80 MHz a 4 MHz RNG clock "
    "sets CECS and CEIS and words keep coming")
{
  rig const r;
  r.clocks();
  r.hclk_from_pll(40);
  auto const start = r.enabled();
  CHECK(r.read(sr) == (cecs | ceis));
  r.run_until(start + first_ps + 300'000);
  CHECK(r.read(sr) == (drdy | cecs | ceis));
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  r.write(sr, 0);
  CHECK(r.read(sr) == cecs);
  r.run_until(r.now() + word_ps + 300'000);
  CHECK(r.read(sr) == (drdy | cecs));
  CHECK(r.cpu_dr() == rng_word_at(0, 1));
}

TEST_CASE(
    "RM0351 27.3.3: at HCLK 64 MHz a 4 MHz RNG clock is 16 times slower and sets no clock error")
{
  rig const r;
  r.clocks();
  r.hclk_from_pll(32);
  auto const start = r.enabled();
  CHECK(r.read(sr) == 0);
  r.run_until(start + first_ps + 300'000);
  CHECK(r.read(sr) == drdy);
}

TEST_CASE("RM0351 27.7.1: with CED 1 a slow RNG clock leaves CECS and CEIS at 0")
{
  rig const r;
  r.clocks();
  r.hclk_from_pll(40);
  r.write(cr, ced);
  auto const start = r.enabled(ced);
  CHECK(r.read(sr) == 0);
  r.run_until(start + first_ps + 300'000);
  CHECK(r.read(sr) == drdy);
  r.write(cr, 0);
  r.write(cr, rngen);
  CHECK(r.read(sr) == (drdy | cecs | ceis));
}

TEST_CASE("RM0351 27.7.1: a CED write with RNGEN 1 before and in it keeps CED, inferred")
{
  rig const r;
  r.clocks();
  r.hclk_from_pll(40);
  static_cast<void>(r.enabled());
  r.write(sr, 0);
  r.write(cr, rngen | ced);
  CHECK(r.read(cr) == rngen);
  CHECK(r.read(sr) == cecs);
  r.write(cr, ced);
  CHECK(r.read(cr) == ced);
  r.write(cr, rngen | ced);
  r.write(cr, rngen);
  CHECK(r.read(cr) == (rngen | ced));
  CHECK((r.read(sr) & cecs) == 0);
}

TEST_CASE(
    "RM0351 27.3.7: a seed error sets SECS and SEIS and stops the words, "
    "27.3.5 and 27.7.3: DR reads 0")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled();
  r.run_until(start + first_ps + 300'000);
  REQUIRE(r.read(sr) == drdy);
  r.source().set_seed_fault(true);
  CHECK(r.read(sr) == (secs | seis));
  CHECK(r.read(dr) == 0);
  CHECK(r.cpu_dr() == 0);
  r.run_until(r.now() + 4 * word_ps);
  CHECK(r.read(sr) == (secs | seis));
  r.source().set_seed_fault(false);
  r.run_until(r.now() + 4 * word_ps);
  CHECK(r.read(sr) == (secs | seis));
}

TEST_CASE(
    "RM0351 27.3.7: SEIS cleared and RNGEN cleared and set recover from a seed error, "
    "SECS clearing at RNGEN 0 inferred")
{
  rig const r;
  r.clocks();
  static_cast<void>(r.enabled());
  r.source().set_seed_fault(true);
  REQUIRE(r.read(sr) == (secs | seis));
  r.write(sr, 0);
  CHECK(r.read(sr) == secs);
  r.write(cr, 0);
  CHECK(r.read(sr) == 0);
  r.write(cr, rngen);
  CHECK(r.read(sr) == (secs | seis));
  r.source().set_seed_fault(false);
  r.write(sr, 0);
  r.write(cr, 0);
  auto const start = r.enabled();
  CHECK(r.read(sr) == 0);
  CHECK(r.due(start + first_ps));
  r.run_until(start + first_ps + 300'000);
  CHECK(r.read(sr) == drdy);
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
}

TEST_CASE(
    "RM0351 27.3.8: RNGEN 0 keeps the DR word and ends the word in progress, "
    "RNGEN 1 waits 46 cycles again")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled();
  r.run_until(start + first_ps + 300'000);
  r.write(cr, 0);
  CHECK(r.read(sr) == drdy);
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  r.write(cr, rngen);
  auto const word_start = r.now();
  CHECK(r.due(word_start + first_ps));
  r.run_until(word_start + first_ps / 2);
  r.write(cr, 0);
  CHECK_FALSE(r.due(word_start + first_ps));
  auto const again = r.enabled();
  CHECK(r.due(again + first_ps));
  r.run_until(again + first_ps + 300'000);
  CHECK(r.cpu_dr() == rng_word_at(0, 1));
}

TEST_CASE(
    "RM0351 27.3.8: a re-enable with a word in DR starts the 46 cycles "
    "at the read that empties it, inferred")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled();
  r.run_until(start + first_ps + 300'000);
  r.write(cr, 0);
  r.write(cr, rngen);
  r.run_until(r.now() + 4 * first_ps);
  CHECK(r.source().drawn() == 1);
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  auto const read_at = r.now();
  CHECK(r.due(read_at + first_ps));
  r.run_until(read_at + first_ps + 300'000);
  CHECK(r.cpu_dr() == rng_word_at(0, 1));
}

TEST_CASE("a snapshot mid-word restores the L4 RNG with the same word at the same time")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled();
  r.run_until(start + first_ps / 2);
  auto const saved = r.f.target->snapshot();
  r.run_until(start + first_ps + 300'000);
  auto const word = r.cpu_dr();
  CHECK(word == rng_word_at(0, 0));
  rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.read(sr) == 0);
  CHECK(other.read(cr) == rngen);
  CHECK(other.due(start + first_ps));
  other.run_until(start + first_ps - 300'000);
  CHECK(other.read(sr) == 0);
  other.run_until(start + first_ps + 300'000);
  CHECK(other.cpu_dr() == word);
}

TEST_CASE("RM0351 6.4.11: RNGRST returns the L4 RNG to its reset values")
{
  rig const r;
  r.clocks();
  auto const start = r.enabled(ie);
  r.run_until(start + first_ps + 300'000);
  REQUIRE(r.read(sr) == drdy);
  r.set("RCC", "AHB2RSTR", "RNGRST", 1);
  r.set("RCC", "AHB2RSTR", "RNGRST", 0);
  CHECK(r.read(cr) == 0);
  CHECK(r.read(sr) == 0);
  CHECK(r.read(dr) == 0);
  CHECK_FALSE(r.due(start + first_ps + word_ps));
}

TEST_CASE("checklist 39: the L4 RNG row refuses a description with a configuration field or NSCR")
{
  for(auto const name : config_fields)
  {
    CAPTURE(name);
    auto desc = l476rc();
    cr_of(rng_of(desc)).fields.push_back({.name = std::string{name}, .lsb = 30, .width = 1});
    CHECK_THROWS_WITH_AS(
        (rig{false, desc}), doctest::Contains(std::string{name}), popcorn::config_error);
  }
  auto  desc  = l476rc();
  auto& rng   = rng_of(desc);
  auto  nscr  = cr_of(rng);
  nscr.name   = "NSCR";
  nscr.offset = 0x0C;
  nscr.fields.clear();
  rng.registers.push_back(nscr);
  CHECK_THROWS_WITH_AS((rig{false, desc}), doctest::Contains("NSCR"), popcorn::config_error);
}

TEST_CASE("checklist 39: the L4 RNG row needs CED, the RM0351 27.7.1 patch field")
{
  auto  desc   = l476rc();
  auto& fields = cr_of(rng_of(desc)).fields;
  REQUIRE(std::erase_if(fields, [](auto const& one) { return one.name == "CED"; }) == 1);
  CHECK_THROWS_WITH_AS((rig{false, desc}), doctest::Contains("CED"), popcorn::config_error);
}
