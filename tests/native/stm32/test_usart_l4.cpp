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
constexpr std::string_view hal           = "l476_l4_usart_hal";
constexpr std::string_view dma_ll        = "l476_l4_dma_ll";
constexpr std::uint64_t    lse_hz        = 32'768;
constexpr address          lpuart1       = 0x4000'8000;
constexpr address          rqr           = 0x18;
constexpr address          apb1enr2      = 0x4002'105C;
constexpr std::uint32_t    rtse          = 1u << 8;
constexpr std::uint32_t    ctse          = 1u << 9;
constexpr std::uint32_t    sbkrq         = 1u << 1;
constexpr std::uint32_t    sbkf          = 1u << 18;

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
  std::shared_ptr<periph::capture_endpoint> lpuart = std::make_shared<periph::capture_endpoint>();
  std::unique_ptr<stm32_fixture>            f;
  std::vector<core::log_record>             logged;
  core::hook_handle                         sink;

  explicit rig(
      std::string_view        entry,
      bool                    strict = false,
      part_description const& desc   = l476rc(),
      std::string_view        elf    = firmware)
  {
    stm32::board board{};
    board.uarts.emplace("USART2", usart2);
    board.uarts.emplace("UART4", uart4);
    board.uarts.emplace("LPUART1", lpuart);
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    options.models             = stm32::factories(board, desc);
    options.models.insert_or_assign(
        "TSC", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f    = std::make_unique<stm32_fixture>(desc, elf, std::move(options));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    if(entry != "main")
    {
      f->set("pc", sym(elf, entry));
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

TEST_CASE("RM0351 41.4.4: LPUART1 at 9600 on LSE and 115200 on PCLK1 takes 256 x fck over BRR")
{
  rig lse{"lpuart_lse"};
  CHECK(lse.finish() == std::vector<std::uint32_t>{874});
  CHECK(text(*lse.lpuart) == hello);
  back_to_back(*lse.lpuart, 10 * ps_per_second * 874 / (256 * lse_hz));
  rig pclk{"lpuart_pclk1"};
  CHECK(pclk.finish() == std::vector<std::uint32_t>{8889});
  CHECK(text(*pclk.lpuart) == hello);
  back_to_back(*pclk.lpuart, 10 * ps_per_second * 8889 / (256 * msi_hz));
}

TEST_CASE(
    "RM0351 41.4.4: LPUART1 BRR 0x2FF sends nothing, 0x300 sends, an RQR bit 4 write does nothing")
{
  rig r{"idle"};
  r.write(apb1enr2, r.read(apb1enr2) | 1u);
  r.write(lpuart1 + brr, 0x2FF);
  r.write(lpuart1 + cr1, ue | te);
  r.cpu_write(lpuart1 + tdr, 'q');
  r.idle_to(r.now_ps() + 1'000'000'000);
  CHECK(r.lpuart->frames.empty());
  r.write(lpuart1 + cr1, 0);
  r.write(lpuart1 + brr, 0x300);
  r.write(lpuart1 + cr1, ue | te);
  auto const isr_before = r.read(lpuart1 + isr);
  r.cpu_write(lpuart1 + rqr, 1u << 4);
  CHECK(r.read(lpuart1 + isr) == isr_before);
  r.cpu_write(lpuart1 + tdr, 'q');
  r.idle_to(r.now_ps() + 1'000'000'000);
  REQUIRE(r.lpuart->frames.size() == 1);
  CHECK(r.lpuart->frames[0].data == 'q');
}

TEST_CASE("RM0351 11.6.7: DMA1 channel 7 at CSELR code 2 feeds USART2 from TXE, TC after the last")
{
  rig r{"dma_tx"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0x0200'0000u, 0});
  CHECK(text(*r.usart2) == hello);
  back_to_back(*r.usart2, pclk1_frame);
}

TEST_CASE("RM0351 11.6.7: DMA1 channel 6 at CSELR code 2 takes each USART2 word from RXNE")
{
  rig r{"dma_rx"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  std::array<periph::host_frame, 4> frames{
      {{.data = 'a'}, {.data = 'b'}, {.data = 'c'}, {.data = 'd'}}
  };
  r.port("USART2").receive_frames(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{'a', 'd', 0, 0});
}

TEST_CASE(
    "RM0351 40.8.8: frames left unread set ORE and IDLE, ICR clears both, RDR keeps the first")
{
  rig r{"idle_overrun"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  std::array<periph::host_frame, 2> frames{
      {{.data = 'a'}, {.data = 'b'}}
  };
  r.port("USART2").receive_frames(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{rxne | (1u << 4) | (1u << 3), rxne, 'a', 0});
}

TEST_CASE(
    "HAL_UART_Transmit_DMA sends hello on USART2 through DMA1, HAL_UART_Receive_IT takes four")
{
  rig        r{"main", false, l476rc(), hal};
  auto const sent = r.f->run_to_stop({.max_instructions = icount{2'000'000}, .stop_when = [&] {
                                        return r.usart2->frames.size() == hello.size();
                                      }});
  REQUIRE(sent.reason == stop_reason::user);
  r.idle_to(r.now_ps() + 2 * pclk1_frame);
  std::array<periph::host_frame, 4> frames{
      {{.data = 'w'}, {.data = 'x'}, {.data = 'y'}, {.data = 'z'}}
  };
  r.port("USART2").receive_frames(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{0x0200'0000u, 'w', 'z'});
  CHECK(text(*r.usart2) == hello);
}

TEST_CASE("the l4_dma_ll channel 7 sequence runs on the USART2 TX request of the USART model")
{
  rig        r{"main", false, l476rc(), dma_ll};
  auto const first = r.f->run_to_stop({.max_instructions = icount{100'000}});
  REQUIRE(first.reason == stop_reason::user);
  REQUIRE(r.f->log() == std::vector<std::uint32_t>{0x7, 0, 1, 1, 0x0200'0000u});
  r.write(apb1enr1, r.read(apb1enr1) | (1u << 17));
  r.write(usart2 + brr, pclk1_brr);
  r.write(usart2 + cr3, 1u << 7);
  r.write(usart2 + cr1, ue | te);
  REQUIRE(r.f->run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  CHECK(r.f->log() == std::vector<std::uint32_t>{0x7, 0, 1, 1, 0x0200'0000u, 0, 1});
  CHECK(r.usart2->frames.empty());
}

TEST_CASE(
    "ES0250 2.21.2: a break requested while D1 shifts with CTS deasserted keeps TC 0 after D1, "
    "with CTS ready TC sets")
{
  for(auto const ready : {false, true})
  {
    CAPTURE(ready);
    rig r{"idle"};
    r.enable_usart2(0);
    r.write(usart2 + cr3, ctse);
    r.write(usart2 + cr1, ue | te);
    auto const start = r.now_ps();
    r.cpu_write(usart2 + tdr, 'x');
    r.cpu_write(usart2 + rqr, sbkrq);
    r.port("USART2").set_cts(ready, vtime{start + pclk1_frame / 2});
    r.idle_to(start + pclk1_frame + pclk1_frame / 10);
    CHECK((r.read(usart2 + isr) & tc) == (ready ? tc : 0));
    CHECK((r.read(usart2 + isr) & sbkf) != 0);
    if(not ready)
    {
      r.idle_to(start + 4 * pclk1_frame);
      CHECK((r.read(usart2 + isr) & (tc | sbkf)) == sbkf);
    }
    r.port("USART2").set_cts(true, vtime{r.now_ps()});
    r.idle_to(r.now_ps() + 3 * pclk1_frame);
    CHECK((r.read(usart2 + isr) & (tc | sbkf)) == tc);
  }
}

TEST_CASE("ES0250 2.21.3: with RTSE the receiver reads ready while RE or UE is 0")
{
  rig r{"idle"};
  r.enable_usart2(0);
  r.write(usart2 + cr3, rtse);
  CHECK(r.port("USART2").ready_to_receive());
  r.write(usart2 + cr1, ue);
  CHECK(r.port("USART2").ready_to_receive());
}

namespace
{
constexpr address cr2 = 0x04;

struct unit
{
  std::string_view name;
  address          base;
  address          clock_register;
  std::uint32_t    clock_bit;
  bool             low_power;
};

constexpr std::array<unit, 3> units{
    {{"USART2", usart2, apb1enr1, 17, false},
     {"UART4", uart4, apb1enr1, 19, false},
     {"LPUART1", lpuart1, apb1enr2, 0, true}}
};

auto field_mask(unit const& one, std::string_view reg_name, std::string_view field_name)
    -> std::uint32_t
{
  static auto const desc = l476rc();
  auto const        peripheral =
      std::ranges::find(desc.peripherals, one.name, &popcorn::peripheral_desc::name);
  REQUIRE(peripheral != desc.peripherals.end());
  auto const reg_found =
      std::ranges::find(peripheral->registers, reg_name, &popcorn::register_desc::name);
  REQUIRE(reg_found != peripheral->registers.end());
  auto const field_found =
      std::ranges::find(reg_found->fields, field_name, &popcorn::field_desc::name);
  REQUIRE(field_found != reg_found->fields.end());
  return (field_found->width >= 32 ? 0xFFFF'FFFFu : ((1u << field_found->width) - 1))
      << field_found->lsb;
}

auto has_field(unit const& one, std::string_view reg_name, std::string_view field_name) -> bool
{
  static auto const desc = l476rc();
  auto const        peripheral =
      std::ranges::find(desc.peripherals, one.name, &popcorn::peripheral_desc::name);
  REQUIRE(peripheral != desc.peripherals.end());
  auto const reg_found =
      std::ranges::find(peripheral->registers, reg_name, &popcorn::register_desc::name);
  REQUIRE(reg_found != peripheral->registers.end());
  return std::ranges::contains(reg_found->fields, field_name, &popcorn::field_desc::name);
}

struct lock_scene
{
  rig  r;
  unit u;

  lock_scene(unit const& one, bool strict = false) : r{"idle", strict}, u{one}
  {
    r.write(u.clock_register, r.read(u.clock_register) | (1u << u.clock_bit));
  }

  void cpu(address offset, std::uint32_t value) const
  {
    r.cpu_write(u.base + offset, value);
  }

  [[nodiscard]] auto read(address offset) const -> std::uint32_t
  {
    return r.read(u.base + offset);
  }

  [[nodiscard]] auto refuses(address offset, std::uint32_t value) const -> bool
  {
    try
    {
      static_cast<void>(r.services().write(u.base + offset, 4, value, popcorn::initiator::cpu));
    }
    catch(popcorn::model_error const&)
    {
      return true;
    }
    return false;
  }

  [[nodiscard]] auto lock_records(std::string_view reg_name) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(r.logged, [&](core::log_record const& record) {
      return record.reg == reg_name
         and record.message.find("written with UE 1") != std::string::npos;
    });
  }
};

struct locked_field
{
  std::string_view reg_name;
  address          offset;
  std::string_view field_name;
  std::string_view section;
};

constexpr std::array<locked_field, 8> l4_cr1{
    {{"CR1", cr1, "M0", "40.8.1"},
     {"CR1", cr1, "M1", "40.8.1"},
     {"CR1", cr1, "OVER8", "40.8.1"},
     {"CR1", cr1, "PCE", "40.8.1"},
     {"CR1", cr1, "PS", "40.8.1"},
     {"CR1", cr1, "WAKE", "40.8.1"},
     {"CR1", cr1, "DEAT", "40.8.1"},
     {"CR1", cr1, "DEDT", "40.8.1"}}
};
constexpr std::array<locked_field, 13> l4_cr2{
    {{"CR2", cr2, "CLKEN", "40.8.2"},
     {"CR2", cr2, "CPHA", "40.8.2"},
     {"CR2", cr2, "CPOL", "40.8.2"},
     {"CR2", cr2, "DATAINV", "40.8.2"},
     {"CR2", cr2, "LBCL", "40.8.2"},
     {"CR2", cr2, "LBDL", "40.8.2"},
     {"CR2", cr2, "LINEN", "40.8.2"},
     {"CR2", cr2, "MSBFIRST", "40.8.2"},
     {"CR2", cr2, "RXINV", "40.8.2"},
     {"CR2", cr2, "STOP", "40.8.2"},
     {"CR2", cr2, "SWAP", "40.8.2"},
     {"CR2", cr2, "TXINV", "40.8.2"},
     {"CR2", cr2, "ADDM7", "40.8.2"}}
};
constexpr std::array<locked_field, 13> l4_cr3{
    {{"CR3", cr3, "DDRE", "40.8.3"},
     {"CR3", cr3, "DEM", "40.8.3"},
     {"CR3", cr3, "DEP", "40.8.3"},
     {"CR3", cr3, "HDSEL", "40.8.3"},
     {"CR3", cr3, "IREN", "40.8.3"},
     {"CR3", cr3, "IRLP", "40.8.3"},
     {"CR3", cr3, "NACK", "40.8.3"},
     {"CR3", cr3, "ONEBIT", "40.8.3"},
     {"CR3", cr3, "OVRDIS", "40.8.3"},
     {"CR3", cr3, "RTSE", "40.8.3"},
     {"CR3", cr3, "SCEN", "40.8.3"},
     {"CR3", cr3, "CTSE", "40.8.3"},
     {"CR3", cr3, "WUS", "40.8.3"}}
};
constexpr std::array<locked_field, 14> l4_lpuart_cr1_2{
    {{"CR1", cr1, "M0", "41.7.1"},
     {"CR1", cr1, "M1", "41.7.1"},
     {"CR1", cr1, "PCE", "41.7.1"},
     {"CR1", cr1, "PS", "41.7.1"},
     {"CR1", cr1, "WAKE", "41.7.1"},
     {"CR1", cr1, "DEAT", "41.7.1"},
     {"CR1", cr1, "DEDT", "41.7.1"},
     {"CR2", cr2, "DATAINV", "41.7.2"},
     {"CR2", cr2, "MSBFIRST", "41.7.2"},
     {"CR2", cr2, "RXINV", "41.7.2"},
     {"CR2", cr2, "SWAP", "41.7.2"},
     {"CR2", cr2, "TXINV", "41.7.2"},
     {"CR2", cr2, "STOP", "41.7.2"},
     {"CR2", cr2, "ADDM7", "41.7.2"}}
};
constexpr std::array<locked_field, 8> l4_lpuart_cr3{
    {{"CR3", cr3, "DDRE", "41.7.3"},
     {"CR3", cr3, "DEM", "41.7.3"},
     {"CR3", cr3, "DEP", "41.7.3"},
     {"CR3", cr3, "HDSEL", "41.7.3"},
     {"CR3", cr3, "OVRDIS", "41.7.3"},
     {"CR3", cr3, "RTSE", "41.7.3"},
     {"CR3", cr3, "CTSE", "41.7.3"},
     {"CR3", cr3, "WUS", "41.7.3"}}
};

void pin_locked(unit const& one, locked_field const& entry)
{
  CAPTURE(one.name);
  CAPTURE(entry.field_name);
  lock_scene t{one};
  auto const mask = field_mask(one, entry.reg_name, entry.field_name);
  if(entry.offset == cr1)
  {
    t.cpu(cr1, mask | ue);
    REQUIRE(t.read(cr1) == (mask | ue));
    t.cpu(cr1, ue);
    CHECK(t.read(cr1) == (mask | ue));
    t.cpu(cr1, 0);
    CHECK(t.read(cr1) == 0);
    return;
  }
  t.cpu(entry.offset, mask);
  REQUIRE(t.read(entry.offset) == mask);
  t.cpu(cr1, ue);
  t.cpu(entry.offset, 0);
  CHECK(t.read(entry.offset) == mask);
  t.cpu(cr1, 0);
  t.cpu(entry.offset, 0);
  CHECK(t.read(entry.offset) == 0);
}
}

TEST_CASE("RM0351 40.8.1 to 40.8.3 and 41.7.1 to 41.7.3: every UE-locked L4 field holds with UE 1")
{
  auto const pin_each = [](unit const& one, auto const& table) {
    for(auto const& entry : table)
    {
      SUBCASE(
          std::string{one.name}
              .append(" ")
              .append(entry.section)
              .append(" ")
              .append(entry.field_name)
              .c_str())
      {
        pin_locked(one, entry);
      }
    }
  };
  pin_each(units[0], l4_cr1);
  pin_each(units[0], l4_cr2);
  pin_each(units[0], l4_cr3);
  pin_each(units[2], l4_lpuart_cr1_2);
  pin_each(units[2], l4_lpuart_cr3);
}

TEST_CASE("RM0351 40.8.1 to 40.8.4: UART4 holds its UE-locked fields with UE 1")
{
  for(auto const& entry : {l4_cr1[0], l4_cr2[1], l4_cr2[9], l4_cr3[0], l4_cr3[12]})
  {
    pin_locked(units[1], entry);
  }
  lock_scene t{units[1]};
  t.cpu(brr, 0x45);
  t.cpu(cr1, ue);
  t.cpu(brr, 0x90);
  CHECK(t.read(brr) == 0x45);
}

TEST_CASE("RM0351 40.8.4 and 41.7.4: BRR holds with UE 1 on USART2 and LPUART1")
{
  for(auto const& one : {units[0], units[2]})
  {
    CAPTURE(one.name);
    lock_scene t{one};
    t.cpu(brr, 0x345);
    t.cpu(cr1, ue);
    t.cpu(brr, 0x400);
    CHECK(t.read(brr) == 0x345);
    t.cpu(cr1, 0);
    t.cpu(brr, 0x400);
    CHECK(t.read(brr) == 0x400);
  }
}

TEST_CASE("RM0351 40.8.2: CLKEN written with UE 1 keeps its value on an L4 USART and logs once")
{
  lock_scene t{units[0]};
  auto const clken = field_mask(units[0], "CR2", "CLKEN");
  t.cpu(cr1, ue);
  t.cpu(cr2, clken);
  t.cpu(cr2, clken);
  CHECK(t.read(cr2) == 0);
  CHECK(t.lock_records("CR2") == 1);
}

TEST_CASE("RM0351 41.7.2: LPUART1 CR2 has no CLKEN field, the L4 USART CR2 has it")
{
  CHECK_FALSE(has_field(units[2], "CR2", "CLKEN"));
  CHECK(has_field(units[0], "CR2", "CLKEN"));
}

TEST_CASE("RM0351 40.8.3 and 41.7.3: WUS written with UE 1 keeps its value on USART2 and LPUART1")
{
  for(auto const& one : {units[0], units[2]})
  {
    CAPTURE(one.name);
    lock_scene t{one};
    auto const wus = field_mask(one, "CR3", "WUS");
    t.cpu(cr3, wus & (2u << 20));
    t.cpu(cr1, ue);
    CHECK(t.read(cr3) == (wus & (2u << 20)));
    t.cpu(cr3, wus & (1u << 20));
    CHECK(t.read(cr3) == (wus & (2u << 20)));
    CHECK(t.lock_records("CR3") == 1);
    t.cpu(cr3, 0);
    CHECK(t.read(cr3) == (wus & (2u << 20)));
    CHECK(t.lock_records("CR3") == 1);
    CHECK(t.r.stop_mode_warnings() == 1);
  }
}

TEST_CASE(
    "RM0351 40.8.3 and 41.7.3: a WUS write is a model_error under strict, the field reads old")
{
  for(auto const& one : {units[0], units[2]})
  {
    CAPTURE(one.name);
    lock_scene t{one, true};
    auto const wus = field_mask(one, "CR3", "WUS") & (1u << 20);
    t.r.write(one.base + cr3, wus);
    t.r.write(one.base + cr1, ue);
    CHECK(t.refuses(cr3, 0));
    CHECK(t.read(cr3) == wus);
  }
}

TEST_CASE(
    "RM0351 40.8.2 and 41.7.2: ADD with UE 1 is taken with RE 0 and kept with RE 1, L4 USART and "
    "LPUART")
{
  for(auto const& one : {units[0], units[2]})
  {
    CAPTURE(one.name);
    lock_scene t{one};
    auto const add  = field_mask(one, "CR2", "ADD");
    auto const low  = add & (0x5Au << 24);
    auto const high = add & (0x11u << 24);
    t.cpu(cr1, ue);
    t.cpu(cr2, low);
    CHECK(t.read(cr2) == low);
    CHECK(t.lock_records("CR2") == 0);
    t.cpu(cr1, ue | re);
    t.cpu(cr2, high);
    CHECK(t.read(cr2) == low);
    CHECK(t.lock_records("CR2") == 1);
    t.cpu(cr1, ue);
    t.cpu(cr2, high);
    CHECK(t.read(cr2) == high);
    t.cpu(cr1, ue | re);
    t.cpu(cr2, 0);
    CHECK(t.read(cr2) == high);
  }
}

TEST_CASE("RM0351 40.8.2: ABRMOD is locked only while ABREN is 1 on an L4 USART")
{
  lock_scene t{units[0]};
  auto const abren = field_mask(units[0], "CR2", "ABREN");
  auto const mode  = field_mask(units[0], "CR2", "ABRMOD") & (1u << 21);
  t.cpu(cr1, ue);
  t.cpu(cr2, mode);
  CHECK(t.read(cr2) == mode);
  t.cpu(cr1, 0);
  t.cpu(cr2, abren | mode);
  t.cpu(cr1, ue);
  t.cpu(cr2, abren);
  CHECK(t.read(cr2) == (abren | mode));
}

TEST_CASE("the L4 lock warning names RM0351")
{
  for(auto const& one : units)
  {
    CAPTURE(one.name);
    lock_scene t{one};
    t.cpu(cr1, ue);
    t.cpu(brr, 0x345);
    auto const found = std::ranges::find_if(t.r.logged, [](core::log_record const& record) {
      return record.message.find("written with UE 1") != std::string::npos;
    });
    REQUIRE(found != t.r.logged.end());
    CHECK(found->message.find("RM0351") != std::string::npos);
    CHECK(found->message.find("RM0456") == std::string::npos);
  }
}

TEST_CASE("an L4 locked write is a model_error under strict peripherals and the field reads old")
{
  struct leg
  {
    std::size_t   unit_index;
    address       offset;
    std::uint32_t value;
    std::uint32_t control1;
  };
  for(auto const& entry : std::array<leg, 6>{
          {{0, cr2, 1u << 11, ue},
           {0, cr2, 0x5Au << 24, ue | re},
           {0, brr, 0x345, ue},
           {0, cr1, 1u << 12, ue},
           {2, brr, 0x345, ue},
           {2, cr2, 0x5Au << 24, ue | re}}
  })
  {
    auto const& one = units[entry.unit_index];
    CAPTURE(one.name);
    CAPTURE(entry.offset);
    lock_scene t{one, true};
    if(entry.offset == cr1)
    {
      t.r.write(one.base + cr1, entry.control1 | entry.value);
      CHECK(t.refuses(cr1, entry.control1));
      CHECK(t.read(cr1) == (entry.control1 | entry.value));
      continue;
    }
    t.r.write(one.base + entry.offset, entry.value);
    t.r.write(one.base + cr1, entry.control1);
    CHECK(t.refuses(entry.offset, 0));
    CHECK(t.read(entry.offset) == entry.value);
  }
}

TEST_CASE(
    "RM0351 40.8.3: SCARCNT takes 0 with UE 1 and keeps a non-zero write on USART2, a debugger "
    "write passes")
{
  lock_scene t{units[0]};
  auto const count = field_mask(units[0], "CR3", "SCARCNT");
  auto const five  = count & (5u << 17);
  auto const three = count & (3u << 17);
  t.cpu(cr3, five);
  t.cpu(cr1, ue);
  t.cpu(cr3, three);
  CHECK(t.read(cr3) == five);
  t.cpu(cr3, 0);
  CHECK(t.read(cr3) == 0);
  t.cpu(cr3, three);
  CHECK(t.read(cr3) == 0);
  t.r.write(usart2 + cr3, three);
  CHECK(t.read(cr3) == three);
}

TEST_CASE(
    "RM0351 40.8.3 and 41.7.3: setting WUS logs once and raises under strict, clearing it passes")
{
  for(auto const& one : {units[0], units[2]})
  {
    CAPTURE(one.name);
    auto const wus = field_mask(one, "CR3", "WUS");
    lock_scene t{one};
    t.cpu(cr3, wus);
    CHECK(t.read(cr3) == wus);
    CHECK(t.r.stop_mode_warnings() == 1);
    t.cpu(cr3, 0);
    t.cpu(cr3, wus);
    CHECK(t.r.stop_mode_warnings() == 1);
    lock_scene c{one};
    c.r.write(one.base + cr3, wus);
    c.cpu(cr3, 0);
    CHECK(c.read(cr3) == 0);
    CHECK(c.r.stop_mode_warnings() == 0);
    lock_scene s{one, true};
    CHECK(s.refuses(cr3, wus));
    CHECK(s.read(cr3) == 0);
  }
}
