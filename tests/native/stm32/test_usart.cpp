#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
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
  auto const icscr1 = std::ranges::find(
      rcc.description().registers, std::string_view{"RCC_ICSCR1"}, &popcorn::register_desc::name);
  REQUIRE(icscr1 != rcc.description().registers.end());
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
    auto const found =
        std::ranges::find(regs.description().registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != regs.description().registers.end());
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
      auto const found =
          std::ranges::find(regs.description().registers, reg, &popcorn::register_desc::name);
      REQUIRE(found != regs.description().registers.end());
      REQUIRE(r.f->target->model("USART1")
                  ->on_write(
                      regs, found->offset, 4, regs.peek(found->offset), value,
                      {.pc = 0, .now = icount{0}})
                  .has_value());
    };
    auto&      rcc    = r.f->target->peripheral("RCC");
    auto const enable = std::ranges::find(
        rcc.description().registers, std::string_view{"RCC_APB2ENR"},
        &popcorn::register_desc::name);
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
    auto const found =
        std::ranges::find(regs.description().registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != regs.description().registers.end());
    REQUIRE(r.f->target->model("USART1")
                ->on_write(
                    regs, found->offset, 4, regs.peek(found->offset), value,
                    {.pc = 0, .now = icount{0}})
                .has_value());
  };
  auto&      rcc    = r.f->target->peripheral("RCC");
  auto const enable = std::ranges::find(
      rcc.description().registers, std::string_view{"RCC_APB2ENR"}, &popcorn::register_desc::name);
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
  auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != desc.registers.end());
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
