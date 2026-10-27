#include "model_base.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;

constexpr std::uint64_t us = 1'000'000;
constexpr std::uint64_t ms = 1'000 * us;
constexpr std::uint64_t s  = 1'000 * ms;

constexpr std::uint32_t loop_at = 0x2000'0100;

class clock_probe final : public popcorn::periph::model
{
public:
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

auto probed(core::clock** clock, stm32::board const& board = {}) -> stm32_fixture
{
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(board, h573());
  options.models.insert_or_assign(
      "CORDIC",
      [clock](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<popcorn::periph::model> {
        *clock = &ctx.clock;
        return std::make_shared<clock_probe>();
      });
  return stm32_fixture{h573(), "h573_boot_ll", std::move(options)};
}

struct rig
{
  core::clock*  clock = nullptr;
  stm32_fixture f;

  rig() : f{probed(&clock)}
  {
    REQUIRE(clock != nullptr);
    REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
    f.set("pc", loop_at | 1u);
  }

  [[nodiscard]] auto reg(std::string_view peripheral, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const found = popcorn::find_register(f.target->peripheral(peripheral).description(), name);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view name) const
      -> popcorn::address
  {
    return f.target->peripheral(peripheral).description().base + reg(peripheral, name).offset;
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view name) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(at(peripheral, name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view peripheral, std::string_view name, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(peripheral, name), 4, value).has_value());
  }

  void
      set(std::string_view peripheral,
          std::string_view name,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const& fields = reg(peripheral, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    auto const mask = ((std::uint64_t{1} << found->width) - 1u) << found->lsb;
    write(
        peripheral, name,
        static_cast<std::uint32_t>(
            (read(peripheral, name) & ~mask) | ((std::uint64_t{value} << found->lsb) & mask)));
  }

  [[nodiscard]] auto
      get(std::string_view peripheral, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    return f.field(peripheral, name, field);
  }

  [[nodiscard]] auto mask_of(std::string_view peripheral, std::string_view name) const
      -> std::uint32_t
  {
    std::uint32_t mask = 0;
    for(auto const& bits : reg(peripheral, name).fields)
    {
      mask |= static_cast<std::uint32_t>(((std::uint64_t{1} << bits.width) - 1u) << bits.lsb);
    }
    return mask;
  }

  void hz(std::string_view domain, std::uint64_t value, std::string_view peripheral = {}) const
  {
    clock->set_hz(clock->domain(domain), value);
    if(peripheral.empty())
    {
      return;
    }
    if(auto* const listener =
           dynamic_cast<stm32::clock_listener*>(f.target->model(std::string{peripheral}));
       listener != nullptr)
    {
      listener->on_clock_change();
    }
  }

  void run_for(std::uint64_t ps) const
  {
    auto const until = vtime{f.target->virtual_clock().time().ps + ps};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = until}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    auto const word = f.target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }

  [[nodiscard]] auto port(std::string_view name) const -> stm32::gpio_port&
  {
    auto* const found = f.model_of<stm32::gpio_port>(name);
    REQUIRE(found != nullptr);
    return *found;
  }
};

auto crc32_mpeg2(std::uint32_t word) -> std::uint32_t
{
  std::uint32_t crc = 0xffff'ffff;
  crc ^= word;
  for(int bit = 0; bit < 32; ++bit)
  {
    crc = (crc & 0x8000'0000u) != 0 ? (crc << 1) ^ 0x04c1'1db7u : crc << 1;
  }
  return crc;
}
}

TEST_CASE("GPIOI drives pin 11 as an output, the LCD reset pin, and PA13 resets to AF0")
{
  rig r;
  CHECK(r.port("GPIOA").mode(13) == stm32::pin_mode::alternate);
  CHECK(r.port("GPIOA").alternate(13) == 0);
  auto& gpioi = r.port("GPIOI");
  CHECK(popcorn::find_register(r.f.target->peripheral("GPIOI").description(), "AFRH") == nullptr);
  CHECK(gpioi.alternate(11) == 0);
  r.set("GPIOI", "MODER", "MODE11", 1);
  CHECK(gpioi.mode(11) == stm32::pin_mode::output);
  CHECK_FALSE(gpioi.level(11));
  r.write("GPIOI", "BSRR", 1u << 11);
  CHECK(gpioi.level(11));
  CHECK(r.read("GPIOI", "ODR") == 1u << 11);
  r.write("GPIOI", "BSRR", 1u << 27);
  CHECK_FALSE(gpioi.level(11));
  CHECK(r.read("GPIOI", "ODR") == 0);
}

TEST_CASE("every H573 GPIO port resets with the reference manual values of its own port")
{
  rig                        r;
  constexpr std::string_view ports = "ABCDEFGHI";
  for(auto const port : ports)
  {
    auto const name    = std::string{"GPIO"} + port;
    auto const moder   = port == 'A' ? 0xabff'ffffu
                       : port == 'B' ? 0xffff'febfu
                       : port == 'I' ? 0x00ff'ffffu
                                     : 0xffff'ffffu;
    auto const ospeedr = port == 'A' ? 0x0c00'0000u : port == 'B' ? 0xc0u : 0u;
    auto const pupdr   = port == 'A' ? 0x6400'0000u : port == 'B' ? 0x100u : 0u;
    CAPTURE(name);
    CHECK(r.read(name, "MODER") == moder);
    CHECK(r.read(name, "OSPEEDR") == ospeedr);
    CHECK(r.read(name, "PUPDR") == pupdr);
  }
}

TEST_CASE("a falling edge on PC13 pends EXTI13 and its interrupt 24, a rising edge does not")
{
  rig r;
  r.set("GPIOC", "MODER", "MODE13", 0);
  r.set("EXTI", "EXTICR4", "EXTI13", 2);
  r.set("EXTI", "FTSR1", "FT13", 1);
  r.set("EXTI", "IMR1", "IM13", 1);
  r.port("GPIOC").drive(13, true);
  CHECK(r.get("EXTI", "FPR1", "FPIF13") == 0);
  CHECK(r.get("EXTI", "RPR1", "RPIF13") == 0);
  CHECK_FALSE(r.pending(24));
  r.port("GPIOC").drive(13, false);
  r.run_for(10 * us);
  CHECK(r.get("EXTI", "FPR1", "FPIF13") == 1);
  CHECK(r.pending(24));
}

TEST_CASE("UART9 sends a frame in 10 bits at 921600 baud from a 250 MHz kernel clock")
{
  rig r;
  r.hz("uart9", 250'000'000);
  r.write("UART9", "BRR", 271);
  r.set("UART9", "CR1_disabled", "UE", 1);
  r.set("UART9", "CR1_enabled", "TE", 1);
  r.run_for(1 * us);
  CHECK(r.get("UART9", "ISR_enabled", "TC") == 1);
  r.write("UART9", "TDR", 0x55);
  CHECK(r.get("UART9", "ISR_enabled", "TC") == 0);
  r.run_for(10 * us);
  CHECK(r.get("UART9", "ISR_enabled", "TC") == 0);
  r.run_for(1 * us);
  CHECK(r.get("UART9", "ISR_enabled", "TC") == 1);
}

TEST_CASE("LPUART1 serves its enabled views and sends a frame in 10 bits at 9600 baud")
{
  rig r;
  r.hz("lpuart", 32'768);
  r.write("LPUART", "BRR", 256 * 32'768 / 9600);
  r.set("LPUART", "CR1_disabled", "UE", 1);
  r.set("LPUART", "CR1_enabled", "TE", 1);
  r.run_for(100 * us);
  CHECK(r.get("LPUART", "ISR_enabled", "TEACK") == 1);
  CHECK(r.get("LPUART", "ISR_enabled", "TC") == 1);
  r.write("LPUART", "TDR", 0x41);
  CHECK(r.get("LPUART", "ISR_enabled", "TC") == 0);
  r.run_for(1'000 * us);
  CHECK(r.get("LPUART", "ISR_enabled", "TC") == 0);
  r.run_for(100 * us);
  CHECK(r.get("LPUART", "ISR_enabled", "TC") == 1);
}

TEST_CASE("I2C4 has no AUTOCR and keeps a TIMINGR write")
{
  rig         r;
  auto const& desc = r.f.target->peripheral("I2C4").description();
  CHECK(popcorn::find_register(desc, "AUTOCR") == nullptr);
  CHECK(r.get("I2C4", "ISR", "TXE") == 1);
  r.write("I2C4", "TIMINGR", 0x3042'0F13);
  CHECK(r.read("I2C4", "TIMINGR") == 0x3042'0F13);
}

TEST_CASE("the RTC counts a calendar from the rtc domain and RTC and TAMP keep PRIVCFGR")
{
  rig r;
  r.hz("rtc", 32'768, "RTC");
  r.write("RTC", "WPR", 0xca);
  r.write("RTC", "WPR", 0x53);
  r.set("RTC", "ICSR", "INIT", 1);
  r.run_for(1 * ms);
  REQUIRE(r.get("RTC", "ICSR", "INITF") == 1);
  r.write("RTC", "TR", 0);
  r.set("RTC", "CR", "BYPSHAD", 1);
  r.set("RTC", "ICSR", "INIT", 0);
  r.run_for(3 * s + 500 * ms);
  CHECK(r.read("RTC", "TR") == 3);
  auto const rtc_mask = r.mask_of("RTC", "PRIVCFGR");
  r.write("RTC", "PRIVCFGR", 0xffff'ffff);
  CHECK(rtc_mask != 0);
  CHECK(r.read("RTC", "PRIVCFGR") == rtc_mask);
  auto const tamp_mask = r.mask_of("TAMP", "PRIVCFGR");
  r.write("TAMP", "PRIVCFGR", 0xffff'ffff);
  CHECK(tamp_mask != 0);
  CHECK(r.read("TAMP", "PRIVCFGR") == tamp_mask);
}

TEST_CASE("both caches finish their invalidation after reset and raise interrupts 104 and 105")
{
  rig r;
  CHECK(r.get("ICACHE", "SR", "BUSYF") == 1);
  CHECK(r.get("DCACHE", "SR", "BUSYF") == 1);
  r.set("ICACHE", "IER", "BSYENDIE", 1);
  r.set("DCACHE", "IER", "BSYENDIE", 1);
  r.run_for(20 * us);
  CHECK(r.get("ICACHE", "SR", "BSYENDF") == 1);
  CHECK(r.get("DCACHE", "SR", "BSYENDF") == 1);
  CHECK(r.pending(104));
  CHECK(r.pending(105));
  r.set("ICACHE", "CR", "EN", 1);
  CHECK(r.get("ICACHE", "CR", "EN") == 1);
  r.set("DCACHE", "CR", "EN", 1);
  CHECK(r.get("DCACHE", "CR", "EN") == 1);
}

TEST_CASE("CRC computes the reset polynomial of a word and SEC_CRC reaches the same unit")
{
  rig r;
  CHECK(r.read("CRC", "INIT") == 0xffff'ffff);
  CHECK(r.read("CRC", "POL") == 0x04c1'1db7);
  r.write("CRC", "DR", 0x1234'5678);
  CHECK(r.read("CRC", "DR") == crc32_mpeg2(0x1234'5678));
  CHECK(r.f.target->mem().read(r.at("SEC_CRC", "DR"), 4).value() == crc32_mpeg2(0x1234'5678));
  r.set("CRC", "CR", "RESET", 1);
  CHECK(r.read("CRC", "DR") == 0xffff'ffff);
}

TEST_CASE("the IWDG warns on interrupt 35 at EWIT and resets the machine at the end of the count")
{
  rig r;
  r.hz("lsi", 32'000, "IWDG");
  r.write("IWDG", "KR", 0x5555);
  r.write("IWDG", "PR", 0);
  r.write("IWDG", "RLR", 31);
  r.run_for(1 * ms);
  r.write("IWDG", "EWCR", 0x8000 | 16);
  r.run_for(1 * ms);
  r.write("IWDG", "KR", 0xcccc);
  r.write("IWDG", "KR", 0xaaaa);
  CHECK_FALSE(r.pending(35));
  r.run_for(1 * ms);
  CHECK_FALSE(r.pending(35));
  r.run_for(1500 * us);
  CHECK(r.pending(35));
  CHECK_FALSE(r.pending(27));
  auto const stopped = r.f.run_to_stop();
  CHECK(stopped.reason == stop_reason::reset_request);
  CHECK(stopped.detail.find("watchdog") != std::string::npos);
}

TEST_CASE("SAI1 block A sends its first frame to the board's endpoint at 64 us per frame")
{
  struct recorder final : stm32::audio_endpoint
  {
    std::vector<std::vector<std::uint32_t>> frames;
    std::vector<std::uint64_t>              at;

    void transmitted(std::span<std::uint32_t const> slots, stm32::audio_format, vtime begin, vtime)
        override
    {
      frames.emplace_back(slots.begin(), slots.end());
      at.push_back(begin.ps);
    }
    void receive(std::span<std::uint32_t>, stm32::audio_format, vtime) override
    {}
  };
  auto const   sink = std::make_shared<recorder>();
  stm32::board board{};
  board.audio["SAI1_A"] = {.endpoint = sink};
  core::clock* clock    = nullptr;
  auto         f        = probed(&clock, board);
  REQUIRE(clock != nullptr);
  REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
  f.set("pc", loop_at | 1u);
  clock->set_hz(clock->domain("sai1"), 4'000'000);
  auto const word = [&](std::string_view reg) {
    auto const found = popcorn::find_register(f.target->peripheral("SAI1").description(), reg);
    REQUIRE(found != nullptr);
    return f.target->peripheral("SAI1").description().base + found->offset;
  };
  auto const put = [&](std::string_view reg, std::uint32_t value) {
    REQUIRE(f.target->mem().write(word(reg), 4, value).has_value());
  };
  put("ACR1", (4u << 5) | (1u << 19) | (8u << 20));
  put("ACR2", 1u << 3);
  put("AFRCR", (31u << 0) | (15u << 8) | (1u << 16) | (1u << 18));
  put("ASLOTR", (1u << 8) | (3u << 16));
  put("ADR", 0x1000);
  put("ADR", 0x1001);
  put("ACR1", (4u << 5) | (1u << 19) | (8u << 20) | (1u << 16));
  auto const until = vtime{f.target->virtual_clock().time().ps + 150 * us};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = until}).reason
      == stop_reason::time);
  REQUIRE_FALSE(sink->frames.empty());
  CHECK(sink->frames.front() == std::vector<std::uint32_t>{0x1000, 0x1001});
  REQUIRE(sink->at.size() >= 2);
  CHECK(sink->at[1] - sink->at[0] == 64 * us);
  CHECK(f.field("SAI1", "ASR", "FLVL") == 0);
}

TEST_CASE("DBGMCU_IDCODE reads DEV_ID 0x484 and ignores writes")
{
  rig r;
  CHECK(r.f.target->model("DBGMCU") != nullptr);
  CHECK(r.read("DBGMCU", "IDCODE") == 0x1001'6484);
  r.write("DBGMCU", "IDCODE", 0);
  CHECK(r.read("DBGMCU", "IDCODE") == 0x1001'6484);
  r.write("DBGMCU", "CR", 0x6);
  CHECK(r.read("DBGMCU", "CR") == 0x6);
}
