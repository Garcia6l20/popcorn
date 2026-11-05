#include "../../../native/src/core/machine_impl.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::initiator;
using popcorn::part_description;
using popcorn::reset_kind;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

struct on_u575
{
  static auto part() -> part_description const&
  {
    return popcorn::testing::u575();
  }
  static constexpr std::string_view                firmware   = "rcc_ll";
  static constexpr std::string_view                timer_reg  = "APB1ENR1";
  static constexpr std::string_view                other_reg  = "AHB1ENR";
  static constexpr std::string_view                other_bit  = "GPDMA1EN";
  static constexpr std::string_view                other      = "GPDMA1";
  static constexpr std::string_view                shared_reg = "AHB2ENR1";
  static constexpr std::string_view                shared_bit = "ADC12EN";
  static constexpr std::array<std::string_view, 2> shared{"ADC1", "ADC12"};
  static constexpr std::array<std::string_view, 2> reset_on{"FLASH", "DCACHE"};
};

struct on_h573
{
  static auto part() -> part_description const&
  {
    return popcorn::testing::h573();
  }
  static constexpr std::string_view                firmware   = "h573_rcc_ll";
  static constexpr std::string_view                timer_reg  = "APB1LENR";
  static constexpr std::string_view                other_reg  = "AHB1ENR";
  static constexpr std::string_view                other_bit  = "GPDMA1EN";
  static constexpr std::string_view                other      = "GPDMA1";
  static constexpr std::string_view                shared_reg = "AHB2ENR";
  static constexpr std::string_view                shared_bit = "ADC12EN";
  static constexpr std::array<std::string_view, 3> shared{"ADC1", "ADC2", "ADCC"};
  static constexpr std::array<std::string_view, 2> reset_on{"FLASH", "DCACHE"};
};

struct on_l152
{
  static auto part() -> part_description const&
  {
    return popcorn::testing::l152re();
  }
  static constexpr std::string_view                firmware   = "l152xe_l1_rcc_ll";
  static constexpr std::string_view                timer_reg  = "APB1ENR";
  static constexpr std::string_view                other_reg  = "AHBENR";
  static constexpr std::string_view                other_bit  = "GPIOAEN";
  static constexpr std::string_view                other      = "GPIOA";
  static constexpr std::string_view                shared_reg = "APB1ENR";
  static constexpr std::string_view                shared_bit = "COMPEN";
  static constexpr std::array<std::string_view, 3> shared{"COMP", "RI", "OPAMP"};
  static constexpr std::array<std::string_view, 1> reset_on{"FLASH"};
};

struct on_l476
{
  static auto part() -> part_description const&
  {
    return popcorn::testing::l476rc();
  }
  static constexpr std::string_view                firmware   = "l476_l4_rcc_ll";
  static constexpr std::string_view                timer_reg  = "APB1ENR1";
  static constexpr std::string_view                other_reg  = "AHB1ENR";
  static constexpr std::string_view                other_bit  = "DMA1EN";
  static constexpr std::string_view                other      = "DMA1";
  static constexpr std::string_view                shared_reg = "AHB2ENR";
  static constexpr std::string_view                shared_bit = "ADCEN";
  static constexpr std::array<std::string_view, 4> shared{"ADC1", "ADC2", "ADC3", "ADC_Common"};
  static constexpr std::array<std::string_view, 1> reset_on{"FLASH"};
};

struct gate_rig
{
  std::string_view firmware;
  stm32_fixture    f;

  gate_rig(
      part_description const& desc,
      std::string_view        image,
      core::machine_options   options = popcorn::testing::stm32_options())
      : firmware{image}, f{desc, image, std::move(options)}
  {
    f.set("pc", sym(image, "idle"));
  }

  [[nodiscard]] auto bus() const -> core::machine_services&
  {
    return core::machine_internals::services(*f.target);
  }

  [[nodiscard]] auto at(std::string_view unit, std::string_view reg) const -> address
  {
    auto const& desc  = f.target->peripheral(unit).description();
    auto const* found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto bit(std::string_view reg, std::string_view field) const -> std::uint32_t
  {
    auto const* found = popcorn::find_register(f.target->peripheral("RCC").description(), reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    return std::uint32_t{1} << bits->lsb;
  }

  void cpu_set(std::string_view reg, std::string_view field, bool on) const
  {
    auto const address = at("RCC", reg);
    auto const old     = bus().read(address, 4, initiator::cpu);
    REQUIRE(old.has_value());
    auto const mask = bit(reg, field);
    REQUIRE(bus().write(address, 4, on ? (*old | mask) : (*old & ~mask), initiator::cpu));
  }

  [[nodiscard]] auto cpu_read(address from) const -> std::uint32_t
  {
    auto const value = bus().read(from, 4, initiator::cpu);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto gated(std::string_view unit) const -> bool
  {
    return f.target->clock_gated(unit);
  }
};

template <typename P>
auto rig_of(core::machine_options options = popcorn::testing::stm32_options()) -> gate_rig
{
  return gate_rig{P::part(), P::firmware, std::move(options)};
}
}

TEST_CASE_TEMPLATE(
    "a CPU or DMA access to TIM2 with its RCC enable clear is lost, the enable lets it through, "
    "clearing it gates it again",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  auto const r   = rig_of<P>();
  auto const arr = r.at("TIM2", "ARR");
  REQUIRE(r.gated("TIM2"));
  auto const before = r.f.target->peripheral("TIM2").peek(arr & 0x3ffu);
  for(auto const by : {initiator::cpu, initiator::dma})
  {
    CAPTURE(by);
    REQUIRE(r.bus().write(arr, 4, 0x1234, by));
    CHECK(r.bus().read(arr, 4, by) == 0u);
  }
  CHECK(r.f.target->peripheral("TIM2").peek(arr & 0x3ffu) == before);
  r.cpu_set(P::timer_reg, "TIM2EN", true);
  CHECK_FALSE(r.gated("TIM2"));
  REQUIRE(r.bus().write(arr, 4, 0x1234, initiator::cpu));
  CHECK(r.cpu_read(arr) == 0x1234u);
  r.cpu_set(P::timer_reg, "TIM2EN", false);
  CHECK(r.gated("TIM2"));
  CHECK(r.cpu_read(arr) == 0u);
  CHECK(r.f.target->mem().read(arr, 4) == 0x1234u);
}

TEST_CASE_TEMPLATE(
    "a shared RCC enable gates every instance it covers",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  auto const r = rig_of<P>();
  for(auto const unit : P::shared)
  {
    CAPTURE(unit);
    CHECK(r.gated(unit));
  }
  r.cpu_set(P::shared_reg, P::shared_bit, true);
  for(auto const unit : P::shared)
  {
    CAPTURE(unit);
    CHECK_FALSE(r.gated(unit));
  }
  r.cpu_set(P::shared_reg, P::shared_bit, false);
  for(auto const unit : P::shared)
  {
    CAPTURE(unit);
    CHECK(r.gated(unit));
  }
}

TEST_CASE_TEMPLATE(
    "an RCC enable that resets to 1 leaves its instance ungated, the blocks with no enable are "
    "never gated",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  auto const r = rig_of<P>();
  for(auto const unit : P::reset_on)
  {
    CAPTURE(unit);
    CHECK_FALSE(r.gated(unit));
  }
  for(auto const unit : {"RCC", "EXTI", "IWDG", "DBGMCU"})
  {
    CAPTURE(unit);
    CHECK_FALSE(r.gated(unit));
  }
  CHECK(r.gated("TIM2"));
}

TEST_CASE_TEMPLATE(
    "an RCC enable bit gates only its own instance, not the one at its position in another "
    "register",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  auto const r = rig_of<P>();
  r.cpu_set(P::other_reg, P::other_bit, true);
  CHECK_FALSE(r.gated(P::other));
  CHECK(r.gated("TIM2"));
  r.cpu_set(P::other_reg, P::other_bit, false);
  CHECK(r.gated(P::other));
}

TEST_CASE_TEMPLATE(
    "a machine reset gates every instance its RCC enable reset value clears",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  auto const r = rig_of<P>();
  r.cpu_set(P::timer_reg, "TIM2EN", true);
  REQUIRE_FALSE(r.gated("TIM2"));
  r.f.target->reset(reset_kind::software);
  CHECK(r.gated("TIM2"));
  for(auto const unit : P::reset_on)
  {
    CAPTURE(unit);
    CHECK_FALSE(r.gated(unit));
  }
}

TEST_CASE_TEMPLATE(
    "a machine built without a reset gates from the RCC reset values",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  popcorn::periph::stm32::register_builtin_models();
  core::machine const target{
      P::part(), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
  CHECK(target.clock_gated("TIM2"));
  for(auto const unit : P::reset_on)
  {
    CAPTURE(unit);
    CHECK_FALSE(target.clock_gated(unit));
  }
  for(auto const unit : P::shared)
  {
    CAPTURE(unit);
    CHECK(target.clock_gated(unit));
  }
}

TEST_CASE_TEMPLATE(
    "a restore follows the RCC enables of the snapshot, in the same machine and in a fresh one",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  auto const r = rig_of<P>();
  r.cpu_set(P::timer_reg, "TIM2EN", true);
  auto const open = r.f.target->snapshot();
  r.cpu_set(P::timer_reg, "TIM2EN", false);
  auto const closed = r.f.target->snapshot();
  r.f.target->restore(open);
  CHECK_FALSE(r.gated("TIM2"));
  r.f.target->restore(closed);
  CHECK(r.gated("TIM2"));

  stm32_fixture const fresh{popcorn::testing::no_reset, P::part(), P::firmware};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(open.data()))});
  CHECK_FALSE(fresh.target->clock_gated("TIM2"));

  auto       image = open.data();
  auto const tim2  = std::ranges::find(
      image.peripherals, std::string_view{"TIM2"}, &core::peripheral_image::peripheral);
  REQUIRE(tim2 != image.peripherals.end());
  tim2->clock_gated = 1;
  r.f.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(image))});
  CHECK_FALSE(r.gated("TIM2"));
}

TEST_CASE_TEMPLATE(
    "under strict_peripherals a CPU access to an instance with its RCC enable clear is a "
    "model_error naming it",
    P,
    on_u575,
    on_h573,
    on_l152,
    on_l476)
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const r               = rig_of<P>(std::move(options));
  auto const arr             = r.at("TIM2", "ARR");
  CHECK_THROWS_WITH_AS(
      static_cast<void>(r.bus().read(arr, 4, initiator::cpu)), doctest::Contains("TIM2"),
      popcorn::model_error);
  r.cpu_set(P::timer_reg, "TIM2EN", true);
  CHECK(r.cpu_read(arr) != 0u);
}

TEST_CASE_TEMPLATE(
    "RM0038 5.1.3 and RM0351 5.1.5: the RTC is gated on RTCEN, which a system reset keeps and a "
    "power-on reset clears",
    P,
    on_l152,
    on_l476)
{
  constexpr auto rtcen_reg = std::same_as<P, on_l152> ? std::string_view{"CSR"} : "BDCR";
  auto const     r         = rig_of<P>();
  CHECK(r.gated("RTC"));
  r.cpu_set(rtcen_reg, "RTCEN", true);
  CHECK_FALSE(r.gated("RTC"));
  r.f.target->reset(reset_kind::software);
  CHECK_FALSE(r.gated("RTC"));
  r.f.target->reset(reset_kind::power_on);
  CHECK(r.gated("RTC"));
}

TEST_CASE_TEMPLATE(
    "RM0456 11.8.36 and RM0481 11.8.32: the RTC and TAMP are gated on RTCAPBEN",
    P,
    on_u575,
    on_h573)
{
  auto const r = rig_of<P>();
  CHECK(r.gated("RTC"));
  CHECK(r.gated("TAMP"));
  r.cpu_set("APB3ENR", "RTCAPBEN", true);
  CHECK_FALSE(r.gated("RTC"));
  CHECK_FALSE(r.gated("TAMP"));
  r.cpu_set("BDCR", "RTCEN", true);
  r.cpu_set("APB3ENR", "RTCAPBEN", false);
  CHECK(r.gated("RTC"));
}

TEST_CASE("the STM32U575 OPAMP is gated on OPAMPEN, RM0456 11.8.36, not on COMPEN")
{
  auto const r = rig_of<on_u575>();
  CHECK(r.gated("OPAMP"));
  r.cpu_set("APB3ENR", "COMPEN", true);
  CHECK(r.gated("OPAMP"));
  CHECK_FALSE(r.gated("COMP"));
  r.cpu_set("APB3ENR", "OPAMPEN", true);
  CHECK_FALSE(r.gated("OPAMP"));
  r.cpu_set("APB3ENR", "COMPEN", false);
  CHECK_FALSE(r.gated("OPAMP"));
  CHECK(r.gated("COMP"));
}

TEST_CASE("an RCC enable named in the table and missing from the description is a config_error")
{
  auto desc = popcorn::testing::u575();
  auto rcc =
      std::ranges::find(desc.peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  REQUIRE(rcc != desc.peripherals.end());
  auto ahb2 = std::ranges::find(
      rcc->registers, std::string_view{"AHB2ENR2"}, &popcorn::register_desc::name);
  REQUIRE(ahb2 != rcc->registers.end());
  std::erase_if(
      ahb2->fields, [](popcorn::field_desc const& field) { return field.name == "FSMCEN"; });
  popcorn::periph::stm32::register_builtin_models();
  CHECK_THROWS_WITH_AS(
      core::machine(desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()),
      doctest::Contains("FSMCEN"), popcorn::config_error);
}
