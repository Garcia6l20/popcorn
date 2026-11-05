#include "../support/inert_model.hpp"
#include "gpio_peers.hpp"
#include "gpio_rows.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
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
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;

constexpr auto          firmware = std::string_view{"l476_l4_gpio_ll"};
constexpr std::uint32_t pwr_cr2  = 0x4000'7004;
constexpr std::uint32_t iosv     = 1u << 9;
constexpr std::uint32_t gpiog    = 0x4800'1800;

auto port(stm32_fixture const& f, std::string_view name) -> stm32::gpio_port&
{
  auto* const found = f.model_of<stm32::gpio_port>(name);
  REQUIRE(found != nullptr);
  return *found;
}

auto analog(stm32_fixture const& f, std::string_view name) -> stm32::analog_switch_source&
{
  auto* const found = f.model_of<stm32::analog_switch_source>(name);
  REQUIRE(found != nullptr);
  return *found;
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

auto cpu_store(stm32_fixture const& f, std::uint32_t at, std::uint32_t value) -> popcorn::stop
{
  constexpr std::uint32_t code = 0x2000'0100;
  REQUIRE(f.target->mem().write(code, 2, 0x6001).has_value());
  REQUIRE(f.target->mem().write(code + 2, 2, 0xbe00).has_value());
  f.set("r0", at);
  f.set("r1", value);
  f.set("pc", code | 1u);
  return f.run_to_stop({.max_instructions = icount{4}});
}

auto with_port_g_bonded() -> part_description
{
  auto part = l476rc();
  auto g    = std::ranges::find(
      part.peripherals, std::string_view{"GPIOG"}, &popcorn::peripheral_desc::name);
  REQUIRE(g != part.peripherals.end());
  g->bonded = 0xffff;
  return part;
}

struct rig
{
  stm32_fixture                 f;
  std::vector<core::log_record> logged{};
  core::hook_handle             sink{};

  explicit rig(
      part_description const& desc,
      core::machine_options   options = popcorn::testing::stm32_options())
      : f{desc, firmware, std::move(options)}
  {
    sink = f.target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    f.set("pc", popcorn::testing::sym(firmware, "idle"));
  }

  [[nodiscard]] auto warnings(std::string_view reg) const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(logged, [&](core::log_record const& r) {
      return r.peripheral == "GPIOG" and r.reg == reg and r.kind == core::log_kind::model_warning;
    }));
  }
};

auto strict_options() -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  return options;
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

TEST_CASE(
    "RM0351 8.4: the LL sequence drives PA5 through BSRR and BRR, AF7, a PC13 pull, ASCR and "
    "LCKR")
{
  stm32_fixture f{l476rc(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{1, 0, 1, 1, 0, 7, 7, 0x700, 0x700, 1, 1, 1, 1, 1, 1});
  CHECK(port(f, "GPIOA").mode(2) == stm32::pin_mode::alternate);
  CHECK(port(f, "GPIOA").alternate(10) == 7);
  CHECK(port(f, "GPIOC").level(13));
  CHECK(port(f, "GPIOC").mode(0) == stm32::pin_mode::analog);
  CHECK(analog(f, "GPIOC").switch_closed(0));
  CHECK_FALSE(analog(f, "GPIOC").switch_closed(1));
  CHECK_FALSE(analog(f, "GPIOA").switch_closed(0));
  CHECK(port(f, "GPIOA").mode(5) == stm32::pin_mode::output);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(port(f, "GPIOA").mode(5) == stm32::pin_mode::analog);
  CHECK(f.field("GPIOA", "LCKR", "LCKK") == 0);
  CHECK_FALSE(analog(f, "GPIOC").switch_closed(0));
}

TEST_CASE("RM0351 8.5.12: ASCR closes the switch of each pin it sets, the debugger write included")
{
  rig        r{l476rc()};
  auto const ascr = 0x4800'0800u + 0x2c;
  write(r.f, ascr, 0x8001u);
  CHECK(analog(r.f, "GPIOC").switch_closed(0));
  CHECK(analog(r.f, "GPIOC").switch_closed(15));
  CHECK_FALSE(analog(r.f, "GPIOC").switch_closed(7));
  REQUIRE(cpu_store(r.f, ascr, 0x80u).reason == stop_reason::user);
  CHECK(analog(r.f, "GPIOC").switch_closed(7));
  CHECK_FALSE(analog(r.f, "GPIOC").switch_closed(0));
  CHECK(read(r.f, ascr) == 0x80u);
  CHECK_THROWS_AS((void)analog(r.f, "GPIOC").switch_closed(16), popcorn::config_error);
}

TEST_CASE(
    "RM0351 8.5.1 and 8.5.3: every port resets MODER and OSPEEDR to its own value, port H to "
    "0x0000000F")
{
  stm32_fixture f{l476rc(), firmware};
  f.target->reset(popcorn::reset_kind::power_on);
  struct expected
  {
    std::string_view port;
    std::uint32_t    moder;
    std::uint32_t    ospeedr;
  };
  for(auto const& [name, moder, ospeedr] : {
          expected{"GPIOA", 0xabff'ffffu, 0x0c00'0000u},
          expected{"GPIOB", 0xffff'febfu, 0u          },
          expected{"GPIOC", 0xffff'ffffu, 0u          },
          expected{"GPIOD", 0xffff'ffffu, 0u          },
          expected{"GPIOE", 0xffff'ffffu, 0u          },
          expected{"GPIOF", 0xffff'ffffu, 0u          },
          expected{"GPIOG", 0xffff'ffffu, 0u          },
          expected{"GPIOH", 0x0000'000fu, 0u          },
  })
  {
    CAPTURE(name);
    auto const base = f.target->peripheral(name).description().base;
    CHECK(read(f, base) == moder);
    CHECK(read(f, base + 8) == ospeedr);
    CHECK(read(f, base + 0x2c) == 0u);
  }
}

TEST_CASE(
    "RM0351 5.4.2: a CPU write configuring PG2 to PG15 with IOSV 0 warns once, inferred, PG0, "
    "PG1, the debugger and IOSV 1 do not")
{
  rig r{with_port_g_bonded()};
  REQUIRE((read(r.f, pwr_cr2) & iosv) == 0);
  REQUIRE(cpu_store(r.f, gpiog + 0x14, 0x3u).reason == stop_reason::user);
  CHECK(r.warnings("ODR") == 0);
  write(r.f, gpiog + 0x14, 0xffffu);
  CHECK(r.warnings("ODR") == 0);
  REQUIRE(cpu_store(r.f, gpiog, 0xffff'ffdfu).reason == stop_reason::user);
  CHECK(r.warnings("MODER") == 1);
  CHECK(read(r.f, gpiog) == 0xffff'ffdfu);
  CHECK(port(r.f, "GPIOG").mode(2) == stm32::pin_mode::output);
  REQUIRE(cpu_store(r.f, gpiog + 0x18, 1u << 15).reason == stop_reason::user);
  CHECK(r.warnings("BSRR") == 0);
  CHECK(r.warnings("MODER") == 1);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged.front().message.contains("PG2"));
  rig valid{with_port_g_bonded()};
  write(valid.f, pwr_cr2, read(valid.f, pwr_cr2) | iosv);
  REQUIRE(cpu_store(valid.f, gpiog, 0xffff'ffdfu).reason == stop_reason::user);
  CHECK(valid.logged.empty());
  rig unbonded{l476rc()};
  REQUIRE(cpu_store(unbonded.f, gpiog, 0xffff'ffdfu).reason == stop_reason::user);
  CHECK(unbonded.logged.empty());
}

class services_probe final : public popcorn::testing::inert_model
{
public:
  explicit services_probe(popcorn::periph::model_context& ctx) : ctx_{ctx}
  {}

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return ctx_.services;
  }

private:
  popcorn::periph::model_context& ctx_;
};

TEST_CASE("RM0351 5.4.2: a DMA write configuring PG2 with IOSV 0 warns once, inferred")
{
  services_probe* probe   = nullptr;
  auto            options = popcorn::testing::stm32_options();
  options.models["SYSCFG"] =
      [&probe](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
        auto made = std::make_shared<services_probe>(ctx);
        probe     = made.get();
        return made;
      };
  rig r{with_port_g_bonded(), std::move(options)};
  REQUIRE(probe != nullptr);
  REQUIRE((read(r.f, pwr_cr2) & iosv) == 0);
  REQUIRE(probe->services().write(gpiog + 0x14, 4, 0x4u, popcorn::initiator::dma).has_value());
  CHECK(read(r.f, gpiog + 0x14) == 0x4u);
  CHECK(r.warnings("ODR") == 1);
  REQUIRE(probe->services().write(gpiog + 0x14, 4, 0x8u, popcorn::initiator::dma).has_value());
  CHECK(r.warnings("ODR") == 1);
}

TEST_CASE("RM0351 5.4.2: under strict_peripherals the IOSV 0 write raises and keeps the register")
{
  for(auto const& [offset, value, reg] : {
          std::tuple{0x00u, 0xffff'ffdfu, std::string_view{"MODER"}  },
          std::tuple{0x04u, 0x4u,         std::string_view{"OTYPER"} },
          std::tuple{0x08u, 0x10u,        std::string_view{"OSPEEDR"}},
          std::tuple{0x0cu, 0x10u,        std::string_view{"PUPDR"}  },
          std::tuple{0x20u, 0x700u,       std::string_view{"AFRL"}   },
          std::tuple{0x14u, 0x4u,         std::string_view{"ODR"}    },
          std::tuple{0x18u, 0x8u,         std::string_view{"BSRR"}   },
          std::tuple{0x28u, 0x8u,         std::string_view{"BRR"}    },
          std::tuple{0x24u, 0x7u,         std::string_view{"AFRH"}   },
          std::tuple{0x2cu, 0x4u,         std::string_view{"ASCR"}   },
  })
  {
    CAPTURE(reg);
    rig        r{with_port_g_bonded(), strict_options()};
    auto const before = read(r.f, gpiog + offset);
    auto const odr    = read(r.f, gpiog + 0x14);
    CHECK_THROWS_AS((void)cpu_store(r.f, gpiog + offset, value), popcorn::model_error);
    CHECK(read(r.f, gpiog + offset) == before);
    CHECK(read(r.f, gpiog + 0x14) == odr);
    CHECK(r.warnings(reg) == 1);
  }
}

TEST_CASE("the L4 GPIO row serves the L476, a variant the description disagrees with throws")
{
  attaches(l476rc(), stm32::l4_gpio);
  auto no_switch          = stm32::l4_gpio;
  no_switch.analog_switch = false;
  CHECK_THROWS_AS(attaches(l476rc(), no_switch), popcorn::config_error);
  CHECK_THROWS_AS(attaches(l476rc(), stm32::l1_gpio_cat46), popcorn::config_error);
  auto secure   = stm32::l4_gpio;
  secure.secure = true;
  CHECK_THROWS_AS(attaches(l476rc(), secure), popcorn::config_error);
  auto u5_names  = stm32::l4_gpio;
  u5_names.names = stm32::gpio_field_names{};
  CHECK_THROWS_AS(attaches(l476rc(), u5_names), popcorn::config_error);
}
