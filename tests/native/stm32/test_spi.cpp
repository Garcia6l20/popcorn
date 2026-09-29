#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t frame_ps = 4'000'000;

class script_target final : public stm32::spi_target
{
public:
  std::vector<std::pair<bool, vtime>>           selects;
  std::vector<std::pair<std::uint32_t, vtime>>  frames;
  std::vector<std::uint8_t>                     sizes;
  std::uint32_t                                 position{};
  static constexpr std::array<std::uint32_t, 4> replies{0xFF, 0x1F, 0x32, 0x17};

  void select(bool selected, vtime at) override
  {
    selects.emplace_back(selected, at);
    position = 0;
  }

  auto exchange(std::uint32_t out, std::uint8_t bits, vtime at) -> std::uint32_t override
  {
    frames.emplace_back(out, at);
    sizes.push_back(bits);
    auto const reply = replies[position % replies.size()];
    ++position;
    return reply;
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(static_cast<std::byte>(position));
  }

  void load(std::span<std::byte const> in) override
  {
    position = in.empty() ? 0 : static_cast<std::uint32_t>(in.front());
  }
};

struct rig
{
  std::shared_ptr<script_target> device = std::make_shared<script_target>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  explicit rig(
      std::string_view entry,
      bool             hardware_select = false,
      int              extra_resets    = 0,
      std::string      instance        = "SPI1")
  {
    board.spi[instance].push_back(
        {.device  = device,
         .cs_port = "GPIOA",
         .cs_pin  = hardware_select ? std::nullopt : std::optional<std::uint8_t>{4}});
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    f              = std::make_unique<stm32_fixture>(u575(), "spi_ll", std::move(options));
    for(int index = 0; index < extra_resets; ++index)
    {
      f->target->reset();
    }
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("spi_ll", "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym("spi_ll", entry));
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop();
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }
};

struct field_at
{
  popcorn::address at;
  std::uint32_t    lsb;
  std::uint32_t    mask;
};

struct bus
{
  stm32_fixture&   f;
  std::string_view spi = "SPI1";

  [[nodiscard]] auto reg(std::string_view peripheral, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, name, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    return *found;
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view name) const
      -> popcorn::address
  {
    return f.target->peripheral(peripheral).description().base + reg(peripheral, name).offset;
  }

  [[nodiscard]] auto
      bits(std::string_view peripheral, std::string_view name, std::string_view field) const
      -> field_at
  {
    auto const& fields = reg(peripheral, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    return {
        at(peripheral, name), found->lsb,
        found->width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << found->width) - 1u};
  }

  [[nodiscard]] auto read(popcorn::address address, std::uint8_t size = 4) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(address, size);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(popcorn::address address, std::uint32_t value, std::uint8_t size = 4) const
  {
    REQUIRE(f.target->mem().write(address, size, value).has_value());
  }

  [[nodiscard]] auto get(std::string_view name, std::string_view field) const -> std::uint32_t
  {
    return get(spi, name, field);
  }

  [[nodiscard]] auto
      get(std::string_view peripheral, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    auto const b = bits(peripheral, name, field);
    return (read(b.at) >> b.lsb) & b.mask;
  }

  void set(std::string_view name, std::string_view field, std::uint32_t value) const
  {
    set(spi, name, field, value);
  }

  void
      set(std::string_view peripheral,
          std::string_view name,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const b = bits(peripheral, name, field);
    write(b.at, (read(b.at) & ~(b.mask << b.lsb)) | ((value & b.mask) << b.lsb));
  }

  void send(std::uint32_t value, std::uint8_t size = 1) const
  {
    write(at(spi, "SPI_TXDR"), value, size);
  }

  [[nodiscard]] auto receive(std::uint8_t size = 1) const -> std::uint32_t
  {
    auto&      regs  = f.target->peripheral(spi);
    auto const value = f.target->model(spi)->on_read(
        regs, reg(spi, "SPI_RXDR").offset, size, {.pc = 0, .now = icount{0}});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  void controller(
      std::uint32_t dsize,
      std::uint32_t tsize,
      std::uint32_t fthlv = 0,
      std::uint32_t mbr   = 0,
      std::uint32_t comm  = 0) const
  {
    if(spi == "SPI3")
    {
      set("RCC", "RCC_APB3ENR", "SPI3EN", 1);
    }
    else
    {
      set("RCC", "RCC_APB2ENR", "SPI1EN", 1);
    }
    set("SPI_CFG1", "MBR", mbr);
    set("SPI_CFG1", "DSIZE", dsize);
    set("SPI_CFG1", "FTHLV", fthlv);
    set("SPI_CFG2", "COMM", comm);
    set("SPI_CFG2", "SSM", 1);
    set("SPI_CFG2", "SSOE", 1);
    set("SPI_CFG2", "MASTER", 1);
    set("SPI_CR2", "TSIZE", tsize);
    set("SPI_CR1", "SSI", 1);
    set("SPI_CR1", "SPE", 1);
  }
};

auto pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  auto const word = f.target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
  REQUIRE(word.has_value());
  return (*word >> (irq % 32) & 1u) != 0;
}

constexpr std::uint32_t spi1_irq = 59;
constexpr std::uint64_t us       = 1'000'000;

void speed_up_core(bus const& b)
{
  auto const icscr1 = b.read(b.at("RCC", "RCC_ICSCR1")) & ~(0xFu << 28);
  b.write(b.at("RCC", "RCC_ICSCR1"), icscr1 | (1u << 28) | (1u << 23));
}

auto sent(script_target const& device) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const& [value, at] : device.frames)
  {
    out.push_back(value);
  }
  return out;
}
}

TEST_CASE("a polled transfer with the chip select low exchanges 4 frames with the device")
{
  rig r{"poll"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xFF, 0x1F, 0x32, 0x17});
  CHECK(sent(*r.device) == std::vector<std::uint32_t>{0x9F, 0, 0, 0});
  REQUIRE(r.device->selects.size() == 2);
  CHECK(r.device->selects[0].first);
  CHECK_FALSE(r.device->selects[1].first);
  CHECK(r.device->selects[0].second <= r.device->frames.front().second);
  CHECK(r.device->selects[1].second >= r.device->frames.back().second);
}

TEST_CASE("an interrupt-driven transfer runs one frame time apart and ends with EOT")
{
  std::vector<std::pair<std::uint32_t, vtime>> first;
  for(int run = 0; run < 2; ++run)
  {
    rig r{"irq_eot"};
    CHECK(r.finish() == std::vector<std::uint32_t>{0xFF, 0x1F, 0x32, 0x17, 4});
    REQUIRE(r.device->frames.size() == 4);
    for(std::size_t index = 1; index < r.device->frames.size(); ++index)
    {
      CHECK(r.device->frames[index].second.ps - r.device->frames[index - 1].second.ps == frame_ps);
    }
    if(run == 0)
    {
      first = r.device->frames;
    }
    else
    {
      CHECK(r.device->frames == first);
    }
  }
}

TEST_CASE("with the chip select high the device sees nothing and MISO reads all ones")
{
  rig r{"no_cs"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF, 0xFF});
  CHECK(r.device->frames.empty());
  CHECK(r.device->selects.empty());
}

TEST_CASE("the hardware SS output selects the device for the transfer only")
{
  rig r{"hw_ss", true};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xFF, 0x1F, 0x32, 0x17});
  REQUIRE(r.device->selects.size() == 2);
  CHECK(r.device->selects[0].first);
  CHECK_FALSE(r.device->selects[1].first);
  CHECK(r.device->selects[1].second >= r.device->frames.back().second);
}

TEST_CASE("after two extra resets one chip select edge still gives one select call")
{
  rig r{"poll", false, 2};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xFF, 0x1F, 0x32, 0x17});
  CHECK(r.device->selects.size() == 2);
}

TEST_CASE("a snapshot mid-transfer restores the device and the remaining frames")
{
  rig r{"irq_eot"};
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000},
                        .stop_when        = [&] { return r.device->frames.size() == 2; }})
          .reason
      == stop_reason::user);
  auto const saved  = r.f->target->snapshot();
  auto const log    = r.finish();
  auto const frames = r.device->frames;
  r.f->target->restore(saved);
  r.device->frames.resize(2);
  CHECK(r.finish() == log);
  CHECK(r.device->frames == frames);
}

TEST_CASE("a core clock change mid-frame keeps the frame end at its virtual time")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 1, 0, 7);
  b.send(0x9F);
  b.set("SPI_CR1", "CSTART", 1);
  auto const start = b.now();
  b.run_until(start + 10 * us);
  speed_up_core(b);
  REQUIRE(r.f->target->virtual_clock().core_hz() == 24'000'000);
  b.run_until(start + 200 * us);
  CHECK(b.get("SPI_SR", "RXP") == 0);
  CHECK(r.device->frames.empty());
  b.run_until(start + 520 * us);
  CHECK(b.get("SPI_SR", "RXP") == 1);
  REQUIRE(r.device->frames.size() == 1);
}

TEST_CASE("a core-only clock change keeps every frame one kernel frame time long")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.set("RCC", "RCC_CR", "MSIKON", 1);
  b.run_until(b.now() + 1'000 * us);
  REQUIRE(b.get("RCC", "RCC_CR", "MSIKRDY") == 1);
  b.set("RCC", "RCC_CCIPR1", "SPI1SEL", 3);
  b.controller(7, 2, 0, 7);
  b.send(0x9F);
  b.send(0x00);
  auto const start = r.f->target->virtual_clock().since_power_on().ps;
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + 10 * us);
  speed_up_core(b);
  REQUIRE(r.f->target->virtual_clock().core_hz() == 24'000'000);
  b.run_until(b.now() + 2'000 * us);
  REQUIRE(r.device->frames.size() == 2);
  CHECK(r.device->frames[0].second.ps - start == 512 * us);
  CHECK(r.device->frames[1].second.ps - start == 1'024 * us);
  CHECK(b.get("SPI_SR", "EOT") == 1);
}

TEST_CASE("a FIFO threshold of 4 frames sets TXP, RXP and DXP by packet")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 0, 3);
  for(std::uint32_t index = 0; index < 13; ++index)
  {
    b.send(index);
  }
  CHECK(b.get("SPI_SR", "TXP") == 0);
  b.set("SPI_CR1", "CSTART", 1);
  auto const start = b.now();
  b.run_until(start + 3 * frame_ps + frame_ps / 2);
  CHECK(b.get("SPI_SR", "TXP") == 1);
  CHECK(b.get("SPI_SR", "RXP") == 0);
  CHECK(b.get("SPI_SR", "DXP") == 0);
  CHECK(b.get("SPI_SR", "RXPLVL") == 3);
  CHECK(b.get("SPI_SR", "RXWNE") == 0);
  b.run_until(start + 4 * frame_ps + frame_ps / 2);
  CHECK(b.get("SPI_SR", "RXP") == 1);
  CHECK(b.get("SPI_SR", "DXP") == 1);
  CHECK(b.get("SPI_SR", "RXPLVL") == 0);
  CHECK(b.get("SPI_SR", "RXWNE") == 1);
  b.run_until(start + 5 * frame_ps + frame_ps / 2);
  CHECK(b.get("SPI_SR", "RXPLVL") == 1);
  CHECK(b.get("SPI_SR", "RXWNE") == 1);
  CHECK(b.receive(4) == 0x1732'1FFF);
  CHECK(b.get("SPI_SR", "RXP") == 0);
  CHECK(b.get("SPI_SR", "RXPLVL") == 1);
  CHECK(b.get("SPI_SR", "RXWNE") == 0);
}

TEST_CASE("the last packet of a transfer sets TXP for the remaining frames, TXTF masks TXPIE")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 6, 3);
  b.set("SPI_IER", "TXPIE", 1);
  b.set("SPI_IER", "DXPIE", 1);
  b.set("SPI_IER", "RXPIE", 1);
  for(std::uint32_t index = 0; index < 13; ++index)
  {
    b.send(index);
    if(index == 3)
    {
      CHECK(b.get("SPI_SR", "TXP") == 1);
      CHECK(b.get("SPI_SR", "TXTF") == 0);
      CHECK(b.get("SPI_SR", "CTSIZE") == 6);
    }
  }
  CHECK(b.get("SPI_SR", "TXP") == 0);
  CHECK(b.get("SPI_SR", "TXTF") == 1);
  CHECK(b.get("SPI_IER", "TXPIE") == 0);
  CHECK(b.get("SPI_IER", "DXPIE") == 0);
  CHECK(b.get("SPI_IER", "RXPIE") == 1);
}

TEST_CASE("16 and 32-bit frames, and 8-bit frames packed into wider accesses")
{
  SUBCASE("16-bit frames")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(15, 2);
    b.send(0xABCD'1234, 4);
    b.set("SPI_CR1", "CSTART", 1);
    b.run_until(b.now() + 2 * 2 * frame_ps + frame_ps / 2);
    REQUIRE(r.device->frames.size() == 2);
    CHECK(sent(*r.device) == std::vector<std::uint32_t>{0x1234, 0xABCD});
    CHECK(r.device->sizes == std::vector<std::uint8_t>{16, 16});
    CHECK(r.device->frames[1].second.ps - r.device->frames[0].second.ps == 2 * frame_ps);
    CHECK(b.get("SPI_SR", "RXPLVL") == 0);
    CHECK(b.get("SPI_SR", "RXWNE") == 1);
    CHECK(b.receive(4) == 0x001F'00FF);
  }
  SUBCASE("32-bit frames")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(31, 1);
    b.send(0x1234'5678, 4);
    b.set("SPI_CR1", "CSTART", 1);
    b.run_until(b.now() + 4 * frame_ps + frame_ps / 2);
    CHECK(sent(*r.device) == std::vector<std::uint32_t>{0x1234'5678});
    CHECK(r.device->sizes == std::vector<std::uint8_t>{32});
    CHECK(b.get("SPI_SR", "EOT") == 1);
    CHECK(b.receive(4) == 0xFF);
  }
  SUBCASE("8-bit frames packed")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(7, 6);
    b.send(0x0403'0201, 4);
    b.send(0x0605, 2);
    b.set("SPI_CR1", "CSTART", 1);
    b.run_until(b.now() + 6 * frame_ps + frame_ps / 2);
    CHECK(sent(*r.device) == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6});
    CHECK(b.receive(4) == 0x1732'1FFF);
    CHECK(b.receive(2) == 0x1FFF);
    CHECK(b.receive(1) == 0);
  }
}

TEST_CASE("a full RX FIFO overruns without MASRX and suspends with it until SUSPC")
{
  SUBCASE("overrun")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(7, 0);
    for(std::uint32_t index = 0; index < 16; ++index)
    {
      b.send(index);
    }
    b.set("SPI_CR1", "CSTART", 1);
    b.run_until(b.now() + 16 * frame_ps + frame_ps / 2);
    CHECK(b.get("SPI_SR", "OVR") == 0);
    b.send(16);
    b.run_until(b.now() + frame_ps + frame_ps / 2);
    CHECK(r.device->frames.size() == 17);
    CHECK(b.get("SPI_SR", "OVR") == 1);
    CHECK(b.receive() == 0xFF);
  }
  SUBCASE("automatic suspension")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(7, 20);
    b.set("SPI_CR1", "MASRX", 1);
    b.set("SPI_IER", "EOTIE", 1);
    for(std::uint32_t index = 0; index < 16; ++index)
    {
      b.send(index);
    }
    b.set("SPI_CR1", "CSTART", 1);
    auto const start = b.now();
    b.run_until(start + frame_ps / 2);
    b.send(16);
    b.run_until(start + 15 * frame_ps + frame_ps / 2);
    CHECK(b.get("SPI_SR", "SUSP") == 0);
    CHECK_FALSE(pending(*r.f, spi1_irq));
    b.run_until(start + 16 * frame_ps + frame_ps / 2);
    CHECK(b.get("SPI_SR", "SUSP") == 1);
    CHECK(b.get("SPI_CR1", "CSTART") == 1);
    CHECK(pending(*r.f, spi1_irq));
    CHECK(b.receive() == 0xFF);
    b.run_until(b.now() + 4 * frame_ps);
    CHECK(r.device->frames.size() == 16);
    CHECK(b.get("SPI_SR", "OVR") == 0);
    b.write(b.at("SPI1", "SPI_IFCR"), 1u << b.bits("SPI1", "SPI_IFCR", "SUSPC").lsb);
    CHECK(b.get("SPI_SR", "SUSP") == 0);
    b.run_until(b.now() + frame_ps + frame_ps / 2);
    CHECK(r.device->frames.size() == 17);
    CHECK(b.get("SPI_SR", "OVR") == 0);
  }
}

TEST_CASE("CSUSP suspends after the current frame and TSIZE 0 runs until SPE clears")
{
  SUBCASE("CSUSP")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(7, 0);
    for(std::uint32_t index = 0; index < 4; ++index)
    {
      b.send(index);
    }
    b.set("SPI_CR1", "CSTART", 1);
    b.run_until(b.now() + frame_ps + frame_ps / 2);
    b.set("SPI_CR1", "CSUSP", 1);
    CHECK(b.get("SPI_CR1", "CSUSP") == 0);
    CHECK(b.get("SPI_SR", "SUSP") == 0);
    b.run_until(b.now() + 4 * frame_ps);
    CHECK(r.device->frames.size() == 2);
    CHECK(b.get("SPI_SR", "SUSP") == 1);
    CHECK(b.get("SPI_CR1", "CSTART") == 0);
    CHECK(b.get("SPI_SR", "EOT") == 0);
  }
  SUBCASE("TSIZE 0")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(7, 0);
    b.send(1);
    b.send(2);
    b.set("SPI_IER", "EOTIE", 1);
    b.set("SPI_CR1", "CSTART", 1);
    CHECK(b.get("SPI_SR", "TXC") == 0);
    b.run_until(b.now() + frame_ps + frame_ps / 2);
    CHECK_FALSE(pending(*r.f, spi1_irq));
    b.run_until(b.now() + 2 * frame_ps);
    CHECK(b.get("SPI_CR1", "CSTART") == 1);
    CHECK(b.get("SPI_SR", "EOT") == 0);
    CHECK(b.get("SPI_SR", "TXC") == 1);
    CHECK(pending(*r.f, spi1_irq));
    b.send(3);
    b.run_until(b.now() + frame_ps + frame_ps / 2);
    CHECK(r.device->frames.size() == 3);
    CHECK(r.device->selects.size() == 1);
    b.set("SPI_CR1", "SPE", 0);
    CHECK(b.get("SPI_CR1", "CSTART") == 0);
    CHECK(b.get("SPI_SR", "TXC") == 1);
    CHECK(r.device->selects.size() == 2);
  }
}

TEST_CASE("TXC copies EOT when TSIZE is set, and re-enabling clears EOT and OVR")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 1);
  CHECK(b.get("SPI_SR", "TXC") == 0);
  b.send(1);
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + frame_ps + frame_ps / 2);
  CHECK(b.get("SPI_SR", "EOT") == 1);
  CHECK(b.get("SPI_SR", "TXC") == 1);
  b.write(b.at("SPI1", "SPI_IFCR"), 1u << b.bits("SPI1", "SPI_IFCR", "EOTC").lsb);
  CHECK(b.get("SPI_SR", "TXC") == 0);
  b.set("SPI_SR", "EOT", 1);
  CHECK(b.get("SPI_SR", "EOT") == 0);
  b.set("SPI_CR1", "SPE", 0);
  CHECK(b.get("SPI_SR", "TXC") == 1);
}

TEST_CASE("simplex receive clocks TSIZE frames out of an idle MOSI")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 3, 0, 0, 2);
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + 3 * frame_ps + frame_ps / 2);
  CHECK(sent(*r.device) == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF});
  CHECK(b.get("SPI_SR", "EOT") == 1);
  CHECK(b.receive() == 0xFF);
  CHECK(b.receive() == 0x1F);
  CHECK(b.receive() == 0x32);
}

TEST_CASE("SPI3 has an 8-byte FIFO and no CTSIZE")
{
  rig r{"idle", true, 0, "SPI3"};
  bus b{*r.f, "SPI3"};
  b.controller(7, 10);
  CHECK(b.get("SPI_SR", "CTSIZE") == 0);
  for(std::uint32_t index = 0; index < 10; ++index)
  {
    b.send(index);
  }
  CHECK(b.get("SPI_SR", "TXP") == 0);
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + 12 * frame_ps);
  CHECK(sent(*r.device) == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7});
}

TEST_CASE("SR is read-only and IFCR clears OVR and SUSP")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 0);
  b.set("SPI_CR1", "MASRX", 1);
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    b.send(index);
  }
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + 16 * frame_ps + frame_ps / 2);
  b.send(16);
  REQUIRE(b.get("SPI_SR", "SUSP") == 1);
  b.set("SPI_CR1", "MASRX", 0);
  b.write(b.at("SPI1", "SPI_IFCR"), 1u << b.bits("SPI1", "SPI_IFCR", "SUSPC").lsb);
  b.run_until(b.now() + frame_ps + frame_ps / 2);
  REQUIRE(b.get("SPI_SR", "OVR") == 1);
  auto const status = b.read(b.at("SPI1", "SPI_SR"));
  b.write(b.at("SPI1", "SPI_SR"), 0);
  b.write(b.at("SPI1", "SPI_SR"), ~std::uint32_t{});
  CHECK(b.read(b.at("SPI1", "SPI_SR")) == status);
  b.write(b.at("SPI1", "SPI_IFCR"), 1u << b.bits("SPI1", "SPI_IFCR", "OVRC").lsb);
  CHECK(b.get("SPI_SR", "OVR") == 0);
  CHECK(b.read(b.at("SPI1", "SPI_IFCR")) == 0);
}

TEST_CASE("a snapshot restored into a fresh machine loads the device state")
{
  rig first{"irq_eot"};
  REQUIRE(
      first.f
          ->run_to_stop(
              {.max_instructions = icount{100'000'000},
               .stop_when        = [&] { return first.device->frames.size() == 2; }})
          .reason
      == stop_reason::user);
  auto const saved  = first.f->target->snapshot();
  auto const log    = first.finish();
  auto const frames = first.device->frames;
  rig        second{"idle"};
  second.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(second.device->position == 2);
  CHECK(second.finish() == log);
  CHECK(second.device->frames == std::vector(frames.begin() + 2, frames.end()));
}

TEST_CASE("a snapshot restored into a machine never reset keeps the chip select subscription")
{
  rig first{"irq_eot"};
  REQUIRE(
      first.f
          ->run_to_stop(
              {.max_instructions = icount{100'000'000},
               .stop_when        = [&] { return first.device->frames.size() == 2; }})
          .reason
      == stop_reason::user);
  auto const saved = first.f->target->snapshot();

  auto         device = std::make_shared<script_target>();
  stm32::board board{};
  board.spi["SPI1"].push_back({.device = device, .cs_port = "GPIOA", .cs_pin = 4});
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(board, u575());
  stm32::register_builtin_models();
  core::machine target{u575(), popcorn::arch::armv8m::factory(), std::move(options)};
  target.load(popcorn::testing::elf("spi_ll"));
  target.restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  auto const result = target.run_slice({.max_instructions = icount{100'000'000}});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::user);
  REQUIRE(device->frames.size() == 2);
  REQUIRE(device->selects.size() == 1);
  CHECK_FALSE(device->selects[0].first);
}

TEST_CASE("enabling EOTIE while TXC is set raises the SPI IRQ at once")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.set("RCC", "RCC_APB2ENR", "SPI1EN", 1);
  REQUIRE(b.get("SPI_SR", "TXC") == 1);
  b.write(0xe000'e104u, 1u << (spi1_irq % 32));
  CHECK_FALSE(pending(*r.f, spi1_irq));
  b.set("SPI_IER", "EOTIE", 1);
  CHECK(pending(*r.f, spi1_irq));
  b.set("SPI_IER", "EOTIE", 0);
  auto const stopped = r.f->run_to_stop(
      {.max_instructions = icount{1'000'000}, .until = sym("spi_ll", "SPI1_IRQHandler")});
  CHECK(stopped.reason == stop_reason::address);
}

TEST_CASE("a frame event delivered before the frame end does not end the frame")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 1);
  b.send(0x9F);
  b.set("SPI_CR1", "CSTART", 1);
  auto const start = b.now();
  b.run_until(start + frame_ps / 2);
  auto* const model = r.f->target->model("SPI1");
  REQUIRE(model != nullptr);
  for(std::uint64_t payload = 0; payload < 1'000; ++payload)
  {
    model->on_event(0, payload);
  }
  CHECK(r.device->frames.empty());
  CHECK(b.get("SPI_SR", "EOT") == 0);
  b.run_until(start + frame_ps + frame_ps / 2);
  REQUIRE(r.device->frames.size() == 1);
  CHECK(
      r.device->frames[0].second.ps
      == r.f->target->virtual_clock().since_power_on().ps
      - b.now()
      + start
      + frame_ps);
}

TEST_CASE("TXP asks for the remaining frames only when they are fewer than a packet")
{
  for(auto const& [size, expected] : {
          std::pair{16u, 1u},
          std::pair{0u,  0u}
  })
  {
    CAPTURE(size);
    rig r{"idle", true};
    bus b{*r.f};
    b.controller(7, size, 3);
    for(std::uint32_t index = 0; index < 14; ++index)
    {
      b.send(index);
    }
    CHECK(b.get("SPI_SR", "TXP") == expected);
  }
}

TEST_CASE("re-enabling the SPI clears EOT and OVR, disabling keeps them")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 17);
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    b.send(index);
  }
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + frame_ps / 2);
  b.send(16);
  b.run_until(b.now() + 17 * frame_ps + frame_ps / 2);
  REQUIRE(b.get("SPI_SR", "EOT") == 1);
  REQUIRE(b.get("SPI_SR", "OVR") == 1);
  b.set("SPI_CR1", "SPE", 0);
  CHECK(b.get("SPI_SR", "EOT") == 1);
  CHECK(b.get("SPI_SR", "OVR") == 1);
  b.set("SPI_CR1", "SPE", 1);
  CHECK(b.get("SPI_SR", "EOT") == 0);
  CHECK(b.get("SPI_SR", "OVR") == 0);
}

TEST_CASE("TXDR writes beyond TSIZE are discarded")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 2);
  for(std::uint32_t index = 1; index <= 4; ++index)
  {
    b.send(index);
  }
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + 3 * frame_ps);
  REQUIRE(b.get("SPI_SR", "EOT") == 1);
  b.write(
      b.at("SPI1", "SPI_IFCR"), (1u << b.bits("SPI1", "SPI_IFCR", "EOTC").lsb)
                                    | (1u << b.bits("SPI1", "SPI_IFCR", "TXTFC").lsb));
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + 3 * frame_ps);
  CHECK(sent(*r.device) == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("24-bit frames take 3 FIFO bytes, 5 frames fill the 16-byte FIFO")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(23, 0);
  for(std::uint32_t index = 1; index <= 4; ++index)
  {
    b.send(index, 4);
  }
  CHECK(b.get("SPI_SR", "TXP") == 1);
  b.send(5, 4);
  CHECK(b.get("SPI_SR", "TXP") == 0);
  b.send(6, 4);
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + 20 * frame_ps);
  CHECK(sent(*r.device) == std::vector<std::uint32_t>{1, 2, 3, 4, 5});
  CHECK(r.device->sizes == std::vector<std::uint8_t>(5, 24));
}
