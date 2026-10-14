#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t    ps_per_second  = 1'000'000'000'000;
constexpr std::uint64_t    instruction_ps = 250'000;
constexpr std::string_view hello          = "hello\r\n";

class bus_probe final : public popcorn::periph::model
{
public:
  explicit bus_probe(popcorn::periph::model_context& ctx) : context{ctx}
  {}

  popcorn::periph::model_context& context;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

struct rig
{
  std::shared_ptr<stm32::capture_endpoint> usart1 = std::make_shared<stm32::capture_endpoint>();
  std::shared_ptr<stm32::capture_endpoint> uart4  = std::make_shared<stm32::capture_endpoint>();
  std::shared_ptr<stm32::capture_endpoint> lpuart = std::make_shared<stm32::capture_endpoint>();
  stm32::board                             board{};
  std::unique_ptr<stm32_fixture>           f;

  explicit rig(std::string_view entry, bool strict = false)
  {
    board.uarts.emplace("USART1", usart1);
    board.uarts.emplace("UART4", uart4);
    board.uarts.emplace("LPUART1", lpuart);
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    options.models             = stm32::factories(board, u575());
    options.models.insert_or_assign(
        "DAC1", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f = std::make_unique<stm32_fixture>(u575(), "usart_ll", std::move(options));
    if(entry != "main")
    {
      REQUIRE(
          f->run_to_stop(
               {.max_instructions = icount{100'000'000}, .until = sym("usart_ll", "main")})
              .reason
          == stop_reason::address);
      f->set("pc", sym("usart_ll", entry));
    }
  }

  auto port(std::string_view name) const -> stm32::uart_port&
  {
    auto* const found = f->model_of<stm32::uart_port>(name);
    REQUIRE(found != nullptr);
    return *found;
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop();
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }
};

auto text(stm32::capture_endpoint const& capture) -> std::string
{
  std::string out;
  for(auto const& frame : capture.frames)
  {
    out.push_back(static_cast<char>(frame.data));
  }
  return out;
}

void spaced(stm32::capture_endpoint const& capture, std::uint64_t frame_ps)
{
  REQUIRE(capture.frames.size() > 1);
  for(std::size_t index = 1; index < capture.frames.size(); ++index)
  {
    auto const gap = capture.frames[index].at.ps - capture.frames[index - 1].at.ps;
    CAPTURE(index);
    CHECK(gap + instruction_ps >= frame_ps);
    CHECK(gap <= frame_ps + instruction_ps);
  }
}

auto frame_of(
    stm32_fixture const& f,
    std::string_view     name,
    std::uint64_t        kernel_hz,
    bool                 low_power) -> std::uint64_t
{
  auto const brr  = f.field(name, "BRR", "BRR");
  auto const baud = low_power ? 256 * kernel_hz / brr : kernel_hz / brr;
  return 10 * ps_per_second / baud;
}
}

TEST_CASE("the USART factory copies the board's endpoints when it is made")
{
  auto         first  = std::make_shared<stm32::capture_endpoint>();
  auto         second = std::make_shared<stm32::capture_endpoint>();
  stm32::board board{};
  board.uarts.emplace("USART1", first);
  auto options             = popcorn::testing::stm32_options();
  options.models           = stm32::factories(board, u575());
  board.uarts.at("USART1") = second;
  stm32_fixture f{u575(), "usart_ll", std::move(options)};
  auto const    stopped = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(text(*first) == hello);
  CHECK(second->frames.empty());
}

TEST_CASE("polled USART1 transmission leaves one frame time apart at 115200 8N1")
{
  rig r{"main"};
  CHECK(r.finish() == std::vector<std::uint32_t>{1});
  CHECK(text(*r.usart1) == hello);
  spaced(*r.usart1, frame_of(*r.f, "USART1", 4'000'000, false));
  CHECK(r.f->target->virtual_clock().time() >= r.usart1->frames.back().at);

  rig again{"main"};
  again.finish();
  CHECK(again.usart1->frames == r.usart1->frames);
}

TEST_CASE("interrupt-driven transmission sends the same frames")
{
  rig        r{"tx_irq"};
  auto const log = r.finish();
  CHECK(text(*r.usart1) == hello);
  spaced(*r.usart1, frame_of(*r.f, "USART1", 4'000'000, false));
  REQUIRE(log.size() == 1);
  CHECK(log[0] == hello.size() + 1);
}

TEST_CASE("frames queued at once reach the receive interrupt one frame time apart, in order")
{
  rig r{"rx_irq"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  std::vector<std::uint16_t> const sent{'p', 'o', 'p', 'c', 'n'};
  r.port("USART1").receive(sent);
  CHECK(r.finish() == std::vector<std::uint32_t>{'p', 'o', 'p', 'c', 'n'});
}

TEST_CASE("the first queued frame arrives one frame time after it is queued")
{
  rig r{"overrun"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  auto const                       start = r.f->target->virtual_clock().time();
  std::vector<std::uint16_t> const one{'x'};
  r.port("USART1").receive(one);
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{10'000'000}, .stop_when = [&] {
                                           return r.f->field("USART1", "ISR_enabled", "RXFNE") != 0;
                                         }});
  REQUIRE(stopped.reason == stop_reason::user);
  auto const frame = frame_of(*r.f, "USART1", 4'000'000, false);
  auto const took  = r.f->target->virtual_clock().time().ps - start.ps;
  CHECK(took + instruction_ps >= frame);
  CHECK(took <= frame + instruction_ps);
}

TEST_CASE("a frame in flight across a core clock change arrives at its virtual time")
{
  rig r{"overrun"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  auto const                       start = r.f->target->virtual_clock().time();
  auto const                       frame = frame_of(*r.f, "USART1", 4'000'000, false);
  std::vector<std::uint16_t> const one{'x'};
  r.port("USART1").receive(one);
  REQUIRE(
      r.f->run_to_stop(
             {.max_instructions = icount{10'000'000}, .until_time = vtime{start.ps + frame / 4}})
          .reason
      == stop_reason::time);
  auto&      rcc    = r.f->target->peripheral("RCC");
  auto const icscr1 = popcorn::find_register(rcc.description(), std::string_view{"RCC_ICSCR1"});
  REQUIRE(icscr1 != nullptr);
  auto const at  = rcc.description().base + icscr1->offset;
  auto const old = r.f->target->mem().read(at, 4);
  REQUIRE(old.has_value());
  REQUIRE(
      r.f->target->mem()
          .write(
              at, 4, (static_cast<std::uint32_t>(*old) & ~(0xFu << 28)) | (1u << 28) | (1u << 23))
          .has_value());
  REQUIRE(r.f->target->virtual_clock().core_hz() == 24'000'000);
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{100'000'000}, .stop_when = [&] {
                                           return r.f->field("USART1", "ISR_enabled", "RXFNE") != 0;
                                         }});
  REQUIRE(stopped.reason == stop_reason::user);
  auto const took = r.f->target->virtual_clock().time().ps - start.ps;
  CHECK(took >= frame);
  CHECK(took <= frame + instruction_ps);
}

TEST_CASE("a FIFO loopback returns 16 frames unchanged")
{
  rig r{"loop_fifo"};
  r.port("USART1").attach(std::make_shared<stm32::loopback_endpoint>(r.port("USART1")));
  CHECK(r.finish() == std::vector<std::uint32_t>{16, 1});
}

TEST_CASE("frames arriving with RDR full set ORE, ICR clears it")
{
  rig r{"overrun"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  std::vector<std::uint16_t> const three{1, 2, 3};
  r.port("USART1").receive(three);
  CHECK(r.finish() == std::vector<std::uint32_t>{1, 0});
}

TEST_CASE("UART4 at 9600 on PCLK1 and LPUART1 at 9600 on LSE keep their frame times")
{
  rig uart4{"uart4_poll"};
  uart4.finish();
  CHECK(text(*uart4.uart4) == hello);
  spaced(*uart4.uart4, frame_of(*uart4.f, "UART4", 4'000'000, false));

  rig lpuart{"lpuart_poll"};
  lpuart.finish();
  CHECK(text(*lpuart.lpuart) == hello);
  spaced(*lpuart.lpuart, frame_of(*lpuart.f, "LPUART1", 32'768, true));
}

TEST_CASE("a zero BRR transmits nothing and logs once")
{
  rig                           r{"tx_poll"};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto&      regs  = r.f->target->peripheral("USART1");
  auto const write = [&](std::string_view reg, std::uint32_t value) {
    auto const found = popcorn::find_register(regs.description(), reg);
    REQUIRE(found != nullptr);
    REQUIRE(r.f->target->model("USART1")
                ->on_write(
                    regs, found->offset, 4, regs.peek(found->offset), value,
                    {.pc = 0, .now = icount{0}})
                .has_value());
  };
  write("CR1_enabled", 0x9);
  write("TDR", 'a');
  write("TDR", 'b');
  CHECK(r.usart1->frames.empty());
  REQUIRE(logged.size() == 1);
  CHECK(logged.front().peripheral == "USART1");
}

TEST_CASE("a snapshot mid-transmission restores to the same remaining frames")
{
  rig r{"main"};
  while(r.usart1->frames.size() < 3)
  {
    REQUIRE_FALSE(r.f->run({.max_instructions = icount{64}}).stopped.has_value());
  }
  auto const saved = r.f->target->snapshot();
  auto const mark  = r.usart1->frames.size();
  r.finish();
  std::vector<stm32::captured_frame> const rest{
      r.usart1->frames.begin() + static_cast<long>(mark), r.usart1->frames.end()};
  r.usart1->frames.resize(mark);
  r.f->target->restore(saved);
  r.finish();
  CHECK(
      std::vector<stm32::captured_frame>{
          r.usart1->frames.begin() + static_cast<long>(mark), r.usart1->frames.end()}
      == rest);
}

TEST_CASE("the transmit buffer holds one frame without FIFO mode and eight with it")
{
  for(bool const with_fifo : {false, true})
  {
    rig        r{"idle"};
    auto&      regs  = r.f->target->peripheral("USART1");
    auto const write = [&](std::string_view reg, std::uint32_t value) {
      auto const found = popcorn::find_register(regs.description(), reg);
      REQUIRE(found != nullptr);
      REQUIRE(r.f->target->model("USART1")
                  ->on_write(
                      regs, found->offset, 4, regs.peek(found->offset), value,
                      {.pc = 0, .now = icount{0}})
                  .has_value());
    };
    auto&      rcc    = r.f->target->peripheral("RCC");
    auto const enable = popcorn::find_register(rcc.description(), std::string_view{"RCC_APB2ENR"});
    auto const usart1 =
        std::ranges::find(enable->fields, std::string_view{"USART1EN"}, &popcorn::field_desc::name);
    REQUIRE(r.f->target->model("RCC")
                ->on_write(
                    rcc, enable->offset, 4, rcc.peek(enable->offset), 1u << usart1->lsb,
                    {.pc = 0, .now = icount{0}})
                .has_value());
    write("BRR", 35);
    write("CR1_enabled", 0x9u | (with_fifo ? 1u << 29 : 0u));
    CAPTURE(with_fifo);
    write("TDR", 'a');
    CHECK(r.f->field("USART1", "ISR_enabled", "TXFNF") == 1);
    write("TDR", 'b');
    CHECK(r.f->field("USART1", "ISR_enabled", "TXFNF") == (with_fifo ? 1u : 0u));
    for(char c = 'c'; c < 'j'; ++c)
    {
      write("TDR", static_cast<std::uint32_t>(c));
    }
    CHECK(r.f->field("USART1", "ISR_enabled", "TXFNF") == 0);
    auto const now = r.f->target->virtual_clock().time();
    REQUIRE(
        r.f->run_to_stop({.max_instructions = icount{1'000'000'000},
                          .until_time       = vtime{now.ps + 2'000'000'000}})
            .reason
        == stop_reason::time);
    CHECK(r.usart1->frames.size() == (with_fifo ? 9u : 2u));
  }
}

TEST_CASE("a FIFO transmission fed from the TX threshold interrupt completes")
{
  rig        r{"tx_fifo_irq"};
  auto const log = r.finish();
  REQUIRE(r.usart1->frames.size() == 16);
  for(std::size_t index = 0; index < 16; ++index)
  {
    CHECK(r.usart1->frames[index].data == 0x30 + index);
  }
  spaced(*r.usart1, frame_of(*r.f, "USART1", 4'000'000, false));
  REQUIRE(log.size() == 1);
  CHECK(log[0] >= 2);
}

TEST_CASE("RXFT rises at the RX FIFO threshold and queued() counts host frames not yet delivered")
{
  rig        r{"idle"};
  auto&      regs  = r.f->target->peripheral("USART1");
  auto const write = [&](std::string_view reg, std::uint32_t value) {
    auto const found = popcorn::find_register(regs.description(), reg);
    REQUIRE(found != nullptr);
    REQUIRE(r.f->target->model("USART1")
                ->on_write(
                    regs, found->offset, 4, regs.peek(found->offset), value,
                    {.pc = 0, .now = icount{0}})
                .has_value());
  };
  auto&      rcc    = r.f->target->peripheral("RCC");
  auto const enable = popcorn::find_register(rcc.description(), std::string_view{"RCC_APB2ENR"});
  auto const usart1 =
      std::ranges::find(enable->fields, std::string_view{"USART1EN"}, &popcorn::field_desc::name);
  REQUIRE(r.f->target->model("RCC")
              ->on_write(
                  rcc, enable->offset, 4, rcc.peek(enable->offset), 1u << usart1->lsb,
                  {.pc = 0, .now = icount{0}})
              .has_value());
  write("CR3", 2u << 25);
  write("BRR", 35);
  write("CR1_enabled", 0x5u | (1u << 29));
  std::vector<std::uint16_t> const five{1, 2, 3, 4, 5};
  r.port("USART1").receive(five);
  CHECK(r.port("USART1").queued() == 5);
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{10'000'000}, .stop_when = [&] {
                                           return r.f->field("USART1", "ISR_enabled", "RXFT") != 0;
                                         }});
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(r.port("USART1").queued() == 1);
  CHECK(r.f->field("USART1", "ISR_enabled", "RXFNE") == 1);
}

TEST_CASE("a GPDMA channel feeds USART1 through its TX request, one frame at a time")
{
  constexpr std::string_view banner =
      "popcorn usart dma banner, sixty-four bytes long, sent by gpdma!\n";
  rig        r{"tx_dma"};
  auto const log = r.finish();
  CHECK(log == std::vector<std::uint32_t>{1});
  CHECK(text(*r.usart1) == banner);
  spaced(*r.usart1, frame_of(*r.f, "USART1", 4'000'000, false));

  rig again{"tx_dma"};
  again.finish();
  CHECK(again.usart1->frames == r.usart1->frames);
}

TEST_CASE("a snapshot mid-DMA transmission restores to the same remaining frames")
{
  rig r{"tx_dma"};
  while(r.usart1->frames.size() < 10)
  {
    REQUIRE_FALSE(r.f->run({.max_instructions = icount{64}}).stopped.has_value());
  }
  auto const saved = r.f->target->snapshot();
  auto const mark  = r.usart1->frames.size();
  r.finish();
  std::vector<stm32::captured_frame> const rest{
      r.usart1->frames.begin() + static_cast<long>(mark), r.usart1->frames.end()};
  r.usart1->frames.resize(mark);
  r.f->target->restore(saved);
  r.finish();
  CHECK(
      std::vector<stm32::captured_frame>{
          r.usart1->frames.begin() + static_cast<long>(mark), r.usart1->frames.end()}
      == rest);
}

TEST_CASE("a circular GPDMA list receives USART2 into a ring with HT and TC under TCEM 2")
{
  rig r{"rx_dma_list"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  std::vector<std::uint16_t> sent;
  for(std::uint16_t index = 0; index < 40; ++index)
  {
    sent.push_back(static_cast<std::uint16_t>(0x40u + index));
  }
  r.port("USART2").receive(sent);
  CHECK(
      r.finish()
      == std::vector<std::uint32_t>{
          0x4800'0008, 0x5400'0000, 0x4800'0008, 0x5400'0000, 0x4800'0008});
  auto const ring = sym("usart_ll", "ring");
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    auto const byte = r.f->target->mem().read(ring + index, 1);
    REQUIRE(byte.has_value());
    CHECK(*byte == 0x40u + (index < 8 ? 32u + index : 16u + index));
  }
}

namespace
{
auto address_of(stm32_fixture const& f, std::string_view peripheral, std::string_view reg)
    -> popcorn::address
{
  auto const& desc  = f.target->peripheral(peripheral).description();
  auto const  found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  return desc.base + found->offset;
}

struct locked_rig
{
  rig                           r;
  std::vector<core::log_record> logged;
  core::hook_handle             sink{r.f->target->add_log_sink(
      [this](core::log_record const& record) { logged.push_back(record); })};

  explicit locked_rig(bool strict = false) : r{"idle", strict}
  {
    set_bit("RCC", "RCC_APB2ENR", 14);
    set_bit("RCC", "RCC_APB3ENR", 6);
  }

  void set_bit(std::string_view peripheral, std::string_view reg, std::uint32_t bit) const
  {
    debugger(peripheral, reg, read(peripheral, reg) | (1u << bit));
  }

  void cpu(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    auto* const probe = r.f->model_of<bus_probe>("DAC1");
    REQUIRE(probe != nullptr);
    REQUIRE(probe->context.services
                .write(address_of(*r.f, peripheral, reg), 4, value, popcorn::initiator::cpu)
                .has_value());
  }

  void dma(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    auto* const probe = r.f->model_of<bus_probe>("DAC1");
    REQUIRE(probe != nullptr);
    REQUIRE(probe->context.services
                .write(address_of(*r.f, peripheral, reg), 4, value, popcorn::initiator::dma)
                .has_value());
  }

  void debugger(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(r.f->target->mem().write(address_of(*r.f, peripheral, reg), 4, value).has_value());
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const value = r.f->target->mem().read(address_of(*r.f, peripheral, reg), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }
};

constexpr std::uint32_t ue = 1;
}

TEST_CASE("a BRR write with UE 1 keeps the baud rate and logs once, firmware")
{
  rig                           r{"brr_while_enabled"};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  CHECK(r.finish() == std::vector<std::uint32_t>{35});
  CHECK(text(*r.usart1) == "ABC");
  auto const frame = 10 * ps_per_second * 35 / 4'000'000;
  REQUIRE(r.usart1->frames.size() == 3);
  auto const gap = r.usart1->frames[2].at.ps - r.usart1->frames[1].at.ps;
  CHECK(gap + instruction_ps >= frame);
  CHECK(gap <= frame + instruction_ps);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].peripheral == "USART1");
  CHECK(logged[0].reg == "BRR");
}

TEST_CASE("a BRR write with UE 1 is a model_error under strict peripherals, firmware")
{
  rig r{"brr_while_enabled", true};
  CHECK_THROWS_WITH_AS(
      static_cast<void>(r.f->run_to_stop()), doctest::Contains("USART1.BRR"), popcorn::model_error);
}

TEST_CASE("UE-locked USART1 and LPUART1 fields keep their value on CPU writes with UE 1")
{
  locked_rig t;
  SUBCASE("USART1")
  {
    t.cpu("USART1", "CR1_enabled", ue);
    t.cpu("USART1", "CR1_enabled", ue | (1u << 12) | (1u << 6));
    CHECK(t.read("USART1", "CR1_enabled") == (ue | (1u << 6)));
    t.cpu("USART1", "CR2", 2u << 12);
    CHECK(t.read("USART1", "CR2") == 0);
    t.cpu("USART1", "CR2", 1u << 21);
    CHECK(t.read("USART1", "CR2") == 1u << 21);
    t.cpu("USART1", "CR3", (1u << 0) | (1u << 12));
    CHECK(t.read("USART1", "CR3") == 1u);
    t.cpu("USART1", "CR3", 1u | (3u << 17));
    CHECK(t.read("USART1", "CR3") == 1u);
    t.cpu("USART1", "CR3", 1u);
    t.cpu("USART1", "BRR", 416);
    CHECK(t.read("USART1", "BRR") == 0);
    t.dma("USART1", "BRR", 416);
    CHECK(t.read("USART1", "BRR") == 0);
    t.cpu("USART1", "PRESC", 3);
    CHECK(t.read("USART1", "PRESC") == 0);
    REQUIRE(t.logged.size() == 5);
    CHECK(t.logged[0].reg == "CR1");
    t.cpu("USART1", "BRR", 0);
    CHECK(t.logged.size() == 5);
    t.debugger("USART1", "BRR", 416);
    CHECK(t.read("USART1", "BRR") == 416);
    CHECK(t.logged.size() == 5);
    t.cpu("USART1", "CR1_enabled", 0);
    t.cpu("USART1", "BRR", 35);
    CHECK(t.read("USART1", "BRR") == 35);
  }
  SUBCASE("LPUART1")
  {
    t.cpu("LPUART1", "CR1", ue);
    t.cpu("LPUART1", "CR1", ue | (1u << 12) | (1u << 29));
    CHECK(t.read("LPUART1", "CR1") == (ue | (1u << 29)));
    t.cpu("LPUART1", "CR2", 2u << 12);
    CHECK(t.read("LPUART1", "CR2") == 0);
    t.cpu("LPUART1", "BRR", 0x300);
    CHECK(t.read("LPUART1", "BRR") == 0);
  }
}

TEST_CASE("SCARCNT takes 0 with UE 1 and keeps a non-zero write, ABRMOD is locked only with ABREN")
{
  locked_rig t;
  t.cpu("USART1", "CR3", 5u << 17);
  t.cpu("USART1", "CR1_enabled", ue);
  t.cpu("USART1", "CR3", 3u << 17);
  CHECK(t.read("USART1", "CR3") == 5u << 17);
  t.cpu("USART1", "CR3", 0);
  CHECK(t.read("USART1", "CR3") == 0);
  t.cpu("USART1", "CR2", 1u << 21);
  CHECK(t.read("USART1", "CR2") == 1u << 21);
  t.cpu("USART1", "CR1_enabled", 0);
  t.cpu("USART1", "CR2", 1u << 20);
  t.cpu("USART1", "CR1_enabled", ue);
  t.cpu("USART1", "CR2", (1u << 20) | (2u << 21));
  CHECK(t.read("USART1", "CR2") == 1u << 20);
}

TEST_CASE("a CR1 write of 0 with UE 1 clears UE and the locked fields, no log, no error")
{
  locked_rig t{true};
  t.cpu("USART1", "CR1_enabled", ue | (1u << 12));
  t.cpu("USART1", "CR1_enabled", 0);
  CHECK(t.read("USART1", "CR1_enabled") == 0);
  CHECK(t.logged.empty());
}

namespace
{
constexpr std::uint64_t us_ps         = 1'000'000;
constexpr std::uint64_t ms_ps         = 1'000'000'000;
constexpr std::uint32_t link_byte     = 1;
constexpr std::uint32_t link_idle     = 2;
constexpr std::uint64_t cycles_per_us = 160;

struct line_post
{
  bool              abort{};
  stm32::line_frame frame{};
  vtime             at{};
};

class mailbox final : public stm32::uart_endpoint
{
public:
  std::vector<line_post>             posts;
  std::vector<stm32::captured_frame> sent;
  int                                transmissions = 0;

  void transmitted(std::uint16_t data, vtime at, bool brk) override
  {
    ++transmissions;
    sent.push_back({.data = data, .at = at, .brk = brk});
  }

  void started(stm32::line_frame const& frame) override
  {
    posts.push_back({.frame = frame});
  }

  void aborted(vtime at) override
  {
    posts.push_back({.abort = true, .at = at});
  }
};

struct member
{
  std::shared_ptr<mailbox>       box = std::make_shared<mailbox>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;
  std::uint64_t                  origin{};
  std::uint64_t                  slice{1'000'000'000};

  explicit member(std::string_view entry, std::uint64_t instructions = 1'000'000'000)
      : slice{instructions}
  {
    board.uarts.emplace("USART1", box);
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    f              = std::make_unique<stm32_fixture>(u575(), "usart_ll", std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("usart_ll", "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym("usart_ll", entry));
    origin = f->target->virtual_clock().time().ps;
  }

  auto port() const -> stm32::uart_port&
  {
    auto* const found = f->model_of<stm32::uart_port>("USART1");
    REQUIRE(found != nullptr);
    return *found;
  }

  auto to(std::uint64_t system_ps, bool hold = false) const -> stop_reason
  {
    for(;;)
    {
      auto const result = f->run(
          {.max_instructions = icount{slice},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{origin + system_ps},
           .hold_from_until  = hold});
      if(result.stopped)
      {
        return result.stopped->reason;
      }
    }
  }
};

auto shifted(vtime at, member const& from, member const& into, std::uint64_t latency) -> vtime
{
  return vtime{at.ps - from.origin + into.origin + latency};
}

void deliver(
    line_post const& post,
    member const&    from,
    member const&    into,
    std::uint64_t    latency,
    std::string_view unit = "USART1")
{
  auto* const port = into.f->model_of<stm32::uart_port>(unit);
  REQUIRE(port != nullptr);
  if(post.abort)
  {
    port->cancel_in_flight(shifted(post.at, from, into, latency), 1);
    return;
  }
  auto frame  = post.frame;
  frame.start = shifted(frame.start, from, into, latency);
  frame.end   = shifted(frame.end, from, into, latency);
  port->receive_at(frame, 1);
}

struct link_result
{
  std::vector<std::uint32_t> log;
  std::vector<line_post>     posts;
};

auto horizon_of(std::vector<line_post> const& posts, member const& sender, std::uint64_t latency)
    -> std::uint64_t
{
  std::uint64_t last = 0;
  for(auto const& post : posts)
  {
    last = std::max(last, (post.abort ? post.at.ps : post.frame.end.ps) - sender.origin);
  }
  return last + latency + ms_ps;
}

struct host_byte
{
  std::uint64_t at{};
  std::uint16_t data{};
};

void inject(member const& b, std::optional<host_byte> const& host)
{
  if(host)
  {
    std::vector<std::uint16_t> const one{host->data};
    b.port().receive(one);
  }
}

void set_baud(member const& b, std::uint32_t baud)
{
  if(baud != 0)
  {
    REQUIRE(b.f->target->mem().write(sym("usart_ll", "link_baud"), 4, baud).has_value());
  }
}

auto oracle(
    std::string_view         script,
    std::uint64_t            latency,
    std::string_view         receiver = "link_rx",
    std::optional<host_byte> host     = {},
    std::uint32_t            rx_baud  = 0) -> link_result
{
  member a{script};
  member b{receiver};
  set_baud(b, rx_baud);
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  for(auto const& post : a.box->posts)
  {
    deliver(post, a, b, latency);
  }
  if(host)
  {
    REQUIRE(b.to(host->at) == stop_reason::time);
    inject(b, host);
  }
  REQUIRE(b.to(horizon_of(a.box->posts, a, latency)) == stop_reason::time);
  return {.log = b.f->log(), .posts = a.box->posts};
}

auto barriers(
    std::string_view         script,
    std::uint64_t            latency,
    std::uint64_t            window,
    std::uint64_t            slice    = 1'000'000'000,
    std::string_view         receiver = "link_rx",
    std::optional<host_byte> host     = {},
    std::uint32_t            rx_baud  = 0) -> link_result
{
  member a{script, slice};
  member b{receiver, slice};
  set_baud(b, rx_baud);
  bool        done      = false;
  std::size_t delivered = 0;
  for(std::uint64_t barrier = window; not done; barrier += window)
  {
    done = a.to(barrier) == stop_reason::user;
    if(host and host->at <= barrier)
    {
      REQUIRE(b.to(host->at) == stop_reason::time);
      inject(b, host);
      host.reset();
    }
    REQUIRE(b.to(barrier, true) == stop_reason::time);
    for(; delivered < a.box->posts.size(); ++delivered)
    {
      deliver(a.box->posts[delivered], a, b, latency);
    }
  }
  REQUIRE(b.to(horizon_of(a.box->posts, a, latency)) == stop_reason::time);
  return {.log = b.f->log(), .posts = a.box->posts};
}

struct entry
{
  std::uint32_t kind{};
  std::uint32_t data{};
  std::uint32_t cycles{};
};

auto entries(std::vector<std::uint32_t> const& log) -> std::vector<entry>
{
  std::vector<entry> out;
  for(std::size_t index = 0; index + 1 < log.size(); index += 2)
  {
    out.push_back(
        {.kind = log[index] >> 16, .data = log[index] & 0xffffu, .cycles = log[index + 1]});
  }
  return out;
}

auto count_of(std::vector<std::uint32_t> const& log, std::uint32_t kind) -> std::size_t
{
  return static_cast<std::size_t>(
      std::ranges::count_if(entries(log), [kind](entry const& one) { return one.kind == kind; }));
}
}

TEST_CASE("a linked receiver gets the scripted frames the same whatever the barrier window")
{
  constexpr auto latency   = 20 * us_ps;
  auto const     reference = oracle("link_tx", latency);
  REQUIRE(count_of(reference.log, link_byte) == 64);
  CHECK(reference.posts.size() == 64);
  for(auto const window : {20 * us_ps, 7 * us_ps, us_ps})
  {
    CAPTURE(window);
    CHECK(barriers("link_tx", latency, window).log == reference.log);
  }
  CHECK(barriers("link_tx", latency, 20 * us_ps, 100).log == reference.log);
}

TEST_CASE("IDLE rises once per gap longer than a frame, at the last end plus L plus a frame")
{
  constexpr auto latency   = 20 * us_ps;
  auto const     reference = oracle("link_tx", latency);
  CHECK(count_of(reference.log, link_idle) == 3);
  auto const               seen = entries(reference.log);
  std::vector<std::size_t> idles;
  for(std::size_t index = 0; index < seen.size(); ++index)
  {
    if(seen[index].kind == link_idle)
    {
      idles.push_back(index);
    }
  }
  CHECK(idles == std::vector<std::size_t>{20, 41, 63 + 3});
}

TEST_CASE("IDLE rises in gaps between one frame and L although the next frame is queued")
{
  constexpr auto latency   = 100 * us_ps;
  auto const     reference = oracle("link_tx", latency);
  CHECK(count_of(reference.log, link_idle) == 3);
  for(auto const window : {100 * us_ps, 30 * us_ps})
  {
    CAPTURE(window);
    CHECK(barriers("link_tx", latency, window).log == reference.log);
  }
}

TEST_CASE("a longer latency shifts every received byte by the difference, exactly")
{
  auto const near = entries(oracle("link_tx", 20 * us_ps).log);
  auto const far  = entries(oracle("link_tx", 40 * us_ps).log);
  REQUIRE(near.size() == far.size());
  for(std::size_t index = 0; index < near.size(); ++index)
  {
    CAPTURE(index);
    CHECK(far[index].kind == near[index].kind);
    CHECK(far[index].data == near[index].data);
    CHECK(far[index].cycles - near[index].cycles == 20 * cycles_per_us);
  }
}

TEST_CASE("each frame reaches RDR at its sender's stop-bit end plus L, at the instruction")
{
  constexpr auto latency = 20 * us_ps;
  member         a{"link_tx"};
  member         b{"link_rx"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  auto const& first = a.box->posts.front().frame;
  CHECK(first.end.ps - first.start.ps == first.frame_ps);
  REQUIRE(b.to(first.start.ps - a.origin) == stop_reason::time);
  deliver(a.box->posts.front(), a, b, latency);
  auto const arrives = first.end.ps - a.origin + latency;
  auto const pending = b.f->target->snapshot().data().events.pending;
  auto const linked  = std::ranges::find(pending, std::uint32_t{1}, &core::event::lane);
  REQUIRE(linked != pending.end());
  CHECK(linked->due_ps == b.origin + arrives);
  REQUIRE(b.to(arrives - 7 * us_ps / 1000) == stop_reason::time);
  CHECK(b.f->field("USART1", "ISR_enabled", "RXFNE") == 0);
  auto const stopped =
      b.f->run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{1}});
  CHECK(stopped.executed.value == 1);
  CHECK(b.f->target->virtual_clock().time().ps >= b.origin + arrives);
  CHECK(b.f->target->virtual_clock().time().ps < b.origin + arrives + 2 * 6'250);
  CHECK(b.f->field("USART1", "ISR_enabled", "RXFNE") == 1);
}

TEST_CASE("an aborted frame never reaches RDR, the next one does, whatever the window")
{
  constexpr auto             latency   = 20 * us_ps;
  auto const                 reference = oracle("link_abort", latency);
  std::vector<std::uint32_t> bytes;
  for(auto const& one : entries(reference.log))
  {
    if(one.kind == link_byte)
    {
      bytes.push_back(one.data);
    }
  }
  CHECK(bytes == std::vector<std::uint32_t>{'a', 'c'});
  CHECK(
      std::ranges::count_if(reference.posts, [](line_post const& post) { return post.abort; })
      == 1);
  for(auto const window : {latency, latency / 4})
  {
    CAPTURE(window);
    CHECK(barriers("link_abort", latency, window).log == reference.log);
  }
}

TEST_CASE("a frame queued ahead of the head becomes the head, both arrive at their own ends")
{
  member b{"link_rx"};
  REQUIRE(b.to(2 * ms_ps) == stop_reason::time);
  auto const base  = b.f->target->virtual_clock().time().ps;
  auto const frame = [&](std::uint16_t data, std::uint64_t end_us) {
    return stm32::line_frame{
        .data     = data,
        .frame_ps = 21 * us_ps,
        .start    = vtime{base + (end_us - 21) * us_ps},
        .end      = vtime{base + end_us * us_ps}};
  };
  b.port().receive_at(frame('x', 200), 1);
  b.port().receive_at(frame('y', 100), 1);
  REQUIRE(b.to(base - b.origin + ms_ps) == stop_reason::time);
  auto const                 seen = entries(b.f->log());
  std::vector<std::uint32_t> bytes;
  for(auto const& one : seen)
  {
    if(one.kind == link_byte)
    {
      bytes.push_back(one.data);
    }
  }
  CHECK(bytes == std::vector<std::uint32_t>{'y', 'x'});
}

TEST_CASE("a reset of the receiver drops the frames queued from the other machine")
{
  member a{"link_tx"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  member b{"link_rx"};
  REQUIRE(b.to(a.box->posts.front().frame.start.ps - a.origin) == stop_reason::time);
  for(std::size_t index = 0; index < 3; ++index)
  {
    deliver(a.box->posts[index], a, b, 20 * us_ps);
  }
  b.f->target->reset();
  REQUIRE(
      b.f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("usart_ll", "main")})
          .reason
      == stop_reason::address);
  b.f->set("pc", sym("usart_ll", "link_rx"));
  b.origin = b.f->target->virtual_clock().time().ps;
  REQUIRE(b.to(ms_ps) == stop_reason::time);
  CHECK(count_of(b.f->log(), link_byte) == 0);
  auto const now = b.f->target->virtual_clock().time().ps;
  b.port().receive_at(
      {.data     = 'z',
       .frame_ps = 21 * us_ps,
       .start    = vtime{now + 79 * us_ps},
       .end      = vtime{now + 100 * us_ps}},
      1);
  std::uint64_t last = 0;
  for(std::size_t index = 0; index < 3; ++index)
  {
    last = std::max(last, shifted(a.box->posts[index].frame.end, a, b, 20 * us_ps).ps);
  }
  REQUIRE(b.to(std::max(last, now + 100 * us_ps) - b.origin + ms_ps) == stop_reason::time);
  std::vector<std::uint32_t> bytes;
  for(auto const& one : entries(b.f->log()))
  {
    if(one.kind == link_byte)
    {
      bytes.push_back(one.data);
    }
  }
  CHECK(bytes == std::vector<std::uint32_t>{'z'});
}

TEST_CASE("a receiver snapshot mid-gap and mid-frame resumes in a fresh machine as the oracle")
{
  constexpr auto latency   = 20 * us_ps;
  constexpr auto window    = 7 * us_ps;
  auto const     reference = oracle("link_tx", latency);
  for(auto const cut : {std::size_t{21}, std::size_t{30}})
  {
    CAPTURE(cut);
    member      a{"link_tx"};
    auto        b         = std::make_unique<member>("link_rx");
    bool        done      = false;
    bool        saved     = false;
    std::size_t delivered = 0;
    for(std::uint64_t barrier = window; not done; barrier += window)
    {
      done = a.to(barrier) == stop_reason::user;
      REQUIRE(b->to(barrier, true) == stop_reason::time);
      for(; delivered < a.box->posts.size(); ++delivered)
      {
        deliver(a.box->posts[delivered], a, *b, latency);
      }
      if(not saved and count_of(b->f->log(), link_byte) >= cut)
      {
        auto const bytes = core::encode_snapshot(b->f->target->snapshot().data());
        auto       fresh = std::make_unique<member>("link_rx");
        fresh->f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
        fresh->origin = b->origin;
        b             = std::move(fresh);
        saved         = true;
      }
    }
    REQUIRE(saved);
    REQUIRE(b->to(horizon_of(a.box->posts, a, latency)) == stop_reason::time);
    CHECK(b->f->log() == reference.log);
  }
}

TEST_CASE("the sender posts each frame at its start bit, aborts once on a UE clear, not on reset")
{
  member a{"link_abort"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  auto const& posts = a.box->posts;
  REQUIRE(posts.size() == 4);
  CHECK_FALSE(posts[0].abort);
  CHECK(posts[1].frame.data == 'b');
  CHECK(posts[2].abort);
  CHECK(posts[2].at.ps > posts[1].frame.start.ps);
  CHECK(posts[2].at.ps < posts[1].frame.end.ps);
  CHECK(posts[3].frame.data == 'c');
  for(auto const& post : posts)
  {
    if(not post.abort)
    {
      CHECK(post.frame.end.ps - post.frame.start.ps == post.frame.frame_ps);
      CHECK(post.frame.word_bits == 8);
      CHECK(post.frame.half_stops == 2);
    }
  }
  CHECK(a.box->transmissions == 2);
  a.f->target->reset();
  CHECK(posts.size() == 4);
}

TEST_CASE("a frame landing on the count of a host frame orders the same whatever the window")
{
  constexpr std::uint16_t host_data = 0x99;
  member                  a{"link_tx"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  REQUIRE(a.box->posts.size() == 64);
  auto const& first  = a.box->posts[40];
  auto const  before = first.frame.start.ps - a.origin - us_ps;

  member probe{"link_rx_2stop"};
  REQUIRE(probe.to(before) == stop_reason::time);
  auto const t0   = probe.f->target->virtual_clock().time().ps;
  auto const brr  = probe.f->field("USART1", "BRR", "BRR");
  auto const f_rx = 22 * ps_per_second / (2 * (160'000'000 / brr));
  deliver(first, a, probe, 0);
  auto const pending = probe.f->target->snapshot().data().events.pending;
  auto const linked  = std::ranges::find(pending, std::uint32_t{1}, &core::event::lane);
  REQUIRE(linked != pending.end());
  REQUIRE(t0 + f_rx > linked->due_ps);
  auto const latency = t0 + f_rx - linked->due_ps;
  CAPTURE(latency);

  auto const host      = host_byte{.at = before, .data = host_data};
  auto const reference = oracle("link_tx", latency, "link_rx_2stop", host);
  auto const seen      = entries(reference.log);
  CHECK(std::ranges::any_of(seen, [](entry const& one) {
    return one.kind == link_byte and one.data == host_data;
  }));
  CHECK(std::ranges::none_of(seen, [](entry const& one) {
    return one.kind == link_byte and one.data == 0x20u + 40u;
  }));
  CHECK(count_of(reference.log, 3) == 1);
  for(auto const window : {latency, latency / 3})
  {
    CAPTURE(window);
    CHECK(
        barriers("link_tx", latency, window, 1'000'000'000, "link_rx_2stop", host).log
        == reference.log);
  }
}

TEST_CASE("an aborted frame ends on the wire's lane at the abort, delivering nothing")
{
  constexpr auto latency = 20 * us_ps;
  member         a{"link_abort"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  member      b{"link_rx"};
  auto const& posts = a.box->posts;
  REQUIRE(posts.size() == 4);
  REQUIRE(b.to(posts[1].frame.start.ps - a.origin) == stop_reason::time);
  deliver(posts[1], a, b, latency);
  deliver(posts[2], a, b, latency);
  auto const pending = b.f->target->snapshot().data().events.pending;
  auto const cut     = std::ranges::find_if(
      pending, [](core::event const& one) { return one.id.kind == 3 and one.lane == 1; });
  REQUIRE(cut != pending.end());
  CHECK(cut->due_ps == shifted(posts[2].at, a, b, latency).ps);
  CHECK(std::ranges::none_of(pending, [](core::event const& one) { return one.id.kind == 2; }));
}

TEST_CASE("an abort behind a queued frame idles the line one frame after the abort at any window")
{
  constexpr auto             latency   = 20 * us_ps;
  auto const                 reference = oracle("link_abort_b2b", latency);
  auto const                 seen      = entries(reference.log);
  std::vector<std::uint32_t> bytes;
  for(auto const& one : seen)
  {
    if(one.kind == link_byte)
    {
      bytes.push_back(one.data);
    }
  }
  CHECK(bytes == std::vector<std::uint32_t>{'a', 'c'});
  auto const& posts = reference.posts;
  REQUIRE(posts.size() == 4);
  REQUIRE(posts[2].abort);
  REQUIRE(seen.size() >= 3);
  CHECK(seen[0].data == 'a');
  CHECK(seen[1].kind == link_idle);
  auto const after_a  = posts[2].at.ps - posts[0].frame.end.ps + posts[0].frame.frame_ps;
  auto const expected = static_cast<std::int64_t>(after_a / 6'250);
  auto const measured = static_cast<std::int64_t>(seen[1].cycles - seen[0].cycles);
  CAPTURE(expected);
  CAPTURE(measured);
  CHECK(measured >= expected - 20);
  CHECK(measured <= expected + 20);
  for(auto const window : {latency, latency / 4, latency / 20})
  {
    CAPTURE(window);
    CHECK(barriers("link_abort_b2b", latency, window).log == reference.log);
  }
}

namespace
{
auto usart_isr(stm32_fixture const& f, std::string_view unit, std::string_view name)
    -> std::uint32_t
{
  return f.field(unit, "ISR_enabled", name);
}

auto ring_bytes(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  auto const                 ring = sym("usart_ll", "ring");
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    auto const byte = f.target->mem().read(ring + index, 1);
    REQUIRE(byte.has_value());
    out.push_back(static_cast<std::uint32_t>(*byte));
  }
  return out;
}

void run_for(stm32_fixture const& f, std::uint64_t ps)
{
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000},
                     .until_time       = vtime{f.target->virtual_clock().time().ps + ps}})
          .reason
      == stop_reason::time);
}

auto rdr_of(stm32_fixture const& f, std::string_view unit) -> std::uint32_t
{
  auto const value = f.target->mem().read(f.target->peripheral(unit).description().base + 0x24, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value) & 0xffu;
}

constexpr std::uint64_t frame_115200 = 10 * ps_per_second / 115'200;
}

TEST_CASE("a held receiver's RX ring keeps filling without its GPDMA freeze bit, no overrun")
{
  rig r{"rx_dma_list"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  r.f->target->hold_core(true);
  std::vector<std::uint16_t> sent;
  for(std::uint16_t index = 0; index < 10; ++index)
  {
    sent.push_back(static_cast<std::uint16_t>(0x40u + index));
  }
  r.port("USART2").receive(sent);
  run_for(*r.f, 12 * frame_115200);
  auto const ring = ring_bytes(*r.f);
  for(std::uint32_t index = 0; index < 10; ++index)
  {
    CHECK(ring[index] == 0x40u + index);
  }
  CHECK(usart_isr(*r.f, "USART2", "ORE") == 0);
}

TEST_CASE("with its GPDMA freeze bit a held receiver's ring stops and the second frame overruns")
{
  rig r{"rx_dma_list"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  popcorn::testing::set_debug_freeze(*r.f, "DBG_GPDMA3_STOP", true);
  r.f->target->hold_core(true);
  std::vector<std::uint16_t> sent;
  for(std::uint16_t index = 0; index < 4; ++index)
  {
    sent.push_back(static_cast<std::uint16_t>(0x40u + index));
  }
  r.port("USART2").receive(sent);
  run_for(*r.f, frame_115200 + frame_115200 / 2);
  CHECK(usart_isr(*r.f, "USART2", "RXFNE") == 1);
  CHECK(usart_isr(*r.f, "USART2", "ORE") == 0);
  run_for(*r.f, frame_115200);
  CHECK(usart_isr(*r.f, "USART2", "ORE") == 1);
  CHECK(ring_bytes(*r.f) == std::vector<std::uint32_t>(16, 0));
  CHECK(rdr_of(*r.f, "USART2") == 0x40u);
}

TEST_CASE("a held core without RX DMA keeps the first byte, overruns, and takes the ISR first")
{
  rig r{"rx_irq"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  r.f->target->hold_core(true);
  std::vector<std::uint16_t> const sent{'p', 'o', 'p'};
  r.port("USART1").receive(sent);
  run_for(*r.f, frame_115200 + frame_115200 / 2);
  CHECK(usart_isr(*r.f, "USART1", "ORE") == 0);
  run_for(*r.f, 2 * frame_115200);
  CHECK(usart_isr(*r.f, "USART1", "ORE") == 1);
  CHECK(rdr_of(*r.f, "USART1") == 'p');
  CHECK(r.port("USART1").queued() == 0);
  r.f->target->hold_core(false);
  auto const step =
      r.f->run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{1}});
  CHECK(step.executed.value == 1);
  auto const handler = sym("usart_ll", "USART1_IRQHandler") & ~popcorn::address{1};
  auto const pc_reg  = r.f->target->regs().find("pc");
  REQUIRE(pc_reg.has_value());
  auto const pc = r.f->target->regs().read(*pc_reg);
  CHECK(pc >= handler);
  CHECK(pc < handler + 16);
}

TEST_CASE("a linked frame ending during a host frame leaves the IDLE to the host frame's end")
{
  member     b{"link_rx"};
  auto const start = 2 * ms_ps;
  REQUIRE(b.to(start) == stop_reason::time);
  auto const                       brr   = b.f->field("USART1", "BRR", "BRR");
  auto const                       frame = 20 * ps_per_second / (2 * (160'000'000 / brr));
  auto const                       now   = b.f->target->virtual_clock().time().ps;
  std::vector<std::uint16_t> const one{0x99};
  b.port().receive(one);
  b.port().receive_at(
      {.data     = 0x42,
       .frame_ps = frame,
       .start    = vtime{now + frame / 2 - frame},
       .end      = vtime{now + frame / 2}},
      1);
  REQUIRE(b.to(start + 3 * frame + 5 * us_ps) == stop_reason::time);
  auto const seen = entries(b.f->log());
  auto const host = std::ranges::find_if(
      seen, [](entry const& one) { return one.kind == link_byte and one.data == 0x99; });
  auto const idle =
      std::ranges::find_if(seen, [](entry const& one) { return one.kind == link_idle; });
  REQUIRE(host != seen.end());
  REQUIRE(idle != seen.end());
  CHECK(idle->cycles - host->cycles + 100 >= frame * cycles_per_us / us_ps);
}

namespace
{
constexpr std::uint32_t link_error = 4;

auto bytes_of(std::vector<std::uint32_t> const& log) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const& one : entries(log))
  {
    if(one.kind == link_byte)
    {
      out.push_back(one.data);
    }
  }
  return out;
}

auto errors_of(std::vector<std::uint32_t> const& log) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const& one : entries(log))
  {
    if(one.kind == link_error)
    {
      out.push_back(one.data);
    }
  }
  return out;
}

auto scripted_bytes() -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < 64; ++index)
  {
    out.push_back(0x20u + index);
  }
  return out;
}

auto at_percent(int tenths_of_percent) -> std::uint32_t
{
  return static_cast<std::uint32_t>(460'800LL * (1000 + tenths_of_percent) / 1000);
}

void same_across_windows(
    std::string_view   script,
    std::uint32_t      rx_baud,
    link_result const& reference)
{
  constexpr auto latency = 20 * us_ps;
  for(auto const window : {latency, 7 * us_ps})
  {
    CAPTURE(window);
    CHECK(
        barriers(script, latency, window, 1'000'000'000, "link_rx_at", {}, rx_baud).log
        == reference.log);
  }
}
}

TEST_CASE("a receiver within 2 percent of the sender's baud reads every byte, no error")
{
  for(auto const tenths : {-20, 20})
  {
    CAPTURE(tenths);
    auto const reference = oracle("link_tx", 20 * us_ps, "link_rx_at", {}, at_percent(tenths));
    CHECK(bytes_of(reference.log) == scripted_bytes());
    CHECK(errors_of(reference.log).empty());
    same_across_windows("link_tx", at_percent(tenths), reference);
  }
}

TEST_CASE("a receiver 10 percent off samples the sender's waveform into the computed bytes")
{
  auto const slow = oracle("link_tx_pattern", 20 * us_ps, "link_rx_at", {}, at_percent(-100));
  auto const fast = oracle("link_tx_pattern", 20 * us_ps, "link_rx_at", {}, at_percent(100));
  std::vector<std::uint32_t> slow_bytes;
  std::vector<std::uint32_t> fast_bytes;
  std::vector<std::uint32_t> fast_errors;
  for(int pair = 0; pair < 8; ++pair)
  {
    slow_bytes.insert(slow_bytes.end(), {0xad, 0xf8});
    fast_bytes.insert(fast_bytes.end(), {0xb5, 0xf0});
    fast_errors.insert(fast_errors.end(), {3, 2});
  }
  slow_bytes.push_back(0xad);
  fast_bytes.push_back(0xb5);
  fast_errors.push_back(3);
  std::vector<std::uint32_t> slow_errors(15, 3);
  slow_errors.insert(slow_errors.end(), {2, 2});
  CHECK(bytes_of(slow.log) == slow_bytes);
  CHECK(errors_of(slow.log) == slow_errors);
  CHECK(bytes_of(fast.log) == fast_bytes);
  CHECK(errors_of(fast.log) == fast_errors);
  same_across_windows("link_tx_pattern", at_percent(-100), slow);
  same_across_windows("link_tx_pattern", at_percent(100), fast);
  CHECK(
      barriers(
          "link_tx_pattern", 20 * us_ps, us_ps, 1'000'000'000, "link_rx_at", {}, at_percent(-100))
          .log
      == slow.log);
}

TEST_CASE("a 115200 receiver on a 460800 sender reads the same garbage at any window")
{
  auto const reference = oracle("link_tx", 20 * us_ps, "link_rx_at", {}, 115'200);
  CHECK(bytes_of(reference.log) != scripted_bytes());
  CHECK_FALSE(errors_of(reference.log).empty());
  same_across_windows("link_tx", 115'200, reference);
}

TEST_CASE("a sender that drops to 115200 between bursts is sampled as a static mismatch")
{
  auto const reference = oracle("link_tx_switch", 20 * us_ps, "link_rx_at", {}, 460'800);
  auto const bytes     = bytes_of(reference.log);
  REQUIRE(bytes.size() >= 5);
  CHECK(
      std::vector<std::uint32_t>(bytes.begin(), bytes.begin() + 4)
      == std::vector<std::uint32_t>(4, 0x55));
  CHECK(bytes[4] != 0x55);
  CHECK_FALSE(errors_of(reference.log).empty());
  same_across_windows("link_tx_switch", 460'800, reference);
}

TEST_CASE("the receiver's error-free band is recorded each side, in 0.25 percent steps")
{
  auto const band = [](std::string_view script, std::vector<std::uint32_t> const& expected) {
    auto const clean = [&](int quarter_percent) {
      auto const baud   = static_cast<std::uint32_t>(460'800LL * (400 + quarter_percent) / 400);
      auto const result = oracle(script, 20 * us_ps, "link_rx_at", {}, baud);
      return bytes_of(result.log) == expected and errors_of(result.log).empty();
    };
    int slower = 0;
    while(clean(slower - 1))
    {
      --slower;
      REQUIRE(slower > -80);
    }
    int faster = 0;
    while(clean(faster + 1))
    {
      ++faster;
      REQUIRE(faster < 80);
    }
    return std::pair{slower * 25, faster * 25};
  };
  auto const script = band("link_tx", scripted_bytes());
  auto const fives  = band("link_tx_55", std::vector<std::uint32_t>(48, 0x55));
  MESSAGE("script error-free " << script.first << " to " << script.second << " hundredths");
  MESSAGE("0x55 error-free " << fives.first << " to " << fives.second << " hundredths");
  CHECK(script == std::pair{-400, 500});
  CHECK(fives == std::pair{-400, 500});
}

TEST_CASE("OVER8 and ONEBIT receivers sample 0x55 at their own offsets")
{
  auto const slow = at_percent(-100);
  auto const run  = [](std::string_view receiver, std::uint32_t baud) {
    return oracle("link_tx_55", 20 * us_ps, receiver, {}, baud);
  };
  auto const over8 = run("link_rx_over8", slow);
  CHECK(bytes_of(over8.log) == std::vector<std::uint32_t>(48, 0xad));
  std::vector<std::uint32_t> back_to_back(47, 3);
  back_to_back.push_back(2);
  CHECK(errors_of(over8.log) == back_to_back);

  auto const onebit = run("link_rx_onebit", slow);
  CHECK(bytes_of(onebit.log) == std::vector<std::uint32_t>(48, 0xad));
  std::vector<std::uint32_t> framing_only(47, 1);
  CHECK(errors_of(onebit.log) == framing_only);

  auto const near = run("link_rx_over8", at_percent(-40));
  CHECK(bytes_of(near.log) == std::vector<std::uint32_t>(48, 0x55));
  CHECK(errors_of(near.log) == std::vector<std::uint32_t>(47, 2));
  auto const near16 = run("link_rx_at", at_percent(-40));
  CHECK(errors_of(near16.log).empty());
}

namespace
{
constexpr std::uint32_t cr1_re     = 1u << 2;
constexpr std::uint32_t cr1_te     = 1u << 3;
constexpr std::uint32_t cr1_peie   = 1u << 8;
constexpr std::uint32_t cr1_ps     = 1u << 9;
constexpr std::uint32_t cr1_pce    = 1u << 10;
constexpr std::uint32_t cr1_m0     = 1u << 12;
constexpr std::uint32_t cr1_m1     = 1u << 28;
constexpr std::uint32_t cr1_fifoen = 1u << 29;
constexpr std::uint32_t isr_pe     = 1u << 0;
constexpr std::uint32_t isr_rxfne  = 1u << 5;
constexpr std::uint32_t isr_idle   = 1u << 4;
constexpr std::uint32_t icr_pecf   = 1u << 0;
constexpr std::uint32_t rqr_rxfrq  = 1u << 3;
constexpr std::uint32_t isr_fe     = 1u << 1;
constexpr std::uint32_t isr_ne     = 1u << 2;
constexpr std::uint32_t isr_ore    = 1u << 3;
constexpr std::uint32_t isr_errors = isr_pe | isr_fe | isr_ne;
constexpr std::uint32_t icr_errors = 0b111u;
constexpr std::uint32_t nvic_ispr1 = 0xe000'e204u;
constexpr std::uint32_t nvic_icpr1 = 0xe000'e284u;
constexpr std::uint32_t usart1_irq = 1u << (61 - 32);

struct line_config
{
  std::uint32_t      bits{8};
  stm32::line_parity parity{};
};

constexpr std::array<line_config, 9> line_configs{
    {
     {.bits = 9, .parity = stm32::line_parity::even},
     {.bits = 9, .parity = stm32::line_parity::odd},
     {.bits = 8, .parity = stm32::line_parity::even},
     {.bits = 8, .parity = stm32::line_parity::odd},
     {.bits = 7, .parity = stm32::line_parity::even},
     {.bits = 7, .parity = stm32::line_parity::odd},
     {.bits = 9, .parity = stm32::line_parity::none},
     {.bits = 8, .parity = stm32::line_parity::none},
     {.bits = 7, .parity = stm32::line_parity::none},
     }
};

constexpr std::array<std::uint32_t, 4> parity_values{0x00, 0x01, 0x7f, 0xff};

auto cr1_of(line_config config) -> std::uint32_t
{
  std::uint32_t out = ue | cr1_re | cr1_te;
  out |= config.bits == 9 ? cr1_m0 : (config.bits == 7 ? cr1_m1 : 0u);
  if(config.parity != stm32::line_parity::none)
  {
    out |= cr1_pce | (config.parity == stm32::line_parity::odd ? cr1_ps : 0u);
  }
  return out;
}

auto ones_of(std::uint32_t word) -> std::uint32_t
{
  std::uint32_t ones = 0;
  for(; word != 0; word >>= 1)
  {
    ones += word & 1u;
  }
  return ones;
}

auto data_bits(line_config config) -> std::uint32_t
{
  return config.parity == stm32::line_parity::none ? config.bits : config.bits - 1;
}

auto expected_word(std::uint32_t value, line_config config) -> std::uint32_t
{
  auto const data = value & ((1u << data_bits(config)) - 1u);
  if(config.parity == stm32::line_parity::none)
  {
    return data;
  }
  auto const odd = config.parity == stm32::line_parity::odd ? 1u : 0u;
  return data | (((ones_of(data) % 2) ^ odd) << (config.bits - 1));
}

struct parity_rig
{
  locked_rig               t;
  std::shared_ptr<mailbox> box = std::make_shared<mailbox>();

  explicit parity_rig(line_config config, std::uint32_t brr = 35, std::uint32_t extra = 0)
  {
    t.r.port("USART1").attach(box);
    configure(config, brr, extra);
  }

  void configure(line_config config, std::uint32_t brr = 35, std::uint32_t extra = 0) const
  {
    t.cpu("USART1", "CR1_enabled", 0);
    t.cpu("USART1", "BRR", brr);
    t.cpu("USART1", "CR1_enabled", cr1_of(config) | extra);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return t.r.f->target->virtual_clock().time().ps;
  }

  void advance(std::uint64_t ps) const
  {
    auto const until = now() + ps;
    for(;;)
    {
      auto const result = t.r.f->run(
          {.max_instructions = icount{1'000'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{until}});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == stop_reason::time);
        return;
      }
    }
  }

  void send(std::uint32_t value) const
  {
    t.cpu("USART1", "TDR", value);
    advance(200 * us_ps);
  }

  void host(std::uint16_t value) const
  {
    std::vector<std::uint16_t> const one{value};
    t.r.port("USART1").receive(one);
    advance(200 * us_ps);
  }

  void linked(stm32::line_frame frame) const
  {
    auto const start = now() + 10 * us_ps;
    frame.end        = vtime{start + (frame.end.ps - frame.start.ps)};
    frame.start      = vtime{start};
    t.r.port("USART1").receive_at(frame, 1);
    advance(300 * us_ps);
  }

  [[nodiscard]] auto isr() const -> std::uint32_t
  {
    return t.read("USART1", "ISR_enabled");
  }

  [[nodiscard]] auto rdr() const -> std::uint32_t
  {
    return t.read("USART1", "RDR");
  }

  [[nodiscard]] auto pop() const -> std::uint32_t
  {
    auto* const probe = t.r.f->model_of<bus_probe>("DAC1");
    REQUIRE(probe != nullptr);
    auto const word = probe->context.services.read(
        address_of(*t.r.f, "USART1", "RDR"), 4, popcorn::initiator::cpu);
    REQUIRE(word.has_value());
    return static_cast<std::uint32_t>(*word);
  }

  void tagged(std::span<stm32::host_frame const> frames) const
  {
    t.r.port("USART1").receive_frames(frames);
  }

  [[nodiscard]] auto pending() const -> bool
  {
    auto const word = t.r.f->target->mem().read(nvic_ispr1, 4);
    REQUIRE(word.has_value());
    return (*word & usart1_irq) != 0;
  }

  void unpend() const
  {
    REQUIRE(t.r.f->target->mem().write(nvic_icpr1, 4, usart1_irq).has_value());
  }

  void clear_rx() const
  {
    t.cpu("USART1", "ICR", icr_pecf);
    t.cpu("USART1", "RQR", rqr_rxfrq);
  }
};

struct received
{
  std::uint32_t word{};
  bool          pe{};

  friend auto operator==(received const&, received const&) -> bool = default;
};

auto across(
    line_config                    sender,
    line_config                    receiver,
    std::span<std::uint32_t const> values,
    std::uint32_t                  receiver_brr = 35) -> std::vector<received>
{
  parity_rig a{sender};
  parity_rig b{receiver, receiver_brr};
  for(auto const value : values)
  {
    a.send(value);
  }
  REQUIRE(a.box->posts.size() == values.size());
  std::vector<received> out;
  for(auto const& post : a.box->posts)
  {
    b.linked(post.frame);
    CHECK((b.isr() & isr_rxfne) != 0);
    out.push_back({.word = b.rdr(), .pe = (b.isr() & isr_pe) != 0});
    b.clear_rx();
  }
  return out;
}
}

TEST_CASE("TX frames carry the parity bit in their word and the endpoint gets the data bits")
{
  parity_rig p{line_configs[0]};
  for(auto const config : line_configs)
  {
    CAPTURE(config.bits);
    CAPTURE(config.parity);
    p.configure(config);
    p.box->posts.clear();
    p.box->sent.clear();
    for(auto const value : parity_values)
    {
      p.send(value);
    }
    REQUIRE(p.box->posts.size() == parity_values.size());
    REQUIRE(p.box->sent.size() == parity_values.size());
    for(std::size_t index = 0; index < parity_values.size(); ++index)
    {
      CAPTURE(parity_values[index]);
      auto const word = expected_word(parity_values[index], config);
      CHECK(p.box->posts[index].frame.data == word);
      CHECK(p.box->posts[index].frame.parity == config.parity);
      CHECK(p.box->posts[index].frame.word_bits == config.bits);
      CHECK(p.box->sent[index].data == (word & ((1u << data_bits(config)) - 1u)));
      if(config.parity != stm32::line_parity::none)
      {
        CHECK(ones_of(word) % 2 == (config.parity == stm32::line_parity::odd ? 1u : 0u));
      }
    }
  }
}

TEST_CASE("PCE 0 sends the TDR word unchanged in the M bits")
{
  parity_rig p{
      {.bits = 9, .parity = stm32::line_parity::none}
  };
  for(auto const value : {0x000u, 0x0ffu, 0x100u, 0x1ffu, 0x2a5u})
  {
    p.send(value);
  }
  std::vector<std::uint32_t> words;
  std::vector<std::uint32_t> sent;
  for(std::size_t index = 0; index < p.box->posts.size(); ++index)
  {
    words.push_back(p.box->posts[index].frame.data);
    sent.push_back(p.box->sent[index].data);
  }
  CHECK(words == std::vector<std::uint32_t>{0x000, 0x0ff, 0x100, 0x1ff, 0x0a5});
  CHECK(sent == words);
}

TEST_CASE("an 8E1 frame takes the time of a 9N1 frame and 11 bit times")
{
  auto const frame_of = [](line_config config) {
    parity_rig p{config};
    p.send(0x55);
    p.send(0x56);
    REQUIRE(p.box->posts.size() == 2);
    REQUIRE(p.box->sent.size() == 2);
    auto const& frame = p.box->posts[0].frame;
    CHECK(frame.end.ps - frame.start.ps == frame.frame_ps);
    CHECK(p.box->sent[0].at == frame.end);
    return frame.frame_ps;
  };
  auto const even = frame_of({.bits = 9, .parity = stm32::line_parity::even});
  auto const odd  = frame_of({.bits = 9, .parity = stm32::line_parity::odd});
  auto const nine = frame_of({.bits = 9, .parity = stm32::line_parity::none});
  auto const n81  = frame_of({.bits = 8, .parity = stm32::line_parity::none});
  CHECK(even == nine);
  CHECK(odd == nine);
  CHECK(even * 10 + 10 >= n81 * 11);
  CHECK(even * 10 <= n81 * 11 + 10);
}

TEST_CASE("host frames reach RDR with the receiver's parity bit and never set PE")
{
  parity_rig p{line_configs[0]};
  for(auto const config : line_configs)
  {
    if(config.parity == stm32::line_parity::none)
    {
      continue;
    }
    CAPTURE(config.bits);
    CAPTURE(config.parity);
    p.configure(config);
    for(auto const value : {0x00u, 0x01u, 0x7fu, 0xffu, 0xc1u})
    {
      CAPTURE(value);
      p.host(static_cast<std::uint16_t>(value));
      CHECK((p.isr() & isr_rxfne) != 0);
      CHECK((p.isr() & isr_pe) == 0);
      CHECK(p.rdr() == expected_word(value, config));
      p.clear_rx();
    }
  }
  p.configure({.bits = 8, .parity = stm32::line_parity::none});
  p.host(0xc1);
  CHECK(p.rdr() == 0xc1);
}

TEST_CASE("a host byte is masked to the M-bit word without a parity")
{
  parity_rig p{
      {.bits = 7, .parity = stm32::line_parity::none}
  };
  p.host(0xff);
  CHECK(p.rdr() == 0x7f);
  p.clear_rx();
  p.configure({.bits = 8, .parity = stm32::line_parity::none});
  p.host(0xc1);
  CHECK(p.rdr() == 0xc1);
  p.clear_rx();
  p.configure({.bits = 9, .parity = stm32::line_parity::none});
  p.host(0x1ff);
  CHECK(p.rdr() == 0x1ff);
}

TEST_CASE(
    "a linked frame of the receiver's parity sets no PE and one of the other parity always does")
{
  for(auto const bits : {9u, 8u, 7u})
  {
    CAPTURE(bits);
    line_config const     even{.bits = bits, .parity = stm32::line_parity::even};
    line_config const     odd{.bits = bits, .parity = stm32::line_parity::odd};
    std::vector<received> same;
    std::vector<received> crossed;
    for(auto const value : parity_values)
    {
      same.push_back({.word = expected_word(value, even), .pe = false});
      crossed.push_back({.word = expected_word(value, odd), .pe = true});
    }
    CHECK(across(even, even, parity_values) == same);
    CHECK(across(odd, even, parity_values) == crossed);
  }
}

TEST_CASE("a 9N1 sender on an 8E1 receiver sets PE on words with an odd count of ones")
{
  std::array<std::uint32_t, 6> const words{0x000, 0x001, 0x003, 0x100, 0x1ff, 0x0fe};
  std::vector<received>              expected;
  for(auto const word : words)
  {
    expected.push_back({.word = word, .pe = ones_of(word) % 2 == 1});
  }
  CHECK(
      across(
          {.bits = 9, .parity = stm32::line_parity::none},
          {.bits = 9, .parity = stm32::line_parity::even}, words)
      == expected);
}

TEST_CASE("a sampled linked frame is checked as sampled: same parity clean and the other one PE")
{
  line_config const     even{.bits = 9, .parity = stm32::line_parity::even};
  line_config const     odd{.bits = 9, .parity = stm32::line_parity::odd};
  std::vector<received> same;
  std::vector<received> crossed;
  for(auto const value : parity_values)
  {
    same.push_back({.word = expected_word(value, even), .pe = false});
    crossed.push_back({.word = expected_word(value, odd), .pe = true});
  }
  CHECK(across(even, even, parity_values, 36) == same);
  CHECK(across(odd, even, parity_values, 36) == crossed);
}

TEST_CASE("PE raises USART1_IRQn with PEIE only and PECF clears it")
{
  line_config const even{.bits = 9, .parity = stm32::line_parity::even};
  parity_rig        a{
      {.bits = 9, .parity = stm32::line_parity::odd}
  };
  parity_rig b{even};
  a.send(0x41);
  REQUIRE(a.box->posts.size() == 1);
  b.linked(a.box->posts[0].frame);
  REQUIRE((b.isr() & isr_pe) != 0);
  b.t.cpu("USART1", "RQR", rqr_rxfrq);
  b.unpend();
  CHECK_FALSE(b.pending());
  b.t.cpu("USART1", "CR3", 1u);
  CHECK((b.isr() & isr_pe) != 0);
  CHECK_FALSE(b.pending());
  b.t.cpu("USART1", "CR1_enabled", cr1_of(even) | cr1_peie);
  CHECK(b.pending());
  b.t.cpu("USART1", "ICR", icr_pecf);
  CHECK((b.isr() & isr_pe) == 0);
  b.unpend();
  CHECK_FALSE(b.pending());
}

TEST_CASE("PCE under strict peripherals runs without a model_error or a warning")
{
  locked_rig t{true};
  t.cpu("USART1", "CR1_enabled", cr1_pce | cr1_m0);
  t.cpu("USART1", "CR1_enabled", ue | cr1_pce | cr1_m0 | cr1_te);
  REQUIRE(t.r.f->run_to_stop({.budget_left = icount{10}}).reason != stop_reason::user);
  CHECK(t.logged.empty());
}

TEST_CASE("a snapshot between host frames under 7E1 restores to the same RDR words")
{
  parity_rig p{
      {.bits = 8, .parity = stm32::line_parity::even},
      35, cr1_fifoen
  };
  std::vector<std::uint16_t> const bytes{0x41, 0xc3, 0x7f, 0x80, 0x55};
  p.t.r.port("USART1").receive(bytes);
  p.advance(150 * us_ps);
  auto const saved = p.t.r.f->target->snapshot();
  auto const drain = [&] {
    p.advance(800 * us_ps);
    std::vector<std::uint32_t> words;
    auto* const                probe = p.t.r.f->model_of<bus_probe>("DAC1");
    REQUIRE(probe != nullptr);
    while((p.isr() & isr_rxfne) != 0)
    {
      auto const word = probe->context.services.read(
          address_of(*p.t.r.f, "USART1", "RDR"), 4, popcorn::initiator::cpu);
      REQUIRE(word.has_value());
      words.push_back(static_cast<std::uint32_t>(*word));
    }
    CHECK((p.isr() & isr_pe) == 0);
    return words;
  };
  auto const first = drain();
  CHECK(first == std::vector<std::uint32_t>{0x41, 0xc3, 0xff, 0x00, 0x55});
  p.t.r.f->target->restore(saved);
  CHECK(drain() == first);
}

namespace
{
using stm32::frame_kind;
using stm32::host_frame;
using stm32::uart_error;

struct rx_view
{
  std::uint32_t word{};
  std::uint32_t errors{};

  friend auto operator==(rx_view const&, rx_view const&) -> bool = default;
};

auto drain_rx(parity_rig const& p) -> std::vector<rx_view>
{
  std::vector<rx_view> out;
  while((p.isr() & isr_rxfne) != 0)
  {
    out.push_back({.word = p.rdr(), .errors = p.isr() & isr_errors});
    p.t.cpu("USART1", "ICR", icr_errors);
    std::ignore = p.pop();
  }
  return out;
}
}

TEST_CASE("in FIFO mode a tagged host frame's flag rises when it reaches RDR, not at arrival")
{
  struct tagged_case
  {
    uart_error    error{};
    line_config   config{};
    std::uint32_t flag{};
  };
  line_config const even{.bits = 9, .parity = stm32::line_parity::even};
  for(auto const [error, config, flag] : std::array<tagged_case, 3>{
          {{uart_error::framing, {}, isr_fe},
           {uart_error::noise, {}, isr_ne},
           {uart_error::parity, even, isr_pe}}
  })
  {
    CAPTURE(error);
    parity_rig                      p{config, 35, cr1_fifoen};
    std::array<host_frame, 3> const frames{
        {{.data = 'a'}, {.data = 'b', .error = error}, {.data = 'c'}}
    };
    p.tagged(frames);
    p.advance(400 * us_ps);
    CHECK(p.rdr() == expected_word('a', config));
    CHECK((p.isr() & isr_errors) == 0);
    CHECK(p.pop() == expected_word('a', config));
    auto const word_b = error == uart_error::parity ? expected_word('b', config) ^ 0x100u
                                                    : expected_word('b', config);
    CHECK(p.rdr() == word_b);
    CHECK((p.isr() & isr_errors) == flag);
    CHECK(p.pop() == word_b);
    CHECK(p.rdr() == expected_word('c', config));
    CHECK((p.isr() & isr_errors) == flag);
  }
}

TEST_CASE("without FIFO a tagged host frame sets its flag with its data, an overrun drops it")
{
  parity_rig                      p{{}};
  std::array<host_frame, 1> const one{{{.data = 'b', .error = uart_error::framing}}};
  p.tagged(one);
  p.advance(200 * us_ps);
  CHECK(p.rdr() == 'b');
  CHECK((p.isr() & isr_errors) == isr_fe);
  p.t.cpu("USART1", "ICR", icr_errors);
  std::ignore = p.pop();
  std::array<host_frame, 2> const two{
      {{.data = 'a'}, {.data = 'c', .error = uart_error::noise}}
  };
  p.tagged(two);
  p.advance(300 * us_ps);
  CHECK(p.rdr() == 'a');
  CHECK((p.isr() & isr_ore) != 0);
  CHECK((p.isr() & isr_errors) == 0);
}

TEST_CASE("a parity tag without PCE delivers the byte untouched, logs once and sets no PE")
{
  parity_rig                      p{{}, 35, cr1_fifoen};
  std::array<host_frame, 2> const frames{
      {{.data = 0xc1, .error = uart_error::parity}, {.data = 0x41, .error = uart_error::parity}}
  };
  p.tagged(frames);
  p.advance(300 * us_ps);
  CHECK(
      drain_rx(p)
      == std::vector<rx_view>{
          {.word = 0xc1, .errors = 0},
          {.word = 0x41, .errors = 0}
  });
  REQUIRE(p.t.logged.size() == 1);
  CHECK(p.t.logged[0].kind == core::log_kind::model_warning);
  CHECK(p.t.logged[0].reg == "CR1");
}

TEST_CASE("a host break reads as 0 with FE and holds the line a frame and 2 bits before the next")
{
  parity_rig                      p{{}, 35, cr1_fifoen};
  auto const                      frame = frame_of(*p.t.r.f, "USART1", 4'000'000, false);
  auto const                      start = p.now();
  constexpr std::uint64_t         slack = 2 * us_ps;
  std::array<host_frame, 2> const frames{
      {{.kind = frame_kind::brk}, {.data = 'x'}}
  };
  p.tagged(frames);
  CHECK(p.t.r.port("USART1").queued() == 2);
  p.advance(start + frame + slack - p.now());
  CHECK((p.isr() & isr_rxfne) != 0);
  CHECK(p.rdr() == 0);
  CHECK((p.isr() & isr_errors) == isr_fe);
  std::ignore = p.pop();
  p.advance(start + 2 * frame + frame / 5 - slack - p.now());
  CHECK((p.isr() & isr_rxfne) == 0);
  p.advance(2 * slack);
  CHECK((p.isr() & isr_rxfne) != 0);
  CHECK(p.rdr() == 'x');
}

TEST_CASE("a lone host break holds the line 2 bits for a byte queued after it was delivered")
{
  parity_rig                      p{{}, 35, cr1_fifoen};
  auto const                      frame = frame_of(*p.t.r.f, "USART1", 4'000'000, false);
  auto const                      start = p.now();
  constexpr std::uint64_t         slack = 2 * us_ps;
  std::array<host_frame, 1> const brk{{{.kind = frame_kind::brk}}};
  p.tagged(brk);
  p.advance(start + frame + slack - p.now());
  CHECK((p.isr() & isr_rxfne) != 0);
  std::ignore = p.pop();
  p.advance(start + frame + frame / 10 - p.now());
  std::array<host_frame, 1> const x{{{.data = 'x'}}};
  p.tagged(x);
  p.advance(start + frame + frame / 5 + frame - slack - p.now());
  CHECK((p.isr() & isr_rxfne) == 0);
  p.advance(2 * slack);
  CHECK((p.isr() & isr_rxfne) != 0);
  CHECK(p.rdr() == 'x');
}

TEST_CASE("IDLE after a lone host break rises one frame after the break ends")
{
  parity_rig                      p{{}, 35, cr1_fifoen};
  auto const                      frame = frame_of(*p.t.r.f, "USART1", 4'000'000, false);
  auto const                      start = p.now();
  constexpr std::uint64_t         slack = 2 * us_ps;
  std::array<host_frame, 1> const brk{{{.kind = frame_kind::brk}}};
  p.tagged(brk);
  p.advance(start + 2 * frame - slack - p.now());
  CHECK((p.isr() & isr_idle) == 0);
  p.advance(2 * slack);
  CHECK((p.isr() & isr_idle) != 0);
}

TEST_CASE("in FIFO mode a linked frame sampled with NE behind a good byte raises NE at RDR")
{
  parity_rig good{{}};
  parity_rig noisy{{}, 39};
  good.send(0x41);
  noisy.send(0x55);
  REQUIRE(good.box->posts.size() == 1);
  REQUIRE(noisy.box->posts.size() == 1);

  parity_rig alone{{}};
  alone.linked(noisy.box->posts[0].frame);
  REQUIRE((alone.isr() & isr_ne) != 0);
  auto const sampled = alone.rdr();

  parity_rig b{{}, 35, cr1_fifoen};
  b.linked(good.box->posts[0].frame);
  b.linked(noisy.box->posts[0].frame);
  CHECK(b.rdr() == 0x41);
  CHECK((b.isr() & isr_ne) == 0);
  std::ignore = b.pop();
  CHECK(b.rdr() == sampled);
  CHECK((b.isr() & isr_ne) != 0);
}

TEST_CASE("tagged host frames still queued survive a snapshot and 1000 restores with their tags")
{
  line_config const               even{.bits = 9, .parity = stm32::line_parity::even};
  parity_rig                      p{even, 35, cr1_fifoen};
  std::array<host_frame, 5> const frames{
      {{.data = 'a'},
       {.data = 'b', .error = uart_error::framing},
       {.kind = frame_kind::brk},
       {.data = 'c', .error = uart_error::noise},
       {.data = 'd', .error = uart_error::parity}}
  };
  p.tagged(frames);
  p.advance(20 * us_ps);
  auto& port = p.t.r.port("USART1");
  REQUIRE(port.queued() == 5);
  auto const saved = p.t.r.f->target->snapshot();
  auto const run   = [&] {
    p.advance(800 * us_ps);
    return drain_rx(p);
  };
  auto const first = run();
  CHECK(
      first
      == std::vector<rx_view>{
          {.word = expected_word('a', even), .errors = 0},
          {.word = expected_word('b', even), .errors = isr_fe},
          {.word = 0, .errors = isr_fe},
          {.word = expected_word('c', even), .errors = isr_ne},
          {.word = expected_word('d', even) ^ 0x100u, .errors = isr_pe}
  });
  for(int index = 0; index < 1000; ++index)
  {
    p.t.r.f->target->restore(saved);
    REQUIRE(port.queued() == 5);
  }
  CHECK(run() == first);

  p.t.r.f->target->restore(saved);
  p.advance(310 * us_ps);
  REQUIRE(port.queued() == 2);
  CHECK((p.isr() & isr_errors) == 0);
  auto const midway = p.t.r.f->target->snapshot();
  CHECK(run() == first);
  for(int index = 0; index < 100; ++index)
  {
    p.t.r.f->target->restore(midway);
    REQUIRE(port.queued() == 2);
  }
  CHECK(run() == first);
}

namespace
{
struct untagged_usart_state
{
  std::vector<std::uint16_t> rx_fifo;
  std::vector<std::uint16_t> host;
};
}

TEST_CASE("a USART state saved before tagged frames loads its RX FIFO and host bytes clean")
{
  parity_rig  p{{}, 35, cr1_fifoen};
  auto* const usart = p.t.r.f->model_of<popcorn::periph::model>("USART1");
  REQUIRE(usart != nullptr);
  std::vector<std::byte> saved;
  reflex::serde::bson::serializer{
      saved
  }
      .dump(untagged_usart_state{.rx_fifo = {0x41, 0x42}, .host = {0x43}});
  usart->load(saved);
  CHECK(p.t.r.port("USART1").queued() == 1);
  CHECK(p.pop() == 0x41);
  CHECK(p.pop() == 0x42);
  CHECK((p.isr() & isr_errors) == 0);
}

TEST_CASE("SBKRQ sends a break after the frame in flight, SBKF set until its low part ends")
{
  member a{"break_tx"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  auto const& posts = a.box->posts;
  REQUIRE(posts.size() == 3);
  CHECK(posts[0].frame.data == 'a');
  CHECK(posts[1].frame.kind == stm32::frame_kind::brk);
  CHECK(posts[2].frame.data == 'b');
  auto const frame = posts[1].frame.frame_ps;
  CHECK(posts[1].frame.start == posts[0].frame.end);
  CHECK(posts[2].frame.start.ps == posts[1].frame.end.ps + 4 * frame / 20);
  REQUIRE(a.box->sent.size() == 3);
  CHECK_FALSE(a.box->sent[0].brk);
  CHECK(a.box->sent[1].brk);
  CHECK(a.box->sent[1].data == 0);
  CHECK_FALSE(a.box->sent[2].brk);

  member again{"break_tx"};
  REQUIRE(again.to(posts[1].frame.start.ps - a.origin + frame / 2) == stop_reason::time);
  CHECK(again.f->field("USART1", "ISR_enabled", "SBKF") == 1);
  REQUIRE(again.to(posts[1].frame.end.ps - a.origin + 1) == stop_reason::time);
  CHECK(again.f->field("USART1", "ISR_enabled", "SBKF") == 0);

  auto const received = oracle("break_tx", 20 * us_ps, "link_rx_at", {}, 115'200);
  CHECK(bytes_of(received.log) == std::vector<std::uint32_t>{'a', 0, 'b'});
  CHECK(errors_of(received.log) == std::vector<std::uint32_t>{1});
  same_across_windows("break_tx", 115'200, received);
}

TEST_CASE(
    "a break and a frame after gaps shorter than a frame cancel the IDLE pending at any window")
{
  auto const received = oracle("break_gap", 20 * us_ps, "link_rx_at", {}, 115'200);
  CHECK(bytes_of(received.log) == std::vector<std::uint32_t>{'a', 0, 'b'});
  CHECK(errors_of(received.log) == std::vector<std::uint32_t>{1});
  CHECK(count_of(received.log, link_idle) == 1);
  same_across_windows("break_gap", 115'200, received);
}

TEST_CASE("a break requested with the TX FIFO full goes ahead of the queued data")
{
  member a{"break_fifo"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  std::vector<std::uint32_t> order;
  for(auto const& post : a.box->posts)
  {
    order.push_back(post.frame.kind == stm32::frame_kind::brk ? 0xbbu : post.frame.data);
  }
  CHECK(order == std::vector<std::uint32_t>{'0', 0xbb, '1', '2', '3', '4', '5', '6', '7'});
}

TEST_CASE("512 bytes back to back then a gap give exactly one IDLE at any window")
{
  constexpr auto latency   = 20 * us_ps;
  auto const     reference = oracle("link_tx_long", latency, "link_rx_quiet");
  auto const     seen      = entries(reference.log);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].kind == link_idle);
  CHECK(seen[0].data == 512);
  member a{"link_tx_long"};
  member b{"link_rx_quiet"};
  REQUIRE(
      a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason == stop_reason::user);
  for(auto const& post : a.box->posts)
  {
    deliver(post, a, b, latency);
  }
  REQUIRE(b.to(horizon_of(a.box->posts, a, latency)) == stop_reason::time);
  REQUIRE(entries(b.f->log()).size() == 1);
  auto const brr     = b.f->field("USART1", "BRR", "BRR");
  auto const f_rx    = 20 * ps_per_second / (2 * (160'000'000 / brr));
  auto const idle_at = shifted(a.box->posts.back().frame.end, a, b, latency).ps + f_rx;
  auto const now     = b.f->target->virtual_clock().time().ps;
  auto const cyccnt  = b.f->target->mem().read(0xe000'1004, 4);
  REQUIRE(cyccnt.has_value());
  auto const idle_cycle = static_cast<std::uint32_t>(*cyccnt - (now - idle_at) / 6'250);
  auto const stamp      = entries(b.f->log())[0].cycles;
  CAPTURE(idle_cycle);
  CHECK(stamp + 1 >= idle_cycle);
  CHECK(stamp < idle_cycle + 200);
  for(auto const window : {latency, latency / 3})
  {
    CAPTURE(window);
    CHECK(
        barriers("link_tx_long", latency, window, 1'000'000'000, "link_rx_quiet").log
        == reference.log);
  }
}

namespace
{
constexpr std::uint32_t ring_half     = 0x48;
constexpr std::uint32_t ring_complete = 0x54;
constexpr std::uint32_t ring_idle     = 0x49;

auto expected_ring_log() -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  std::uint32_t              received = 0;
  while(received < 200)
  {
    for(int index = 0; index < 37 and received < 200; ++index)
    {
      ++received;
      if(received % 64 == 32)
      {
        out.push_back(ring_half << 24 | 32);
      }
      if(received % 64 == 0)
      {
        out.push_back(ring_complete << 24 | 0);
      }
    }
    out.push_back(ring_idle << 24 | received % 64);
  }
  return out;
}

auto big_ring(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  auto const                 ring = sym("usart_ll", "big_ring");
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < 64; ++index)
  {
    auto const byte = f.target->mem().read(ring + index, 1);
    REQUIRE(byte.has_value());
    out.push_back(static_cast<std::uint32_t>(*byte));
  }
  return out;
}

auto expected_big_ring() -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out(64);
  for(std::uint32_t index = 0; index < 200; ++index)
  {
    out[index % 64] = index & 0xffu;
  }
  return out;
}
}

TEST_CASE("an RX DMA ring takes bursts with IDLE, HT and TC in order, BNDT as received")
{
  rig r{"dma_ring"};
  REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
  std::uint16_t next = 0;
  for(int sent = 0; sent < 200;)
  {
    std::vector<std::uint16_t> burst;
    for(int index = 0; index < 37 and sent < 200; ++index, ++sent)
    {
      burst.push_back(next++);
    }
    r.port("USART2").receive(burst);
    run_for(*r.f, (burst.size() + 3) * frame_115200);
  }
  CHECK(r.f->log() == expected_ring_log());
  CHECK(big_ring(*r.f) == expected_big_ring());
}

TEST_CASE("BNDT read at seeded instants is the same at two slicings")
{
  auto const polls = [](std::uint64_t slice) {
    rig r{"dma_ring"};
    REQUIRE(r.f->run_to_stop().reason == stop_reason::user);
    std::vector<std::uint16_t> stream;
    for(std::uint16_t index = 0; index < 200; ++index)
    {
      stream.push_back(index);
    }
    r.port("USART2").receive(stream);
    auto const                                   start = r.f->target->virtual_clock().time().ps;
    std::mt19937                                 seed{7};
    std::uniform_int_distribution<std::uint64_t> pick{0, 201 * frame_115200};
    std::vector<std::uint64_t>                   instants(40);
    for(auto& at : instants)
    {
      at = start + pick(seed);
    }
    std::ranges::sort(instants);
    std::vector<std::uint32_t> seen;
    for(auto const at : instants)
    {
      for(;;)
      {
        auto const result = r.f->run(
            {.max_instructions = icount{slice},
             .wall_budget      = std::chrono::seconds{60},
             .until_time       = vtime{at}});
        if(result.stopped)
        {
          REQUIRE(result.stopped->reason == stop_reason::time);
          break;
        }
      }
      seen.push_back(r.f->field("GPDMA1", "GPDMA_C3BR1", "BNDT"));
    }
    return seen;
  };
  auto const coarse = polls(1'000'000);
  CHECK(coarse == polls(997));
  CHECK(std::ranges::adjacent_find(coarse) != coarse.end());
}

TEST_CASE("the RX DMA ring fed by another machine's USART gives the host-fed log at any window")
{
  constexpr auto latency = 20 * us_ps;
  auto const     run     = [latency](std::uint64_t window) {
    member a{"link_tx_bursts"};
    member b{"dma_ring"};
    REQUIRE(b.to(5 * ms_ps) == stop_reason::user);
    bool        done        = false;
    std::size_t delivered   = 0;
    auto const  deliver_new = [&] {
      for(; delivered < a.box->posts.size(); ++delivered)
      {
        deliver(a.box->posts[delivered], a, b, latency, "USART2");
      }
    };
    if(window == 0)
    {
      REQUIRE(
          a.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason
          == stop_reason::user);
      deliver_new();
    }
    else
    {
      for(std::uint64_t barrier = window; not done; barrier += window)
      {
        done = a.to(barrier) == stop_reason::user;
        REQUIRE(b.to(barrier, true) == stop_reason::time);
        deliver_new();
      }
    }
    REQUIRE(b.to(horizon_of(a.box->posts, a, latency)) == stop_reason::time);
    return std::pair{b.f->log(), big_ring(*b.f)};
  };
  auto const reference = run(0);
  CHECK(reference.first == expected_ring_log());
  CHECK(reference.second == expected_big_ring());
  CHECK(run(latency) == reference);
  CHECK(run(7 * us_ps) == reference);
}
