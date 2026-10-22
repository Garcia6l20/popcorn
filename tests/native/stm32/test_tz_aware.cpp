#include "gtzc.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
using popcorn::initiator;
using popcorn::testing::u575;

constexpr bus_attrs secure_priv{.by = initiator::cpu, .secure = true, .privileged = true};
constexpr bus_attrs secure_unpriv{.by = initiator::cpu, .secure = true, .privileged = false};
constexpr bus_attrs nonsecure_priv{.by = initiator::cpu, .secure = false, .privileged = true};
constexpr bus_attrs nonsecure_unpriv{.by = initiator::cpu, .secure = false, .privileged = false};

class probe final : public periph::model
{
public:
  explicit probe(periph::model_context& ctx) : ctx_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0;
  }

  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return ctx_.services;
  }

private:
  periph::model_context& ctx_;
};

auto unit(std::string_view name) -> popcorn::peripheral_desc const&
{
  return *std::ranges::find(u575().peripherals, name, &popcorn::peripheral_desc::name);
}

auto bit(std::string_view peripheral, std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const* found = popcorn::find_register(unit(peripheral), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << (bits->lsb + 8 * (found->offset % 4));
}

auto bit_index(std::string_view peripheral, std::string_view reg, std::string_view field)
    -> std::uint32_t
{
  auto const* found = popcorn::find_register(unit(peripheral), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return bits->lsb + 8 * (found->offset % 4);
}

struct rig
{
  std::unique_ptr<core::machine> target;
  probe*                         bus{};

  explicit rig(bool trustzone, stm32::option_bytes wanted = {.tzen = true}, bool strict = false)
  {
    stm32::register_builtin_models();
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    if(trustzone)
    {
      wanted.tzen = true;
      REQUIRE(stm32::configure_option_bytes(u575(), wanted, options).has_value());
    }
    options.models.emplace(
        "LPGPIO1", [this](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          auto made = std::make_shared<probe>(ctx);
          bus       = made.get();
          return made;
        });
    target = std::make_unique<core::machine>(u575(), popcorn::arch::armv8m::factory(), options);
    target->reset();
  }

  [[nodiscard]] static auto at(std::string_view peripheral, std::string_view reg) -> address
  {
    auto const& p     = unit(peripheral);
    auto const* found = popcorn::find_register(p, reg);
    REQUIRE(found != nullptr);
    return p.base + (found->offset & ~3u);
  }

  void
      write(std::string_view peripheral, std::string_view reg, std::uint32_t value, bus_attrs attrs)
          const
  {
    REQUIRE(bus->services().write(at(peripheral, reg), 4, value, attrs).has_value());
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg, bus_attrs attrs) const
      -> std::uint32_t
  {
    auto const value = bus->services().read(at(peripheral, reg), 4, attrs);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto peek(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    return target->peripheral(peripheral).peek(at(peripheral, reg) - unit(peripheral).base);
  }

  void poke(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    target->peripheral(peripheral).poke(at(peripheral, reg) - unit(peripheral).base, value);
  }

  [[nodiscard]] auto port(std::string_view name) const -> stm32::gpio_port&
  {
    auto* const found = dynamic_cast<stm32::gpio_port*>(target->model(name));
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto irq_pending(std::uint32_t irq) const -> bool
  {
    auto const word = target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }
};

constexpr std::uint32_t exti3_irq = 14;

TEST_CASE("RM0456 13.3.18 Table 128: a Non-secure access to a Secure pin's fields is RAZ/WI")
{
  rig const r{true};
  CHECK(r.peek("GPIOA", "SECCFGR") == 0xffff);
  CHECK(r.read("GPIOA", "SECCFGR", nonsecure_unpriv) == 0xffff);
  auto const moder = r.peek("GPIOA", "MODER");
  CHECK(r.read("GPIOA", "MODER", nonsecure_priv) == 0);
  r.write("GPIOA", "MODER", 0, nonsecure_priv);
  CHECK(r.peek("GPIOA", "MODER") == moder);

  r.write("GPIOA", "SECCFGR", 0xffff & ~(1u << 5), nonsecure_priv);
  CHECK(r.peek("GPIOA", "SECCFGR") == 0xffff);
  r.write("GPIOA", "SECCFGR", 0xffff & ~(1u << 5), secure_unpriv);
  CHECK(r.peek("GPIOA", "SECCFGR") == (0xffff & ~(1u << 5)));

  auto const pin5 = 0x3u << 10;
  auto const pin4 = 0x3u << 8;
  CHECK(r.read("GPIOA", "MODER", nonsecure_priv) == (moder & pin5));
  r.write("GPIOA", "MODER", 0x1u << 10 | 0x1u << 8, nonsecure_priv);
  CHECK((r.peek("GPIOA", "MODER") & pin5) == 0x1u << 10);
  CHECK((r.peek("GPIOA", "MODER") & pin4) == (moder & pin4));

  r.write("GPIOA", "ODR", 0x30, nonsecure_priv);
  CHECK(r.peek("GPIOA", "ODR") == 0x20);
  CHECK(r.port("GPIOA").level(5));
  CHECK_FALSE(r.port("GPIOA").level(4));
  r.write("GPIOA", "BSRR", 1u << (16 + 5) | 1u << 4, nonsecure_priv);
  CHECK(r.peek("GPIOA", "ODR") == 0);
  r.write("GPIOA", "BRR", 1u << 4, secure_priv);
  r.write("GPIOA", "BSRR", 1u << 4, secure_priv);
  CHECK(r.peek("GPIOA", "ODR") == 0x10);
  CHECK(r.read("GPIOA", "ODR", nonsecure_priv) == 0);
  CHECK(r.read("GPIOA", "ODR", secure_unpriv) == 0x10);
}

TEST_CASE("RM0456 13.4.13: GPIOI_SECCFGR resets to 0x00FF under TZEN=1 on the STM32U575")
{
  rig const r{true};
  CHECK(r.peek("GPIOI", "SECCFGR") == 0x00ff);
  CHECK(r.peek("GPIOH", "SECCFGR") == 0xffff);
}

TEST_CASE("RM0456 13.4.13, P1: GPIOx_SECCFGR is RAZ/WI under TZEN=0, every pin open")
{
  rig const r{false};
  CHECK(r.read("GPIOA", "SECCFGR", nonsecure_priv) == 0);
  r.write("GPIOA", "SECCFGR", 0, nonsecure_priv);
  CHECK(r.peek("GPIOA", "SECCFGR") == 0xffff);
  CHECK(r.peek("GPIOI", "SECCFGR") == 0xffff);
  r.write("GPIOA", "MODER", 0x1u << 8, nonsecure_unpriv);
  CHECK(r.read("GPIOA", "MODER", nonsecure_unpriv) == 0x1u << 8);
}

TEST_CASE("RM0456 23.5.1: a Secure EXTI line is hidden from Non-secure accesses")
{
  rig const r{true};
  r.write("GPIOA", "MODER", 0, secure_priv);
  r.write("EXTI", "SECCFGR1", 1u << 3, nonsecure_priv);
  CHECK(r.peek("EXTI", "SECCFGR1") == 0);
  CHECK(r.peek("GTZC2_TZIC", "SR2") == bit("GTZC2_TZIC", "SR2", "EXTIF"));
  r.write("EXTI", "SECCFGR1", 1u << 3, secure_priv);
  CHECK(r.read("EXTI", "SECCFGR1", nonsecure_unpriv) == 1u << 3);

  r.write("EXTI", "RTSR1", 1u << 3 | 1u << 2, nonsecure_priv);
  CHECK(r.peek("EXTI", "RTSR1") == 1u << 2);
  r.write("EXTI", "RTSR1", 1u << 3 | 1u << 2, secure_priv);
  r.write("EXTI", "IMR1", 1u << 3, nonsecure_priv);
  CHECK(r.peek("EXTI", "IMR1") == 0);

  r.port("GPIOA").drive(3, true);
  CHECK(r.peek("EXTI", "RPR1") == 1u << 3);
  CHECK(r.read("EXTI", "RPR1", nonsecure_priv) == 0);
  CHECK(r.read("EXTI", "RPR1", secure_priv) == 1u << 3);
  r.write("EXTI", "RPR1", 1u << 3, nonsecure_priv);
  CHECK(r.peek("EXTI", "RPR1") == 1u << 3);
  CHECK_FALSE(r.irq_pending(exti3_irq));

  r.write("EXTI", "IMR1", 1u << 3, secure_priv);
  CHECK(r.irq_pending(exti3_irq));
  r.write("EXTI", "IMR1", 0, nonsecure_priv);
  CHECK(r.peek("EXTI", "IMR1") == 1u << 3);
  r.write("EXTI", "RPR1", 1u << 3, secure_priv);
  CHECK(r.peek("EXTI", "RPR1") == 0);
}

TEST_CASE("RM0456 23.5.2, 23.6.6, 23.6.7: EXTI privilege, SECCFGR1 privilege and LOCKR")
{
  rig const r{true};
  r.write("EXTI", "PRIVCFGR1", 1u << 4, nonsecure_unpriv);
  CHECK(r.peek("EXTI", "PRIVCFGR1") == 0);
  r.write("EXTI", "PRIVCFGR1", 1u << 4, nonsecure_priv);
  CHECK(r.peek("EXTI", "PRIVCFGR1") == 1u << 4);
  r.write("EXTI", "FTSR1", 1u << 4 | 1u << 5, nonsecure_unpriv);
  CHECK(r.peek("EXTI", "FTSR1") == 1u << 5);
  r.write("EXTI", "SECCFGR1", 1u << 4 | 1u << 6, secure_unpriv);
  CHECK(r.peek("EXTI", "SECCFGR1") == 1u << 6);
  r.write("EXTI", "PRIVCFGR1", 1u << 4 | 1u << 6, nonsecure_priv);
  CHECK(r.peek("EXTI", "PRIVCFGR1") == 1u << 4);

  CHECK(r.read("EXTI", "LOCKR", nonsecure_priv) == 0);
  r.write("EXTI", "LOCKR", 1, nonsecure_priv);
  CHECK(r.peek("EXTI", "LOCKR") == 0);
  r.write("EXTI", "LOCKR", 1, secure_priv);
  r.write("EXTI", "LOCKR", 0, secure_priv);
  CHECK(r.peek("EXTI", "LOCKR") == 1);
  r.write("EXTI", "SECCFGR1", 0, secure_priv);
  r.write("EXTI", "PRIVCFGR1", 0, secure_priv);
  CHECK(r.peek("EXTI", "SECCFGR1") == 1u << 6);
  CHECK(r.peek("EXTI", "PRIVCFGR1") == 1u << 4);
}

TEST_CASE(
    "EXTI_SECCFGR1 stays storage under TZEN=0, popcorn's rule, RM0456 23.6.6 leaves it unstated")
{
  rig const r{false};
  r.write("EXTI", "SECCFGR1", 1u << 3, nonsecure_priv);
  CHECK(r.read("EXTI", "SECCFGR1", nonsecure_priv) == 1u << 3);
  r.write("EXTI", "RTSR1", 1u << 3, nonsecure_priv);
  CHECK(r.read("EXTI", "RTSR1", nonsecure_priv) == 1u << 3);
}

TEST_CASE("RM0456 11.5.1 Table 117: RCC_SECCFGR.PLL1SEC makes the PLL1 bits Secure")
{
  rig const r{true};
  r.write("RCC", "SECCFGR", bit("RCC", "SECCFGR", "PLL1SEC"), nonsecure_priv);
  CHECK(r.peek("RCC", "SECCFGR") == 0);
  CHECK(r.peek("GTZC2_TZIC", "SR2") == bit("GTZC2_TZIC", "SR2", "RCCF"));
  r.write("RCC", "SECCFGR", bit("RCC", "SECCFGR", "PLL1SEC"), secure_unpriv);
  CHECK(r.read("RCC", "SECCFGR", nonsecure_unpriv) == bit("RCC", "SECCFGR", "PLL1SEC"));

  r.write("RCC", "PLL1CFGR", 0x1, nonsecure_priv);
  CHECK(r.peek("RCC", "PLL1CFGR") == 0);
  r.write("RCC", "PLL1CFGR", 0x1, secure_priv);
  CHECK(r.peek("RCC", "PLL1CFGR") == 0x1);
  CHECK(r.read("RCC", "PLL1CFGR", nonsecure_priv) == 0);
  r.write("RCC", "PLL2CFGR", 0x1, nonsecure_priv);
  CHECK(r.peek("RCC", "PLL2CFGR") == 0x1);
  auto const pll1 = bit("RCC", "CIER", "PLL1RDYIE");
  auto const pll2 = bit("RCC", "CIER", "PLL2RDYIE");
  r.write("RCC", "CIER", pll1 | pll2, nonsecure_priv);
  CHECK(r.peek("RCC", "CIER") == pll2);
}

TEST_CASE("RM0456 11.5.1: a peripheral's RCC bits follow its TZSC or own security")
{
  rig const r{
      true, {.tzen = true, .secwm1 = stm32::no_area{}, .secwm2 = stm32::no_area{}}
  };
  auto const tim2 = bit("RCC", "APB1ENR1", "TIM2EN");
  auto const tim3 = bit("RCC", "APB1ENR1", "TIM3EN");
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), secure_priv);
  r.write("RCC", "APB1ENR1", tim2 | tim3, nonsecure_priv);
  CHECK(r.peek("RCC", "APB1ENR1") == tim3);
  r.write("RCC", "APB1RSTR1", bit("RCC", "APB1RSTR1", "TIM2RST"), nonsecure_priv);
  CHECK(r.peek("RCC", "APB1RSTR1") == 0);
  r.write("RCC", "APB1ENR1", tim2 | tim3, secure_priv);
  CHECK(r.read("RCC", "APB1ENR1", nonsecure_priv) == tim3);

  auto const gpioa = bit("RCC", "AHB2ENR1", "GPIOAEN");
  auto const reset = r.peek("RCC", "AHB2ENR1");
  r.write("RCC", "AHB2ENR1", reset | gpioa, nonsecure_priv);
  CHECK((r.peek("RCC", "AHB2ENR1") & gpioa) == 0);
  r.write("GPIOA", "SECCFGR", 0, secure_priv);
  r.write("RCC", "AHB2ENR1", reset | gpioa, nonsecure_priv);
  CHECK((r.peek("RCC", "AHB2ENR1") & gpioa) == gpioa);

  auto const flash = bit("RCC", "AHB1ENR", "FLASHEN");
  r.write("RCC", "AHB1ENR", r.peek("RCC", "AHB1ENR") & ~flash, nonsecure_priv);
  CHECK((r.peek("RCC", "AHB1ENR") & flash) == 0);
  r.write("RCC", "AHB1ENR", r.peek("RCC", "AHB1ENR") | flash, secure_priv);
  r.write("FLASH", "SEC1BBR1", 1, secure_priv);
  r.write("RCC", "AHB1ENR", r.peek("RCC", "AHB1ENR") & ~flash, nonsecure_priv);
  CHECK((r.peek("RCC", "AHB1ENR") & flash) == flash);
  r.write("FLASH", "SECKEYR", 0x4567'0123, secure_priv);
  r.write("FLASH", "SECKEYR", 0xcdef'89ab, secure_priv);
  r.write("FLASH", "SECCR", bit("FLASH", "SECCR", "INV"), secure_priv);
  REQUIRE(r.peek("FLASH", "SECCR") == bit("FLASH", "SECCR", "INV"));
  r.write("RCC", "AHB1ENR", r.peek("RCC", "AHB1ENR") & ~flash, nonsecure_priv);
  CHECK((r.peek("RCC", "AHB1ENR") & flash) == 0);
}

TEST_CASE("RM0456 11.5.1: RNGRST of a Secure RNG ignores a Non-secure write, ADC12RST stores")
{
  rig const r{
      true, {.tzen = true, .secwm1 = stm32::no_area{}, .secwm2 = stm32::no_area{}}
  };
  auto const rngrst = bit("RCC", "AHB2RSTR1", "RNGRST");
  auto const adcrst = bit("RCC", "AHB2RSTR1", "ADC12RST");
  auto const htcr   = r.peek("RNG", "HTCR");
  r.write("RNG", "CR", bit("RNG", "CR", "CONDRST"), secure_priv);
  r.write("RNG", "HTCR", 0xA2B0, secure_priv);
  REQUIRE(r.peek("RNG", "HTCR") == 0xA2B0);
  r.write("GTZC1_TZSC", "SECCFGR3", bit("GTZC1_TZSC", "SECCFGR3", "RNGSEC"), secure_priv);
  r.write("RCC", "AHB2RSTR1", rngrst | adcrst, nonsecure_priv);
  CHECK(r.peek("RCC", "AHB2RSTR1") == adcrst);
  CHECK(r.peek("RNG", "HTCR") == 0xA2B0);
  r.write("RCC", "AHB2RSTR1", 0, nonsecure_priv);
  CHECK(r.peek("RCC", "AHB2RSTR1") == 0);
  CHECK(r.peek("RNG", "HTCR") == 0xA2B0);
  r.write("RCC", "AHB2RSTR1", rngrst, secure_priv);
  CHECK(r.peek("RCC", "AHB2RSTR1") == rngrst);
  CHECK(r.peek("RNG", "HTCR") == htcr);
}

TEST_CASE("RM0456 11.5.2: RCC SPRIV and NSPRIV make the RCC bits privileged")
{
  rig const  r{true};
  auto const pll1 = bit("RCC", "SECCFGR", "PLL1SEC");
  r.write("RCC", "SECCFGR", pll1, secure_priv);
  r.write("RCC", "PRIVCFGR", 0x3, nonsecure_unpriv);
  CHECK(r.peek("RCC", "PRIVCFGR") == 0);
  r.write("RCC", "PRIVCFGR", 0x3, nonsecure_priv);
  CHECK(r.peek("RCC", "PRIVCFGR") == 0x2);
  r.write("RCC", "PRIVCFGR", 0x3, secure_priv);
  CHECK(r.peek("RCC", "PRIVCFGR") == 0x3);
  r.write("RCC", "PLL1CFGR", 0x1, secure_unpriv);
  CHECK(r.peek("RCC", "PLL1CFGR") == 0);
  r.write("RCC", "PLL2CFGR", 0x1, nonsecure_unpriv);
  CHECK(r.peek("RCC", "PLL2CFGR") == 0);
  r.write("RCC", "SECCFGR", 0, secure_unpriv);
  CHECK(r.peek("RCC", "SECCFGR") == pll1);
  CHECK(r.read("RCC", "SECCFGR", nonsecure_unpriv) == pll1);
  CHECK(r.read("RCC", "PRIVCFGR", nonsecure_unpriv) == 0x3);
}

TEST_CASE("RM0456 11.8.20: RCC_CICR clears RCC_CIFR under TrustZone, by HSISEC for HSIRDYC")
{
  rig const  r{true};
  auto const hsi    = bit("RCC", "CIFR", "HSIRDYF");
  auto const pll1   = bit("RCC", "CIFR", "PLL1RDYF");
  auto const clears = bit("RCC", "CICR", "HSIRDYC") | bit("RCC", "CICR", "PLL1RDYC");
  r.poke("RCC", "CIFR", hsi | pll1);
  r.write("RCC", "CICR", bit("RCC", "CICR", "HSIRDYC"), nonsecure_priv);
  CHECK(r.peek("RCC", "CIFR") == pll1);
  CHECK(r.peek("RCC", "CICR") == 0);

  r.write("RCC", "SECCFGR", bit("RCC", "SECCFGR", "HSISEC"), secure_priv);
  r.poke("RCC", "CIFR", hsi | pll1);
  r.write("RCC", "CICR", clears, nonsecure_priv);
  CHECK(r.peek("RCC", "CIFR") == hsi);
  r.write("RCC", "CICR", clears, secure_priv);
  CHECK(r.peek("RCC", "CIFR") == 0);
}

TEST_CASE("RM0456 11.8.51, P1: RCC_SECCFGR reads 0 and ignores writes under TZEN=0")
{
  rig const r{false};
  r.write("RCC", "SECCFGR", 0x1fff, nonsecure_priv);
  CHECK(r.read("RCC", "SECCFGR", nonsecure_priv) == 0);
  CHECK(r.peek("RCC", "SECCFGR") == 0);
  r.write("RCC", "PLL1CFGR", 0x1, nonsecure_unpriv);
  CHECK(r.peek("RCC", "PLL1CFGR") == 0x1);
}

TEST_CASE("RM0456 10.8 Table 109: PWR_SECCFGR, RCC and GPIO secure the PWR bits")
{
  rig const r{true};
  r.write("PWR", "SECCFGR", bit("PWR", "SECCFGR", "LPMSEC"), nonsecure_priv);
  CHECK(r.peek("PWR", "SECCFGR") == 0);
  CHECK(r.peek("GTZC2_TZIC", "SR2") == bit("GTZC2_TZIC", "SR2", "PWRF"));
  r.write("PWR", "CR2", 0x1, nonsecure_priv);
  CHECK(r.peek("PWR", "CR2") == 0x1);
  r.write("PWR", "SECCFGR", bit("PWR", "SECCFGR", "LPMSEC"), secure_priv);
  CHECK(r.read("PWR", "SECCFGR", nonsecure_unpriv) == bit("PWR", "SECCFGR", "LPMSEC"));
  CHECK(r.read("PWR", "CR2", nonsecure_priv) == 0);
  r.write("PWR", "CR2", 0, nonsecure_priv);
  CHECK(r.peek("PWR", "CR2") == 0x1);

  r.write("PWR", "PUCRA", 0x3, nonsecure_priv);
  CHECK(r.peek("PWR", "PUCRA") == 0);
  r.write("GPIOA", "SECCFGR", 0xfffe, secure_priv);
  r.write("PWR", "PUCRA", 0x3, nonsecure_priv);
  CHECK(r.peek("PWR", "PUCRA") == 0x1);

  auto const vos = 0x3u << 16;
  r.write("PWR", "VOSR", vos, nonsecure_priv);
  CHECK((r.peek("PWR", "VOSR") & vos) == vos);
  r.write("RCC", "SECCFGR", bit("RCC", "SECCFGR", "SYSCLKSEC"), secure_priv);
  r.write("PWR", "VOSR", 0, nonsecure_priv);
  CHECK((r.peek("PWR", "VOSR") & vos) == vos);
  CHECK((r.read("PWR", "VOSR", nonsecure_priv) & vos) == 0);

  r.write("PWR", "PRIVCFGR", 0x1, secure_priv);
  r.write("PWR", "CR2", 0, secure_unpriv);
  CHECK(r.peek("PWR", "CR2") == 0x1);
  r.write("PWR", "CR2", 0, secure_priv);
  CHECK(r.peek("PWR", "CR2") == 0);
}

TEST_CASE("RM0456 10.8.1, P1: PWR_SECCFGR is RAZ/WI under TZEN=0")
{
  rig const r{false};
  r.write("PWR", "SECCFGR", 0xf0ff, nonsecure_priv);
  CHECK(r.read("PWR", "SECCFGR", nonsecure_priv) == 0);
  CHECK(r.peek("PWR", "SECCFGR") == 0);
  r.write("PWR", "CR2", 0x1, nonsecure_unpriv);
  CHECK(r.peek("PWR", "CR2") == 0x1);
}

TEST_CASE("RM0456 15.2.2 Table 131: SYSCFG Secure registers, events and CFGR1 bits")
{
  rig const r{true};
  r.write("SYSCFG", "SECCFGR", bit("SYSCFG", "SECCFGR", "CLASSBSEC"), nonsecure_priv);
  CHECK(r.peek("SYSCFG", "SECCFGR") == 0);
  auto const event = bit("GTZC2_TZIC", "SR2", "SYSCFGF");
  CHECK(r.peek("GTZC2_TZIC", "SR2") == event);
  r.write("GTZC2_TZIC", "FCR2", event, secure_priv);
  r.write("SYSCFG", "CFGR2", 0x1, nonsecure_priv);
  CHECK(r.peek("SYSCFG", "CFGR2") == 0x1);
  CHECK(r.peek("GTZC2_TZIC", "SR2") == 0);

  r.write("SYSCFG", "SECCFGR", bit("SYSCFG", "SECCFGR", "CLASSBSEC"), secure_unpriv);
  CHECK(r.read("SYSCFG", "CFGR2", nonsecure_priv) == 0);
  CHECK(r.peek("GTZC2_TZIC", "SR2") == event);
  CHECK(r.read("SYSCFG", "CFGR2", secure_unpriv) == 0x1);

  r.write(
      "SYSCFG", "SECCFGR",
      bit("SYSCFG", "SECCFGR", "FPUSEC") | bit("SYSCFG", "SECCFGR", "CLASSBSEC"), secure_unpriv);
  CHECK(r.peek("SYSCFG", "SECCFGR") == bit("SYSCFG", "SECCFGR", "CLASSBSEC"));
  r.write("SYSCFG", "CSLCKR", 0x1, secure_unpriv);
  CHECK(r.peek("SYSCFG", "CSLCKR") == 0);
  r.write("SYSCFG", "CSLCKR", 0x1, secure_priv);
  CHECK(r.peek("SYSCFG", "CSLCKR") == 0x1);
  CHECK(r.read("SYSCFG", "CSLCKR", nonsecure_priv) == 0);

  auto const fmp6 = bit("SYSCFG", "CFGR1", "PB6_FMP");
  auto const fmp7 = bit("SYSCFG", "CFGR1", "PB7_FMP");
  r.write("GPIOB", "SECCFGR", 0xffff & ~(1u << 7), secure_priv);
  r.write("SYSCFG", "CFGR1", fmp6 | fmp7, nonsecure_priv);
  CHECK((r.peek("SYSCFG", "CFGR1") & (fmp6 | fmp7)) == fmp7);
}

TEST_CASE("RM0456 15.3.1, P1: SYSCFG_SECCFGR is RAZ/WI under TZEN=0")
{
  rig const r{false};
  r.write("SYSCFG", "SECCFGR", 0xb, nonsecure_priv);
  CHECK(r.read("SYSCFG", "SECCFGR", nonsecure_priv) == 0);
  CHECK(r.peek("SYSCFG", "SECCFGR") == 0);
  r.write("SYSCFG", "CFGR2", 0x1, nonsecure_unpriv);
  CHECK(r.read("SYSCFG", "CFGR2", nonsecure_unpriv) == 0x1);
}
}

namespace
{
auto warnings_of(std::vector<core::log_record> const& logged, std::string_view reg)
    -> std::ptrdiff_t
{
  return std::ranges::count_if(logged, [&](core::log_record const& record) {
    return record.kind == core::log_kind::model_warning
       and record.peripheral == "SYSCFG"
       and record.reg == reg;
  });
}
}

TEST_CASE("SYSCFG storage registers log one warning per register on the first landed write")
{
  for(auto const trustzone : {false, true})
  {
    CAPTURE(trustzone);
    rig const                     r{trustzone};
    std::vector<core::log_record> logged;
    auto const                    sink =
        r.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    r.write("SYSCFG", "CFGR2", 0x1, secure_priv);
    r.write("SYSCFG", "CFGR2", 0x0, secure_priv);
    r.write("SYSCFG", "CCCR", 0x1, secure_priv);
    r.write("SYSCFG", "SECCFGR", 0x1, secure_priv);
    std::ignore = r.read("SYSCFG", "MESR", secure_priv);
    CHECK(warnings_of(logged, "CFGR2") == 1);
    CHECK(warnings_of(logged, "CCCR") == 1);
    CHECK(warnings_of(logged, "SECCFGR") == 0);
    CHECK(warnings_of(logged, "MESR") == 0);
  }
}

TEST_CASE("RM0456 15.2.2 Table 131: a SYSCFG write the TrustZone rules refuse logs no warning")
{
  rig const                     r{true};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  r.write("SYSCFG", "SECCFGR", bit("SYSCFG", "SECCFGR", "SYSCFGSEC"), secure_priv);
  r.write("SYSCFG", "CCCSR", 0x1, nonsecure_priv);
  CHECK(r.peek("SYSCFG", "CCCSR") != 0x1);
  CHECK(warnings_of(logged, "CCCSR") == 0);
  r.write("SYSCFG", "CCCSR", 0x1, secure_priv);
  CHECK(warnings_of(logged, "CCCSR") == 1);
}

TEST_CASE("under strict_peripherals a SYSCFG storage register write raises model_error, unstored")
{
  for(auto const trustzone : {false, true})
  {
    CAPTURE(trustzone);
    rig const  r{trustzone, {.tzen = true}, true};
    auto const at = rig::at("SYSCFG", "CFGR2");
    CHECK_THROWS_AS(
        std::ignore = r.bus->services().write(at, 4, 0x1, secure_priv), popcorn::model_error);
    CHECK(r.peek("SYSCFG", "CFGR2") == 0);
    CHECK_THROWS_AS(
        std::ignore = r.bus->services().write(at, 4, 0x1, secure_priv), popcorn::model_error);
    CHECK_NOTHROW(std::ignore = r.read("SYSCFG", "CFGR2", secure_priv));
    CHECK_NOTHROW(r.write("SYSCFG", "SECCFGR", 0x1, secure_priv));
  }
}

TEST_CASE("a host view access to a SYSCFG storage register raises model_error, SECCFGR opens")
{
  for(auto const trustzone : {false, true})
  {
    CAPTURE(trustzone);
    rig const  r{trustzone};
    auto const at = rig::at("SYSCFG", "CFGR1");
    CHECK_THROWS_AS(std::ignore = r.target->mem().read(at, 4), popcorn::model_error);
    CHECK_THROWS_AS(std::ignore = r.target->mem().write(at, 4, 0x1), popcorn::model_error);
    CHECK_NOTHROW(std::ignore = r.target->mem().read(rig::at("SYSCFG", "SECCFGR"), 4));
    r.poke("SYSCFG", "CFGR1", 0x1);
    CHECK(r.peek("SYSCFG", "CFGR1") == 0x1);
  }
}

TEST_CASE("a SYSCFG snapshot holds no model bytes and restores from the SYSCFG storage")
{
  rig const r{false};
  r.write("SYSCFG", "CFGR2", 0x1, nonsecure_priv);
  auto const taken = r.target->snapshot();
  CHECK(std::ranges::none_of(taken.data().models, [](core::snapshot_part const& part) {
    return part.owner == "SYSCFG";
  }));
  r.write("SYSCFG", "CFGR2", 0, nonsecure_priv);
  REQUIRE_NOTHROW(r.target->restore(taken));
  CHECK(r.peek("SYSCFG", "CFGR2") == 0x1);
}

namespace
{
constexpr address sram1_secure_block    = 0x2000'0000;
constexpr address sram1_nonsecure_block = 0x2000'4000;
constexpr address loop_at               = 0x2000'8000;

void set_register(rig const& r, std::string_view name, std::uint64_t value)
{
  r.target->regs().write(*r.target->regs().find(name), value);
}

void fill(rig const& r, address at, std::uint32_t first, std::size_t words)
{
  for(std::size_t i = 0; i < words; ++i)
  {
    REQUIRE(r.target->mem().write(at + 4 * i, 4, first + static_cast<std::uint32_t>(i)));
  }
}

[[nodiscard]] auto words_at(rig const& r, address at, std::size_t words)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::size_t i = 0; i < words; ++i)
  {
    auto const value = r.target->mem().read(at + 4 * i, 4);
    REQUIRE(value.has_value());
    out.push_back(static_cast<std::uint32_t>(*value));
  }
  return out;
}

void open_destination_block(rig const& r)
{
  r.write("GTZC1_MPCBB1", "SECCFGR1", 0xffff'fffe, secure_priv);
}

void arm_copy(rig const& r, address from, address to, std::uint32_t extra_tr1 = 0)
{
  auto const tr1 = (2u << bit_index("GPDMA1", "C0TR1", "SDW_LOG2"))
                 | bit("GPDMA1", "C0TR1", "SINC")
                 | (2u << bit_index("GPDMA1", "C0TR1", "DDW_LOG2"))
                 | bit("GPDMA1", "C0TR1", "DINC")
                 | extra_tr1;
  r.write("GPDMA1", "PRIVCFGR", 1, secure_priv);
  r.write("GPDMA1", "C0TR1", tr1, secure_priv);
  r.write("GPDMA1", "C0TR2", bit("GPDMA1", "C0TR2", "SWREQ"), secure_priv);
  r.write("GPDMA1", "C0BR1", 16, secure_priv);
  r.write("GPDMA1", "C0SAR", from, secure_priv);
  r.write("GPDMA1", "C0DAR", to, secure_priv);
  r.write(
      "GPDMA1", "C0CR", bit("GPDMA1", "C0CR", "EN") | bit("GPDMA1", "C0CR", "TCIE"), secure_priv);
  REQUIRE(r.target->mem().write(loop_at, 2, 0xe7fe));
  set_register(r, "pc", loop_at | 1u);
}

void run_for(rig const& r, std::uint64_t instructions)
{
  auto const result = r.target->run_slice({.max_instructions = icount{instructions}});
  REQUIRE_FALSE(result.stopped.has_value());
}

void copy(rig const& r, address from, address to, std::uint32_t extra_tr1 = 0)
{
  arm_copy(r, from, to, extra_tr1);
  run_for(r, 20'000);
}

[[nodiscard]] auto status(rig const& r) -> std::uint32_t
{
  return r.peek("GPDMA1", "C0SR");
}
}

TEST_CASE("RM0456 17.4.14, 5.4.2: a Non-secure channel reads Secure SRAM as zeros, no DTEF")
{
  rig const r{true};
  open_destination_block(r);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  fill(r, sram1_nonsecure_block, 0x5555'0000, 4);
  copy(r, sram1_secure_block, sram1_nonsecure_block);
  CHECK(words_at(r, sram1_nonsecure_block, 4) == std::vector<std::uint32_t>(4, 0));
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
  CHECK((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  CHECK((status(r) & bit("GPDMA1", "C0SR", "DTEF")) == 0);
  CHECK(r.peek("GPDMA1", "MISR") == 1);
  CHECK(r.peek("GPDMA1", "SMISR") == 0);
}

TEST_CASE("RM0456 17.4.14, 5.4.2: a Non-secure channel's writes into Secure SRAM are dropped")
{
  rig const r{true};
  open_destination_block(r);
  fill(r, sram1_nonsecure_block, 0x5555'0000, 4);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  copy(r, sram1_nonsecure_block, sram1_secure_block);
  CHECK(
      words_at(r, sram1_secure_block, 4)
      == std::vector<std::uint32_t>{0xc0de'0000, 0xc0de'0001, 0xc0de'0002, 0xc0de'0003});
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
  CHECK((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  CHECK((status(r) & bit("GPDMA1", "C0SR", "DTEF")) == 0);
}

TEST_CASE("RM0456 17.4.14: a Secure channel moves data with SSEC and DSEC, SMISR shows it")
{
  rig const r{true};
  open_destination_block(r);
  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  copy(
      r, sram1_secure_block, sram1_secure_block + 0x100,
      bit("GPDMA1", "C0TR1", "SSEC") | bit("GPDMA1", "C0TR1", "DSEC"));
  CHECK(
      words_at(r, sram1_secure_block + 0x100, 4)
      == std::vector<std::uint32_t>{0xc0de'0000, 0xc0de'0001, 0xc0de'0002, 0xc0de'0003});
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);
  CHECK(r.peek("GPDMA1", "SMISR") == 1);
  CHECK(r.peek("GPDMA1", "MISR") == 0);
  CHECK(r.read("GPDMA1", "SMISR", nonsecure_priv) == 0);
  CHECK(r.read("GPDMA1", "SMISR", secure_priv) == 1);
}

TEST_CASE("RM0456 17.4.14: a Secure channel with DSEC clear writes Non-secure, SSEC clear reads it")
{
  rig const r{true};
  open_destination_block(r);
  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  copy(r, sram1_secure_block, sram1_nonsecure_block, bit("GPDMA1", "C0TR1", "SSEC"));
  CHECK(
      words_at(r, sram1_nonsecure_block, 4)
      == std::vector<std::uint32_t>{0xc0de'0000, 0xc0de'0001, 0xc0de'0002, 0xc0de'0003});
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);

  r.write("GPDMA1", "C0FCR", bit("GPDMA1", "C0FCR", "TCF"), secure_priv);
  copy(r, sram1_nonsecure_block, sram1_nonsecure_block + 0x100);
  CHECK(
      words_at(r, sram1_nonsecure_block + 0x100, 4)
      == std::vector<std::uint32_t>{0xc0de'0000, 0xc0de'0001, 0xc0de'0002, 0xc0de'0003});
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);
}

TEST_CASE("RM0456 17.8.1 to 17.8.3: GPDMA channel registers, SECCFGR, PRIVCFGR and RCFGLOCKR")
{
  rig const r{true};
  r.write("GPDMA1", "SECCFGR", 1, nonsecure_priv);
  CHECK(r.peek("GPDMA1", "SECCFGR") == 0);
  auto const event = bit("GTZC1_TZIC", "SR4", "GPDMA1F");
  CHECK(r.peek("GTZC1_TZIC", "SR4") == event);
  r.write("GTZC1_TZIC", "FCR4", event, secure_priv);
  r.write("GPDMA1", "SECCFGR", 1, secure_unpriv);
  CHECK(r.peek("GPDMA1", "SECCFGR") == 0);

  auto const ssec = bit("GPDMA1", "C0TR1", "SSEC");
  r.write("GPDMA1", "C0TR1", ssec, secure_priv);
  CHECK(r.peek("GPDMA1", "C0TR1") == 0);
  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  r.write("GPDMA1", "C0TR1", ssec, nonsecure_priv);
  CHECK(r.peek("GPDMA1", "C0TR1") == 0);
  r.write("GPDMA1", "C0TR1", ssec, secure_priv);
  CHECK(r.peek("GPDMA1", "C0TR1") == ssec);
  r.write("GPDMA1", "C0SAR", 0x2000'0000, secure_priv);
  CHECK(r.read("GPDMA1", "C0SAR", nonsecure_priv) == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == event);
  r.write("GPDMA1", "C0SAR", 0x2000'0100, nonsecure_priv);
  CHECK(r.peek("GPDMA1", "C0SAR") == 0x2000'0000);
  CHECK(r.read("GPDMA1", "SECCFGR", nonsecure_unpriv) == 1);
  r.write("GPDMA1", "C1SAR", 0x2000'0100, nonsecure_priv);
  CHECK(r.peek("GPDMA1", "C1SAR") == 0x2000'0100);

  r.write("GPDMA1", "PRIVCFGR", 0x3, nonsecure_priv);
  CHECK(r.peek("GPDMA1", "PRIVCFGR") == 0x2);
  CHECK(r.read("GPDMA1", "C1SAR", nonsecure_unpriv) == 0);
  r.write("GPDMA1", "C1SAR", 0, nonsecure_unpriv);
  CHECK(r.peek("GPDMA1", "C1SAR") == 0x2000'0100);

  r.write("GPDMA1", "RCFGLOCKR", 1, nonsecure_priv);
  CHECK(r.peek("GPDMA1", "RCFGLOCKR") == 0);
  r.write("GPDMA1", "RCFGLOCKR", 1, secure_priv);
  r.write("GPDMA1", "RCFGLOCKR", 0, secure_priv);
  CHECK(r.peek("GPDMA1", "RCFGLOCKR") == 1);
  r.write("GPDMA1", "SECCFGR", 0, secure_priv);
  CHECK(r.peek("GPDMA1", "SECCFGR") == 1);
  r.write("GPDMA1", "SECCFGR", 0x3, secure_priv);
  CHECK(r.peek("GPDMA1", "SECCFGR") == 0x3);
  r.write("GPDMA1", "C1TR1", bit("GPDMA1", "C1TR1", "SSEC"), secure_priv);
  REQUIRE(r.peek("GPDMA1", "C1TR1") == bit("GPDMA1", "C1TR1", "SSEC"));
  r.write("GPDMA1", "SECCFGR", 0x1, secure_priv);
  CHECK(r.peek("GPDMA1", "C1TR1") == 0);
}

TEST_CASE(
    "RM0456 17.8.3, P1: DMA RCFGLOCKR ignores writes under TZEN=0, SECCFGR stays storage by "
    "popcorn's rule")
{
  rig const r{false};
  r.write("GPDMA1", "RCFGLOCKR", 0xffff, nonsecure_priv);
  CHECK(r.read("GPDMA1", "RCFGLOCKR", nonsecure_priv) == 0);
  r.write("LPDMA1", "RCFGLOCKR", 0xf, nonsecure_priv);
  CHECK(r.read("LPDMA1", "RCFGLOCKR", nonsecure_priv) == 0);
  r.write("GPDMA1", "SECCFGR", 0x1, nonsecure_priv);
  CHECK(r.read("GPDMA1", "SECCFGR", nonsecure_priv) == 0x1);
}

TEST_CASE("RM0456 17.4.15, 5.4.2: an unprivileged channel reads privileged SRAM as zeros, no event")
{
  rig const r{true};
  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  fill(r, sram1_secure_block + 0x100, 0x5555'0000, 4);
  copy(
      r, sram1_secure_block, sram1_secure_block + 0x100,
      bit("GPDMA1", "C0TR1", "SSEC") | bit("GPDMA1", "C0TR1", "DSEC"));
  r.write("GPDMA1", "PRIVCFGR", 0, secure_priv);
  r.write("GPDMA1", "C0BR1", 16, secure_priv);
  r.write("GPDMA1", "C0SAR", sram1_secure_block, secure_priv);
  r.write("GPDMA1", "C0DAR", sram1_secure_block + 0x200, secure_priv);
  r.write("GPDMA1", "C0FCR", bit("GPDMA1", "C0FCR", "TCF"), secure_priv);
  r.write(
      "GPDMA1", "C0CR", bit("GPDMA1", "C0CR", "EN") | bit("GPDMA1", "C0CR", "TCIE"), secure_priv);
  REQUIRE_FALSE(r.target->run_slice({.max_instructions = icount{20'000}}).stopped.has_value());
  CHECK(
      words_at(r, sram1_secure_block + 0x100, 4)
      == std::vector<std::uint32_t>{0xc0de'0000, 0xc0de'0001, 0xc0de'0002, 0xc0de'0003});
  CHECK(words_at(r, sram1_secure_block + 0x200, 4) == std::vector<std::uint32_t>(4, 0));
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);
  CHECK((status(r) & bit("GPDMA1", "C0SR", "DTEF")) == 0);
}

TEST_CASE("RM0456 18.8.1: an LPDMA Secure channel's registers are RAZ/WI to Non-secure accesses")
{
  rig const r{true};
  r.write("LPDMA1", "SECCFGR", 0x2, secure_priv);
  r.write("LPDMA1", "C1SAR", 0x2800'0000, secure_priv);
  CHECK(r.read("LPDMA1", "C1SAR", nonsecure_priv) == 0);
  r.write("LPDMA1", "C1SAR", 0, nonsecure_priv);
  CHECK(r.peek("LPDMA1", "C1SAR") == 0x2800'0000);
  CHECK(r.peek("GTZC2_TZIC", "SR2") == bit("GTZC2_TZIC", "SR2", "LPDMA1F"));
}

TEST_CASE("Q9: RTC_SECCFGR and TAMP_SECCFGR stay storage, one warning each under TrustZone only")
{
  for(auto const trustzone : {true, false})
  {
    CAPTURE(trustzone);
    rig const                     r{trustzone};
    std::vector<core::log_record> logged;
    auto const                    sink =
        r.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    r.write("RTC", "SECCFGR", 0x1, secure_priv);
    r.write("RTC", "PRIVCR", 0x1, secure_priv);
    r.write("TAMP", "SECCFGR", 0x1, secure_priv);
    r.write("TAMP", "PRIVCR", 0x1, secure_priv);
    CHECK(r.peek("RTC", "SECCFGR") == 0x1);
    CHECK(r.peek("TAMP", "SECCFGR") == 0x1);
    auto const count = [&](std::string_view peripheral) {
      return std::ranges::count_if(logged, [&](core::log_record const& record) {
        return record.kind == core::log_kind::model_warning and record.peripheral == peripheral;
      });
    };
    CHECK(count("RTC") == (trustzone ? 1 : 0));
    CHECK(count("TAMP") == (trustzone ? 1 : 0));
  }
}

TEST_CASE(
    "RM0456 11.5.1: BDRST and the RTC clock bits are Secure with a Secure RTC or TAMP function")
{
  rig const  r{true};
  auto const reset = bit("RCC", "BDCR", "BDRST");
  auto const pulse = [&](bus_attrs attrs) {
    r.write("RCC", "BDCR", r.peek("RCC", "BDCR") | reset, attrs);
    r.write("RCC", "BDCR", r.peek("RCC", "BDCR") & ~reset, secure_priv);
  };
  auto const kept = [&](std::uint32_t seccfgr_tamp, std::uint32_t seccfgr_rtc) {
    r.write("TAMP", "SECCFGR", seccfgr_tamp, secure_priv);
    r.write("RTC", "SECCFGR", seccfgr_rtc, secure_priv);
    r.write("TAMP", "BKP0R", 0x5ec0'0001, secure_priv);
    pulse(nonsecure_priv);
    return r.peek("TAMP", "BKP0R") == 0x5ec0'0001;
  };
  auto const rwsec = std::uint32_t{8} << bit_index("TAMP", "SECCFGR", "BKPRWSEC");
  auto const wsec  = std::uint32_t{2} << bit_index("TAMP", "SECCFGR", "BKPWSEC");

  CHECK(kept(rwsec, 0));
  CHECK((r.peek("RCC", "BDCR") & reset) == 0);
  CHECK(r.read("RCC", "BDCR", nonsecure_priv) == (r.peek("RCC", "BDCR") & ~reset));
  pulse(secure_priv);
  CHECK(r.peek("TAMP", "BKP0R") == 0);
  CHECK(r.peek("TAMP", "SECCFGR") == 0);

  CHECK(kept(wsec, 0));
  CHECK(kept(bit("TAMP", "SECCFGR", "TAMPSEC"), 0));
  CHECK(kept(bit("TAMP", "SECCFGR", "CNT1SEC"), 0));
  CHECK(kept(0, bit("RTC", "SECCFGR", "INITSEC")));
  CHECK(kept(0, bit("RTC", "SECCFGR", "SEC")));
  CHECK_FALSE(kept(bit("TAMP", "SECCFGR", "BHKLOCK"), 0));
  CHECK_FALSE(kept(0, 0));

  auto const clock = bit("RCC", "APB3ENR", "RTCAPBEN");
  r.write("RCC", "APB3ENR", r.peek("RCC", "APB3ENR") & ~clock, nonsecure_priv);
  REQUIRE((r.peek("RCC", "APB3ENR") & clock) == 0);
  r.write("RTC", "SECCFGR", bit("RTC", "SECCFGR", "SEC"), secure_priv);
  r.write("RCC", "APB3ENR", r.peek("RCC", "APB3ENR") | clock, nonsecure_priv);
  CHECK((r.peek("RCC", "APB3ENR") & clock) == 0);
  r.write("RCC", "APB3ENR", r.peek("RCC", "APB3ENR") | clock, secure_priv);
  CHECK((r.peek("RCC", "APB3ENR") & clock) == clock);
}

TEST_CASE("RM0456 17.4.14: a Secure channel loads its linked-list item with Secure transfers")
{
  rig const  r{true};
  auto const item   = sram1_secure_block + 0x300;
  auto const target = sram1_secure_block + 0x400;
  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  fill(r, sram1_secure_block, 0xc0de'0000, 8);
  REQUIRE(r.target->mem().write(item, 4, target));
  REQUIRE(r.target->mem().write(item + 4, 4, 0));
  r.write("GPDMA1", "C0LBAR", sram1_secure_block & 0xffff'0000u, secure_priv);
  r.write(
      "GPDMA1", "C0LLR",
      (item & 0xfffcu) | bit("GPDMA1", "C0LLR", "UDA") | bit("GPDMA1", "C0LLR", "ULL"),
      secure_priv);
  copy(
      r, sram1_secure_block, sram1_secure_block + 0x100,
      bit("GPDMA1", "C0TR1", "SSEC") | bit("GPDMA1", "C0TR1", "DSEC"));
  CHECK(
      words_at(r, target, 4)
      == std::vector<std::uint32_t>{0xc0de'0004, 0xc0de'0005, 0xc0de'0006, 0xc0de'0007});
  CHECK((status(r) & bit("GPDMA1", "C0SR", "ULEF")) == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);
}

TEST_CASE("RM0456 17.4.14: a Non-secure channel loads its linked-list item Non-secure, as zeros")
{
  rig const  r{true};
  auto const item   = sram1_secure_block + 0x300;
  auto const target = sram1_nonsecure_block + 0x180;
  open_destination_block(r);
  fill(r, sram1_nonsecure_block, 0x5555'0000, 8);
  REQUIRE(r.target->mem().write(item, 4, target));
  REQUIRE(r.target->mem().write(item + 4, 4, 0));
  r.write("GPDMA1", "C0LBAR", sram1_secure_block & 0xffff'0000u, secure_priv);
  r.write(
      "GPDMA1", "C0LLR",
      (item & 0xfffcu) | bit("GPDMA1", "C0LLR", "UDA") | bit("GPDMA1", "C0LLR", "ULL"),
      secure_priv);
  copy(r, sram1_nonsecure_block, sram1_nonsecure_block + 0x100);
  CHECK(words_at(r, target, 4) == std::vector<std::uint32_t>(4, 0));
  CHECK(r.peek("GPDMA1", "C0DAR") != target + 16);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
}

TEST_CASE("RM0456 17.8.7: a Non-secure channel's linked-list load cannot set SSEC or DSEC")
{
  rig const  r{true};
  auto const item    = sram1_nonsecure_block + 0x1c0;
  auto const secured = bit("GPDMA1", "C0TR1", "SSEC") | bit("GPDMA1", "C0TR1", "DSEC");
  auto const reused  = sram1_nonsecure_block + 0x180;
  auto const widths  = (2u << bit_index("GPDMA1", "C0TR1", "SDW_LOG2"))
                     | bit("GPDMA1", "C0TR1", "SINC")
                     | (2u << bit_index("GPDMA1", "C0TR1", "DDW_LOG2"))
                     | bit("GPDMA1", "C0TR1", "DINC");
  open_destination_block(r);
  fill(r, sram1_nonsecure_block, 0x5555'0000, 4);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  fill(r, reused, 0xaaaa'0000, 4);
  REQUIRE(r.target->mem().write(item, 4, widths | secured));
  REQUIRE(r.target->mem().write(item + 4, 4, 0));
  r.write("GPDMA1", "C0LBAR", sram1_nonsecure_block & 0xffff'0000u, secure_priv);
  r.write(
      "GPDMA1", "C0LLR",
      (item & 0xfffcu) | bit("GPDMA1", "C0LLR", "UT1") | bit("GPDMA1", "C0LLR", "ULL"),
      secure_priv);
  copy(r, sram1_nonsecure_block, sram1_nonsecure_block + 0x100);
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  CHECK(r.read("GPDMA1", "C0TR1", secure_priv) == widths);
  CHECK(r.peek("GPDMA1", "C0TR1") == widths);

  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  REQUIRE(r.peek("GPDMA1", "SECCFGR") == 1);
  CHECK(r.peek("GPDMA1", "C0TR1") == widths);
  r.write("GPDMA1", "C0FCR", bit("GPDMA1", "C0FCR", "TCF"), secure_priv);
  r.write("GPDMA1", "C0BR1", 16, secure_priv);
  r.write("GPDMA1", "C0SAR", sram1_secure_block, secure_priv);
  r.write("GPDMA1", "C0DAR", reused, secure_priv);
  r.write("GPDMA1", "C0CR", bit("GPDMA1", "C0CR", "EN"), secure_priv);
  auto const result = r.target->run_slice({.max_instructions = icount{20'000}});
  REQUIRE_FALSE(result.stopped.has_value());
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  CHECK(words_at(r, reused, 4) == std::vector<std::uint32_t>(4, 0));
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
}

TEST_CASE("RM0456 23.5.1: a Secure line's EXTICRm byte is RAZ/WI to Non-secure accesses")
{
  rig const r{true};
  r.write("EXTI", "SECCFGR1", 1u << 5, secure_priv);
  r.write("EXTI", "EXTICR2", 0x0202, secure_priv);
  REQUIRE(r.peek("EXTI", "EXTICR2") == 0x0202);
  CHECK(r.read("EXTI", "EXTICR2", nonsecure_priv) == 0x0002);
  CHECK(r.read("EXTI", "EXTICR2", secure_priv) == 0x0202);
  r.write("EXTI", "EXTICR2", 0x0101, nonsecure_priv);
  CHECK(r.peek("EXTI", "EXTICR2") == 0x0201);
  r.write("EXTI", "EXTICR3", 0x0303, nonsecure_priv);
  CHECK(r.peek("EXTI", "EXTICR3") == 0x0303);
}

TEST_CASE(
    "RM0456 13.3.18 Table 128: a Secure pin's IDR, LCKR and HSLVR bits are RAZ/WI to Non-secure")
{
  rig const r{true};
  r.write("GPIOA", "SECCFGR", 0xffff & ~(1u << 5), secure_priv);
  r.write("GPIOA", "MODER", r.peek("GPIOA", "MODER") & ~(0xfu << 8), secure_priv);
  r.port("GPIOA").drive(4, true);
  r.port("GPIOA").drive(5, true);
  REQUIRE((r.peek("GPIOA", "IDR") & 0x30u) == 0x30u);
  CHECK((r.read("GPIOA", "IDR", nonsecure_priv) & 0x30u) == 0x20u);
  CHECK((r.read("GPIOA", "IDR", secure_priv) & 0x30u) == 0x30u);

  r.write("GPIOA", "LCKR", 1u << 4 | 1u << 5, nonsecure_priv);
  CHECK(r.peek("GPIOA", "LCKR") == 1u << 5);
  r.write("GPIOA", "LCKR", 1u << 4 | 1u << 5, secure_priv);
  CHECK(r.read("GPIOA", "LCKR", nonsecure_priv) == 1u << 5);

  r.write("GPIOA", "HSLVR", 1u << 4 | 1u << 5, nonsecure_priv);
  CHECK(r.peek("GPIOA", "HSLVR") == 1u << 5);
  r.write("GPIOA", "HSLVR", 1u << 4 | 1u << 5, secure_priv);
  CHECK(r.peek("GPIOA", "HSLVR") == (1u << 4 | 1u << 5));
  CHECK(r.read("GPIOA", "HSLVR", nonsecure_priv) == 1u << 5);
}

TEST_CASE("RM0456 17.4.15: GPDMA_MISR hides a privileged channel from an unprivileged read")
{
  rig const r{true};
  open_destination_block(r);
  fill(r, sram1_nonsecure_block, 0x5555'0000, 4);
  copy(r, sram1_nonsecure_block, sram1_nonsecure_block + 0x100);
  REQUIRE(r.peek("GPDMA1", "PRIVCFGR") == 1);
  REQUIRE(r.peek("GPDMA1", "MISR") == 1);
  CHECK(r.read("GPDMA1", "MISR", nonsecure_priv) == 1);
  CHECK(r.read("GPDMA1", "MISR", nonsecure_unpriv) == 0);
  r.write("GPDMA1", "PRIVCFGR", 0, secure_priv);
  CHECK(r.read("GPDMA1", "MISR", nonsecure_unpriv) == 1);
}

TEST_CASE("RM0456 10.8 Table 109: PWR_UCPDR is RAZ/WI to Non-secure while the TZSC secures UCPD1")
{
  rig const  r{true};
  auto const ucpd = bit("PWR", "UCPDR", "UCPD_DBDIS");
  r.write("PWR", "UCPDR", ucpd, nonsecure_priv);
  REQUIRE(r.peek("PWR", "UCPDR") == ucpd);
  r.write("PWR", "UCPDR", 0, nonsecure_priv);
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "UCPD1SEC"), secure_priv);
  r.write("PWR", "UCPDR", ucpd, nonsecure_priv);
  CHECK(r.peek("PWR", "UCPDR") == 0);
  r.write("PWR", "UCPDR", ucpd, secure_priv);
  CHECK(r.peek("PWR", "UCPDR") == ucpd);
  CHECK(r.read("PWR", "UCPDR", nonsecure_priv) == 0);
  CHECK(r.read("PWR", "UCPDR", secure_priv) == ucpd);
}

TEST_CASE("a DMA transfer drops its firewall grant on an MPCBB write between two bursts")
{
  rig const  r{true};
  auto const to = sram1_nonsecure_block + 0x100;
  open_destination_block(r);
  fill(r, sram1_nonsecure_block, 0x5555'0000, 4);
  fill(r, to, 0xaaaa'0000, 4);
  arm_copy(r, sram1_nonsecure_block, to);
  run_for(r, 6);
  auto const done = (16 - r.peek("GPDMA1", "C0BR1")) / 4;
  REQUIRE(done >= 1);
  REQUIRE(done < 4);
  r.write("GTZC1_MPCBB1", "SECCFGR1", 0xffff'ffff, secure_priv);
  run_for(r, 20'000);
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  auto const words = words_at(r, to, 4);
  for(std::uint32_t i = 0; i < 4; ++i)
  {
    CAPTURE(i);
    CHECK(words[i] == (i < done ? 0x5555'0000 + i : 0xaaaa'0000 + i));
  }
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
}

TEST_CASE(
    "popcorn takes a CxTR1 write while EN, RM0456 17.8.10 makes it read-only: the transfer drops "
    "its cached attributes on it between two bursts")
{
  rig const  r{true};
  auto const to = sram1_secure_block + 0x100;
  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  fill(r, to, 0xaaaa'0000, 4);
  auto const secured = bit("GPDMA1", "C0TR1", "SSEC") | bit("GPDMA1", "C0TR1", "DSEC");
  arm_copy(r, sram1_secure_block, to, secured);
  run_for(r, 6);
  auto const done = (16 - r.peek("GPDMA1", "C0BR1")) / 4;
  REQUIRE(done >= 1);
  REQUIRE(done < 4);
  r.write(
      "GPDMA1", "C0TR1", r.peek("GPDMA1", "C0TR1") & ~bit("GPDMA1", "C0TR1", "SSEC"), secure_priv);
  run_for(r, 20'000);
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  auto const words = words_at(r, to, 4);
  for(std::uint32_t i = 0; i < 4; ++i)
  {
    CAPTURE(i);
    CHECK(words[i] == (i < done ? 0xc0de'0000 + i : 0));
  }
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
}

TEST_CASE("RM0456 17.4.14: a Secure channel turned Non-secure and restarted reads Secure SRAM as 0")
{
  rig const  r{true};
  auto const secured = bit("GPDMA1", "C0TR1", "SSEC") | bit("GPDMA1", "C0TR1", "DSEC");
  auto const to      = sram1_nonsecure_block + 0x100;
  open_destination_block(r);
  r.write("GPDMA1", "SECCFGR", 1, secure_priv);
  fill(r, sram1_secure_block, 0xc0de'0000, 4);
  copy(r, sram1_secure_block, sram1_secure_block + 0x100, secured);
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  REQUIRE(r.peek("GTZC1_TZIC", "SR4") == 0);

  r.write("GPDMA1", "SECCFGR", 0, secure_priv);
  REQUIRE((r.peek("GPDMA1", "C0TR1") & secured) == 0);
  fill(r, to, 0xaaaa'0000, 4);
  r.write("GPDMA1", "C0FCR", bit("GPDMA1", "C0FCR", "TCF"), nonsecure_priv);
  r.write("GPDMA1", "C0BR1", 16, nonsecure_priv);
  r.write("GPDMA1", "C0SAR", sram1_secure_block, nonsecure_priv);
  r.write("GPDMA1", "C0DAR", to, nonsecure_priv);
  r.write("GPDMA1", "C0CR", bit("GPDMA1", "C0CR", "EN"), nonsecure_priv);
  run_for(r, 20'000);
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  CHECK(words_at(r, to, 4) == std::vector<std::uint32_t>(4, 0));
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
}

TEST_CASE("a restore drops the firewall grant a DMA transfer took after the snapshot")
{
  rig const  r{true};
  auto const to = sram1_nonsecure_block + 0x100;
  fill(r, sram1_nonsecure_block, 0x5555'0000, 4);
  fill(r, to, 0xaaaa'0000, 4);
  arm_copy(r, sram1_nonsecure_block, to);
  run_for(r, 6);
  auto const done = (16 - r.peek("GPDMA1", "C0BR1")) / 4;
  REQUIRE(done >= 1);
  REQUIRE(done < 3);
  auto const saved = r.target->snapshot();
  open_destination_block(r);
  run_for(r, 4);
  REQUIRE((16 - r.peek("GPDMA1", "C0BR1")) / 4 > done);
  r.target->restore(saved);
  run_for(r, 20'000);
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  CHECK(
      words_at(r, to, 4)
      == std::vector<std::uint32_t>{0xaaaa'0000, 0xaaaa'0001, 0xaaaa'0002, 0xaaaa'0003});
}

TEST_CASE("a DMA firewall grant stops at the MPCBB block end, the next block asked again")
{
  rig const  r{true};
  auto const from = sram1_nonsecure_block + 0x1f8;
  auto const to   = sram1_nonsecure_block + 0x100;
  open_destination_block(r);
  fill(r, from, 0x5555'0000, 4);
  fill(r, to, 0xaaaa'0000, 4);
  copy(r, from, to);
  REQUIRE((status(r) & bit("GPDMA1", "C0SR", "TCF")) != 0);
  CHECK(words_at(r, to, 4) == std::vector<std::uint32_t>{0x5555'0000, 0x5555'0001, 0, 0});
  CHECK(r.peek("GTZC1_TZIC", "SR4") == bit("GTZC1_TZIC", "SR4", "SRAM1F"));
}

TEST_CASE("a filtered debugger flash write grant ends at its page, a Secure page asked again")
{
  rig const r{
      true, {.secwm1 = stm32::page_range{.first = 0, .last = 0}, .secwm2 = stm32::no_area{}}
  };
  auto const          secure_page    = address{0x0800'0000};
  auto const          nonsecure_page = address{0x0800'2000};
  constexpr bus_attrs filtered{
      .by = initiator::debugger, .secure = false, .privileged = true, .filtered = true};
  auto const before = r.target->mem().read(secure_page + 0x1ffc, 4);
  REQUIRE(before.has_value());
  core::memory_grant grant;
  REQUIRE(r.bus->services().write(nonsecure_page, 4, 0x1234'5678, filtered, grant).has_value());
  CHECK(r.target->mem().read(nonsecure_page, 4) == 0x1234'5678u);
  CHECK(grant.first == nonsecure_page);
  REQUIRE(
      r.bus->services().write(secure_page + 0x1ffc, 4, 0x5ec0'0001, filtered, grant).has_value());
  CHECK(r.target->mem().read(secure_page + 0x1ffc, 4) == before);
}

TEST_CASE("a filtered debugger block access goes through the bus firewall")
{
  rig const  r{true};
  auto const secret = std::uint32_t{0x5ec0'0001};
  REQUIRE(r.target->mem().write(sram1_secure_block, 4, secret));
  constexpr bus_attrs filtered{
      .by = initiator::debugger, .secure = false, .privileged = true, .filtered = true};
  std::array<std::byte, 8> out{};
  out.fill(std::byte{0xff});
  REQUIRE(r.bus->services().read_block(sram1_secure_block, out, filtered).has_value());
  CHECK(std::ranges::all_of(out, [](std::byte b) { return b == std::byte{0}; }));
  std::array<std::byte, 4> const in{};
  REQUIRE(r.bus->services().write_block(sram1_secure_block, in, filtered).has_value());
  CHECK(words_at(r, sram1_secure_block, 1) == std::vector<std::uint32_t>{secret});
}
