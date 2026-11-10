#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace
{
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint32_t moder   = 0x00;
constexpr std::uint32_t otyper  = 0x04;
constexpr std::uint32_t pupdr   = 0x0C;
constexpr std::uint32_t afrl    = 0x20;
constexpr std::uint32_t seccfgr = 0x30;
constexpr std::uint32_t idr     = 0x10;
constexpr std::uint32_t odr     = 0x14;
constexpr std::uint32_t bsrr    = 0x18;
constexpr std::uint32_t brr     = 0x28;

constexpr std::uint32_t rcc_ahb3enr = 0x94;
constexpr std::uint8_t  pa1         = 1;
constexpr std::uint8_t  pa3         = 3;

auto access(bool secure) -> popcorn::periph::access_ctx
{
  return {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu, .secure = secure};
}

struct rig
{
  std::unique_ptr<stm32_fixture> f;

  explicit rig(bool trustzone = false, popcorn::part_description const& part = u575())
  {
    stm32::register_builtin_models();
    auto options   = popcorn::testing::stm32_options();
    options.raz_wi = stm32::raz_wi_map(part);
    if(trustzone)
    {
      REQUIRE(stm32::configure_option_bytes(part, {.tzen = true}, options).has_value());
    }
    f = std::make_unique<stm32_fixture>(part, "rcc_ll", std::move(options));
    f->set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  }

  [[nodiscard]] auto port() const -> stm32::gpio_port&
  {
    auto* const found = f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(found != nullptr);
    return *found;
  }

  void write(
      std::string_view peripheral,
      std::uint32_t    offset,
      std::uint32_t    value,
      std::uint8_t     size   = 4,
      bool             secure = true) const
  {
    auto& regs = f->target->peripheral(peripheral);
    REQUIRE(f->target->model(peripheral)
                ->on_write(regs, offset, size, regs.peek(offset & ~3u), value, access(secure))
                .has_value());
  }

  [[nodiscard]] auto
      read(std::string_view peripheral, std::uint32_t offset, bool secure = true) const
      -> std::uint32_t
  {
    auto&      regs  = f->target->peripheral(peripheral);
    auto const value = f->target->model(peripheral)->on_read(regs, offset, 4, access(secure));
    REQUIRE(value.has_value());
    return *value;
  }

  void lpgpio_pad(std::uint8_t pin) const
  {
    write("GPIOA", moder, (read("GPIOA", moder) & ~(3u << (2 * pin))) | (2u << (2 * pin)));
    write("GPIOA", afrl, (read("GPIOA", afrl) & ~(0xFu << (4 * pin))) | (11u << (4 * pin)));
  }
};
}

TEST_CASE("RM0456 14.4: MODER and ODR store 16 bits, BSRR and BRR read 0, a BSRR set wins")
{
  rig const r;
  r.write("LPGPIO1", moder, 0xFFFF'FFFF);
  CHECK(r.read("LPGPIO1", moder) == 0xFFFF);
  r.write("LPGPIO1", odr, 0x0003);
  CHECK(r.read("LPGPIO1", odr) == 0x0003);
  r.write("LPGPIO1", bsrr, (0x0002u << 16) | 0x0004u);
  CHECK(r.read("LPGPIO1", odr) == 0x0005);
  CHECK(r.read("LPGPIO1", bsrr) == 0);
  r.write("LPGPIO1", bsrr, (0x0004u << 16) | 0x0004u);
  CHECK(r.read("LPGPIO1", odr) == 0x0005);
  r.write("LPGPIO1", brr, 0x0001);
  CHECK(r.read("LPGPIO1", odr) == 0x0004);
  CHECK(r.read("LPGPIO1", brr) == 0);
  r.write("LPGPIO1", bsrr + 2, 0x0004, 2);
  CHECK(r.read("LPGPIO1", odr) == 0x0000);
  r.write("LPGPIO1", odr + 1, 0x80, 1);
  CHECK(r.read("LPGPIO1", odr) == 0x8000);
}

TEST_CASE("an output pin drives the pad that is in the LPGPIO alternate function")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  CHECK(r.port().level(pa1));
  r.write("LPGPIO1", brr, 1);
  CHECK_FALSE(r.port().level(pa1));
  r.write("LPGPIO1", bsrr, 1);
  CHECK(r.port().level(pa1));
  r.write("LPGPIO1", moder, 0);
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("a pad outside the alternate function mode is not driven, and a drive waits for it")
{
  rig const r;
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  CHECK_FALSE(r.port().level(pa1));
  r.lpgpio_pad(pa1);
  CHECK(r.port().level(pa1));
  r.write("GPIOA", moder, r.read("GPIOA", moder) & ~(3u << 2));
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("an open-drain pad pulls low for a low ODR and floats to its pull for a high one")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("GPIOA", otyper, 1u << pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  CHECK_FALSE(r.port().level(pa1));
  r.write("GPIOA", pupdr, 1u << (2 * pa1));
  CHECK(r.port().level(pa1));
  r.write("LPGPIO1", odr, 0);
  CHECK_FALSE(r.port().level(pa1));
  r.write("LPGPIO1", odr, 1);
  r.port().drive(pa1, false);
  CHECK_FALSE(r.port().level(pa1));
  r.port().drive(pa1, std::nullopt);
  CHECK(r.port().level(pa1));
}

TEST_CASE("a push-pull LPGPIO level wins over a board drive of the pad, the output pin precedence")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  r.port().drive(pa1, false);
  CHECK(r.port().level(pa1));
  r.write("LPGPIO1", odr, 0);
  r.port().drive(pa1, true);
  CHECK_FALSE(r.port().level(pa1));
  r.write("LPGPIO1", moder, 0);
  CHECK(r.port().level(pa1));
  r.port().drive(pa1, std::nullopt);
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("an open-drain pad takes the board drive for an LPGPIO high, an LPGPIO low wins")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("GPIOA", otyper, 1u << pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  r.port().drive(pa1, true);
  CHECK(r.port().level(pa1));
  r.port().drive(pa1, false);
  CHECK_FALSE(r.port().level(pa1));
  r.write("LPGPIO1", odr, 0);
  r.port().drive(pa1, true);
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("IDR reads the pad level of a pin in the LPGPIO alternate function, else 0")
{
  rig const r;
  r.port().drive(pa1, true);
  r.port().drive(pa3, true);
  CHECK(r.read("LPGPIO1", idr) == 0);
  r.lpgpio_pad(pa1);
  CHECK(r.read("LPGPIO1", idr) == 0x1);
  r.lpgpio_pad(pa3);
  CHECK(r.read("LPGPIO1", idr) == 0x3);
  r.port().drive(pa1, false);
  CHECK(r.read("LPGPIO1", idr) == 0x2);
  r.write("LPGPIO1", idr, 0xFFFF);
  CHECK(r.read("LPGPIO1", idr) == 0x2);
}

TEST_CASE("IDR sees the pad an output pin drives, on the pad of its own pin number")
{
  rig const r;
  r.lpgpio_pad(pa3);
  r.write("LPGPIO1", moder, 1u << 1);
  r.write("LPGPIO1", odr, 1u << 1);
  CHECK(r.port().level(pa3));
  CHECK_FALSE(r.port().level(pa1));
  CHECK(r.read("LPGPIO1", idr) == 0x2);
}

TEST_CASE("a software reset restores the registers and releases the pads")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  REQUIRE(r.port().level(pa1));
  r.f->target->reset(popcorn::reset_kind::software);
  CHECK(r.read("LPGPIO1", moder) == 0);
  CHECK(r.read("LPGPIO1", odr) == 0);
  r.lpgpio_pad(pa1);
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("an LPGPIO1RST pulse restores the registers and releases the pads")
{
  rig const            r;
  stm32_fixture const& f = *r.f;
  popcorn::testing::enable_clocks(
      *f.target, {
                     {"RCC_AHB3ENR", "LPGPIO1EN"}
  });
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  REQUIRE(r.port().level(pa1));
  popcorn::testing::rcc_reset_pulse(f, "RCC_AHB3RSTR", "LPGPIO1RST");
  CHECK(r.read("LPGPIO1", moder) == 0);
  CHECK(r.read("LPGPIO1", odr) == 0);
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("the register access goes through the LPGPIO1EN clock gate")
{
  rig const  r;
  auto const base = r.f->target->peripheral("LPGPIO1").description().base;
  REQUIRE(r.f->target->mem().write(base + moder, 4, 0x1).has_value());
  popcorn::testing::enable_clocks(
      *r.f->target, {
                        {"RCC_AHB3ENR", "LPGPIO1EN"}
  });
  REQUIRE(r.f->target->mem().write(base + moder, 4, 0x1).has_value());
  CHECK(r.read("LPGPIO1", moder) == 1);
}

TEST_CASE("a snapshot keeps the pad drive and the registers")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  auto const saved = r.f->target->snapshot();
  r.write("LPGPIO1", odr, 0);
  REQUIRE_FALSE(r.port().level(pa1));
  r.f->target->restore(saved);
  CHECK(r.port().level(pa1));
  r.port().drive(5, true);
  CHECK(r.port().level(pa1));
  CHECK(r.read("LPGPIO1", odr) == 1);
  r.write("LPGPIO1", odr, 0);
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("RM0456 14.3.5: a Non-secure access to a pin whose pad is secure is RAZ/WI, no bus error")
{
  rig const r{true};
  r.write("GPIOA", seccfgr, 1u << pa1);
  r.lpgpio_pad(pa1);
  r.lpgpio_pad(pa3);
  r.write("LPGPIO1", moder, 0x3);
  r.write("LPGPIO1", odr, 0x3);
  r.port().drive(pa1, true);
  r.port().drive(pa3, true);
  CHECK(r.read("LPGPIO1", moder, false) == 0x2);
  CHECK(r.read("LPGPIO1", odr, false) == 0x2);
  CHECK(r.read("LPGPIO1", idr, false) == 0x2);
  CHECK(r.read("LPGPIO1", moder, true) == 0x3);
  CHECK(r.read("LPGPIO1", idr, true) == 0x3);
  r.write("LPGPIO1", moder, 0, 4, false);
  CHECK(r.read("LPGPIO1", moder) == 0x1);
  r.write("LPGPIO1", odr, 0xFFFF, 4, false);
  CHECK(r.read("LPGPIO1", odr) == 0x7);
}

TEST_CASE("a Non-secure BSRR and BRR leave the bits of a secure pad, both halves of BSRR")
{
  rig const r{true};
  r.write("GPIOA", seccfgr, 1u << pa1);
  r.lpgpio_pad(pa1);
  r.lpgpio_pad(pa3);
  r.write("LPGPIO1", bsrr, 0x1, 4, false);
  CHECK(r.read("LPGPIO1", odr) == 0);
  r.write("LPGPIO1", odr, 0x1);
  r.write("LPGPIO1", bsrr, (0x1u << 16) | 0x2u, 4, false);
  CHECK(r.read("LPGPIO1", odr) == 0x3);
  r.write("LPGPIO1", bsrr, 0x2u << 16, 4, false);
  CHECK(r.read("LPGPIO1", odr) == 0x1);
  r.write("LPGPIO1", odr, 0x3);
  r.write("LPGPIO1", brr, 0x3, 4, false);
  CHECK(r.read("LPGPIO1", odr) == 0x1);
  r.write("LPGPIO1", brr, 0x3);
  CHECK(r.read("LPGPIO1", odr) == 0);
}

TEST_CASE("the Non-secure filter follows the pad's SECCFGR bit as it changes")
{
  rig const r{true};
  r.write("GPIOA", seccfgr, 0);
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 1);
  CHECK(r.read("LPGPIO1", moder, false) == 1);
  r.write("GPIOA", seccfgr, 1u << pa1);
  CHECK(r.read("LPGPIO1", moder, false) == 0);
  r.write("GPIOA", seccfgr, 0);
  CHECK(r.read("LPGPIO1", moder, false) == 1);
}

TEST_CASE("under TZEN=0 a Non-secure access reaches every bit")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 3, 4, false);
  CHECK(r.read("LPGPIO1", moder, false) == 3);
}

TEST_CASE("drive_chip_digital refuses a pin the port lacks")
{
  rig const r;
  CHECK_THROWS_AS(r.port().drive_chip_digital(16, true), popcorn::config_error);
}

TEST_CASE("a chip drive reaches the change listeners once per level change")
{
  rig const r;
  r.lpgpio_pad(pa1);
  auto heard = 0;
  auto sub   = r.port().on_change([&](std::uint8_t pin, bool) { heard += pin == pa1 ? 1 : 0; });
  r.port().drive_chip_digital(pa1, true);
  CHECK(heard == 1);
  r.port().drive_chip_digital(pa1, true);
  CHECK(heard == 1);
  r.port().drive_chip_digital(pa1, false);
  CHECK(heard == 2);
  r.port().drive_chip_digital(pa1, std::nullopt);
  CHECK(heard == 2);
}

TEST_CASE("a description without an lpgpio1_p<y> AF11 alternate keeps LPGPIO1 unmodelled")
{
  stm32::register_builtin_models();
  auto desc = u575();
  for(auto& p : desc.peripherals)
  {
    if(p.alternates)
    {
      std::erase_if(*p.alternates, [](auto const& a) { return a.signal.starts_with("lpgpio1_p"); });
    }
  }
  stm32_fixture const f{std::move(desc), "rcc_ll", popcorn::testing::stm32_options()};
  CHECK(f.target->model("LPGPIO1") == nullptr);
  CHECK(f.target->model("GPIOA") != nullptr);
}

TEST_CASE("a pad outside AFSEL 11 takes the drive and reads 0 in IDR, inferred")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.write("GPIOA", afrl, (r.read("GPIOA", afrl) & ~(0xFu << (4 * pa1))) | (7u << (4 * pa1)));
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  CHECK(r.port().level(pa1));
  CHECK(r.read("LPGPIO1", idr) == 0);
}

TEST_CASE("PA1 driven high reads 1 in IDR at AFSEL 11 and 0 at AFSEL 1, which TIM2_CH2 takes")
{
  rig const r;
  r.lpgpio_pad(pa1);
  r.port().drive(pa1, true);
  CHECK(r.read("LPGPIO1", idr) == 0x1);
  r.write("GPIOA", afrl, (r.read("GPIOA", afrl) & ~(0xFu << (4 * pa1))) | (1u << (4 * pa1)));
  CHECK(r.read("LPGPIO1", idr) == 0);
}

TEST_CASE("a part that bonds fewer pads stores the bits of an unbonded pin and reads 0 for it")
{
  rig const r{false, popcorn::testing::u575cgu()};
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 0x21);
  r.write("LPGPIO1", odr, 0x21);
  CHECK(r.read("LPGPIO1", moder) == 0x21);
  CHECK(r.read("LPGPIO1", odr) == 0x21);
  CHECK(r.port().level(pa1));
  CHECK(r.read("LPGPIO1", idr) == 0x1);
}

TEST_CASE("an STM32U585 drives and reads its pads")
{
  rig const r{false, popcorn::testing::u585()};
  r.lpgpio_pad(pa1);
  r.write("LPGPIO1", moder, 1);
  r.write("LPGPIO1", odr, 1);
  CHECK(r.port().level(pa1));
  CHECK(r.read("LPGPIO1", idr) == 0x1);
  r.write("LPGPIO1", brr, 1);
  CHECK_FALSE(r.port().level(pa1));
}

TEST_CASE("RM0456 14.3.6: the LPGPIO1 clock bit is Secure while a pad is, through secure_function")
{
  rig const  r{true};
  auto const clock = [&](bool secure) { return (r.read("RCC", rcc_ahb3enr, secure) & 1u) != 0; };
  r.write("RCC", rcc_ahb3enr, 1);
  CHECK(clock(true));
  CHECK_FALSE(clock(false));
  for(auto const port : {"GPIOA", "GPIOB", "GPIOC", "GPIOD", "GPIOE"})
  {
    r.write(port, seccfgr, 0);
  }
  CHECK(clock(false));
  r.write("GPIOD", seccfgr, 1u << 13);
  CHECK_FALSE(clock(false));
}
