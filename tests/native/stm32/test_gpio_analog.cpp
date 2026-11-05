#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::part_description;
using popcorn::testing::stm32_fixture;

struct family
{
  std::string_view name;
  part_description const& (*description)();
  std::string_view firmware;
  std::string_view reset_register;
};

auto families() -> std::vector<family>
{
  return {
      {"U575",   popcorn::testing::u575,   "rcc_ll",            "RCC_AHB2RSTR1"},
      {"H573",   popcorn::testing::h573,   "h573_rcc_ll",       "RCC_AHB2RSTR" },
      {"L152RE", popcorn::testing::l152re, "l152xe_l1_gpio_ll", "AHBRSTR"      },
      {"L476RC", popcorn::testing::l476rc, "l476_l4_gpio_ll",   "AHB2RSTR"     },
  };
}

constexpr std::uint8_t pa4 = 4;

struct rig
{
  family                         of;
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  explicit rig(family const& which, bool strict = false) : of{which}
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(which.description(), which.firmware, std::move(options));
    f->set("pc", popcorn::testing::sym(which.firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
  }

  [[nodiscard]] auto port() const -> stm32::gpio_port&
  {
    auto* const found = f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(logged, [](core::log_record const& r) {
      return r.peripheral == "GPIOA" and r.kind == core::log_kind::model_warning;
    }));
  }

  void gpio_reset() const
  {
    popcorn::testing::rcc_reset_pulse(*f, of.reset_register, "GPIOARST");
  }
};
}

TEST_CASE("the board slot of a pin reads back, an undriven pin reads 0, a release clears it")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const r{which};
    CHECK(r.port().analog_level(pa4) == 0);
    CHECK_FALSE(r.port().analog_drive(pa4).has_value());
    r.port().drive_analog(pa4, 1'250'000);
    CHECK(r.port().analog_drive(pa4) == std::optional<std::uint32_t>{1'250'000});
    CHECK(r.port().analog_level(pa4) == 1'250'000);
    CHECK(r.port().analog_level(5) == 0);
    r.port().drive(pa4, true);
    r.port().drive(pa4, std::nullopt);
    CHECK(r.port().analog_level(pa4) == 1'250'000);
    r.port().drive_analog(pa4, std::nullopt);
    CHECK_FALSE(r.port().analog_drive(pa4).has_value());
    CHECK(r.port().analog_level(pa4) == 0);
  }
}

TEST_CASE("the board slot survives a software and a power-on reset and a GPIOARST pulse")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const r{which};
    r.port().drive_analog(pa4, 2'000'000);
    r.gpio_reset();
    CHECK(r.port().analog_level(pa4) == 2'000'000);
    for(auto const kind : {popcorn::reset_kind::software, popcorn::reset_kind::power_on})
    {
      r.f->target->reset(kind);
      CHECK(r.port().analog_drive(pa4) == std::optional<std::uint32_t>{2'000'000});
    }
  }
}

TEST_CASE(
    "the chip slot survives a GPIOARST pulse, a software and a power-on reset, its driver alone "
    "clears it, the reset rule inferred")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const r{which};
    r.port().drive_chip_analog(pa4, 900'000);
    r.gpio_reset();
    CHECK(r.port().analog_level(pa4) == 900'000);
    for(auto const kind : {popcorn::reset_kind::software, popcorn::reset_kind::power_on})
    {
      r.f->target->reset(kind);
      CHECK(r.port().analog_level(pa4) == 900'000);
    }
    r.port().drive_chip_analog(pa4, std::nullopt);
    CHECK(r.port().analog_level(pa4) == 0);
  }
}

TEST_CASE(
    "the chip slot wins over the board slot, the first overlap on a pin warns once, the board "
    "level shows again when the chip slot clears")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const r{which};
    r.port().drive_analog(pa4, 1'000'000);
    CHECK(r.warnings() == 0);
    r.port().drive_chip_analog(pa4, 3'000'000);
    CHECK(r.port().analog_level(pa4) == 3'000'000);
    CHECK(r.port().analog_drive(pa4) == std::optional<std::uint32_t>{1'000'000});
    CHECK(r.warnings() == 1);
    r.port().drive_analog(pa4, 1'500'000);
    r.port().drive_chip_analog(pa4, std::nullopt);
    CHECK(r.port().analog_level(pa4) == 1'500'000);
    r.port().drive_chip_analog(pa4, 2'000'000);
    CHECK(r.warnings() == 1);
    r.port().drive_analog(5, 100'000);
    r.port().drive_chip_analog(5, 200'000);
    CHECK(r.warnings() == 2);
  }
}

TEST_CASE("an overlap under strict_peripherals warns and raises no model_error")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const r{which, true};
    r.port().drive_chip_analog(pa4, 3'000'000);
    CHECK_NOTHROW(r.port().drive_analog(pa4, 1'000'000));
    CHECK(r.warnings() == 1);
    CHECK(r.port().analog_level(pa4) == 3'000'000);
  }
}

TEST_CASE(
    "an analog listener runs before a slot changes and reads the old level, a write of the same "
    "value calls nothing, a dropped subscription stops it")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const                  r{which};
    std::vector<std::uint32_t> seen;
    auto                       subscription = r.port().on_analog_changing([&](std::uint8_t pin) {
      CHECK(pin == pa4);
      seen.push_back(r.port().analog_level(pin));
    });
    r.port().drive_analog(pa4, 1'000'000);
    r.port().drive_analog(pa4, 1'000'000);
    r.port().drive_chip_analog(pa4, 2'000'000);
    r.port().drive_chip_analog(pa4, std::nullopt);
    CHECK(seen == std::vector<std::uint32_t>{0, 1'000'000, 2'000'000});
    subscription.reset();
    r.port().drive_analog(pa4, std::nullopt);
    CHECK(seen.size() == 3);
  }
}

TEST_CASE("every analog call on a pin outside the port throws config_error")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const r{which};
    CHECK_THROWS_AS(r.port().drive_analog(16, 1), popcorn::config_error);
    CHECK_THROWS_AS(static_cast<void>(r.port().analog_drive(16)), popcorn::config_error);
    CHECK_THROWS_AS(r.port().drive_chip_analog(16, 1), popcorn::config_error);
    CHECK_THROWS_AS(static_cast<void>(r.port().analog_level(16)), popcorn::config_error);
  }
}

TEST_CASE("a snapshot restores both analog slots of a pin, through its saved bytes")
{
  for(auto const& which : families())
  {
    CAPTURE(which.name);
    rig const r{which};
    r.port().drive_analog(pa4, 1'000'000);
    r.port().drive_chip_analog(pa4, 2'000'000);
    r.port().drive_analog(7, 700'000);
    auto const bytes = core::encode_snapshot(r.f->target->snapshot().data());
    r.port().drive_analog(pa4, std::nullopt);
    r.port().drive_chip_analog(pa4, std::nullopt);
    r.port().drive_chip_analog(7, 50'000);
    r.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
    CHECK(r.port().analog_drive(pa4) == std::optional<std::uint32_t>{1'000'000});
    CHECK(r.port().analog_level(pa4) == 2'000'000);
    CHECK(r.port().analog_level(7) == 700'000);
    r.port().drive_chip_analog(pa4, std::nullopt);
    CHECK(r.port().analog_level(pa4) == 1'000'000);
  }
}
