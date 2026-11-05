#include "gpio_peers.hpp"
#include "gpio_rows.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::testing::l152rc;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;

constexpr auto          firmware   = std::string_view{"l152xe_l1_gpio_ll"};
constexpr std::uint32_t gpioa      = 0x4002'0000;
constexpr std::uint32_t gpioa_odr  = gpioa + 0x14;
constexpr std::uint32_t gpioa_brr  = gpioa + 0x28;
constexpr std::uint32_t gpioa_mode = gpioa + 0x00;

auto port(stm32_fixture const& f, std::string_view name) -> stm32::gpio_port&
{
  auto* const found = f.model_of<stm32::gpio_port>(name);
  REQUIRE(found != nullptr);
  return *found;
}

auto parked(part_description const& desc) -> stm32_fixture
{
  stm32_fixture f{desc, firmware};
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  return f;
}

auto read(stm32_fixture const& f, std::uint32_t at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void write(stm32_fixture const& f, std::uint32_t at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}

void cpu_store(stm32_fixture const& f, std::uint32_t at, std::uint32_t value)
{
  constexpr std::uint32_t code = 0x2000'0100;
  REQUIRE(f.target->mem().write(code, 2, 0x6001).has_value());
  auto const set = [&](std::string_view name, std::uint64_t to) {
    f.target->regs().write(*f.target->regs().find(name), to);
  };
  set("r0", at);
  set("r1", value);
  set("pc", code | 1u);
  REQUIRE_FALSE(f.target->run_slice({.max_instructions = icount{1}}).stopped.has_value());
}

auto with_gpio(part_description const& desc, stm32::gpio_variant variant) -> core::machine_options
{
  stm32::register_builtin_models();
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(stm32::board{}, desc);
  options.models.insert_or_assign("GPIOA", stm32::gpio_factory(variant));
  return options;
}

void attaches(part_description const& desc, stm32::gpio_variant variant)
{
  core::machine{desc, popcorn::arch::armv8m::factory(), with_gpio(desc, variant)};
}
}

TEST_CASE("RM0038 7.4: the LL sequence drives PA5 through BSRR and BRR, AF7, a PC13 pull and LCKR")
{
  stm32_fixture f{l152re(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{1, 0, 1, 1, 0, 7, 7, 0x700, 0x700, 1, 1, 1, 1, 1});
  CHECK(port(f, "GPIOA").mode(2) == stm32::pin_mode::alternate);
  CHECK(port(f, "GPIOA").alternate(2) == 7);
  CHECK(port(f, "GPIOA").alternate(10) == 7);
  CHECK(port(f, "GPIOC").level(13));
  port(f, "GPIOC").drive(13, false);
  CHECK(f.field("GPIOC", "IDR", "IDR13") == 0);
  CHECK(port(f, "GPIOA").mode(5) == stm32::pin_mode::output);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(port(f, "GPIOA").mode(5) == stm32::pin_mode::input);
  CHECK(f.field("GPIOA", "LCKR", "LCKK") == 0);
}

TEST_CASE("RM0038 7.4.11: a Cat.5 BRR store clears ODR, on Cat.3 BRR is absent and the pin keeps")
{
  for(auto const* desc : {&l152re(), &l152rc()})
  {
    CAPTURE(desc->part);
    auto const f       = parked(*desc);
    auto const has_brr = desc == &l152re();
    popcorn::testing::enable_clocks(
        *f.target, {
                       {"AHBENR", "GPIOAEN"}
    });
    CHECK(
        (popcorn::find_register(f.target->peripheral("GPIOA").description(), "BRR") != nullptr)
        == has_brr);
    write(f, gpioa_mode, (read(f, gpioa_mode) & ~(3u << 10)) | (1u << 10));
    write(f, gpioa_odr, 1u << 5);
    REQUIRE(port(f, "GPIOA").level(5));
    cpu_store(f, gpioa_brr, 1u << 5);
    CHECK(((read(f, gpioa_odr) >> 5) & 1u) == (has_brr ? 0u : 1u));
    CHECK(port(f, "GPIOA").level(5) == not has_brr);
  }
}

TEST_CASE("the L1 GPIO rows serve Cat.3 and Cat.5, a variant the description disagrees with throws")
{
  attaches(l152rc(), stm32::l1_gpio_cat13);
  attaches(l152re(), stm32::l1_gpio_cat46);
  CHECK_THROWS_AS(attaches(l152rc(), stm32::l1_gpio_cat46), popcorn::config_error);
  CHECK_THROWS_AS(attaches(l152re(), stm32::l1_gpio_cat13), popcorn::config_error);
  auto secure   = stm32::l1_gpio_cat46;
  secure.secure = true;
  CHECK_THROWS_AS(attaches(l152re(), secure), popcorn::config_error);
  auto slew = stm32::l1_gpio_cat46;
  slew.slew = true;
  CHECK_THROWS_AS(attaches(l152re(), slew), popcorn::config_error);
  auto u5_names  = stm32::l1_gpio_cat46;
  u5_names.names = stm32::gpio_field_names{};
  CHECK_THROWS_AS(attaches(l152re(), u5_names), popcorn::config_error);
}

TEST_CASE("an L1 port, without GPIOx_ASCR, keeps the analog switch of every pin closed")
{
  auto const  f     = parked(l152re());
  auto* const ports = f.model_of<stm32::analog_switch_source>("GPIOA");
  REQUIRE(ports != nullptr);
  for(std::uint8_t pin = 0; pin < 16; ++pin)
  {
    CHECK(ports->switch_closed(pin));
  }
}
