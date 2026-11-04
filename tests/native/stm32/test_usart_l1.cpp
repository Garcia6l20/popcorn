#include "../support/inert_model.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>
#include <popcorn/periph/uart.hpp>

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
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware      = "l152xe_l1_usart_ll";
constexpr std::string_view firmware_b    = "l152xb_l1_usart_ll";
constexpr std::string_view hello         = "hello\r\n";
constexpr std::uint64_t    ps_per_second = 1'000'000'000'000;
constexpr std::uint64_t    msi_hz        = 2'097'152;
constexpr address          usart1        = 0x4001'3800;
constexpr address          rcc_cfgr      = 0x4002'3800 + 0x08;
constexpr address          sr            = 0x00;
constexpr address          dr            = 0x04;
constexpr address          cr1           = 0x0C;
constexpr std::uint32_t    rxne          = 1u << 5;
constexpr std::uint32_t    tc            = 1u << 6;
constexpr std::uint32_t    txe           = 1u << 7;
constexpr std::uint32_t    sbk           = 1u << 0;
constexpr std::uint32_t    te            = 1u << 3;
constexpr std::uint32_t    re            = 1u << 2;
constexpr std::uint32_t    ue            = 1u << 13;
constexpr address          cr3           = 0x14;
constexpr std::uint64_t    frame         = 10 * ps_per_second * 18 / msi_hz;
constexpr std::uint32_t    ore           = 1u << 3;
constexpr std::uint32_t    idle_flag     = 1u << 4;
constexpr std::uint32_t    eie           = 1u << 0;
constexpr std::uint32_t    dmar          = 1u << 6;
constexpr address          nvic_ispr1    = 0xE000'E204;
constexpr std::uint32_t    usart1_irq    = 1u << (37 - 32);

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;
};

class recording_endpoint final : public periph::uart_endpoint
{
public:
  std::vector<periph::line_frame> started_frames;

  void transmitted(std::uint16_t, vtime, bool) override
  {}

  void started(periph::line_frame const& frame) override
  {
    started_frames.push_back(frame);
  }
};

struct rig
{
  std::shared_ptr<periph::capture_endpoint> usart1 = std::make_shared<periph::capture_endpoint>();
  std::shared_ptr<periph::capture_endpoint> usart2 = std::make_shared<periph::capture_endpoint>();
  std::shared_ptr<periph::capture_endpoint> uart4  = std::make_shared<periph::capture_endpoint>();
  stm32::board                              board{};
  std::unique_ptr<stm32_fixture>            f;
  std::string                               name;

  explicit rig(
      std::string_view        entry,
      part_description const& desc = l152re(),
      std::string_view        elf  = firmware)
      : name{elf}
  {
    board.uarts.emplace("USART1", usart1);
    board.uarts.emplace("USART2", usart2);
    board.uarts.emplace("UART4", uart4);
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, desc);
    options.models.insert_or_assign(
        "DAC", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f = std::make_unique<stm32_fixture>(desc, elf, std::move(options));
    if(entry != "main")
    {
      REQUIRE(
          f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(elf, "main")}).reason
          == stop_reason::address);
      f->set("pc", sym(elf, entry));
    }
  }

  auto port(std::string_view unit) const -> stm32::uart_link_port&
  {
    auto* const found = f->model_of<stm32::uart_link_port>(unit);
    REQUIRE(found != nullptr);
    return *found;
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{2'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }

  auto read(address at) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(address at, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at, 4, value).has_value());
  }

  void idle_to(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  auto cpu_read(address at) const -> std::uint32_t
  {
    auto* const probe = f->model_of<bus_probe>("DAC");
    REQUIRE(probe != nullptr);
    auto const word = probe->context.services.read(at, 4, popcorn::initiator::cpu);
    REQUIRE(word.has_value());
    return static_cast<std::uint32_t>(*word);
  }

  void cpu_write(address at, std::uint32_t value) const
  {
    auto* const probe = f->model_of<bus_probe>("DAC");
    REQUIRE(probe != nullptr);
    REQUIRE(probe->context.services.write(at, 4, value, popcorn::initiator::cpu).has_value());
  }

  void parked_receiver(std::uint32_t control3 = 0) const
  {
    f->set("pc", sym(name, "idle"));
    write(::usart1 + 0x08, 18);
    write(::usart1 + cr3, control3);
    write(::usart1 + cr1, ue | te | re);
  }

  void queue(std::span<periph::host_frame const> frames) const
  {
    port("USART1").receive_frames(frames);
  }

  [[nodiscard]] auto pending() const -> bool
  {
    return (read(nvic_ispr1) & usart1_irq) != 0;
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
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

auto brr(rig const& r, std::string_view unit) -> std::uint32_t
{
  return r.f->field(unit, "BRR", "DIV_Mantissa") << 4 | r.f->field(unit, "BRR", "DIV_Fraction");
}

auto frame_ps(std::uint64_t clocks_per_bit, std::uint64_t hz, std::uint64_t bits = 10)
    -> std::uint64_t
{
  return bits * ps_per_second * clocks_per_bit / hz;
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
}

TEST_CASE("RM0038 Table 158: USART_SR reads TXE and TC after reset on every USART and UART")
{
  for(auto const* desc : {&l152re(), &l152rb()})
  {
    stm32_fixture f{*desc, desc == &l152re() ? firmware : firmware_b};
    auto const    cat5 = desc == &l152re();
    for(auto const unit : {"USART1", "USART2", "USART3", "UART4", "UART5"})
    {
      CAPTURE(unit);
      auto const present = std::ranges::contains(
          desc->peripherals, std::string_view{unit}, &popcorn::peripheral_desc::name);
      CHECK(present == (cat5 or std::string_view{unit}.starts_with("USART")));
      if(present)
      {
        auto const base = f.target->peripheral(unit).description().base;
        auto const word = f.target->mem().read(base + sr, 4);
        REQUIRE(word.has_value());
        CHECK(*word == (txe | tc));
      }
    }
  }
}

TEST_CASE("RM0038 27.3.2: polled USART1 sends hello back to back at PCLK2 over BRR, 8N1")
{
  for(auto const& [desc, elf] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(elf);
    rig r{"main", *desc, elf};
    CHECK(r.finish() == std::vector<std::uint32_t>{txe | tc, 1});
    CHECK(text(*r.usart1) == hello);
    CHECK(brr(r, "USART1") == 18);
    back_to_back(*r.usart1, frame_ps(18, msi_hz));
    rig again{"main", *desc, elf};
    again.finish();
    CHECK(again.usart1->frames == r.usart1->frames);
  }
}

TEST_CASE("RM0038 27.4: TXEIE drives the same hello from USART1_IRQn")
{
  rig r{"tx_irq"};
  CHECK(r.finish() == std::vector<std::uint32_t>{7});
  CHECK(text(*r.usart1) == hello);
  back_to_back(*r.usart1, frame_ps(18, msi_hz));
}

TEST_CASE(
    "RM0038 6.3.3: USART1 runs on PCLK2 and UART4 on PCLK1, each prescaler halves only its own")
{
  for(auto const& [ppre, usart1_hz, uart4_hz] : {
          std::tuple{4u << 11, msi_hz / 2, msi_hz    },
          std::tuple{4u << 8,  msi_hz,     msi_hz / 2}
  })
  {
    CAPTURE(ppre);
    rig r{"main"};
    r.write(rcc_cfgr, ppre);
    r.finish();
    CHECK(text(*r.usart1) == hello);
    back_to_back(*r.usart1, frame_ps(18, usart1_hz));
    rig u{"uart4_poll"};
    u.write(rcc_cfgr, ppre);
    u.finish();
    CHECK(text(*u.uart4) == hello);
    back_to_back(*u.uart4, frame_ps(218, uart4_hz));
  }
}

TEST_CASE("RM0038 27.4: host frames reach DR one frame apart and RXNEIE takes each")
{
  rig r{"rx_irq"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  std::array<periph::host_frame, 4> frames{
      {{.data = 'a'}, {.data = 'b'}, {.data = 'c'}, {.data = 'd'}}
  };
  r.port("USART1").receive_frames(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{4, 'a', 'd', 0});
}

TEST_CASE("RM0038 27.3.4: OVER8 takes DIV_Fraction[2:0] as eighths, USART2 at 115200")
{
  rig        r{"over8_poll"};
  auto const log = r.finish();
  REQUIRE(log.size() == 1);
  CHECK(log[0] == 0x22);
  CHECK(text(*r.usart2) == hello);
  back_to_back(*r.usart2, frame_ps((0x22u & 0xFFF0u) | ((0x22u & 7u) << 1), 2 * msi_hz));
}

TEST_CASE("RM0038 27.6.1: a write of 0 clears TC, TXE ignores the write")
{
  rig r{"tc_clear"};
  CHECK(r.finish() == std::vector<std::uint32_t>{txe | tc, txe, txe});
}

TEST_CASE("RM0038 27.6.1: a write of 0 to RXNE drops the received word, the next one arrives")
{
  rig r{"main"};
  r.f->set("pc", sym(firmware, "idle"));
  r.write(usart1 + cr1, ue | te | re);
  r.write(usart1 + 0x08, 18);
  std::array<periph::host_frame, 2> frames{
      {{.data = 'x'}, {.data = 'y'}}
  };
  r.port("USART1").receive_frames(std::span{frames}.first(1));
  r.idle_to(r.now_ps() + frame_ps(18, msi_hz) + 1'000'000);
  REQUIRE((r.read(usart1 + sr) & rxne) != 0);
  CHECK(r.read(usart1 + dr) == 'x');
  r.write(usart1 + sr, ~rxne);
  CHECK((r.read(usart1 + sr) & (rxne | tc)) == tc);
  r.port("USART1").receive_frames(std::span{frames}.subspan(1));
  r.idle_to(r.now_ps() + frame_ps(18, msi_hz) + 1'000'000);
  CHECK((r.read(usart1 + sr) & (rxne | (1u << 3))) == rxne);
  CHECK(r.read(usart1 + dr) == 'y');
}

TEST_CASE("RM0038 27.6.4: M with STOP 2 sends a 9-bit word in a 12-bit frame")
{
  rig r{"nine_bits"};
  r.finish();
  REQUIRE(r.usart1->frames.size() == 2);
  CHECK(r.usart1->frames[0].data == 0x1A5);
  CHECK(r.usart1->frames[1].data == 0x0FF);
  back_to_back(*r.usart1, frame_ps(18, msi_hz, 12));
}

TEST_CASE("RM0038 27.3.2: SBK written 0 before the break starts cancels it")
{
  rig r{"main"};
  r.f->set("pc", sym(firmware, "idle"));
  r.write(usart1 + 0x08, 18);
  r.write(usart1 + cr1, ue | te);
  r.write(usart1 + dr, 0x41);
  r.write(usart1 + cr1, ue | te | sbk);
  CHECK((r.read(usart1 + cr1) & sbk) != 0);
  r.write(usart1 + cr1, ue | te);
  CHECK((r.read(usart1 + cr1) & sbk) == 0);
  r.idle_to(r.now_ps() + 3 * frame);
  REQUIRE(r.usart1->frames.size() == 1);
  CHECK(r.usart1->frames[0].data == 0x41);
  CHECK_FALSE(r.usart1->frames[0].brk);
  CHECK((r.read(usart1 + sr) & tc) == tc);
}

TEST_CASE("RM0038 27.3.2: SBK sends a break after the frame in flight, then 1 bit high")
{
  rig r{"main"};
  r.f->set("pc", sym(firmware, "idle"));
  r.write(usart1 + 0x08, 18);
  r.write(usart1 + cr1, ue | te);
  auto const t0 = r.now_ps();
  r.write(usart1 + dr, 0x41);
  r.write(usart1 + cr1, ue | te | sbk);
  r.write(usart1 + dr, 0x42);
  r.idle_to(t0 + frame + frame / 2);
  REQUIRE(r.usart1->frames.size() == 1);
  r.write(usart1 + cr1, ue | te);
  CHECK((r.read(usart1 + cr1) & sbk) != 0);
  r.idle_to(r.now_ps() + 3 * frame);
  REQUIRE(r.usart1->frames.size() == 3);
  CHECK(r.usart1->frames[0].data == 0x41);
  CHECK(r.usart1->frames[1].brk);
  CHECK(r.usart1->frames[1].at.ps - r.usart1->frames[0].at.ps == frame);
  CHECK(r.usart1->frames[2].data == 0x42);
  CHECK(r.usart1->frames[2].at.ps - r.usart1->frames[1].at.ps == frame + 2 * frame / 20);
  CHECK((r.read(usart1 + cr1) & sbk) == 0);
}

TEST_CASE(
    "SBK written with TE 0 leaves once TE is 1, written with UE 0 it reads 0 and never leaves")
{
  {
    rig r{"main"};
    r.f->set("pc", sym(firmware, "idle"));
    r.write(usart1 + 0x08, 18);
    r.write(usart1 + cr1, ue | sbk);
    CHECK((r.read(usart1 + cr1) & sbk) != 0);
    r.idle_to(r.now_ps() + 2 * frame);
    CHECK(r.usart1->frames.empty());
    r.write(usart1 + cr1, ue | te | sbk);
    r.idle_to(r.now_ps() + 2 * frame);
    REQUIRE(r.usart1->frames.size() == 1);
    CHECK(r.usart1->frames[0].brk);
  }
  rig r{"main"};
  r.f->set("pc", sym(firmware, "idle"));
  r.write(usart1 + 0x08, 18);
  r.write(usart1 + cr1, te | sbk);
  CHECK((r.read(usart1 + cr1) & sbk) == 0);
  r.write(usart1 + cr1, ue | te);
  r.idle_to(r.now_ps() + 2 * frame);
  CHECK(r.usart1->frames.empty());
}

TEST_CASE(
    "ES0242 2.11.3 and 2.11.8: a break requested while D1 shifts sets TC after D1 with nCTS "
    "ready, keeps it 0 with nCTS deasserted, leaves either way, then TC rises, inferred")
{
  for(auto const ready : {false, true})
  {
    CAPTURE(ready);
    rig r{"idle"};
    r.write(::usart1 + 0x08, 18);
    r.write(::usart1 + cr3, 1u << 9);
    r.write(::usart1 + cr1, ue | te);
    r.cpu_write(::usart1 + sr, 0);
    auto const start = r.now_ps();
    r.cpu_write(::usart1 + dr, 'x');
    r.cpu_write(::usart1 + cr1, ue | te | sbk);
    r.port("USART1").set_cts(ready, vtime{start + frame / 2});
    r.idle_to(start + frame + frame / 10);
    CHECK((r.read(::usart1 + sr) & tc) == (ready ? tc : 0));
    CHECK((r.read(::usart1 + cr1) & sbk) != 0);
    r.idle_to(start + 3 * frame);
    REQUIRE(r.usart1->frames.size() == 2);
    CHECK_FALSE(r.usart1->frames[0].brk);
    CHECK(r.usart1->frames[1].brk);
    CHECK((r.read(::usart1 + cr1) & sbk) == 0);
    CHECK((r.read(::usart1 + sr) & tc) != 0);
    r.cpu_write(::usart1 + dr, 'y');
    r.idle_to(start + 6 * frame);
    CHECK(r.usart1->frames.size() == (ready ? 3u : 2u));
  }
}

TEST_CASE("RM0038 27.6.4: UE written 0 mid-frame stops the output at the end of that frame")
{
  rig r{"main"};
  r.f->set("pc", sym(firmware, "idle"));
  r.write(usart1 + 0x08, 18);
  r.write(usart1 + cr1, ue | te);
  r.write(usart1 + sr, ~tc);
  auto const t0 = r.now_ps();
  r.write(usart1 + dr, 0x41);
  r.write(usart1 + dr, 0x42);
  r.idle_to(t0 + frame / 2);
  r.write(usart1 + cr1, te);
  CHECK((r.read(usart1 + sr) & tc) == 0);
  r.idle_to(t0 + 3 * frame);
  REQUIRE(r.usart1->frames.size() == 1);
  CHECK(r.usart1->frames[0].data == 0x41);
  CHECK(r.usart1->frames[0].at.ps == t0 + frame);
  CHECK((r.read(usart1 + sr) & tc) == tc);
}

TEST_CASE("RM0038 Table 157: UART4 sends with CTSE and RTSE set and CTS not ready, logged once")
{
  rig        r{"uart4_poll"};
  auto const records = std::make_shared<std::vector<core::log_record>>();
  auto const sink    = r.f->target->add_log_sink(
      [records](core::log_record const& record) { records->push_back(record); });
  constexpr address uart4 = 0x4000'4C00;
  r.port("UART4").set_cts(false, vtime{r.now_ps()});
  r.write(uart4 + cr3, (1u << 9) | (1u << 8));
  r.write(uart4 + cr3, (1u << 9) | (1u << 8));
  CHECK(r.finish() == std::vector<std::uint32_t>{1});
  CHECK(text(*r.uart4) == hello);
  auto const flow = std::ranges::count_if(*records, [](core::log_record const& record) {
    return record.message.find("Table 157") != std::string::npos;
  });
  CHECK(flow == 1);
}

TEST_CASE("RM0038 27.5: UART4 sends hello at 9600 on PCLK1 on RETx")
{
  rig r{"uart4_poll"};
  CHECK(r.finish() == std::vector<std::uint32_t>{1});
  CHECK(text(*r.uart4) == hello);
  auto const div = brr(r, "UART4");
  CHECK(div == 218);
  back_to_back(*r.uart4, frame_ps(div, msi_hz));
}

TEST_CASE("the sci2 model refuses an instance outside the L1 APB windows")
{
  auto desc  = l152re();
  auto found = std::ranges::find(
      desc.peripherals, std::string_view{"USART3"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  found->base = 0x5000'0000;
  CHECK_THROWS_AS(stm32_fixture(desc, firmware), popcorn::config_error);
}

TEST_CASE("UartLink: USART1 posts its frame shape at the start bit, USART2 takes it at its end")
{
  rig        r{"main"};
  auto const line = std::make_shared<recording_endpoint>();
  r.port("USART1").attach(line);
  r.f->set("pc", sym(firmware, "idle"));
  constexpr address usart2 = 0x4000'4400;
  for(auto const base : {usart1, usart2})
  {
    r.write(base + 0x08, 18);
    r.write(base + cr1, ue | te | re);
  }
  auto const start = r.now_ps();
  r.write(usart1 + dr, 0x5A);
  REQUIRE(line->started_frames.size() == 1);
  auto const sent = line->started_frames[0];
  CHECK(sent.data == 0x5A);
  CHECK(sent.word_bits == 8);
  CHECK(sent.half_stops == 2);
  CHECK(sent.frame_ps == frame_ps(18, msi_hz));
  CHECK(sent.start.ps == start);
  CHECK(sent.end.ps == start + sent.frame_ps);
  r.port("USART2").receive_at(sent, 1);
  r.idle_to(sent.end.ps - 1'000'000);
  CAPTURE(r.now_ps());
  CHECK((r.read(usart2 + sr) & rxne) == 0);
  r.idle_to(sent.end.ps + 1'000'000);
  CHECK((r.read(usart2 + sr) & rxne) != 0);
  CHECK(r.read(usart2 + dr) == 0x5A);
}

TEST_CASE("RM0038 27.3.13: DMA1 channel 4 feeds USART1 from TXE, TC rises after the last frame")
{
  rig r{"dma_tx"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0, 0, 1});
  CHECK(text(*r.usart1) == hello);
  back_to_back(*r.usart1, frame);
}

TEST_CASE("RM0038 27.3.13: DMA1 channel 5 takes each received word from RXNE")
{
  rig r{"dma_rx"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  std::array<periph::host_frame, 4> frames{
      {{.data = 'k'}, {.data = 'l'}, {.data = 'm'}, {.data = 'n'}}
  };
  r.queue(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{'k', 'n', 0, 0});
}

TEST_CASE("RM0038 27.6.1: IDLE rises one frame after the last frame, SR then DR clears it")
{
  rig r{"main"};
  r.parked_receiver();
  std::array<periph::host_frame, 1> frames{{{.data = 'q'}}};
  auto const                        t0 = r.now_ps();
  r.queue(frames);
  r.idle_to(t0 + 2 * frame - 1'000'000);
  CHECK((r.read(usart1 + sr) & (rxne | idle_flag)) == rxne);
  r.idle_to(t0 + 2 * frame + 1'000'000);
  CHECK((r.read(usart1 + sr) & (rxne | idle_flag)) == (rxne | idle_flag));
  CHECK(r.cpu_read(usart1 + dr) == 'q');
  CHECK((r.read(usart1 + sr) & idle_flag) == idle_flag);
  CHECK((r.cpu_read(usart1 + sr) & idle_flag) == idle_flag);
  static_cast<void>(r.read(usart1 + dr));
  CHECK((r.read(usart1 + sr) & idle_flag) == idle_flag);
  CHECK(r.cpu_read(usart1 + dr) == 0);
  CHECK((r.read(usart1 + sr) & (rxne | idle_flag)) == 0);
}

TEST_CASE("RM0038 27.6.1: the LL IDLE clear in firmware, after polling it")
{
  rig r{"idle_clear"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  std::array<periph::host_frame, 1> frames{{{.data = 'q'}}};
  r.queue(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{rxne | idle_flag, 'q', 0});
}

TEST_CASE("RM0038 27.6.1: a DR read clears only the flags the SR read before it saw")
{
  rig r{"late_idle"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  std::array<periph::host_frame, 1> frames{{{.data = 'z'}}};
  auto const                        t0 = r.now_ps();
  r.queue(frames);
  r.idle_to(t0 + 2 * frame + 1'000'000);
  REQUIRE((r.read(usart1 + sr) & idle_flag) != 0);
  r.write(sym(firmware, "release"), 1);
  CHECK(r.finish() == std::vector<std::uint32_t>{0, 'z', idle_flag, 0});
}

TEST_CASE(
    "RM0038 27.6.1: a word arriving with RXNE set sets ORE, DR keeps the first, SR then DR clears")
{
  rig r{"overrun"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  std::array<periph::host_frame, 2> frames{
      {{.data = 'a'}, {.data = 'b'}}
  };
  r.queue(frames);
  CHECK(r.finish() == std::vector<std::uint32_t>{rxne | ore, 'a', 0});
}

TEST_CASE("RM0038 27.6.6: EIE raises USART1_IRQn on ORE only with DMAR")
{
  rig r{"main"};
  r.parked_receiver(eie);
  std::array<periph::host_frame, 2> frames{
      {{.data = 'a'}, {.data = 'b'}}
  };
  auto const t0 = r.now_ps();
  r.queue(frames);
  r.idle_to(t0 + 2 * frame + 1'000'000);
  REQUIRE((r.read(usart1 + sr) & ore) != 0);
  CHECK_FALSE(r.pending());
  r.write(usart1 + cr3, eie | dmar);
  CHECK(r.pending());
}

TEST_CASE("a snapshot between the SR read and the DR read keeps the clear sequence")
{
  rig r{"main"};
  r.parked_receiver();
  std::array<periph::host_frame, 1> frames{{{.data = 'q'}}};
  auto const                        t0 = r.now_ps();
  r.queue(frames);
  r.idle_to(t0 + 2 * frame + 1'000'000);
  REQUIRE((r.cpu_read(usart1 + sr) & idle_flag) != 0);
  auto const saved = r.f->target->snapshot();
  rig        other{"main"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  for(auto const* one : {&r, &other})
  {
    CHECK(one->cpu_read(usart1 + dr) == 'q');
    CHECK((one->read(usart1 + sr) & (rxne | idle_flag)) == 0);
  }
}

TEST_CASE("HAL_UART_Transmit sends hello, HAL_UART_Receive_IT takes 4 words on USART1_IRQn")
{
  for(auto const& [desc, elf] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(elf);
    rig r{"hal_tx_rx", *desc, elf};
    for(int step = 0; step < 100 and r.f->log().empty(); ++step)
    {
      r.idle_to(r.now_ps() + 1'000'000'000);
    }
    REQUIRE(r.f->log() == std::vector<std::uint32_t>{0x5A5A});
    CHECK(text(*r.usart1) == hello);
    std::array<periph::host_frame, 4> frames{
        {{.data = '1'}, {.data = '2'}, {.data = '3'}, {.data = '4'}}
    };
    r.queue(frames);
    CHECK(r.finish() == std::vector<std::uint32_t>{0x5A5A, '1', '4', 0});
  }
}

TEST_CASE(
    "RM0038 27.6.1: an SR read then a DR write clears TC and PE, without the read or by the "
    "debugger neither")
{
  constexpr std::uint32_t pce = 1u << 10;
  constexpr std::uint32_t pe  = 1u << 0;
  for(bool const read_first : {true, false})
  {
    CAPTURE(read_first);
    rig r{"main"};
    r.f->set("pc", sym(firmware, "idle"));
    r.write(usart1 + 0x08, 18);
    r.write(usart1 + cr1, ue | re | pce);
    std::array<periph::host_frame, 1> frames{{{.data = 0x31, .error = periph::uart_error::parity}}};
    auto const                        t0 = r.now_ps();
    r.queue(frames);
    r.idle_to(t0 + frame + 1'000'000);
    REQUIRE((r.read(usart1 + sr) & (tc | pe)) == (tc | pe));
    if(read_first)
    {
      static_cast<void>(r.cpu_read(usart1 + sr));
      r.write(usart1 + dr, 0x00);
      CHECK((r.read(usart1 + sr) & (tc | pe)) == (tc | pe));
    }
    r.cpu_write(usart1 + dr, 0x00);
    CHECK((r.read(usart1 + sr) & (tc | pe)) == (read_first ? 0 : (tc | pe)));
  }
}

TEST_CASE("RM0038 27.6.1: an SR read then a DR read clears PE, FE and NF and keeps TC")
{
  constexpr std::uint32_t pce = 1u << 10;
  constexpr std::uint32_t pe  = 1u << 0;
  constexpr std::uint32_t fe  = 1u << 1;
  constexpr std::uint32_t nf  = 1u << 2;
  for(auto const& [error, flag] : {
          std::pair{periph::uart_error::parity,  pe},
          std::pair{periph::uart_error::framing, fe},
          std::pair{periph::uart_error::noise,   nf}
  })
  {
    CAPTURE(flag);
    rig r{"main"};
    r.f->set("pc", sym(firmware, "idle"));
    r.write(usart1 + 0x08, 18);
    r.write(usart1 + cr1, ue | re | pce);
    std::array<periph::host_frame, 1> frames{{{.data = 0x31, .error = error}}};
    auto const                        t0 = r.now_ps();
    r.queue(frames);
    r.idle_to(t0 + frame + 1'000'000);
    REQUIRE((r.read(usart1 + sr) & (tc | flag)) == (tc | flag));
    static_cast<void>(r.cpu_read(usart1 + sr));
    static_cast<void>(r.cpu_read(usart1 + dr));
    CHECK((r.read(usart1 + sr) & (tc | pe | fe | nf | rxne)) == tc);
  }
}

TEST_CASE("RM0038 27.6.1: a DR write with no SR read since TC rose sends and leaves TC set")
{
  for(bool const read_first : {true, false})
  {
    CAPTURE(read_first);
    rig r{"main"};
    r.f->set("pc", sym(firmware, "idle"));
    r.write(usart1 + 0x08, 18);
    r.write(usart1 + cr1, ue | te);
    REQUIRE((r.read(usart1 + sr) & tc) == tc);
    if(read_first)
    {
      static_cast<void>(r.cpu_read(usart1 + sr));
    }
    auto const t0 = r.now_ps();
    r.cpu_write(usart1 + dr, 0x41);
    r.idle_to(t0 + frame / 2);
    CHECK((r.read(usart1 + sr) & tc) == (read_first ? 0 : tc));
    r.idle_to(t0 + 2 * frame);
    REQUIRE(r.usart1->frames.size() == 1);
    CHECK((r.read(usart1 + sr) & tc) == tc);
  }
}

TEST_CASE("RM0038 27.6.1: a write of 0 over a set ORE and IDLE leaves both, clears RXNE")
{
  auto writable = l152re();
  auto unit     = std::ranges::find(
      writable.peripherals, std::string_view{"USART1"}, &popcorn::peripheral_desc::name);
  REQUIRE(unit != writable.peripherals.end());
  auto status =
      std::ranges::find(unit->registers, std::string_view{"SR"}, &popcorn::register_desc::name);
  REQUIRE(status != unit->registers.end());
  for(auto& bits : status->fields)
  {
    bits.acc = popcorn::access::read_write;
  }
  for(part_description const* desc : {&l152re(), static_cast<part_description const*>(&writable)})
  {
    CAPTURE(desc == &writable);
    rig r{"main", *desc};
    r.parked_receiver();
    std::array<periph::host_frame, 2> frames{
        {{.data = 'a'}, {.data = 'b'}}
    };
    auto const t0 = r.now_ps();
    r.queue(frames);
    r.idle_to(t0 + 3 * frame + 1'000'000);
    REQUIRE((r.read(usart1 + sr) & (ore | idle_flag | rxne)) == (ore | idle_flag | rxne));
    r.write(usart1 + sr, 0);
    CHECK((r.read(usart1 + sr) & (ore | idle_flag | rxne)) == (ore | idle_flag));
  }
}
