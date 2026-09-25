#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/register_file.hpp>

#include "../../../native/src/periph/register_layer.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <format>
#include <meta>
#include <string>
#include <vector>

namespace
{
namespace periph = popcorn::periph;
using popcorn::access;
using popcorn::read_effect;
using popcorn::write_effect;

auto field(
    std::string  name,
    std::uint8_t lsb,
    std::uint8_t width,
    access       acc,
    write_effect on_write = write_effect::modify,
    read_effect  on_read  = read_effect::none) -> popcorn::field_desc
{
  return {
      .name     = std::move(name),
      .lsb      = lsb,
      .width    = width,
      .acc      = acc,
      .on_write = on_write,
      .on_read  = on_read};
}

auto reg(
    std::string                      name,
    std::uint32_t                    offset,
    access                           acc,
    std::vector<popcorn::field_desc> fields,
    std::uint32_t                    reset = 0,
    std::uint8_t                     width = 32) -> popcorn::register_desc
{
  return {
      .name         = std::move(name),
      .offset       = offset,
      .width        = width,
      .reset        = reset,
      .reset_mask   = 0xffff'ffff,
      .acc          = acc,
      .fields       = std::move(fields),
      .alternate_of = std::nullopt};
}

auto unit(
    std::vector<popcorn::register_desc> registers,
    std::vector<popcorn::cross_rule>    crosses = {}) -> popcorn::peripheral_desc
{
  return {
      .name        = "UNIT",
      .ip          = "test",
      .ip_version  = "v1",
      .base        = 0x4000'0000,
      .size        = 0x40,
      .registers   = std::move(registers),
      .cross_rules = std::move(crosses),
      .irqs        = {},
      .alias_of    = std::nullopt};
}

struct bench
{
  popcorn::peripheral_desc desc;
  periph::register_file    regs{desc};
  periph::register_layout  layout{desc};
  periph::layer_state      state{};

  explicit bench(popcorn::peripheral_desc peripheral) : desc{std::move(peripheral)}
  {
    std::vector<std::uint8_t> const none(layout.words(), 0);
    periph::layer_reset(layout, regs, state, none);
  }
};

auto expected_bit(write_effect effect, bool old, bool written) -> bool
{
  switch(effect)
  {
    case write_effect::modify:
      return written;
    case write_effect::one_to_clear:
      return written ? false : old;
    case write_effect::one_to_set:
      return written ? true : old;
    case write_effect::one_to_toggle:
      return written ? not old : old;
    case write_effect::zero_to_clear:
      return written ? old : false;
    case write_effect::zero_to_set:
      return written ? old : true;
    case write_effect::zero_to_toggle:
      return written ? old : not old;
    case write_effect::clear:
      return false;
    case write_effect::set:
      return true;
  }
  return old;
}

constexpr std::array<std::uint32_t, 4> patterns{0x0000'0000, 0xffff'ffff, 0x5555'5555, 0xaaaa'aaaa};
}

TEST_CASE("every write effect matches a bit by bit reference")
{
  template for(constexpr auto e :
               std::define_static_array(std::meta::enumerators_of(^^popcorn::write_effect)))
  {
    constexpr write_effect effect = [:e:];
    for(auto const [lsb, width] : std::array<std::array<std::uint8_t, 2>, 4>{
            {{0, 1}, {0, 4}, {28, 1}, {28, 4}}
    })
    {
      for(auto const before : patterns)
      {
        for(auto const written : patterns)
        {
          bench b{unit({reg(
              "R", 0, access::read_only, {field("F", lsb, width, access::read_write, effect)})})};
          b.regs.poke(0, before);
          periph::layer_write(b.layout, b.regs, b.state, 0, 4, written);
          std::uint32_t expected = before;
          for(std::uint32_t bit = lsb; bit < lsb + width; ++bit)
          {
            auto const value =
                expected_bit(effect, (before >> bit & 1u) != 0, (written >> bit & 1u) != 0);
            expected = value ? expected | (1u << bit) : expected & ~(1u << bit);
          }
          INFO(
              std::format(
                  "{} lsb {} width {} before {:#x} written {:#x}", effect, lsb, width, before,
                  written));
          CHECK(b.regs.peek(0) == expected);
        }
      }
    }
  }
}

TEST_CASE("read effects apply after the read returns the old value")
{
  bench b{unit(
      {reg("C", 0, access::read_write,
           {field("F", 0, 8, access::read_write, write_effect::modify, read_effect::clear)}),
       reg("S", 4, access::read_write,
           {field("F", 0, 8, access::read_write, write_effect::modify, read_effect::set)}),
       reg("M", 8, access::read_write,
           {field("F", 0, 8, access::read_write, write_effect::modify, read_effect::modify)}),
       reg("N", 12, access::read_write, {field("F", 0, 8, access::read_write)})})};
  for(std::uint32_t const offset : {0u, 4u, 8u, 12u})
  {
    b.regs.poke(offset, 0x0000'005a);
  }
  CHECK(periph::layer_read(b.layout, b.regs, 0, 4) == 0x5a);
  CHECK(b.regs.peek(0) == 0);
  CHECK(periph::layer_read(b.layout, b.regs, 4, 4) == 0x5a);
  CHECK(b.regs.peek(4) == 0xff);
  CHECK(periph::layer_read(b.layout, b.regs, 8, 4) == 0x5a);
  CHECK(b.regs.peek(8) == 0x5a);
  CHECK(periph::layer_read(b.layout, b.regs, 12, 4) == 0x5a);
  CHECK(b.regs.peek(12) == 0x5a);
}

TEST_CASE("access kinds gate reads and writes, write-once locks until reset")
{
  bench b{unit(
      {reg("RO", 0, access::read_only, {}, 0x1234), reg("WO", 4, access::write_only, {}),
       reg("ONCE", 8, access::write_once, {}), reg("RONCE", 12, access::read_write_once, {})})};
  periph::layer_write(b.layout, b.regs, b.state, 0, 4, 0xffff'ffff);
  CHECK(periph::layer_read(b.layout, b.regs, 0, 4) == 0x1234);
  periph::layer_write(b.layout, b.regs, b.state, 4, 4, 0x77);
  CHECK(b.regs.peek(4) == 0x77);
  CHECK(periph::layer_read(b.layout, b.regs, 4, 4) == 0);
  periph::layer_write(b.layout, b.regs, b.state, 8, 4, 1);
  periph::layer_write(b.layout, b.regs, b.state, 8, 4, 2);
  CHECK(b.regs.peek(8) == 1);
  CHECK(periph::layer_read(b.layout, b.regs, 8, 4) == 0);
  periph::layer_write(b.layout, b.regs, b.state, 12, 4, 3);
  periph::layer_write(b.layout, b.regs, b.state, 12, 4, 4);
  CHECK(periph::layer_read(b.layout, b.regs, 12, 4) == 3);
  CHECK(b.state.write_locked.front() == 0b1100);
  std::vector<std::uint8_t> const none(b.layout.words(), 0);
  periph::layer_reset(b.layout, b.regs, b.state, none);
  CHECK(b.state.write_locked.front() == 0);
  periph::layer_write(b.layout, b.regs, b.state, 12, 4, 5);
  CHECK(periph::layer_read(b.layout, b.regs, 12, 4) == 5);
}

TEST_CASE("sub-word accesses touch their lane only")
{
  bench b{unit({reg("W", 0, access::read_write, {}), reg("V", 4, access::read_write, {})})};
  b.regs.poke(4, 0x1122'3344);
  periph::layer_write(b.layout, b.regs, b.state, 5, 1, 0xab);
  CHECK(b.regs.peek(4) == 0x1122'ab44);
  b.regs.poke(0, 0xcafe'f00d);
  CHECK(periph::layer_read(b.layout, b.regs, 2, 2) == 0xcafe);
  CHECK(periph::layer_read(b.layout, b.regs, 1, 1) == 0xf0);
}

TEST_CASE("a cross rule clears its target on a write of 1 only")
{
  bench b{
      unit(
          {reg("ISR", 0, access::read_only, {field("TC", 6, 1, access::read_only)}, 0xc0),
            reg("ICR", 4, access::write_only,
            {field("TCCF", 6, 1, access::write_only, write_effect::one_to_clear)})},
          {{.reg          = "ICR",
            .field        = "TCCF",
            .target_reg   = "ISR",
            .target_field = "TC",
            .effect       = popcorn::cross_effect::clear}}
          )
  };
  CHECK(b.regs.peek(0) == 0xc0);
  periph::layer_write(b.layout, b.regs, b.state, 4, 4, 0);
  CHECK(b.regs.peek(0) == 0xc0);
  periph::layer_write(b.layout, b.regs, b.state, 4, 4, 1u << 6);
  CHECK(b.regs.peek(0) == 0x80);
}

TEST_CASE("an alternate register shares its primary's storage and reset")
{
  auto alternate = reg("ALT", 0, access::read_write, {field("LOW", 0, 16, access::read_write)});
  alternate.alternate_of = "MAIN";
  alternate.reset        = 0xffff;
  bench b{unit({reg("MAIN", 0, access::read_write, {}, 0x1234'5678), alternate})};
  CHECK(b.regs.peek(0) == 0x1234'5678);
  CHECK(b.layout.word_of("ALT") == b.layout.word_of("MAIN"));
  periph::layer_write(b.layout, b.regs, b.state, 0, 4, 0xdead'beef);
  CHECK(periph::layer_read(b.layout, b.regs, 0, 4) == 0xdead'beef);
}

TEST_CASE("reset applies reset_mask and leaves claimed words to the model")
{
  auto partial       = reg("P", 0, access::read_write, {}, 0xffff'ffff);
  partial.reset_mask = 0x0000'ff00;
  bench b{unit({partial, reg("Q", 4, access::read_write, {}, 0x42)})};
  CHECK(b.regs.peek(0) == 0x0000'ff00);
  b.regs.poke(4, 7);
  std::vector<std::uint8_t> claimed(b.layout.words(), 0);
  claimed[1] = 1;
  periph::layer_reset(b.layout, b.regs, b.state, claimed);
  CHECK(b.regs.peek(4) == 7);
}

TEST_CASE("registers and rules the flat storage cannot hold are configuration errors")
{
  CHECK_THROWS_AS(
      periph::register_layout{unit({reg("WIDE", 2, access::read_write, {}, 0, 32)})},
      popcorn::config_error);
  CHECK_THROWS_AS(
      periph::register_layout{
          unit(
              {reg("A", 0, access::read_write, {})},
              {{.reg          = "A",
                                                       .field        = "NONE",
                                                       .target_reg   = "A",
                                                       .target_field = "NONE",
                                                       .effect = popcorn::cross_effect::set}}
              )
  },
      popcorn::config_error);
}

TEST_CASE("layouts refuse duplicate names, shared offsets and conflicting alternates")
{
  CHECK_THROWS_AS(
      periph::register_layout{
          unit({reg("A", 0, access::read_write, {}), reg("A", 4, access::read_write, {})})},
      popcorn::config_error);
  CHECK_THROWS_AS(
      periph::register_layout{
          unit({reg("A", 0, access::read_write, {}), reg("B", 0, access::read_write, {})})},
      popcorn::config_error);
  auto alternate =
      reg("ALT", 0, access::read_write,
          {field("F", 0, 8, access::read_write, write_effect::one_to_clear)});
  alternate.alternate_of = "MAIN";
  CHECK_THROWS_AS(
      periph::register_layout{unit({reg("MAIN", 0, access::read_write, {}), alternate})},
      popcorn::config_error);
}

TEST_CASE("write-once locks exist only for layouts with write-once bits, per word")
{
  bench plain{unit({reg("A", 0, access::read_write, {})})};
  periph::layer_write(plain.layout, plain.regs, plain.state, 0, 4, 1);
  CHECK(plain.state.write_locked.empty());
  bench mixed{unit({reg(
      "M", 0, access::read_write,
      {field("LOW", 0, 8, access::read_write), field("HIGH", 24, 8, access::write_once)})})};
  periph::layer_write(mixed.layout, mixed.regs, mixed.state, 0, 1, 0x11);
  CHECK(mixed.state.write_locked.front() == 0);
  periph::layer_write(mixed.layout, mixed.regs, mixed.state, 3, 1, 0x22);
  CHECK(mixed.state.write_locked.front() == 1);
  periph::layer_write(mixed.layout, mixed.regs, mixed.state, 3, 1, 0x33);
  CHECK(mixed.regs.peek(0) == 0x2200'0011);
}

TEST_CASE("globs match with star and question mark")
{
  CHECK(periph::glob_match("v1*", "v1.2"));
  CHECK(periph::glob_match("*", ""));
  CHECK_FALSE(periph::glob_match("v?", "v10"));
  CHECK(periph::glob_match("v?", "v1"));
  CHECK(periph::glob_match("sci3_*_Cube", "sci3_v1_2_Cube"));
}

TEST_CASE("storage and rules of a U575-sized description")
{
  std::vector<std::uint32_t> sizes{557'056, 8192, 4096, 4096};
  sizes.resize(100, 1024);
  std::size_t storage = 0;
  std::size_t rules   = 0;
  for(auto const size : sizes)
  {
    auto peripheral = unit({});
    peripheral.size = size;
    for(std::uint32_t i = 0; i < 25; ++i)
    {
      peripheral.registers.push_back(
          reg(std::format("R{}", i), 4 * i, access::read_write,
              {field("F", 0, 4, access::read_write)}));
    }
    periph::register_layout const layout{peripheral};
    storage += std::size_t{layout.words()} * 4;
    rules += layout.footprint();
  }
  MESSAGE(
      std::format(
          "U575-sized storage {} bytes, compiled rules {} bytes, total {} bytes", storage, rules,
          storage + rules));
  CHECK(storage + rules < 4 * 1024 * 1024);
}
