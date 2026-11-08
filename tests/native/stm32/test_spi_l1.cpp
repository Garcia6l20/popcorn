#include "../support/inert_model.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
using popcorn::testing::l152rc;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware   = "l152xe_l1_spi_ll";
constexpr std::string_view firmware_b = "l152xb_l1_spi_ll";
constexpr address          spi1       = 0x4001'3000;
constexpr address          spi2       = 0x4000'3800;
constexpr address          spi3       = 0x4000'3C00;
constexpr address          cr1        = 0x00;
constexpr address          cr2        = 0x04;
constexpr address          sr         = 0x08;
constexpr address          dr         = 0x0C;
constexpr address          crcpr      = 0x10;
constexpr address          rxcrcr     = 0x14;
constexpr address          txcrcr     = 0x18;
constexpr std::uint32_t    mstr       = 1u << 2;
constexpr std::uint32_t    br4        = 1u << 3;
constexpr std::uint32_t    spe        = 1u << 6;
constexpr std::uint32_t    lsbfirst   = 1u << 7;
constexpr std::uint32_t    ssi        = 1u << 8;
constexpr std::uint32_t    ssm        = 1u << 9;
constexpr std::uint32_t    rxonly     = 1u << 10;
constexpr std::uint32_t    crcnext    = 1u << 12;
constexpr std::uint32_t    crcen      = 1u << 13;
constexpr std::uint32_t    bidioe     = 1u << 14;
constexpr std::uint32_t    bidimode   = 1u << 15;
constexpr std::uint32_t    rxdmaen    = 1u << 0;
constexpr std::uint32_t    ssoe       = 1u << 2;
constexpr std::uint32_t    frf        = 1u << 4;
constexpr std::uint32_t    errie      = 1u << 5;
constexpr std::uint32_t    rxne       = 1u << 0;
constexpr std::uint32_t    txe        = 1u << 1;
constexpr std::uint32_t    crcerr     = 1u << 4;
constexpr std::uint32_t    modf       = 1u << 5;
constexpr std::uint32_t    ovr        = 1u << 6;
constexpr std::uint32_t    bsy        = 1u << 7;
constexpr address          rcc        = 0x4002'3800;
constexpr address          rcc_cfgr   = rcc + 0x08;
constexpr address          apb2rstr   = rcc + 0x14;
constexpr address          apb2enr    = rcc + 0x20;
constexpr address          apb1enr    = rcc + 0x24;
constexpr address          ahbenr     = rcc + 0x1C;
constexpr std::uint32_t    dma1_bit   = 1u << 24;
constexpr address          dma1       = 0x4002'6000;
constexpr address          gpioa      = 0x4002'0000;
constexpr address          moder      = 0x00;
constexpr address          bsrr       = 0x18;
constexpr std::uint32_t    gpioa_bit  = 1u << 0;
constexpr address          ccr2       = dma1 + 0x1C;
constexpr address          cndtr2     = dma1 + 0x20;
constexpr address          cpar2      = dma1 + 0x24;
constexpr address          cmar2      = dma1 + 0x28;
constexpr std::uint32_t    dma_minc   = 1u << 7;
constexpr std::uint32_t    dma_en     = 1u << 0;
constexpr address          rx_buffer  = 0x2000'4000;
constexpr std::uint32_t    spi1_bit   = 1u << 12;
constexpr std::uint32_t    spi2_bit   = 1u << 14;
constexpr address          nvic_ispr1 = 0xE000'E204;
constexpr std::uint32_t    spi1_irq   = 1u << (35 - 32);
constexpr std::uint64_t    us         = 1'000'000;
constexpr std::uint64_t    frame8     = 8ull * 4 * 1'000'000'000'000 / 2'097'152;
constexpr std::uint64_t    frame16    = 16ull * 4 * 1'000'000'000'000 / 2'097'152;
constexpr std::uint32_t    master     = mstr | br4 | ssm | ssi;

auto l152rba() -> part_description const&
{
  static auto const desc = popcorn::testing::description_from("POPCORN_L152RBA_DESC");
  return desc;
}

auto l152rca() -> part_description const&
{
  static auto const desc = popcorn::testing::description_from("POPCORN_L152RCA_DESC");
  return desc;
}

class wire_target final : public periph::spi_target
{
public:
  struct frame
  {
    std::uint32_t out;
    std::uint8_t  bits;
    std::uint64_t at;
  };

  std::vector<frame>          frames;
  std::vector<bool>           selects;
  std::vector<std::uint32_t>  replies;
  std::optional<std::uint8_t> declared;
  std::vector<std::uint64_t>  tickets;
  bool                        defer{};
  std::size_t                 throw_at{};
  std::size_t                 throw_at_select{};
  std::optional<std::size_t>  restored;
  int                         attaches{};
  int                         detaches{};

  void select(bool selected, vtime) override
  {
    selects.push_back(selected);
    if(selects.size() == throw_at_select)
    {
      throw std::runtime_error{"hook"};
    }
  }

  auto exchange(std::uint32_t out, std::uint8_t bits, vtime at) -> std::uint32_t override
  {
    frames.push_back({out, bits, at.ps});
    if(frames.size() == throw_at)
    {
      throw std::runtime_error{"hook"};
    }
    return replies.empty() ? out : replies[(frames.size() - 1) % replies.size()];
  }

  auto exchange_deferred(std::uint32_t out, std::uint8_t bits, vtime at, std::uint64_t ticket)
      -> std::optional<std::uint32_t> override
  {
    tickets.push_back(ticket);
    if(defer)
    {
      frames.push_back({out, bits, at.ps});
      return std::nullopt;
    }
    return exchange(out, bits, at);
  }

  void attached(periph::spi_target_port&) override
  {
    ++attaches;
  }

  void detached() noexcept override
  {
    ++detaches;
  }

  auto mode() const -> std::optional<std::uint8_t> override
  {
    return declared;
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(static_cast<std::byte>(frames.size()));
  }

  void load(std::span<std::byte const> in) override
  {
    restored = static_cast<std::size_t>(in.front());
  }

  [[nodiscard]] auto outs() const -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> out;
    for(auto const& one : frames)
    {
      out.push_back(one.out);
    }
    return out;
  }
};

class controller_probe final : public periph::spi_controller_sink
{
public:
  std::vector<std::uint32_t> received;

  void frame(std::uint32_t, std::uint32_t got, vtime) override
  {
    received.push_back(got);
  }
};

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;
};

struct rig
{
  std::shared_ptr<wire_target>      device     = std::make_shared<wire_target>();
  std::shared_ptr<controller_probe> controller = std::make_shared<controller_probe>();
  stm32::board                      board{};
  std::unique_ptr<stm32_fixture>    f;
  std::string                       name;
  std::vector<core::log_record>     logged;
  core::hook_handle                 log_hook;

  explicit rig(
      std::string_view            entry,
      part_description const&     desc       = l152re(),
      std::string_view            elf        = firmware,
      bool                        controlled = false,
      bool                        strict     = false,
      std::optional<std::uint8_t> cs_pin     = std::nullopt)
      : name{elf}
  {
    if(controlled)
    {
      board.spi_controllers["SPI1"] = controller;
    }
    else
    {
      board.spi["SPI1"].push_back(
          {.device = device, .cs_port = cs_pin ? "GPIOA" : "", .cs_pin = cs_pin});
    }
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(board, desc);
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "DAC", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f        = std::make_unique<stm32_fixture>(desc, elf, std::move(options));
    log_hook = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    if(entry == "parked")
    {
      f->set("pc", sym(elf, "idle"));
      write(apb2enr, spi1_bit);
      write(apb1enr, spi2_bit);
      write(spi1 + cr2, ssoe);
      return;
    }
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(elf, "main")}).reason
        == stop_reason::address);
    f->set("pc", sym(elf, entry));
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

  void idle_to(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  void stop_clock(std::string_view domain) const
  {
    auto* const probe = f->model_of<bus_probe>("DAC");
    REQUIRE(probe != nullptr);
    probe->context.clock.set_hz(probe->context.clock.domain(domain), 0);
  }

  void receive_by_dma(std::uint32_t items) const
  {
    write(ahbenr, dma1_bit);
    write(cndtr2, items);
    write(cpar2, spi1 + dr);
    write(cmar2, rx_buffer);
    write(ccr2, dma_minc | dma_en);
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto port() const -> periph::spi_controller_port&
  {
    auto* const found = dynamic_cast<periph::spi_controller_port*>(f->target->model("SPI1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto warnings(std::string_view text) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.message.find(text) != std::string::npos;
    });
  }
};

auto with_spi1(part_description desc, auto&& change) -> part_description
{
  auto found = std::ranges::find(
      desc.peripherals, std::string_view{"SPI1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  auto control2 =
      std::ranges::find(found->registers, std::string_view{"CR2"}, &popcorn::register_desc::name);
  REQUIRE(control2 != found->registers.end());
  change(control2->fields);
  return desc;
}

TEST_CASE("RM0038 Table 162: SR 0x0002, CRCPR 0x0007, CR1 and CR2 0 after reset")
{
  for(auto const* desc : {&l152re(), &l152rb(), &l152rba()})
  {
    rig r{"parked", *desc, desc == &l152re() ? firmware : firmware_b};
    r.f->target->reset();
    auto bases = std::vector<address>{spi1, spi2};
    if(desc == &l152re())
    {
      bases.push_back(spi3);
    }
    for(auto const base : bases)
    {
      CAPTURE(base);
      CHECK(r.read(base + cr1) == 0);
      CHECK(r.read(base + cr2) == 0);
      CHECK(r.read(base + sr) == txe);
      CHECK(r.read(base + crcpr) == 7);
      CHECK(r.read(base + rxcrcr) == 0);
      CHECK(r.read(base + txcrcr) == 0);
    }
    CHECK(r.f->target->model("SPI1") != nullptr);
    if(desc == &l152re())
    {
      CHECK(r.f->target->model("SPI3") != nullptr);
    }
    else
    {
      CHECK_THROWS_AS(static_cast<void>(r.f->target->model("SPI3")), popcorn::config_error);
    }
  }
}

TEST_CASE("RM0038 28.3.5: LL full duplex at 8 bits exchanges four frames with a loopback device")
{
  for(auto const& [desc, elf] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    rig r{"master8", *desc, elf};
    CHECK(r.finish() == std::vector<std::uint32_t>{0x11, 0x22, 0x33, 0x44, txe});
    CHECK(r.device->outs() == std::vector<std::uint32_t>{0x11, 0x22, 0x33, 0x44});
    CHECK(std::ranges::all_of(r.device->frames, [](auto const& one) { return one.bits == 8; }));
    CHECK(r.device->selects == std::vector<bool>{true, false});
  }
}

TEST_CASE("RM0038 28.5.1 DFF: LL full duplex at 16 bits sends and receives half-words")
{
  rig r{"master16"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0x1234, 0xABCD, txe});
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x1234, 0xABCD});
  CHECK(std::ranges::all_of(r.device->frames, [](auto const& one) { return one.bits == 16; }));
}

TEST_CASE("RM0038 28.5.1 BR: frames last DFF bits of PCLK over 2 << BR, back to back with BSY 1")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0xA1);
  CHECK((r.read(spi1 + sr) & (txe | bsy)) == (txe | bsy));
  r.cpu_write(spi1 + dr, 0xA2);
  CHECK((r.read(spi1 + sr) & (txe | bsy)) == bsy);
  r.idle_to(t0 + frame8 + frame8 / 2);
  CHECK((r.read(spi1 + sr) & (txe | bsy | rxne)) == (txe | bsy | rxne));
  r.idle_to(t0 + 3 * frame8);
  CHECK((r.read(spi1 + sr) & (bsy | ovr)) == ovr);
  REQUIRE(r.device->frames.size() == 2);
  CHECK(r.device->frames[0].at == t0 + frame8);
  CHECK(r.device->frames[1].at == t0 + 2 * frame8);
  r.write(spi1 + cr1, master);
  r.write(spi1 + cr1, master | (1u << 11) | spe);
  auto const t1 = r.now_ps();
  r.cpu_write(spi1 + dr, 0xBEEF);
  r.idle_to(t1 + 2 * frame16);
  REQUIRE(r.device->frames.size() == 3);
  CHECK(r.device->frames[2].at == t1 + frame16);
  CHECK(r.device->frames[2].out == 0xBEEF);
}

TEST_CASE("RM0038 6.3.3: SPI1 runs on PCLK2 and SPI2 on PCLK1, each prescaler halves only its own")
{
  for(auto const& [ppre, slowed] : {
          std::pair{4u << 11, spi1},
          std::pair{4u << 8,  spi2}
  })
  {
    CAPTURE(slowed);
    rig r{"parked"};
    r.write(rcc_cfgr, ppre);
    for(auto const base : {spi1, spi2})
    {
      r.write(base + cr1, master | spe);
      auto const t0 = r.now_ps();
      r.cpu_write(base + dr, 0x55);
      auto const expected = base == slowed ? 2 * frame8 : frame8;
      r.idle_to(t0 + expected - us);
      CHECK((r.read(base + sr) & rxne) == 0);
      r.idle_to(t0 + expected + us);
      CHECK((r.read(base + sr) & rxne) == rxne);
    }
  }
}

TEST_CASE(
    "RM0038 28.5.1 LSBFIRST: the device gets the frame bit reversed, its reply lands reversed")
{
  rig r{"parked"};
  r.device->replies = {0x80};
  r.write(spi1 + cr1, master | lsbfirst | spe);
  r.cpu_write(spi1 + dr, 0x01);
  r.idle_to(r.now_ps() + 2 * frame8);
  REQUIRE(r.device->frames.size() == 1);
  CHECK(r.device->frames[0].out == 0x80);
  CHECK(r.cpu_read(spi1 + dr) == 0x01);
}

TEST_CASE("RM0038 28.3.6: LL CRC-8 over 123456789 with CRCPR 7 sends 0xF4, CRC-8/SMBUS")
{
  rig r{"crc8"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xF4, 0xF4, 0xF4, 0, txe});
  REQUIRE(r.device->frames.size() == 10);
  CHECK(r.device->frames.back().out == 0xF4);
  CHECK(r.f->field("SPI1", "CR1", "CRCNEXT") == 0);
}

TEST_CASE("RM0038 28.3.6: LL CRC-16 with CRCPR 0x1021 over 12345678 sends 0x9015, CRC-16/XMODEM")
{
  rig r{"crc16"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0x9015, 0x9015, 0x9015, 0, txe});
  REQUIRE(r.device->frames.size() == 5);
  CHECK(r.device->frames.back().out == 0x9015);
}

TEST_CASE(
    "RM0038 28.3.6: a CRC frame off RXCRCR sets CRCERR, CRCNEXT ends 0 inferred, write 0 clears")
{
  rig r{"parked"};
  r.device->replies = {0x5A};
  r.write(spi1 + crcpr, 7);
  r.write(spi1 + cr1, master | crcen);
  r.write(spi1 + cr1, master | crcen | spe);
  r.cpu_write(spi1 + dr, 0x31);
  r.write(spi1 + cr1, master | crcen | crcnext | spe);
  r.idle_to(r.now_ps() + 3 * frame8);
  REQUIRE(r.device->frames.size() == 2);
  CHECK(r.device->frames[1].out == 0x97);
  CHECK(r.read(spi1 + txcrcr) == 0x97);
  CHECK(r.read(spi1 + rxcrcr) == 0x81);
  CHECK((r.read(spi1 + sr) & (crcerr | ovr)) == (crcerr | ovr));
  CHECK((r.read(spi1 + cr1) & crcnext) == 0);
  r.write(spi1 + sr, ~crcerr);
  CHECK((r.read(spi1 + sr) & crcerr) == 0);
}

TEST_CASE(
    "RM0038 28.3.6: CRCEN written from 0 to 1 clears both CRC registers, a rerun gives the same")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | crcen);
  for(int run = 0; run < 2; ++run)
  {
    CAPTURE(run);
    r.write(spi1 + cr1, master | crcen | spe);
    r.cpu_write(spi1 + dr, 0x31);
    r.write(spi1 + cr1, master | crcen | crcnext | spe);
    r.idle_to(r.now_ps() + 3 * frame8);
    CHECK(r.read(spi1 + txcrcr) == 0x97);
    CHECK(r.read(spi1 + rxcrcr) == 0x97);
    CHECK((r.read(spi1 + sr) & crcerr) == 0);
    static_cast<void>(r.cpu_read(spi1 + dr));
    static_cast<void>(r.cpu_read(spi1 + sr));
    r.write(spi1 + cr1, master);
    r.write(spi1 + cr1, master | crcen);
    CHECK(r.read(spi1 + txcrcr) == 0);
    CHECK(r.read(spi1 + rxcrcr) == 0);
  }
}

TEST_CASE("RM0038 28.3.6 inferred: the CRC runs on across a CRC phase, no data frame clears it")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | crcen);
  r.write(spi1 + cr1, master | crcen | spe);
  r.cpu_write(spi1 + dr, 0x31);
  r.write(spi1 + cr1, master | crcen | crcnext | spe);
  r.idle_to(r.now_ps() + 3 * frame8);
  REQUIRE(r.device->outs() == std::vector<std::uint32_t>{0x31, 0x97});
  r.cpu_write(spi1 + dr, 0x31);
  r.idle_to(r.now_ps() + 2 * frame8);
  CHECK(r.read(spi1 + txcrcr) == 0x7B);
  CHECK(r.read(spi1 + rxcrcr) == 0x7B);
}

TEST_CASE("RM0038 28.3.6: LSBFIRST 1 runs the CRC on the wire bits, 0x01 gives 0x89")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | lsbfirst | crcen);
  r.write(spi1 + cr1, master | lsbfirst | crcen | spe);
  r.cpu_write(spi1 + dr, 0x01);
  r.write(spi1 + cr1, master | lsbfirst | crcen | crcnext | spe);
  r.idle_to(r.now_ps() + 3 * frame8);
  REQUIRE(r.device->frames.size() == 2);
  CHECK(r.device->frames[0].out == 0x80);
  CHECK(r.read(spi1 + txcrcr) == 0x89);
  CHECK(r.read(spi1 + rxcrcr) == 0x89);
  CHECK(r.device->frames[1].out == 0x91);
  CHECK((r.read(spi1 + sr) & crcerr) == 0);
}

TEST_CASE(
    "RM0038 28.3.9: target receive-only RX DMA with CRCEN checks the frame after the last data")
{
  for(bool const gap : {false, true})
  {
    for(std::uint32_t const sent_crc : {0xD4u, 0x00u})
    {
      CAPTURE(gap);
      CAPTURE(sent_crc);
      rig r{"parked", l152re(), firmware, true};
      r.receive_by_dma(3);
      r.write(spi1 + cr2, rxdmaen);
      r.write(spi1 + cr1, ssm | rxonly | crcen);
      r.write(spi1 + cr1, ssm | rxonly | crcen | spe);
      auto const start = vtime{r.now_ps() + us};
      if(gap)
      {
        std::vector<std::uint32_t> const data{0x11, 0x22, 0x33};
        std::vector<std::uint32_t> const crc{sent_crc};
        static_cast<void>(r.port().transfer(start, frame8, 8, data));
        static_cast<void>(r.port().transfer(vtime{start.ps + 5 * frame8}, frame8, 8, crc));
      }
      else
      {
        std::vector<std::uint32_t> const frames{0x11, 0x22, 0x33, sent_crc};
        static_cast<void>(r.port().transfer(start, frame8, 8, frames));
      }
      r.idle_to(start.ps + 7 * frame8);
      CHECK((r.read(rx_buffer) & 0xFF'FFFF) == 0x33'2211);
      CHECK(r.read(spi1 + rxcrcr) == 0xD4);
      CHECK(
          (r.read(spi1 + sr) & (crcerr | rxne | ovr)) == ((sent_crc == 0xD4 ? 0 : crcerr) | rxne));
      CHECK(r.read(spi1 + dr) == sent_crc);
    }
  }
}

TEST_CASE(
    "RM0038 28.3.9: controller receive-only RX DMA with CRCEN checks the frame in flight inferred")
{
  for(std::uint32_t const sent_crc : {0xD4u, 0x00u})
  {
    CAPTURE(sent_crc);
    rig r{"parked"};
    r.device->replies = {0x11, 0x22, 0x33, sent_crc};
    r.receive_by_dma(3);
    r.write(spi1 + cr2, rxdmaen | ssoe);
    r.write(spi1 + cr1, master | rxonly | crcen);
    r.write(spi1 + cr1, master | rxonly | crcen | spe);
    auto const t0 = r.now_ps();
    r.idle_to(t0 + 4 * frame8 + frame8 / 2);
    CHECK((r.read(rx_buffer) & 0xFF'FFFF) == 0x33'2211);
    CHECK(r.read(spi1 + rxcrcr) == 0xD4);
    CHECK((r.read(spi1 + sr) & (crcerr | rxne | ovr)) == ((sent_crc == 0xD4 ? 0 : crcerr) | rxne));
    CHECK(r.read(spi1 + dr) == sent_crc);
  }
}

TEST_CASE("RM0038 28.3.9: LL DMA full duplex with CRCEN sends the CRC after the last DMA write")
{
  rig r{"dma_crc"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0x31 | 0x39 << 8, 0xF4, 0xF4, 0xF4, 0, txe});
  REQUIRE(r.device->frames.size() == 10);
  CHECK(r.device->frames.back().out == 0xF4);
}

TEST_CASE("RM0038 28.3.10: LL overrun keeps the first frame, DR then SR read clears OVR")
{
  rig r{"overrun"};
  CHECK(r.finish() == std::vector<std::uint32_t>{1, 0x5A, 1, txe});
}

TEST_CASE("RM0038 28.3.10: an SR read before the DR read keeps OVR, debugger reads change nothing")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  r.cpu_write(spi1 + dr, 0x01);
  r.cpu_write(spi1 + dr, 0x02);
  r.idle_to(r.now_ps() + 3 * frame8);
  REQUIRE((r.read(spi1 + sr) & (ovr | rxne)) == (ovr | rxne));
  static_cast<void>(r.read(spi1 + dr));
  static_cast<void>(r.cpu_read(spi1 + sr));
  CHECK((r.read(spi1 + sr) & (ovr | rxne)) == (ovr | rxne));
  static_cast<void>(r.cpu_read(spi1 + sr));
  CHECK(r.cpu_read(spi1 + dr) == 0x01);
  static_cast<void>(r.read(spi1 + sr));
  CHECK((r.read(spi1 + sr) & (ovr | rxne)) == ovr);
  static_cast<void>(r.cpu_read(spi1 + sr));
  CHECK((r.read(spi1 + sr) & ovr) == 0);
  r.write(spi1 + dr, 0x77);
  r.idle_to(r.now_ps() + 2 * frame8);
  CHECK(r.device->frames.size() == 2);
}

TEST_CASE(
    "RM0038 28.3.10: frames after the DR read and before the SR read of the OVR clear are lost")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  r.cpu_write(spi1 + dr, 0x01);
  r.cpu_write(spi1 + dr, 0x02);
  r.idle_to(r.now_ps() + 3 * frame8);
  REQUIRE((r.read(spi1 + sr) & (ovr | rxne)) == (ovr | rxne));
  CHECK(r.cpu_read(spi1 + dr) == 0x01);
  r.cpu_write(spi1 + dr, 0x03);
  r.idle_to(r.now_ps() + 2 * frame8);
  CHECK(r.device->frames.size() == 3);
  CHECK((r.read(spi1 + sr) & (ovr | rxne)) == ovr);
  CHECK(r.read(spi1 + dr) == 0x01);
  static_cast<void>(r.cpu_read(spi1 + sr));
  CHECK((r.read(spi1 + sr) & ovr) == 0);
  r.cpu_write(spi1 + dr, 0x04);
  r.idle_to(r.now_ps() + 2 * frame8);
  CHECK((r.read(spi1 + sr) & (ovr | rxne)) == rxne);
  CHECK(r.cpu_read(spi1 + dr) == 0x04);
}

TEST_CASE("RM0038 28.3.6 inferred: RXCRCR takes the frames lost while OVR is set")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | crcen);
  r.write(spi1 + cr1, master | crcen | spe);
  r.cpu_write(spi1 + dr, 0x11);
  r.cpu_write(spi1 + dr, 0x22);
  r.idle_to(r.now_ps() + 3 * frame8);
  REQUIRE((r.read(spi1 + sr) & (ovr | rxne)) == (ovr | rxne));
  CHECK(r.cpu_read(spi1 + dr) == 0x11);
  r.cpu_write(spi1 + dr, 0x33);
  r.idle_to(r.now_ps() + 2 * frame8);
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x11, 0x22, 0x33});
  CHECK((r.read(spi1 + sr) & (ovr | rxne)) == ovr);
  CHECK(r.read(spi1 + dr) == 0x11);
  CHECK(r.read(spi1 + rxcrcr) == 0xD4);
  CHECK(r.read(spi1 + txcrcr) == 0xD4);
}

TEST_CASE("RM0038 28.3.5: LL receive-only runs frames back to back, SPE 0 lets the last one land")
{
  rig r{"rx_only"};
  r.device->replies = {0xC1, 0xC2, 0xC3};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xC1, 0xC2, 0xC3, 0});
  REQUIRE(r.device->frames.size() == 3);
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF});
  CHECK(r.device->frames[1].at == r.device->frames[0].at + frame8);
  CHECK(r.device->frames[2].at == r.device->frames[1].at + frame8);
}

TEST_CASE("RM0038 Table 159: LL TXEIE and RXNEIE drive four frames from SPI1_IRQn")
{
  rig r{"irq"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0x3433'3231, txe});
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x31, 0x32, 0x33, 0x34});
}

TEST_CASE(
    "RM0038 28.3.7: bidirectional transmit receives nothing, bidirectional receive keeps BSY 0")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | bidimode | bidioe | spe);
  r.cpu_write(spi1 + dr, 0x11);
  r.cpu_write(spi1 + dr, 0x22);
  r.idle_to(r.now_ps() + 3 * frame8);
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x11, 0x22});
  CHECK((r.read(spi1 + sr) & (rxne | ovr | bsy)) == 0);
  r.write(spi1 + cr1, master | bidimode);
  r.write(spi1 + cr1, master | bidimode | spe);
  auto const t0 = r.now_ps();
  r.idle_to(t0 + frame8 / 2);
  CHECK((r.read(spi1 + sr) & bsy) == 0);
  r.idle_to(t0 + frame8 + frame8 / 2);
  CHECK(r.device->frames.size() == 3);
  CHECK((r.read(spi1 + sr) & (rxne | bsy)) == rxne);
  r.write(spi1 + cr1, master | rxonly);
  r.write(spi1 + cr1, master | rxonly | spe);
  CHECK((r.read(spi1 + sr) & bsy) == bsy);
}

TEST_CASE(
    "RM0038 28.3.10: SSM 1 SSI 0 faults, SR then CR1 clears MODF, SPE 0 in that write inferred")
{
  for(bool const by_write : {false, true})
  {
    CAPTURE(by_write);
    rig r{"parked"};
    r.write(spi1 + cr2, errie);
    r.write(spi1 + cr1, mstr | ssm | spe);
    CHECK((r.read(spi1 + sr) & modf) == modf);
    CHECK((r.read(spi1 + cr1) & (mstr | spe)) == 0);
    CHECK((r.read(nvic_ispr1) & spi1_irq) == spi1_irq);
    r.write(spi1 + cr1, master | spe);
    CHECK((r.read(spi1 + cr1) & (mstr | spe)) == 0);
    CHECK((r.read(spi1 + sr) & modf) == modf);
    r.write(spi1 + sr, ~crcerr);
    r.write(spi1 + cr1, master | spe);
    CHECK((r.read(spi1 + sr) & modf) == modf);
    if(by_write)
    {
      r.cpu_write(spi1 + sr, ~crcerr);
    }
    else
    {
      static_cast<void>(r.cpu_read(spi1 + sr));
    }
    r.write(spi1 + cr1, master | spe);
    CHECK((r.read(spi1 + sr) & modf) == 0);
    CHECK((r.read(spi1 + cr1) & (mstr | spe)) == 0);
    r.write(spi1 + cr1, master | spe);
    CHECK((r.read(spi1 + cr1) & (mstr | spe)) == (mstr | spe));
  }
}

TEST_CASE(
    "RM0038 28.3.10: a controller selecting a controller-mode SPI with SSM 0 and SSOE 0 faults")
{
  for(bool const output : {false, true})
  {
    CAPTURE(output);
    rig r{"parked", l152re(), firmware, true};
    r.write(spi1 + cr2, output ? ssoe : 0);
    r.write(spi1 + cr1, mstr | br4 | spe);
    std::vector<std::uint32_t> const frames{0x12};
    static_cast<void>(r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, frames));
    r.idle_to(r.now_ps() + 2 * us + frame8);
    CHECK((r.read(spi1 + sr) & modf) == (output ? 0 : modf));
    CHECK((r.read(spi1 + cr1) & (mstr | spe)) == (output ? mstr | spe : 0));
    CHECK(r.controller->received == std::vector<std::uint32_t>{0xFF});
  }
}

TEST_CASE("RM0038 28.3.2: a target sends its buffer, then the last frame received, inferred")
{
  rig r{"parked", l152re(), firmware, true};
  r.write(spi1 + cr1, ssm | spe);
  r.cpu_write(spi1 + dr, 0xA1);
  CHECK((r.read(spi1 + sr) & (txe | bsy)) == bsy);
  std::vector<std::uint32_t> const frames{0x10, 0x20, 0x30};
  auto const start = r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, frames);
  r.idle_to(start.ps + frame8 + frame8 / 2);
  CHECK(r.cpu_read(spi1 + dr) == 0x10);
  r.idle_to(start.ps + 4 * frame8);
  CHECK(r.controller->received == std::vector<std::uint32_t>{0xA1, 0x10, 0x20});
  CHECK((r.read(spi1 + sr) & (rxne | ovr | bsy)) == (rxne | ovr));
  CHECK(r.cpu_read(spi1 + dr) == 0x20);
}

TEST_CASE(
    "RM0038 28.3.2: a target drives all ones in receive-only mode and ignores frames with SSI 1")
{
  rig r{"parked", l152re(), firmware, true};
  r.write(spi1 + cr1, ssm | rxonly | spe);
  r.cpu_write(spi1 + dr, 0xA1);
  std::vector<std::uint32_t> const frames{0x10};
  auto start = r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, frames);
  r.idle_to(start.ps + 2 * frame8);
  CHECK(r.controller->received == std::vector<std::uint32_t>{0xFF});
  CHECK(r.cpu_read(spi1 + dr) == 0x10);
  r.write(spi1 + cr1, ssm | ssi | spe);
  start = r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, frames);
  r.idle_to(start.ps + 2 * frame8);
  CHECK(r.controller->received == std::vector<std::uint32_t>{0xFF, 0xFF});
  CHECK((r.read(spi1 + sr) & (rxne | txe)) == 0);
}

TEST_CASE("RM0038 28.3.6: a target with CRCNEXT sends TXCRCR next and checks the controller's CRC")
{
  for(std::uint32_t const sent_crc : {0x97u, 0x00u})
  {
    CAPTURE(sent_crc);
    rig r{"parked", l152re(), firmware, true};
    r.write(spi1 + cr1, ssm | crcen);
    r.write(spi1 + cr1, ssm | crcen | spe);
    r.cpu_write(spi1 + dr, 0x31);
    r.write(spi1 + cr1, ssm | crcen | crcnext | spe);
    std::vector<std::uint32_t> const frames{0x31, sent_crc};
    auto const start = r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, frames);
    r.idle_to(start.ps + 3 * frame8);
    CHECK(r.controller->received == std::vector<std::uint32_t>{0x31, 0x97});
    CHECK(r.read(spi1 + rxcrcr) == 0x97);
    CHECK((r.read(spi1 + sr) & crcerr) == (sent_crc == 0x97 ? 0 : crcerr));
  }
}

TEST_CASE("RM0038 28.3.6: a target's RXCRCR takes the frames clocked with NSS high or SPE 0")
{
  for(std::uint32_t const away : {ssm | ssi | crcen | spe, ssm | crcen})
  {
    CAPTURE(away);
    rig r{"parked", l152re(), firmware, true};
    r.write(spi1 + cr1, ssm | crcen);
    r.write(spi1 + cr1, ssm | crcen | spe);
    auto const send = [&r](std::uint32_t frame) {
      std::vector<std::uint32_t> const frames{frame};
      auto const start = r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, frames);
      r.idle_to(start.ps + 2 * frame8);
    };
    send(0x11);
    CHECK(r.cpu_read(spi1 + dr) == 0x11);
    r.write(spi1 + cr1, away);
    send(0x22);
    CHECK((r.read(spi1 + sr) & (rxne | ovr)) == 0);
    r.write(spi1 + cr1, ssm | crcen | spe);
    send(0x33);
    CHECK(r.cpu_read(spi1 + dr) == 0x33);
    CHECK(r.read(spi1 + rxcrcr) == 0xD4);
    CHECK(r.controller->received.size() == 3);
  }
}

TEST_CASE("RM0038 28.3.8 inferred: SPE 0 mid-frame lets the frame land, the buffered word waits")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x01);
  r.cpu_write(spi1 + dr, 0x02);
  r.idle_to(t0 + frame8 / 2);
  r.write(spi1 + cr1, master);
  r.idle_to(t0 + 3 * frame8);
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x01});
  CHECK(r.device->selects == std::vector<bool>{true, false});
  CHECK((r.read(spi1 + sr) & (rxne | txe | bsy)) == rxne);
  r.write(spi1 + cr1, master | spe);
  r.idle_to(r.now_ps() + 2 * frame8);
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x01, 0x02});
}

TEST_CASE("a chip select pin on GPIOA selects the device low and releases it high")
{
  rig r{"parked", l152re(), firmware, false, false, 4};
  r.write(ahbenr, gpioa_bit);
  r.write(gpioa + bsrr, 1u << 4);
  r.write(gpioa + moder, 1u << 8);
  CHECK(r.device->selects.empty());
  r.write(gpioa + bsrr, 1u << (16 + 4));
  CHECK(r.device->selects == std::vector<bool>{true});
  r.write(gpioa + bsrr, 1u << 4);
  CHECK(r.device->selects == std::vector<bool>{true, false});
}

TEST_CASE("a snapshot restores the state each attached device saved")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x01);
  r.idle_to(t0 + 2 * frame8);
  REQUIRE(r.device->frames.size() == 1);
  auto const saved = r.f->target->snapshot();
  rig        other{"parked"};
  REQUIRE_FALSE(other.device->restored.has_value());
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.device->restored == 1);
}

TEST_CASE("a snapshot mid-frame restores the frame in flight and both buffers")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x01);
  r.cpu_write(spi1 + dr, 0x02);
  r.idle_to(t0 + frame8 / 2);
  auto const saved = r.f->target->snapshot();
  rig        other{"parked"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  for(auto const* one : {&r, &other})
  {
    one->idle_to(t0 + 3 * frame8);
    CHECK(one->device->frames.size() == 2);
    CHECK(one->cpu_read(spi1 + dr) == 0x01);
    CHECK((one->read(spi1 + sr) & ovr) == ovr);
  }
  CHECK(other.device->frames.front().at == t0 + frame8);
}

TEST_CASE(
    "checklist 31: an SPI1RST pulse drops the frame in flight, a new frame keeps its own time")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x01);
  r.idle_to(t0 + frame8 / 2);
  r.write(apb2rstr, spi1_bit);
  r.write(apb2rstr, 0);
  CHECK(r.read(spi1 + cr1) == 0);
  r.write(spi1 + cr2, ssoe);
  r.write(spi1 + cr1, master | spe);
  auto const t1 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x02);
  r.idle_to(t0 + frame8 + 1);
  CHECK(r.device->frames.empty());
  CHECK((r.read(spi1 + sr) & rxne) == 0);
  r.idle_to(t1 + frame8);
  REQUIRE(r.device->outs() == std::vector<std::uint32_t>{0x02});
  CHECK(r.device->frames.front().at == t1 + frame8);
}

TEST_CASE("checklist 31: an SPI1RST pulse mid-transfer, a new target transfer keeps its own time")
{
  rig r{"parked", l152re(), firmware, true};
  r.write(spi1 + cr1, ssm | spe);
  std::vector<std::uint32_t> const first{0x10};
  auto const                       s0 = r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, first);
  r.idle_to(s0.ps + frame8 / 2);
  r.write(apb2rstr, spi1_bit);
  r.write(apb2rstr, 0);
  r.write(spi1 + cr1, ssm | spe);
  std::vector<std::uint32_t> const second{0x20};
  auto const s1 = r.port().transfer(vtime{s0.ps + 2 * frame8}, frame8, 8, second);
  CHECK(s1.ps == s0.ps + 2 * frame8);
  r.idle_to(s1.ps + frame8 / 2);
  CHECK(r.controller->received.empty());
  CHECK((r.read(spi1 + sr) & rxne) == 0);
  r.idle_to(s1.ps + frame8 + frame8 / 2);
  CHECK(r.controller->received.size() == 1);
  CHECK(r.cpu_read(spi1 + dr) == 0x20);
}

TEST_CASE("a controller frame size other than DFF lands masked, logged once")
{
  rig r{"parked", l152re(), firmware, true};
  r.write(spi1 + cr1, ssm | spe);
  std::vector<std::uint32_t> const frames{0x1234};
  auto const start = r.port().transfer(vtime{r.now_ps() + us}, frame16, 16, frames);
  r.idle_to(start.ps + frame16 + frame16 / 2);
  CHECK(r.cpu_read(spi1 + dr) == 0x34);
  auto const again = r.port().transfer(vtime{r.now_ps() + us}, frame16, 16, frames);
  r.idle_to(again.ps + frame16 + frame16 / 2);
  CHECK(r.warnings("16-bit frames to a 8-bit DFF") == 1);
}

TEST_CASE("a stopped SPI bus clock transfers no frame, logged once")
{
  rig r{"parked"};
  r.write(spi1 + cr1, master | spe);
  r.stop_clock("pclk2");
  r.cpu_write(spi1 + dr, 0x01);
  r.cpu_write(spi1 + dr, 0x02);
  r.idle_to(r.now_ps() + 3 * frame8);
  CHECK(r.device->frames.empty());
  CHECK(r.warnings("SPI bus clock is stopped") == 1);
}

TEST_CASE("a device declaring another SPI mode fails the run at its first frame")
{
  rig r{"parked"};
  r.device->declared = 3;
  r.write(spi1 + cr1, master | spe);
  r.cpu_write(spi1 + dr, 0x01);
  auto refused = std::string{};
  try
  {
    static_cast<void>(r.f->target->run_slice(
        {.max_instructions = icount{1'000'000}, .until_time = vtime{r.now_ps() + 2 * frame8}}));
  }
  catch(popcorn::config_error const& error)
  {
    refused = error.what();
  }
  CHECK(refused.contains("the SPI mode 3 the device on SS declares, not mode 0"));
}

TEST_CASE(
    "RM0038 28.2.1: FRF 1 is logged on the Cat.3 to Cat.6 SPI only, Cat.1 and Cat.2 have none")
{
  for(auto const& [desc, elf, ti] : {
          std::tuple{&l152re(),  firmware,   true },
          std::tuple{&l152rb(),  firmware_b, false},
          std::tuple{&l152rba(), firmware_b, false},
          std::tuple{&l152rc(),  firmware_b, true },
          std::tuple{&l152rca(), firmware_b, true }
  })
  {
    CAPTURE(ti);
    rig r{"parked", *desc, elf};
    r.write(spi1 + cr2, frf | ssoe);
    r.write(spi1 + cr1, master | spe);
    r.write(spi1 + cr1, master);
    r.write(spi1 + cr1, master | spe);
    CHECK(r.warnings("TI frame format") == (ti ? 1 : 0));
    r.cpu_write(spi1 + dr, 0x5A);
    r.idle_to(r.now_ps() + 2 * frame8);
    CHECK(r.device->outs() == std::vector<std::uint32_t>{0x5A});
  }
}

TEST_CASE(
    "checklist 53: under strict_peripherals FRF 1 raises model_error at the write setting SPE")
{
  rig r{"parked", l152re(), firmware, false, true};
  r.write(spi1 + cr2, frf | ssoe);
  auto* const probe = r.f->model_of<bus_probe>("DAC");
  REQUIRE(probe != nullptr);
  CHECK_THROWS_AS(
      static_cast<void>(
          probe->context.services.write(spi1 + cr1, 4, master | spe, popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(r.warnings("TI frame format") == 1);
}

TEST_CASE("checklist 39: the TI row needs FRF, the Cat.1 row builds without it, no row takes FRXTH")
{
  auto const drop_frf = [](auto& fields) {
    std::erase_if(fields, [](popcorn::field_desc const& one) { return one.name == "FRF"; });
  };
  auto const add_frxth = [](auto& fields) {
    fields.push_back({.name = "FRXTH", .lsb = 12, .width = 1});
  };
  CHECK_THROWS_AS(stm32_fixture(with_spi1(l152re(), drop_frf), firmware), popcorn::config_error);
  CHECK_NOTHROW(stm32_fixture(with_spi1(l152rb(), drop_frf), firmware_b));
  CHECK_THROWS_AS(stm32_fixture(with_spi1(l152rb(), add_frxth), firmware_b), popcorn::config_error);
}

TEST_CASE("each frame goes through exchange_deferred with a new ticket, no device is attached")
{
  auto device = std::shared_ptr<wire_target>{};
  {
    rig r{"parked"};
    device = r.device;
    r.write(spi1 + cr1, master | spe);
    r.cpu_write(spi1 + dr, 0x11);
    r.cpu_write(spi1 + dr, 0x22);
    r.idle_to(r.now_ps() + 3 * frame8);
    CHECK(device->outs() == std::vector<std::uint32_t>{0x11, 0x22});
    REQUIRE(device->tickets.size() == 2);
    CHECK(device->tickets[0] < device->tickets[1]);
  }
  CHECK(device->attaches == 0);
  CHECK(device->detaches == 0);
}

TEST_CASE("a device leaving its reply pending fails the run, the frame lands as all ones")
{
  rig r{"parked"};
  r.device->defer = true;
  r.write(spi1 + cr1, master | spe);
  r.cpu_write(spi1 + dr, 0x5A);
  auto refused = std::string{};
  try
  {
    static_cast<void>(r.f->target->run_slice(
        {.max_instructions = icount{1'000'000}, .until_time = vtime{r.now_ps() + 2 * frame8}}));
  }
  catch(popcorn::config_error const& error)
  {
    refused = error.what();
  }
  CHECK(refused.contains(
      "a reply at the frame's end from the device on SS, the STM32L1 SPI completes no deferred "
      "frame"));
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x5A});
  CHECK((r.read(spi1 + sr) & (rxne | bsy)) == rxne);
  CHECK(r.cpu_read(spi1 + dr) == 0xFF);
  r.device->defer = false;
  r.cpu_write(spi1 + dr, 0x33);
  r.idle_to(r.now_ps() + 2 * frame8);
  CHECK(r.device->outs() == std::vector<std::uint32_t>{0x5A, 0x33});
  CHECK(r.cpu_read(spi1 + dr) == 0x33);
}

auto unit_refusal(auto&& call) -> std::string
{
  rig r{"parked", l152re(), firmware, true};
  r.write(spi1 + cr1, ssm | spe);
  auto refused = std::string{};
  try
  {
    call(r.port(), vtime{r.now_ps() + us});
  }
  catch(popcorn::config_error const& error)
  {
    refused = error.what();
  }
  std::vector<std::uint32_t> const frames{0x10};
  auto const start = r.port().transfer(vtime{r.now_ps() + us}, frame8, 8, frames);
  r.idle_to(start.ps + 2 * frame8);
  CHECK(r.controller->received == std::vector<std::uint32_t>{0x00});
  CHECK(r.cpu_read(spi1 + dr) == 0x10);
  return refused;
}

TEST_CASE("spi_controller_port::select() throws config_error, the L1 SPI takes transfer() only")
{
  CHECK(unit_refusal([](periph::spi_controller_port& port, vtime at) {
          port.select(at, 1, 1);
        }).contains("devices.SPI1, expected transfer() only, the STM32L1 SPI takes no select()"));
}

TEST_CASE("spi_controller_port::frame() throws config_error, the L1 SPI takes transfer() only")
{
  CHECK(unit_refusal([](periph::spi_controller_port& port, vtime at) {
          port.frame(0x10, 8, frame8, at, 1, 1);
        }).contains("devices.SPI1, expected transfer() only, the STM32L1 SPI takes no frame()"));
}

TEST_CASE("spi_controller_port::deselect() throws config_error, the L1 SPI takes transfer() only")
{
  CHECK(unit_refusal([](periph::spi_controller_port& port, vtime at) {
          port.deselect(at, 1, 1);
        }).contains("devices.SPI1, expected transfer() only, the STM32L1 SPI takes no deselect()"));
}

TEST_CASE("spi_controller_port::drop() throws config_error, the L1 SPI takes transfer() only")
{
  CHECK(unit_refusal([](periph::spi_controller_port& port, vtime at) {
          port.drop(at, 1, 1);
        }).contains("devices.SPI1, expected transfer() only, the STM32L1 SPI takes no drop()"));
}
}

TEST_CASE("a device that throws from exchange answers as an absent one and the transfer goes on")
{
  rig r{"master8"};
  r.device->throw_at = 2;
  CHECK_THROWS_AS(std::ignore = r.f->run_to_stop(), std::runtime_error);
  CHECK(r.finish() == std::vector<std::uint32_t>{0x11, 0xFF, 0x33, 0x44, txe});
}

TEST_CASE("a device that throws from select with the hardware SS output lets the transfer finish")
{
  for(auto const nth : {std::size_t{1}, std::size_t{2}})
  {
    CAPTURE(nth);
    rig r{"master8"};
    r.device->throw_at_select = nth;
    CHECK_THROWS_AS(std::ignore = r.f->run_to_stop(), std::runtime_error);
    CHECK(r.finish() == std::vector<std::uint32_t>{0x11, 0x22, 0x33, 0x44, txe});
  }
}
