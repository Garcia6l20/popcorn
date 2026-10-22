#include "dma.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/devices/at25sf.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t frame_ps = 4'000'000;

class script_target final : public periph::spi_target
{
public:
  std::vector<std::pair<bool, vtime>>           selects;
  std::vector<std::pair<std::uint32_t, vtime>>  frames;
  std::vector<std::uint8_t>                     sizes;
  std::vector<popcorn::reset_kind>              resets;
  std::vector<std::pair<vtime, std::size_t>>    cuts;
  bool                                          throw_at_cut{};
  std::uint32_t                                 position{};
  std::optional<std::uint8_t>                   declared;
  mutable int                                   mode_calls{};
  static constexpr std::array<std::uint32_t, 4> replies{0xFF, 0x1F, 0x32, 0x17};

  auto mode() const -> std::optional<std::uint8_t> override
  {
    ++mode_calls;
    return declared;
  }

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

  void reset(popcorn::reset_kind kind) override
  {
    resets.push_back(kind);
  }

  void power_cut(vtime at) override
  {
    cuts.emplace_back(at, resets.size());
    if(throw_at_cut)
    {
      throw std::runtime_error{"cut"};
    }
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
    auto const  found = popcorn::find_register(desc, name);
    REQUIRE(found != nullptr);
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

constexpr std::uint32_t    spi1_irq   = 59;
constexpr popcorn::address nvic_icpr1 = 0xe000'e284u;
constexpr std::uint64_t    us         = 1'000'000;

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

TEST_CASE("every machine reset reaches the device once with its kind")
{
  using popcorn::reset_kind;
  rig r{"poll", false, 2};
  CHECK(
      r.device->resets
      == std::vector{reset_kind::power_on, reset_kind::power_on, reset_kind::power_on});
  r.f->target->reset(reset_kind::software);
  r.f->target->reset(reset_kind::brown_out);
  CHECK(
      r.device->resets
      == std::vector{
          reset_kind::power_on, reset_kind::power_on, reset_kind::power_on, reset_kind::software,
          reset_kind::brown_out});
  CHECK(r.device->cuts.empty());
}

TEST_CASE("SPI1RST resets the controller and leaves its board device alone")
{
  using popcorn::reset_kind;
  rig         r{"poll"};
  auto const  before = r.device->resets;
  auto&       regs   = r.f->target->peripheral("SPI1");
  auto const* at     = popcorn::find_register(regs.description(), "SPI_CFG1");
  REQUIRE(at != nullptr);
  regs.poke(at->offset, 0x0007'0003);
  rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "SPI1RST");
  CHECK(regs.peek(at->offset) == 0x0007'0007);
  CHECK(r.device->resets == before);
  r.f->target->reset(reset_kind::software);
  CHECK(r.device->resets.size() == before.size() + 1);
}

TEST_CASE("a power cycle cuts the device at the time since power-on, then resets it at power-on")
{
  using popcorn::reset_kind;
  rig r{"poll"};
  r.f->target->reset(reset_kind::software);
  auto const at = r.f->target->virtual_clock().since_power_on();
  REQUIRE(at > r.f->target->virtual_clock().time());
  auto const seen = r.device->resets.size();
  r.f->target->power_cycle();
  CHECK(
      r.device->cuts
      == std::vector{
          std::pair{at, seen}
  });
  REQUIRE(r.device->resets.size() == seen + 1);
  CHECK(r.device->resets.back() == reset_kind::power_on);
  CHECK(r.f->target->virtual_clock().since_power_on() == vtime{});
}

TEST_CASE("a device that throws at the cut does not stop the cut of the next one on the bus")
{
  using popcorn::reset_kind;
  auto const   first  = std::make_shared<script_target>();
  auto const   second = std::make_shared<script_target>();
  stm32::board board{};
  first->throw_at_cut = true;
  board.spi["SPI1"].push_back({.device = first, .cs_port = "GPIOA", .cs_pin = 4});
  board.spi["SPI1"].push_back({.device = second, .cs_port = "GPIOB", .cs_pin = 0});
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(board, u575());
  stm32_fixture const f{u575(), "spi_ll", std::move(options)};
  CHECK_THROWS_AS(f.target->power_cycle(), std::runtime_error);
  CHECK(first->cuts.size() == 1);
  CHECK(second->cuts.size() == 1);
  CHECK(first->resets.back() == reset_kind::power_on);
  CHECK(second->resets.back() == reset_kind::power_on);
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
  b.set("SPI_CR1", "SPE", 1);
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

namespace
{
class recorded_flash final : public periph::spi_target
{
public:
  explicit recorded_flash(std::shared_ptr<popcorn::periph::devices::at25sf> device)
      : flash{std::move(device)}
  {}

  std::shared_ptr<popcorn::periph::devices::at25sf> flash;
  std::vector<vtime>                                frames;

  void select(bool selected, vtime at) override
  {
    flash->select(selected, at);
  }

  auto exchange(std::uint32_t out, std::uint8_t bits, vtime at) -> std::uint32_t override
  {
    frames.push_back(at);
    return flash->exchange(out, bits, at);
  }

  void save(std::vector<std::byte>& out) const override
  {
    flash->save(out);
  }

  void load(std::span<std::byte const> in) override
  {
    flash->load(in);
  }

  void reset(popcorn::reset_kind kind) override
  {
    flash->reset(kind);
  }
};

struct flash_rig
{
  std::shared_ptr<popcorn::periph::devices::at25sf> flash =
      std::make_shared<popcorn::periph::devices::at25sf>();
  std::shared_ptr<recorded_flash> device = std::make_shared<recorded_flash>(flash);
  stm32::board                    board{};
  std::unique_ptr<stm32_fixture>  f;

  explicit flash_rig(std::string_view entry)
  {
    board.spi["SPI1"].push_back({.device = device, .cs_port = "GPIOA", .cs_pin = 4});
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    f              = std::make_unique<stm32_fixture>(u575(), "spi_ll", std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("spi_ll", "main")})
            .reason
        == stop_reason::address);
    enter(entry);
  }

  void enter(std::string_view entry) const
  {
    f->set("pc", sym("spi_ll", entry));
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop();
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }

  auto at(std::uint32_t address) const -> std::uint32_t
  {
    return std::to_integer<std::uint32_t>(flash->contents()[address]);
  }
};
}

TEST_CASE("the AT25SF on SPI1 answers the JEDEC ID read")
{
  flash_rig r{"poll"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xFF, 0x1F, 0x32, 0x17});
}

TEST_CASE("a page program through SPI1 polls BUSY for tPP, then reads back, twice the same")
{
  std::vector<std::uint32_t> first;
  for(int run = 0; run < 2; ++run)
  {
    flash_rig  r{"write_read"};
    auto const log = r.finish();
    REQUIRE(log.size() == 2);
    CHECK(log[0] >= 2);
    CHECK(log[0] <= 600 / 8 + 1);
    CHECK(log[1] == 16);
    for(std::uint32_t index = 0; index < 16; ++index)
    {
      CHECK(r.at(0x1000 + index) == 0xA0 + index);
    }
    CHECK(r.at(0x1010) == 0xFF);
    if(run == 0)
    {
      first = log;
    }
    else
    {
      CHECK(log == first);
    }
  }
}

TEST_CASE("a sector erase through SPI1 polls BUSY for tSE and reads 0xFF")
{
  flash_rig r{"write_read"};
  REQUIRE(r.finish().size() == 2);
  r.enter("erase");
  auto const log = r.finish();
  REQUIRE(log.size() == 4);
  CHECK(log[2] >= 2);
  CHECK(log[2] <= 60'000 / 8 + 1);
  CHECK(log[3] == 16);
  CHECK(r.at(0x1000) == 0xFF);
  CHECK(log[2] * 10 >= log[0] * 900);
  CHECK(log[2] * 10 <= log[0] * 1'100);
}

TEST_CASE("a snapshot after a page program restores the programmed page into a fresh device")
{
  flash_rig r{"write_read"};
  REQUIRE(r.finish().size() == 2);
  auto const saved = r.f->target->snapshot();
  flash_rig  other{"idle"};
  REQUIRE(other.at(0x1000) == 0xFF);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(std::ranges::equal(other.flash->contents(), r.flash->contents()));
  other.enter("erase");
  auto const log = other.finish();
  REQUIRE(log.size() == 4);
  CHECK(log[3] == 16);
}

TEST_CASE("GPDMA channels 2 and 3 read 64 bytes from the AT25SF through the SPI1 requests")
{
  std::vector<vtime>     first;
  std::vector<std::byte> first_state;
  for(int run = 0; run < 2; ++run)
  {
    flash_rig r{"dma"};
    CHECK(r.finish() == std::vector<std::uint32_t>{64, 1, 1});
    auto const  state  = core::encode_snapshot(r.f->target->snapshot().data());
    auto const& frames = r.device->frames;
    REQUIRE(frames.size() >= 68);
    auto const dma_frames = std::vector(frames.end() - 68, frames.end());
    for(std::size_t index = 1; index < dma_frames.size(); ++index)
    {
      CAPTURE(index);
      CHECK(dma_frames[index].ps - dma_frames[index - 1].ps == frame_ps);
    }
    if(run == 0)
    {
      first       = frames;
      first_state = state;
    }
    else
    {
      CHECK(frames == first);
      CHECK(state == first_state);
    }
  }
}

TEST_CASE("a snapshot mid-DMA read restores to the same bytes and the same frame times")
{
  flash_rig r{"dma"};
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000},
                        .stop_when =
                            [&] {
                              auto const left   = r.f->field("GPDMA1", "GPDMA_C3BR1", "BNDT");
                              auto const armed  = r.f->field("SPI1", "SPI_CFG1", "RXDMAEN") != 0;
                              auto const moving = left > 0 and left < 30;
                              return armed and moving;
                            }})
          .reason
      == stop_reason::user);
  auto const saved  = r.f->target->snapshot();
  auto const mark   = r.device->frames.size();
  auto const log    = r.finish();
  auto const frames = r.device->frames;
  CHECK(log == std::vector<std::uint32_t>{64, 1, 1});

  flash_rig other{"idle"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.finish() == log);
  CHECK(
      other.device->frames == std::vector(frames.begin() + static_cast<long>(mark), frames.end()));
}

namespace
{
class request_probe final : public popcorn::periph::model, public stm32::dma_controller
{
public:
  std::map<std::uint32_t, bool> levels;

  void on_request(std::uint32_t request, bool level) override
  {
    levels[request] = level;
  }

  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    if(signal == "spi1_rx_dma")
    {
      return 6;
    }
    if(signal == "spi1_tx_dma")
    {
      return 7;
    }
    return std::nullopt;
  }

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

struct probe_rig
{
  std::shared_ptr<script_target> device = std::make_shared<script_target>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  probe_rig()
  {
    board.spi["SPI1"].push_back({.device = device, .cs_port = "GPIOA"});
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    options.models.insert_or_assign(
        "GPDMA1",
        [](popcorn::periph::model_context&,
           popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
          return std::make_shared<request_probe>();
        });
    f = std::make_unique<stm32_fixture>(u575(), "spi_ll", std::move(options));
    f->set("pc", sym("spi_ll", "idle"));
  }

  [[nodiscard]] auto level(std::uint32_t request) const -> bool
  {
    auto* const probe = f->model_of<request_probe>("GPDMA1");
    REQUIRE(probe != nullptr);
    auto const found = probe->levels.find(request);
    return found != probe->levels.end() and found->second;
  }
};

constexpr std::uint32_t rx_request = 6;
constexpr std::uint32_t tx_request = 7;
}

TEST_CASE("the SPI1 DMA requests follow TXP, RXP and a partial last packet after EOT")
{
  probe_rig r;
  bus       b{*r.f};
  b.controller(7, 5, 3);
  CHECK(b.get("SPI_SR", "TXP") == 1);
  CHECK_FALSE(r.level(tx_request));
  b.set("SPI_CFG1", "TXDMAEN", 1);
  CHECK(r.level(tx_request));
  CHECK_FALSE(r.level(rx_request));
  for(std::uint32_t index = 0; index < 5; ++index)
  {
    b.send(index);
  }
  CHECK_FALSE(r.level(tx_request));
  b.set("SPI_CR1", "CSTART", 1);
  auto const start = b.now();
  b.run_until(start + 3 * frame_ps + frame_ps / 2);
  CHECK_FALSE(r.level(rx_request));
  b.run_until(start + 4 * frame_ps + frame_ps / 2);
  CHECK(b.get("SPI_SR", "RXP") == 1);
  CHECK_FALSE(r.level(rx_request));
  b.set("SPI_CFG1", "RXDMAEN", 1);
  CHECK(r.level(rx_request));
  CHECK(b.receive(2) == 0x1FFF);
  CHECK(b.get("SPI_SR", "RXP") == 0);
  CHECK_FALSE(r.level(rx_request));
  CHECK(b.receive(2) == 0x1732);
  CHECK_FALSE(r.level(rx_request));
  b.run_until(start + 5 * frame_ps + frame_ps / 2);
  REQUIRE(b.get("SPI_SR", "EOT") == 1);
  CHECK(b.get("SPI_SR", "RXP") == 0);
  CHECK(r.level(rx_request));
  CHECK(b.receive() == 0xFF);
  CHECK_FALSE(r.level(rx_request));
  b.set("SPI_CR1", "SPE", 0);
  CHECK_FALSE(r.level(tx_request));
  CHECK_FALSE(r.level(rx_request));
}

namespace
{
class page_program_injector final : public periph::spi_target
{
public:
  std::shared_ptr<popcorn::periph::devices::at25sf> flash =
      std::make_shared<popcorn::periph::devices::at25sf>();
  std::uint32_t frames{};

  void select(bool selected, vtime at) override
  {
    flash->select(selected, at);
  }

  auto exchange(std::uint32_t out, std::uint8_t bits, vtime at) -> std::uint32_t override
  {
    if(frames++ == 0)
    {
      for(std::uint32_t const byte : {0x02u, 0x00u, 0x10u, 0x00u, 0x55u})
      {
        static_cast<void>(flash->exchange(byte, 8, at));
      }
      return 0xFF;
    }
    return flash->exchange(out, bits, at);
  }

  void save(std::vector<std::byte>& out) const override
  {
    flash->save(out);
  }

  void load(std::span<std::byte const> in) override
  {
    flash->load(in);
  }

  void reset(popcorn::reset_kind kind) override
  {
    flash->reset(kind);
  }
};
}

TEST_CASE("a supply reset drops the instruction of a hardware-SS device, other resets end it")
{
  for(auto const kind : {popcorn::reset_kind::power_on, popcorn::reset_kind::software})
  {
    CAPTURE(kind);
    auto         device = std::make_shared<page_program_injector>();
    stm32::board board{};
    board.spi["SPI1"].push_back({.device = device});
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    stm32_fixture f{u575(), "spi_ll", std::move(options)};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("spi_ll", "main")})
            .reason
        == stop_reason::address);
    auto const now = f.target->virtual_clock().time();
    device->flash->select(true, now);
    static_cast<void>(device->flash->exchange(0x06, 8, now));
    device->flash->select(false, now);
    f.set("pc", sym("spi_ll", "hw_ss"));
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{100'000'000},
                       .stop_when        = [&] { return device->frames > 0; }})
            .reason
        == stop_reason::user);
    f.target->reset(kind);
    auto const programmed = device->flash->contents()[0x1000] == std::byte{0x55};
    CHECK(programmed == (kind == popcorn::reset_kind::software));
  }
}

TEST_CASE("the SPI IRQ stays low while SPE is clear, except for MODF")
{
  rig                        r{"idle", true};
  bus                        b{*r.f};
  constexpr popcorn::address icpr1 = 0xe000'e284u;
  b.set("RCC", "RCC_APB2ENR", "SPI1EN", 1);
  b.set("SPI_CFG2", "SSM", 1);
  b.set("SPI_CFG2", "MASTER", 1);
  b.set("SPI_CR1", "SSI", 1);
  b.set("SPI_CR2", "TSIZE", 4);
  REQUIRE(b.get("SPI_SR", "TXP") == 1);
  b.set("SPI_IER", "TXPIE", 1);
  b.set("SPI_IER", "EOTIE", 1);
  CHECK_FALSE(pending(*r.f, spi1_irq));
  b.set("SPI_CR1", "SPE", 1);
  CHECK(pending(*r.f, spi1_irq));
  b.set("SPI_CR1", "SPE", 0);
  b.write(icpr1, 1u << (spi1_irq % 32));
  CHECK_FALSE(pending(*r.f, spi1_irq));
  auto&      regs   = r.f->target->peripheral("SPI1");
  auto const status = b.reg("SPI1", "SPI_SR").offset;
  regs.poke(status, regs.peek(status) | 1u << b.bits("SPI1", "SPI_SR", "MODF").lsb);
  CHECK_FALSE(pending(*r.f, spi1_irq));
  b.set("SPI_IER", "MODFIE", 1);
  CHECK(pending(*r.f, spi1_irq));
}

namespace
{
class controller_probe final : public periph::spi_controller_sink
{
public:
  struct heard
  {
    std::uint32_t sent;
    std::uint32_t received;
    vtime         at;

    friend auto operator==(heard const&, heard const&) -> bool = default;
  };

  std::vector<heard>          frames;
  std::optional<std::uint8_t> declared;
  mutable int                 mode_calls{};

  void frame(std::uint32_t sent, std::uint32_t received, vtime at) override
  {
    frames.push_back({sent, received, at});
  }

  auto mode() const -> std::optional<std::uint8_t> override
  {
    ++mode_calls;
    return declared;
  }

  [[nodiscard]] auto received() const -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> out;
    for(auto const& frame : frames)
    {
      out.push_back(frame.received);
    }
    return out;
  }
};

constexpr std::uint64_t mhz_frame = 8 * us;

struct target_rig
{
  std::shared_ptr<controller_probe> sink = std::make_shared<controller_probe>();
  stm32::board                      board{};
  std::unique_ptr<stm32_fixture>    f;
  std::vector<core::log_record>     logged;
  core::hook_handle                 log_hook;

  explicit target_rig(bool attached = true, bool probe = false)
  {
    if(attached)
    {
      board.spi_controllers["SPI1"] = sink;
    }
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    if(probe)
    {
      options.models.insert_or_assign(
          "GPDMA1",
          [](popcorn::periph::model_context&,
             popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
            return std::make_shared<request_probe>();
          });
    }
    f = std::make_unique<stm32_fixture>(u575(), "spi_ll", std::move(options));
    f->set("pc", sym("spi_ll", "idle"));
    log_hook = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
  }

  [[nodiscard]] auto port() const -> periph::spi_controller_port&
  {
    auto* const found = dynamic_cast<periph::spi_controller_port*>(f->target->model("SPI1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  auto transfer(
      std::uint64_t                     start,
      std::vector<std::uint32_t> const& frames,
      std::uint8_t                      bits = 8) const -> std::uint64_t
  {
    return port().transfer(vtime{start}, mhz_frame * bits / 8, bits, frames).ps;
  }

  [[nodiscard]] auto level(std::uint32_t request) const -> bool
  {
    auto* const probe = f->model_of<request_probe>("GPDMA1");
    REQUIRE(probe != nullptr);
    auto const found = probe->levels.find(request);
    return found != probe->levels.end() and found->second;
  }
};

void target(
    bus const&    b,
    std::uint32_t dsize,
    std::uint32_t tsize,
    std::uint32_t comm  = 0,
    std::uint32_t fthlv = 0)
{
  b.set("RCC", "RCC_APB2ENR", "SPI1EN", 1);
  b.set("SPI_CFG1", "DSIZE", dsize);
  b.set("SPI_CFG1", "FTHLV", fthlv);
  b.set("SPI_CFG2", "COMM", comm);
  b.set("SPI_CFG2", "MASTER", 0);
  b.set("SPI_CR2", "TSIZE", tsize);
  b.set("SPI_CR1", "SPE", 1);
}

auto drain(bus const& b) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  while(b.get("SPI_SR", "RXPLVL") != 0 or b.get("SPI_SR", "RXWNE") != 0)
  {
    out.push_back(b.receive());
  }
  return out;
}
}

TEST_CASE("a 4-frame transfer at 1 MHz clocks the target: 8 us frames, RX FIFO, EOT at TSIZE")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 4);
  for(std::uint32_t const frame : {0xA1u, 0xA2u, 0xA3u, 0xA4u})
  {
    b.send(frame);
  }
  auto const start = b.now() + us;
  CHECK(r.transfer(start, {1, 2, 3, 4}) == start);
  b.run_until(start + 4 * mhz_frame - mhz_frame / 2);
  CHECK(r.sink->frames.size() == 3);
  CHECK(b.get("SPI_SR", "EOT") == 0);
  b.run_until(start + 4 * mhz_frame);
  REQUIRE(r.sink->frames.size() == 4);
  for(std::uint32_t index = 0; index < 4; ++index)
  {
    CHECK(
        r.sink->frames[index]
        == controller_probe::heard{
            index + 1, 0xA1 + index, vtime{start + (index + 1) * mhz_frame}});
  }
  CHECK(b.get("SPI_SR", "EOT") == 1);
  CHECK(b.get("SPI_SR", "TXC") == 1);
  CHECK(b.get("SPI_SR", "UDR") == 0);
  CHECK(drain(b) == std::vector<std::uint32_t>{1, 2, 3, 4});
  CHECK(r.logged.empty());
}

TEST_CASE("an empty TX FIFO underruns: UDRDR, the last frame received, or zeros in simplex TX")
{
  struct underrun_case
  {
    std::uint32_t              config;
    std::uint32_t              comm;
    std::vector<std::uint32_t> heard;
  };
  for(auto const& c : {
          underrun_case{0, 0, {0xA1, 0xA2, 0x5A, 0x5A, 0xA5}},
          underrun_case{1, 0, {0xA1, 0xA2, 2, 3, 0xA5}      },
          underrun_case{1, 1, {0xA1, 0xA2, 0, 0, 0xA5}      },
  })
  {
    CAPTURE(c.config);
    CAPTURE(c.comm);
    target_rig r;
    bus        b{*r.f};
    b.set("SPI_CFG1", "UDRCFG", c.config);
    b.set("SPI_UDRDR", "UDRDR", 0x5A);
    target(b, 7, 0, c.comm);
    b.set("SPI_IER", "UDRIE", 1);
    b.send(0xA1);
    b.send(0xA2);
    auto const start = b.now() + us;
    r.transfer(start, {1, 2, 3, 4});
    b.run_until(start + mhz_frame + mhz_frame / 2);
    CHECK_FALSE(pending(*r.f, spi1_irq));
    b.run_until(start + 2 * mhz_frame + mhz_frame / 2);
    CHECK(b.get("SPI_SR", "UDR") == 1);
    CHECK(pending(*r.f, spi1_irq));
    b.send(0xA5);
    b.run_until(start + 4 * mhz_frame);
    b.set("SPI_IFCR", "UDRC", 1);
    CHECK(b.get("SPI_SR", "UDR") == 0);
    b.write(nvic_icpr1, 1u << (spi1_irq % 32));
    CHECK_FALSE(pending(*r.f, spi1_irq));
    auto const next = b.now() + us;
    r.transfer(next, {5});
    b.run_until(next + mhz_frame);
    CHECK(r.sink->received() == c.heard);
    CHECK(b.get("SPI_SR", "UDR") == 0);
    CHECK(
        drain(b)
        == (c.comm == 1 ? std::vector<std::uint32_t>{}
                        : std::vector<std::uint32_t>{1, 2, 3, 4, 5}));
  }
}

TEST_CASE("a full RX FIFO in target mode sets OVR and drops the frame")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 0);
  std::vector<std::uint32_t> frames;
  for(std::uint32_t index = 0; index < 17; ++index)
  {
    frames.push_back(index);
  }
  auto const start = b.now() + us;
  r.transfer(start, frames);
  b.run_until(start + 16 * mhz_frame);
  CHECK(b.get("SPI_SR", "OVR") == 0);
  b.run_until(start + 17 * mhz_frame);
  CHECK(b.get("SPI_SR", "OVR") == 1);
  CHECK(drain(b) == std::vector(frames.begin(), frames.begin() + 16));
}

TEST_CASE("with SSM 1 the SSI bit against SSIOP selects the target, the controller's NSS ignored")
{
  struct select_case
  {
    std::uint32_t ssi;
    std::uint32_t polarity;
    bool          lands;
  };
  for(auto const c : {
          select_case{1, 0, false},
          select_case{0, 0, true },
          select_case{1, 1, true },
          select_case{0, 1, false}
  })
  {
    CAPTURE(c.ssi);
    CAPTURE(c.polarity);
    target_rig r;
    bus        b{*r.f};
    b.set("SPI_CFG2", "SSM", 1);
    b.set("SPI_CFG2", "SSIOP", c.polarity);
    b.set("SPI_CR1", "SSI", c.ssi);
    target(b, 7, 0);
    b.send(0xA1);
    auto const start = b.now() + us;
    r.transfer(start, {0x42});
    b.run_until(start + mhz_frame);
    CHECK(r.sink->received() == std::vector<std::uint32_t>{c.lands ? 0xA1u : 0xFFu});
    CHECK(drain(b) == (c.lands ? std::vector<std::uint32_t>{0x42} : std::vector<std::uint32_t>{}));
    CHECK(b.get("SPI_SR", "UDR") == 0);
  }
}

TEST_CASE("with SSM 0 a transfer selected while SPE is clear is ignored to its end")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 0);
  b.set("SPI_CR1", "SPE", 0);
  auto const start = b.now() + us;
  r.transfer(start, {1, 2, 3});
  b.run_until(start + mhz_frame + mhz_frame / 2);
  b.set("SPI_CR1", "SPE", 1);
  b.send(0xA1);
  b.run_until(start + 3 * mhz_frame);
  CHECK(r.sink->received() == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF});
  CHECK(drain(b).empty());
  r.transfer(b.now(), {4});
  b.run_until(b.now() + mhz_frame);
  CHECK(r.sink->received().back() == 0xA1);
  CHECK(drain(b) == std::vector<std::uint32_t>{4});
}

TEST_CASE("after TSIZE frames the target ignores frames until SPE is cleared")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 2);
  b.send(0xA1);
  b.send(0xA2);
  auto const start = b.now() + us;
  r.transfer(start, {1, 2, 3});
  b.run_until(start + 3 * mhz_frame);
  CHECK(r.sink->received() == std::vector<std::uint32_t>{0xA1, 0xA2, 0xFF});
  CHECK(b.get("SPI_SR", "EOT") == 1);
  CHECK(b.get("SPI_SR", "UDR") == 0);
  CHECK(drain(b) == std::vector<std::uint32_t>{1, 2});
  b.set("SPI_CR1", "SPE", 0);
  b.set("SPI_CR1", "SPE", 1);
  b.send(0xA3);
  r.transfer(b.now(), {4});
  b.run_until(b.now() + mhz_frame);
  CHECK(r.sink->received().back() == 0xA3);
}

TEST_CASE("the SPI1 DMA requests follow TXP and RXP in target mode")
{
  target_rig r{true, true};
  bus        b{*r.f};
  target(b, 7, 0);
  b.set("SPI_CFG1", "TXDMAEN", 1);
  CHECK(r.level(tx_request));
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    b.send(index);
  }
  CHECK_FALSE(r.level(tx_request));
  b.set("SPI_CFG1", "RXDMAEN", 1);
  CHECK_FALSE(r.level(rx_request));
  auto const start = b.now() + us;
  r.transfer(start, {7});
  b.run_until(start + mhz_frame);
  CHECK(r.level(tx_request));
  CHECK(r.level(rx_request));
  CHECK(b.receive() == 7);
  CHECK_FALSE(r.level(rx_request));
}

TEST_CASE("a snapshot mid-transfer restores the queue and the remaining frames at their times")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 0);
  for(std::uint32_t index = 0; index < 6; ++index)
  {
    b.send(0xA0 + index);
  }
  auto const start = b.now() + us;
  r.transfer(start, {1, 2, 3, 4});
  CHECK(r.transfer(start, {5, 6}) == start + 4 * mhz_frame);
  b.run_until(start + 2 * mhz_frame + mhz_frame / 2);
  REQUIRE(r.sink->frames.size() == 2);
  auto const saved = r.f->target->snapshot();
  b.run_until(start + 7 * mhz_frame);
  auto const whole = r.sink->frames;
  REQUIRE(whole.size() == 6);
  auto const rest = std::vector(whole.begin() + 2, whole.end());

  r.f->target->restore(saved);
  r.sink->frames.clear();
  b.run_until(start + 7 * mhz_frame);
  CHECK(r.sink->frames == rest);
  CHECK(drain(b) == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6});

  target_rig other;
  bus        o{*other.f};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  o.run_until(start + 7 * mhz_frame);
  CHECK(other.sink->frames == rest);
  CHECK(drain(o) == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6});
}

TEST_CASE("transfers queue back to back, start at their own time when later, and reset drops them")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 0);
  auto const start = b.now() + us;
  CHECK(r.transfer(start, {1, 2}) == start);
  CHECK(r.transfer(start, {3}) == start + 2 * mhz_frame);
  CHECK(r.transfer(start + 10 * mhz_frame, {4}) == start + 10 * mhz_frame);
  b.run_until(start + 11 * mhz_frame);
  REQUIRE(r.sink->frames.size() == 4);
  CHECK(r.sink->frames[2].at.ps == start + 3 * mhz_frame);
  CHECK(r.sink->frames[3].at.ps == start + 11 * mhz_frame);
  CHECK(drain(b) == std::vector<std::uint32_t>{1, 2, 3, 4});

  r.transfer(b.now() + us, {5});
  r.f->target->reset();
  b.run_until(b.now() + 20 * mhz_frame);
  CHECK(r.sink->frames.size() == 4);
}

TEST_CASE("a transfer is refused before anything is queued")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 0);
  auto const now = b.now();
  CHECK_THROWS_AS(r.transfer(now + us, {0x100}), std::invalid_argument);
  CHECK_THROWS_AS(r.transfer(now + us, {}), std::invalid_argument);
  CHECK_THROWS_AS(
      r.port().transfer(vtime{now + us}, mhz_frame, 3, std::vector<std::uint32_t>{1}),
      std::invalid_argument);
  CHECK_THROWS_AS(
      r.port().transfer(vtime{now + us}, 0, 8, std::vector<std::uint32_t>{1}),
      std::invalid_argument);
  CHECK_THROWS_AS(
      r.port().transfer(vtime{~std::uint64_t{} - 1}, 2, 8, std::vector<std::uint32_t>{1}),
      std::invalid_argument);
  b.run_until(now + us);
  CHECK_THROWS_AS(r.transfer(now, {1}), std::invalid_argument);
  std::vector<std::uint32_t> const half(2048, 0);
  r.transfer(b.now() + us, half);
  r.transfer(b.now() + us, std::vector<std::uint32_t>(2047, 0));
  CHECK_THROWS_AS(r.transfer(b.now() + us, {1, 2}), popcorn::config_error);
  CHECK(r.transfer(b.now() + us, {1}) == b.now() + us + 4095 * mhz_frame);
  CHECK_THROWS_AS(r.transfer(b.now() + us, {1}), popcorn::config_error);
  b.run_until(b.now() + us + 4096 * mhz_frame);
  CHECK(r.sink->frames.size() == 4096);
  for(int round = 0; round < 1000; ++round)
  {
    r.transfer(b.now(), {1, 2, 3, 4});
    b.run_until(b.now() + 4 * mhz_frame);
  }
  CHECK(r.sink->frames.size() == 8096);
  CHECK(r.transfer(b.now(), std::vector<std::uint32_t>(4096, 0)) == b.now());

  target_rig bare{false};
  CHECK_THROWS_AS(bare.transfer(0, {1}), popcorn::config_error);
}

TEST_CASE("a controller frame size other than DSIZE + 1 lands masked and logs once")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 0);
  b.send(0xA1);
  b.send(0xA2);
  auto const start = b.now() + us;
  r.transfer(start, {0x1234, 0x5678}, 16);
  b.run_until(start + 4 * mhz_frame);
  CHECK(r.sink->received() == std::vector<std::uint32_t>{0xA1, 0xA2});
  CHECK(b.receive(2) == 0x7834);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
}

TEST_CASE("a controller-mode SPI selected by the controller takes a mode fault without SSOE")
{
  for(std::uint32_t const output : {0u, 1u})
  {
    CAPTURE(output);
    target_rig r;
    bus        b{*r.f};
    target(b, 7, 0);
    b.set("SPI_CR1", "SPE", 0);
    b.set("SPI_CFG2", "MASTER", 1);
    b.set("SPI_CFG2", "SSOE", output);
    b.set("SPI_IER", "MODFIE", 1);
    b.set("SPI_CR1", "SPE", 1);
    b.send(0xA1);
    CHECK_FALSE(pending(*r.f, spi1_irq));
    auto const start = b.now() + us;
    r.transfer(start, {1});
    b.run_until(start + mhz_frame);
    CHECK(r.sink->received() == std::vector<std::uint32_t>{0xFF});
    CHECK(drain(b).empty());
    auto const fault = output == 0 ? 1u : 0u;
    CHECK(b.get("SPI_SR", "MODF") == fault);
    CHECK(b.get("SPI_CR1", "SPE") == 1 - fault);
    CHECK(b.get("SPI_CFG2", "MASTER") == 1 - fault);
    CHECK(pending(*r.f, spi1_irq) == (fault != 0));
    if(fault != 0)
    {
      b.set("SPI_CR1", "SPE", 1);
      CHECK(b.get("SPI_CR1", "SPE") == 0);
      b.set("SPI_IFCR", "MODFC", 1);
      b.set("SPI_CR1", "SPE", 1);
      CHECK(b.get("SPI_CR1", "SPE") == 1);
      b.send(0xA2);
      r.transfer(b.now(), {2});
      b.run_until(b.now() + mhz_frame);
      CHECK(r.sink->received().back() == 0xA2);
      CHECK(drain(b) == std::vector<std::uint32_t>{2});
    }
  }
}

TEST_CASE("enabling a controller-mode SPI while the controller selects it takes a mode fault")
{
  struct enable_case
  {
    std::uint32_t output;
    bool          during;
    bool          fault;
  };
  for(auto const& c : {
          enable_case{0, true,  true },
          enable_case{1, true,  false},
          enable_case{0, false, false},
  })
  {
    CAPTURE(c.output);
    CAPTURE(c.during);
    target_rig r;
    bus        b{*r.f};
    target(b, 7, 0);
    b.set("SPI_CR1", "SPE", 0);
    b.set("SPI_IER", "MODFIE", 1);
    auto const start = b.now() + us;
    r.transfer(start, {1, 2, 3, 4});
    b.run_until(c.during ? start + mhz_frame + mhz_frame / 2 : start + 4 * mhz_frame);
    b.set("SPI_CFG2", "MASTER", 1);
    b.set("SPI_CFG2", "SSOE", c.output);
    b.set("SPI_CR1", "SPE", 1);
    CHECK(b.get("SPI_SR", "MODF") == (c.fault ? 1u : 0u));
    CHECK(b.get("SPI_CR1", "SPE") == (c.fault ? 0u : 1u));
    CHECK(b.get("SPI_CFG2", "MASTER") == (c.fault ? 0u : 1u));
    CHECK(pending(*r.f, spi1_irq) == c.fault);
    b.run_until(start + 4 * mhz_frame);
    CHECK(r.sink->received() == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF, 0xFF});
    CHECK(drain(b).empty());
  }
}

TEST_CASE("clearing SPE during a host frame drops that frame from the FIFO and from TSIZE")
{
  target_rig r;
  bus        b{*r.f};
  target(b, 7, 2);
  b.send(0xA1);
  b.send(0xA2);
  auto const start = b.now() + us;
  r.transfer(start, {1, 2, 3});
  b.run_until(start + mhz_frame + mhz_frame / 2);
  b.set("SPI_CR1", "SPE", 0);
  b.run_until(start + 3 * mhz_frame);
  CHECK(r.sink->received() == std::vector<std::uint32_t>{0xA1, 0xFF, 0xFF});
  CHECK(b.get("SPI_SR", "RXPLVL") == 0);
  CHECK(b.get("SPI_SR", "RXWNE") == 0);
  b.set("SPI_CR1", "SPE", 1);
  b.send(0xB1);
  b.send(0xB2);
  r.transfer(b.now(), {4, 5});
  auto const next = b.now();
  b.run_until(next + mhz_frame + mhz_frame / 2);
  CHECK(b.get("SPI_SR", "EOT") == 0);
  b.run_until(next + 2 * mhz_frame);
  CHECK(b.get("SPI_SR", "EOT") == 1);
  CHECK(drain(b) == std::vector<std::uint32_t>{4, 5});
}

TEST_CASE("TXC stays set during a host frame the target ignores")
{
  target_rig r;
  bus        b{*r.f};
  b.set("SPI_CFG2", "SSM", 1);
  b.set("SPI_CR1", "SSI", 1);
  target(b, 7, 0);
  REQUIRE(b.get("SPI_SR", "TXC") == 1);
  auto const start = b.now() + us;
  r.transfer(start, {1});
  b.run_until(start + mhz_frame / 2);
  CHECK(b.get("SPI_SR", "TXC") == 1);
  b.run_until(start + mhz_frame);
  CHECK(r.sink->received() == std::vector<std::uint32_t>{0xFF});
}

TEST_CASE("an SPI instance with a controller and a device is refused at the build")
{
  stm32::board board{};
  board.spi_controllers["SPI1"] = std::make_shared<controller_probe>();
  board.spi["SPI1"].push_back({.device = std::make_shared<script_target>(), .cs_port = "GPIOA"});
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(board, u575());
  CHECK_THROWS_AS(stm32_fixture(u575(), "spi_ll", std::move(options)), popcorn::config_error);
}

namespace
{
auto refusal(bus const& b, std::uint64_t ps) -> std::string
{
  try
  {
    auto const stopped =
        b.f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}});
    CHECK(stopped.reason == stop_reason::time);
  }
  catch(popcorn::config_error const& e)
  {
    return e.what();
  }
  return {};
}

void spi_mode(bus const& b, std::uint32_t cpol, std::uint32_t cpha)
{
  b.set("SPI_CFG2", "CPOL", cpol);
  b.set("SPI_CFG2", "CPHA", cpha);
}
}

TEST_CASE("LSBFRST puts the value bit reversed on the wire both ways in controller mode")
{
  SUBCASE("8-bit frames")
  {
    for(std::uint32_t const lsb_first : {0u, 1u})
    {
      CAPTURE(lsb_first);
      rig r{"idle", true};
      bus b{*r.f};
      b.set("SPI_CFG2", "LSBFRST", lsb_first);
      b.controller(7, 2);
      b.send(0x01);
      b.send(0x03);
      b.set("SPI_CR1", "CSTART", 1);
      b.run_until(b.now() + 2 * frame_ps + frame_ps / 2);
      CHECK(
          sent(*r.device)
          == (lsb_first != 0 ? std::vector<std::uint32_t>{0x80, 0xC0}
                             : std::vector<std::uint32_t>{0x01, 0x03}));
      CHECK(b.receive() == 0xFF);
      CHECK(b.receive() == (lsb_first != 0 ? 0xF8u : 0x1Fu));
    }
  }
  SUBCASE("12-bit frames")
  {
    rig r{"idle", true};
    bus b{*r.f};
    b.set("SPI_CFG2", "LSBFRST", 1);
    b.controller(11, 1);
    b.send(0x001, 2);
    b.set("SPI_CR1", "CSTART", 1);
    b.run_until(b.now() + 2 * frame_ps);
    CHECK(sent(*r.device) == std::vector<std::uint32_t>{0x800});
    CHECK(r.device->sizes == std::vector<std::uint8_t>{12});
    CHECK(b.receive(2) == 0xFF0);
  }
}

TEST_CASE("LSBFRST puts the value bit reversed on the wire both ways in target mode")
{
  target_rig r;
  bus        b{*r.f};
  b.set("SPI_CFG2", "LSBFRST", 1);
  b.set("SPI_CFG1", "UDRCFG", 0);
  b.set("SPI_UDRDR", "UDRDR", 0x0E);
  target(b, 7, 0);
  b.send(0xA1);
  auto const start = b.now() + us;
  r.transfer(start, {0x01, 0x03});
  b.run_until(start + 2 * mhz_frame);
  CHECK(r.sink->received() == std::vector<std::uint32_t>{0x85, 0x70});
  CHECK(drain(b) == std::vector<std::uint32_t>{0x80, 0xC0});
}

TEST_CASE("a device declaring another SPI mode fails the run at its first frame, both named")
{
  struct mode_case
  {
    std::optional<std::uint8_t> declared;
    std::uint32_t               cpol;
    std::uint32_t               cpha;
    std::string                 refused;
  };
  for(auto const& c : {
          mode_case{0,            1, 1, "the SPI mode 0 the device on SS declares, not mode 3"},
          mode_case{3,            1, 1, ""                                                    },
          mode_case{std::nullopt, 1, 1, ""                                                    },
          mode_case{1,            1, 0, "the SPI mode 1 the device on SS declares, not mode 2"},
          mode_case{2,            1, 0, ""                                                    },
          mode_case{2,            0, 1, "the SPI mode 2 the device on SS declares, not mode 1"},
  })
  {
    CAPTURE(c.declared);
    CAPTURE(c.cpol);
    CAPTURE(c.cpha);
    rig r{"idle", true};
    bus b{*r.f};
    r.device->declared = c.declared;
    spi_mode(b, c.cpol, c.cpha);
    b.controller(7, 4);
    for(std::uint32_t const frame : {1u, 2u, 3u, 4u})
    {
      b.send(frame);
    }
    b.set("SPI_CR1", "CSTART", 1);
    auto const end     = b.now() + 4 * frame_ps + frame_ps / 2;
    auto const refused = refusal(b, end);
    if(c.refused.empty())
    {
      CHECK(refused.empty());
    }
    else
    {
      CHECK(refused.contains(c.refused));
      CHECK(refused.contains(std::format("CPOL {} CPHA {}", c.cpol, c.cpha)));
      CHECK(refused.contains("devices.SPI1"));
      CHECK(r.device->frames.size() == 1);
      CHECK(refusal(b, end).empty());
    }
    CHECK(sent(*r.device) == std::vector<std::uint32_t>{1, 2, 3, 4});
    CHECK(b.get("SPI_SR", "EOT") == 1);
    CHECK(r.device->mode_calls == 1);
  }
}

TEST_CASE("every device declaring another SPI mode in the same frame is named")
{
  auto second      = std::make_shared<script_target>();
  second->declared = 2;
  stm32::board board{};
  auto         first = std::make_shared<script_target>();
  first->declared    = 1;
  board.spi["SPI1"].push_back({.device = first, .cs_port = "GPIOA"});
  board.spi["SPI1"].push_back({.device = second, .cs_port = "GPIOA"});
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(board, u575());
  stm32_fixture f{u575(), "spi_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("spi_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("spi_ll", "idle"));
  bus b{f};
  spi_mode(b, 1, 1);
  b.controller(7, 1);
  b.send(1);
  b.set("SPI_CR1", "CSTART", 1);
  auto const refused = refusal(b, b.now() + frame_ps + frame_ps / 2);
  CHECK(refused.contains(
      "the SPI mode 1 the device on SS declares and the SPI mode 2 the device on SS declares, "
      "not mode 3 the firmware set, CPOL 1 CPHA 1"));
  CHECK(first->frames.size() == 1);
  CHECK(second->frames.size() == 1);
}

TEST_CASE("the mode of a device is read once per transfer and after each chip select edge")
{
  SUBCASE("CSTART on the hardware SS output")
  {
    rig r{"idle", true};
    bus b{*r.f};
    r.device->declared = 0;
    b.controller(7, 2);
    for(int transfer = 1; transfer <= 2; ++transfer)
    {
      b.send(1);
      b.send(2);
      b.set("SPI_CR1", "CSTART", 1);
      b.run_until(b.now() + 2 * frame_ps + frame_ps / 2);
      b.set("SPI_IFCR", "EOTC", 1);
      b.set("SPI_IFCR", "TXTFC", 1);
      CHECK(r.device->mode_calls == transfer);
    }
    CHECK(r.device->frames.size() == 4);
  }
  SUBCASE("CSTART with the chip select held low")
  {
    rig r{"idle"};
    bus b{*r.f};
    r.device->declared = 0;
    b.set("RCC", "RCC_AHB2ENR1", "GPIOAEN", 1);
    b.set("GPIOA", "GPIO_BSRR", "BS4", 1);
    b.set("GPIOA", "GPIO_MODER", "MODE4", 1);
    b.set("GPIOA", "GPIO_BSRR", "BR4", 1);
    b.controller(7, 2);
    for(int transfer = 1; transfer <= 2; ++transfer)
    {
      b.send(1);
      b.send(2);
      b.set("SPI_CR1", "CSTART", 1);
      b.run_until(b.now() + 2 * frame_ps + frame_ps / 2);
      b.set("SPI_IFCR", "EOTC", 1);
      b.set("SPI_IFCR", "TXTFC", 1);
      CHECK(r.device->mode_calls == transfer);
    }
    CHECK(r.device->frames.size() == 4);
    CHECK(r.device->selects.size() == 1);
  }
  SUBCASE("chip select edges within one CSTART")
  {
    rig r{"idle"};
    bus b{*r.f};
    r.device->declared = 0;
    b.set("RCC", "RCC_AHB2ENR1", "GPIOAEN", 1);
    b.set("GPIOA", "GPIO_BSRR", "BS4", 1);
    b.set("GPIOA", "GPIO_MODER", "MODE4", 1);
    b.set("GPIOA", "GPIO_BSRR", "BR4", 1);
    b.controller(7, 0);
    b.set("SPI_CR1", "CSTART", 1);
    for(int selection = 1; selection <= 2; ++selection)
    {
      b.send(1);
      b.send(2);
      b.run_until(b.now() + 2 * frame_ps + frame_ps / 2);
      CHECK(r.device->mode_calls == selection);
      b.set("GPIOA", "GPIO_BSRR", "BS4", 1);
      b.set("GPIOA", "GPIO_BSRR", "BR4", 1);
    }
    CHECK(r.device->frames.size() == 4);
    CHECK(r.device->selects.size() == 5);
    r.device->declared = 3;
    b.send(3);
    CHECK(refusal(b, b.now() + frame_ps + frame_ps / 2).contains("the device on PA4 declares"));
  }
}

TEST_CASE("a controller declaring another SPI mode fails the run at each transfer's first frame")
{
  for(std::uint8_t const declared : {0, 3})
  {
    CAPTURE(declared);
    target_rig r;
    bus        b{*r.f};
    r.sink->declared = declared;
    spi_mode(b, 1, 1);
    target(b, 7, 0);
    for(int transfer = 1; transfer <= 2; ++transfer)
    {
      auto const start = b.now() + us;
      r.transfer(start, {1, 2, 3, 4});
      auto const refused = refusal(b, start + 4 * mhz_frame);
      if(declared == 0)
      {
        CHECK(refused.contains("the SPI mode 0 the controller declares, not mode 3"));
        CHECK(r.sink->frames.empty() == (transfer == 1));
        CHECK(refusal(b, start + 4 * mhz_frame).empty());
      }
      else
      {
        CHECK(refused.empty());
      }
      CHECK(r.sink->frames.size() == static_cast<std::size_t>(4 * transfer));
      CHECK(r.sink->mode_calls == transfer);
    }
    CHECK(drain(b) == std::vector<std::uint32_t>{1, 2, 3, 4, 1, 2, 3, 4});
  }
}

TEST_CASE("a controller transfer the target ignores reads no mode")
{
  target_rig r;
  bus        b{*r.f};
  r.sink->declared = 0;
  spi_mode(b, 1, 1);
  b.set("RCC", "RCC_APB2ENR", "SPI1EN", 1);
  auto const start = b.now() + us;
  r.transfer(start, {1, 2});
  CHECK(refusal(b, start + 2 * mhz_frame).empty());
  CHECK(r.sink->mode_calls == 0);
}

TEST_CASE("SPI1RST lowers the TX request on the controller")
{
  probe_rig r;
  bus       b{*r.f};
  b.controller(7, 5, 3);
  b.set("SPI_CFG1", "TXDMAEN", 1);
  REQUIRE(r.level(tx_request));
  rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "SPI1RST");
  CHECK_FALSE(r.level(tx_request));
}

TEST_CASE("SPI1RST deselects a hardware-SS device that was selected, without resetting it")
{
  rig r{"idle", true};
  bus b{*r.f};
  b.controller(7, 1, 0, 7);
  b.send(0x9F);
  b.set("SPI_CR1", "CSTART", 1);
  b.run_until(b.now() + us);
  REQUIRE_FALSE(r.device->selects.empty());
  REQUIRE(r.device->selects.back().first);
  auto const resets = r.device->resets;
  rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "SPI1RST");
  CHECK_FALSE(r.device->selects.back().first);
  CHECK(r.device->resets == resets);
}

TEST_CASE("SPI1RST moves the frame generation past every event queued before it, twice")
{
  probe_rig                 r;
  bus                       b{*r.f};
  std::vector<std::int64_t> seen;
  for(int pulse = 0; pulse < 2; ++pulse)
  {
    b.controller(7, 1, 0, 7);
    b.send(0x5A);
    b.set("SPI_CR1", "CSTART", 1);
    b.run_until(b.now() + frame_ps);
    auto const before = popcorn::testing::saved_integer(*r.f, "SPI1", "generation");
    REQUIRE(before.has_value());
    REQUIRE(popcorn::testing::queued_event(*r.f, "SPI1", 0, static_cast<std::uint64_t>(*before)));
    seen.push_back(*before);
    rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "SPI1RST");
    auto const after = popcorn::testing::saved_integer(*r.f, "SPI1", "generation");
    REQUIRE(after.has_value());
    CHECK(*after > *before);
  }
  CHECK(seen[1] > seen[0]);
}

TEST_CASE("SPI1RST moves the host generation past a host transfer queued before it, twice")
{
  target_rig                r;
  bus                       b{*r.f};
  std::vector<std::int64_t> seen;
  for(int pulse = 0; pulse < 2; ++pulse)
  {
    target(b, 7, 0);
    r.transfer(b.now() + 10 * mhz_frame, {1, 2});
    auto const before =
        popcorn::testing::saved_integer(*r.f, "SPI1", "host_generation").value_or(0);
    REQUIRE(popcorn::testing::queued_event(*r.f, "SPI1", 1, static_cast<std::uint64_t>(before)));
    seen.push_back(before);
    rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "SPI1RST");
    CHECK(popcorn::testing::saved_integer(*r.f, "SPI1", "host_generation").value_or(0) > before);
  }
  CHECK(seen[1] > seen[0]);
}
