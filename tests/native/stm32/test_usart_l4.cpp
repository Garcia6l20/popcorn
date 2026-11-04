#include "../support/inert_model.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>
#include <popcorn/periph/uart.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware      = "l476_l4_usart_ll";
constexpr std::string_view hello         = "hello\r\n";
constexpr std::uint64_t    ps_per_second = 1'000'000'000'000;
constexpr std::uint64_t    msi_hz        = 4'000'000;
constexpr std::uint64_t    hsi16_hz      = 16'000'000;
constexpr address          usart2        = 0x4000'4400;
constexpr address          uart4         = 0x4000'4C00;
constexpr address          cr1           = 0x00;
constexpr address          cr3           = 0x08;
constexpr address          brr           = 0x0C;
constexpr address          isr           = 0x1C;
constexpr address          icr           = 0x20;
constexpr address          tdr           = 0x28;
constexpr address          apb1enr1      = 0x4002'1058;
constexpr address          nvic_ispr1    = 0xE000'E204;
constexpr address          nvic_icpr1    = 0xE000'E284;
constexpr std::uint32_t    ue            = 1u << 0;
constexpr std::uint32_t    uesm          = 1u << 1;
constexpr std::uint32_t    re            = 1u << 2;
constexpr std::uint32_t    te            = 1u << 3;
constexpr std::uint32_t    rxneie        = 1u << 5;
constexpr std::uint32_t    txeie         = 1u << 7;
constexpr std::uint32_t    wufie         = 1u << 22;
constexpr std::uint32_t    nf            = 1u << 2;
constexpr std::uint32_t    rxne          = 1u << 5;
constexpr std::uint32_t    tc            = 1u << 6;
constexpr std::uint32_t    txe           = 1u << 7;
constexpr std::uint32_t    usart2_irq    = 1u << (38 - 32);
constexpr std::uint32_t    uart4_irq     = 1u << (52 - 32);
constexpr std::uint32_t    pclk1_brr     = 35;
constexpr std::uint64_t    pclk1_frame   = 10 * ps_per_second * pclk1_brr / msi_hz;

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;
};

struct rig
{
  std::shared_ptr<periph::capture_endpoint> usart2 = std::make_shared<periph::capture_endpoint>();
  std::shared_ptr<periph::capture_endpoint> uart4  = std::make_shared<periph::capture_endpoint>();
  std::unique_ptr<stm32_fixture>            f;
  std::vector<core::log_record>             logged;
  core::hook_handle                         sink;

  explicit rig(std::string_view entry, bool strict = false, part_description const& desc = l476rc())
  {
    stm32::board board{};
    board.uarts.emplace("USART2", usart2);
    board.uarts.emplace("UART4", uart4);
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    options.models             = stm32::factories(board, desc);
    options.models.insert_or_assign(
        "TSC", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f    = std::make_unique<stm32_fixture>(desc, firmware, std::move(options));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    if(entry != "main")
    {
      f->set("pc", sym(firmware, entry));
    }
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{2'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }

  [[nodiscard]] auto read(address at) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(address at, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at, 4, value).has_value());
  }

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    auto* const probe = f->model_of<bus_probe>("TSC");
    REQUIRE(probe != nullptr);
    return probe->context.services;
  }

  void cpu_write(address at, std::uint32_t value) const
  {
    REQUIRE(services().write(at, 4, value, popcorn::initiator::cpu).has_value());
  }

  void idle_to(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void enable_usart2(std::uint32_t control1) const
  {
    write(apb1enr1, read(apb1enr1) | (1u << 17));
    write(::usart2 + brr, pclk1_brr);
    write(::usart2 + cr1, control1);
  }

  auto port(std::string_view unit) const -> stm32::uart_link_port&
  {
    auto* const found = f->model_of<stm32::uart_link_port>(unit);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto stop_mode_warnings() const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [](core::log_record const& record) {
      return record.message.find("Stop mode") != std::string::npos;
    });
  }
};

auto text(periph::capture_endpoint const& capture) -> std::string
{
  std::string out;
  for(auto const& frame : capture.frames)
  {
    out.push_back(static_cast<char>(frame.data));
  }
  return out;
}

void back_to_back(periph::capture_endpoint const& capture, std::uint64_t frame)
{
  REQUIRE(capture.frames.size() > 1);
  for(std::size_t index = 1; index < capture.frames.size(); ++index)
  {
    CAPTURE(index);
    CHECK(capture.frames[index].at.ps - capture.frames[index - 1].at.ps == frame);
  }
}

auto with_usart2(std::function<void(popcorn::peripheral_desc&)> const& edit) -> part_description
{
  auto       desc  = l476rc();
  auto const found = std::ranges::find(
      desc.peripherals, std::string_view{"USART2"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  edit(*found);
  return desc;
}

auto register_of(popcorn::peripheral_desc& unit, std::string_view name) -> popcorn::register_desc&
{
  auto const found = std::ranges::find(unit.registers, name, &popcorn::register_desc::name);
  REQUIRE(found != unit.registers.end());
  return *found;
}

auto erase_field(popcorn::register_desc& reg, std::string_view name) -> std::size_t
{
  return std::erase_if(
      reg.fields, [&](popcorn::field_desc const& one) { return one.name == name; });
}
}

TEST_CASE(
    "RM0351 40.8.8: ISR resets to TXE and TC, bit 25 0 inferred, USART2 sends hello at 115200")
{
  rig r{"main"};
  CHECK(r.finish() == std::vector<std::uint32_t>{txe | tc, pclk1_brr, 1});
  CHECK(text(*r.usart2) == hello);
  back_to_back(*r.usart2, pclk1_frame);
}

TEST_CASE("RM0351 6.4.28: USART2 on HSI16 through CCIPR.USART2SEL keeps BRR 139 frames")
{
  rig r{"hsi16_poll"};
  CHECK(r.finish() == std::vector<std::uint32_t>{139});
  CHECK(text(*r.usart2) == hello);
  back_to_back(*r.usart2, 10 * ps_per_second * 139 / hsi16_hz);
}

TEST_CASE("RM0351 40.8.1: TXEIE drives hello from UART4_IRQn 52 at PCLK1")
{
  rig r{"uart4_tx_irq"};
  CHECK(r.finish() == std::vector<std::uint32_t>{7});
  CHECK(text(*r.uart4) == hello);
  back_to_back(*r.uart4, pclk1_frame);
}

TEST_CASE("RM0351 40.8.1: RXNEIE takes each host frame into UART4_IRQn")
{
  rig r{"uart4_rx_irq"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  CHECK((r.read(nvic_ispr1) & uart4_irq) == 0);
  std::array<periph::host_frame, 4> frames{
      {{.data = 'a'}, {.data = 'b'}, {.data = 'c'}, {.data = 'd'}}
  };
  r.port("UART4").receive_frames(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{4, 'a', 'b', 'c', 'd'});
}

TEST_CASE("RM0351 40.8.8: TXE and RXNE raise USART2_IRQn 38 through TXEIE and RXNEIE")
{
  rig r{"idle"};
  r.enable_usart2(ue | te | re);
  CHECK(r.read(usart2 + isr) == (txe | tc | (1u << 21) | (1u << 22)));
  CHECK((r.read(nvic_ispr1) & usart2_irq) == 0);
  r.write(usart2 + cr1, ue | te | re | txeie);
  CHECK((r.read(nvic_ispr1) & usart2_irq) != 0);
  r.write(usart2 + cr1, ue | te | re | rxneie);
  r.write(nvic_icpr1, usart2_irq);
  CHECK((r.read(nvic_ispr1) & usart2_irq) == 0);
  std::array<periph::host_frame, 1> frame{{{.data = 'z'}}};
  r.port("USART2").receive_frames(frame);
  r.idle_to(r.now_ps() + 2 * pclk1_frame);
  CHECK((r.read(usart2 + isr) & rxne) != 0);
  CHECK((r.read(nvic_ispr1) & usart2_irq) != 0);
}

TEST_CASE("RM0351 40.8.8 and 40.8.9: a host frame tagged with noise sets NF, ICR.NCF clears it")
{
  rig r{"idle"};
  r.enable_usart2(ue | re);
  std::array<periph::host_frame, 1> frame{{{.data = 'n', .error = periph::uart_error::noise}}};
  r.port("USART2").receive_frames(frame);
  r.idle_to(r.now_ps() + 2 * pclk1_frame);
  CHECK((r.read(usart2 + isr) & (nf | rxne)) == (nf | rxne));
  r.cpu_write(usart2 + icr, nf);
  CHECK((r.read(usart2 + isr) & (nf | rxne)) == rxne);
}

TEST_CASE("RM0351 40.8: offset 0x2C holds no PRESC, a CPU write there leaves the frame time")
{
  rig r{"idle"};
  r.enable_usart2(ue | te);
  static_cast<void>(r.services().write(usart2 + 0x2C, 4, 1, popcorn::initiator::cpu));
  auto const start = r.now_ps();
  r.cpu_write(usart2 + tdr, 'p');
  r.idle_to(start + 2 * pclk1_frame);
  REQUIRE(r.usart2->frames.size() == 1);
  CHECK(r.usart2->frames[0].at.ps - start == pclk1_frame);
}

TEST_CASE("an L4 USART description without the single BRR field or ISR.TXE throws config_error")
{
  auto const split_brr = with_usart2([](popcorn::peripheral_desc& unit) {
    auto& baud = register_of(unit, "BRR");
    REQUIRE(baud.fields.size() == 1);
    baud.fields[0].name  = "DIV_Fraction";
    baud.fields[0].width = 4;
    auto mantissa        = baud.fields[0];
    mantissa.name        = "DIV_Mantissa";
    mantissa.lsb         = 4;
    mantissa.width       = 12;
    baud.fields.push_back(mantissa);
  });
  CHECK_THROWS_WITH_AS(
      (rig{"idle", false, split_brr}), doctest::Contains("BRR.BRR"), popcorn::config_error);
  auto const no_txe = with_usart2([](popcorn::peripheral_desc& unit) {
    REQUIRE(erase_field(register_of(unit, "ISR"), "TXE") == 1);
  });
  CHECK_THROWS_WITH_AS(
      (rig{"idle", false, no_txe}), doctest::Contains("ISR.TXE"), popcorn::config_error);
  auto const no_ncf = with_usart2([](popcorn::peripheral_desc& unit) {
    REQUIRE(erase_field(register_of(unit, "ICR"), "NCF") == 1);
  });
  CHECK_THROWS_WITH_AS(
      (rig{"idle", false, no_ncf}), doctest::Contains("ICR.NCF"), popcorn::config_error);
}

TEST_CASE("a write setting UESM or WUFIE logs once and raises under strict, a kept bit passes")
{
  rig r{"idle"};
  r.enable_usart2(0);
  r.cpu_write(usart2 + cr3, wufie);
  r.cpu_write(usart2 + cr1, uesm);
  CHECK(r.read(usart2 + cr3) == wufie);
  CHECK(r.read(usart2 + cr1) == uesm);
  CHECK(r.stop_mode_warnings() == 1);
  rig s{"idle", true};
  s.enable_usart2(0);
  CHECK_THROWS_AS(
      static_cast<void>(s.services().write(usart2 + cr3, 4, wufie, popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(s.read(usart2 + cr3) == 0);
  CHECK_THROWS_AS(
      static_cast<void>(s.services().write(uart4 + cr1, 4, uesm, popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(s.read(uart4 + cr1) == 0);
  s.write(usart2 + cr3, wufie);
  CHECK(s.read(usart2 + cr3) == wufie);
  s.write(usart2 + cr1, uesm);
  s.cpu_write(usart2 + cr1, uesm | te);
  CHECK(s.read(usart2 + cr1) == (uesm | te));
  s.cpu_write(usart2 + cr1, te);
  CHECK_THROWS_AS(
      static_cast<void>(s.services().write(usart2 + cr1, 4, uesm | te, popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(s.read(usart2 + cr1) == te);
}

TEST_CASE("a DMA write setting UESM raises under strict before the store")
{
  rig s{"idle", true};
  s.enable_usart2(0);
  CHECK_THROWS_AS(
      static_cast<void>(s.services().write(usart2 + cr1, 4, uesm, popcorn::initiator::dma)),
      popcorn::model_error);
  CHECK(s.read(usart2 + cr1) == 0);
}
