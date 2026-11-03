#include "../support/inert_model.hpp"
#include "dma.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware  = "l476_l4_rcc_ll";
constexpr address          spi1      = 0x4001'3000;
constexpr address          cr1       = 0x00;
constexpr address          cr2       = 0x04;
constexpr address          sr        = 0x08;
constexpr address          dr        = 0x0C;
constexpr address          crcpr     = 0x10;
constexpr address          rxcrcr    = 0x14;
constexpr address          txcrcr    = 0x18;
constexpr std::uint32_t    mstr      = 1u << 2;
constexpr std::uint32_t    spe       = 1u << 6;
constexpr std::uint32_t    ssi       = 1u << 8;
constexpr std::uint32_t    ssm       = 1u << 9;
constexpr std::uint32_t    rxonly    = 1u << 10;
constexpr std::uint32_t    crcl      = 1u << 11;
constexpr std::uint32_t    cpha      = 1u << 0;
constexpr std::uint32_t    lsbfirst  = 1u << 7;
constexpr std::uint32_t    nssp      = 1u << 3;
constexpr std::uint32_t    ldma_rx   = 1u << 13;
constexpr std::uint32_t    ldma_tx   = 1u << 14;
constexpr std::uint32_t    crcerr    = 1u << 4;
constexpr std::uint32_t    crcnext   = 1u << 12;
constexpr std::uint32_t    crcen     = 1u << 13;
constexpr std::uint32_t    bidimode  = 1u << 15;
constexpr std::uint32_t    rxdmaen   = 1u << 0;
constexpr std::uint32_t    txdmaen   = 1u << 1;
constexpr std::uint32_t    ssoe      = 1u << 2;
constexpr std::uint32_t    frf       = 1u << 4;
constexpr std::uint32_t    rxneie    = 1u << 6;
constexpr std::uint32_t    txeie     = 1u << 7;
constexpr std::uint32_t    frxth     = 1u << 12;
constexpr std::uint32_t    rxne      = 1u << 0;
constexpr std::uint32_t    txe       = 1u << 1;
constexpr std::uint32_t    ovr       = 1u << 6;
constexpr std::uint32_t    bsy       = 1u << 7;
constexpr address          apb2rstr  = 0x4002'1040;
constexpr std::uint32_t    spi1rst   = 1u << 12;
constexpr address          ispr1     = 0xE000'E204;
constexpr std::uint32_t    spi1_irq  = 1u << (35 - 32);
constexpr std::uint32_t    master    = mstr | (1u << 3) | ssm | ssi;
constexpr std::uint32_t    rx_signal = 1;
constexpr std::uint32_t    tx_signal = 2;

constexpr auto ds(std::uint32_t bits) -> std::uint32_t
{
  return (bits - 1) << 8;
}

constexpr auto frlvl(std::uint32_t status) -> std::uint32_t
{
  return status >> 9 & 3u;
}

constexpr auto ftlvl(std::uint32_t status) -> std::uint32_t
{
  return status >> 11 & 3u;
}

constexpr auto frame_ps(std::uint64_t bits, std::uint32_t br = 1) -> std::uint64_t
{
  return bits * (2u << br) * 1'000'000'000'000 / 4'000'000;
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

  std::vector<frame>                          frames;
  std::vector<std::uint32_t>                  outs;
  std::vector<std::uint32_t>                  replies;
  std::vector<std::pair<bool, std::uint64_t>> selects;

  void select(bool selected, vtime at) override
  {
    selects.emplace_back(selected, at.ps);
  }

  auto exchange(std::uint32_t out, std::uint8_t bits, vtime at) -> std::uint32_t override
  {
    frames.push_back({out, bits, at.ps});
    outs.push_back(out);
    return replies.empty() ? out : replies[(frames.size() - 1) % replies.size()];
  }

  auto exchange_deferred(std::uint32_t out, std::uint8_t bits, vtime at, std::uint64_t)
      -> std::optional<std::uint32_t> override
  {
    return exchange(out, bits, at);
  }

  void attached(periph::spi_target_port&) override
  {}

  void detached() noexcept override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}
};

class controller_probe final : public periph::spi_controller_sink
{
public:
  void frame(std::uint32_t, std::uint32_t, vtime) override
  {}
};

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;
};

class request_probe final : public popcorn::testing::inert_model, public stm32::dma_controller
{
public:
  std::array<bool, 3> level{};

  void on_request(std::uint32_t request, bool high) override
  {
    level.at(request) = high;
  }

  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    return signal == "spi1_rx_dma" ? std::optional{rx_signal}
         : signal == "spi1_tx_dma" ? std::optional{tx_signal}
                                   : std::nullopt;
  }
};

struct rig
{
  std::shared_ptr<wire_target>   device = std::make_shared<wire_target>();
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  explicit rig(bool strict = false, part_description const& desc = l476rc(), bool target = false)
  {
    stm32::board board{};
    if(target)
    {
      board.spi_controllers["SPI1"] = std::make_shared<controller_probe>();
    }
    else
    {
      board.spi["SPI1"].push_back({.device = device});
    }
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(board, desc);
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "TSC", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    options.models.insert_or_assign(
        "DMA1", [](periph::model_context&, popcorn::peripheral_desc const&) {
          return std::make_shared<request_probe>();
        });
    f = std::make_unique<stm32_fixture>(desc, firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    write(spi1 + cr2, ds(8) | ssoe);
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

  [[nodiscard]] auto cpu_read(address at, std::uint8_t size = 4) const -> std::uint32_t
  {
    auto const word = services().read(at, size, popcorn::initiator::cpu);
    REQUIRE(word.has_value());
    return static_cast<std::uint32_t>(*word);
  }

  void cpu_write(address at, std::uint32_t value, std::uint8_t size = 4) const
  {
    REQUIRE(services().write(at, size, value, popcorn::initiator::cpu).has_value());
  }

  [[nodiscard]] auto port() const -> periph::spi_controller_port&
  {
    auto* const found = dynamic_cast<periph::spi_controller_port*>(f->target->model("SPI1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  void last_but_one(std::uint32_t request) const
  {
    auto* const found = dynamic_cast<stm32::dma_requester*>(f->target->model("SPI1"));
    REQUIRE(found != nullptr);
    found->last_but_one(dma(), request);
  }

  [[nodiscard]] auto dma() const -> request_probe&
  {
    auto* const probe = f->model_of<request_probe>("DMA1");
    REQUIRE(probe != nullptr);
    return *probe;
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

  [[nodiscard]] auto frame_due() const -> std::optional<std::uint64_t>
  {
    auto const& units = f->target->description().peripherals;
    auto const  found =
        std::ranges::find(units, std::string_view{"SPI1"}, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    auto const owner   = static_cast<std::uint32_t>(found - units.begin());
    auto const pending = f->target->snapshot().data().events.pending;
    auto const event   = std::ranges::find_if(
        pending, [&](auto const& one) { return one.id.owner == owner and one.id.kind == 0; });
    return event == pending.end() ? std::nullopt : std::optional{event->due_ps};
  }

  [[nodiscard]] auto warnings(std::string_view text) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.message.find(text) != std::string::npos;
    });
  }
};

auto without(std::string_view reg, std::string_view field) -> part_description
{
  auto desc  = l476rc();
  auto found = std::ranges::find(
      desc.peripherals, std::string_view{"SPI1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  auto owner = std::ranges::find(found->registers, reg, &popcorn::register_desc::name);
  REQUIRE(owner != found->registers.end());
  auto const erased = std::erase_if(
      owner->fields, [&](popcorn::field_desc const& one) { return one.name == field; });
  REQUIRE(erased == 1);
  return desc;
}

auto crc_block(
    rig const&                        r,
    std::vector<std::uint32_t> const& words,
    std::uint32_t                     control,
    std::uint32_t                     bits,
    std::uint32_t                     crc_frames) -> std::vector<std::uint32_t>
{
  auto const size = static_cast<std::uint8_t>(bits > 8 ? 2 : 1);
  auto       got  = std::vector<std::uint32_t>{};
  for(std::size_t index = 0; index < words.size(); ++index)
  {
    r.cpu_write(spi1 + dr, words[index], size);
    if(index + 1 == words.size())
    {
      r.write(spi1 + cr1, control | crcnext | spe);
    }
    r.idle_to(r.now_ps() + frame_ps(bits));
    got.push_back(r.cpu_read(spi1 + dr, size));
  }
  for(std::uint32_t index = 0; index < crc_frames; ++index)
  {
    r.idle_to(r.now_ps() + frame_ps(bits));
    got.push_back(r.cpu_read(spi1 + dr, size));
  }
  return got;
}

void crc_setup(rig const& r, std::uint32_t cr2_value, std::uint32_t control, std::uint32_t poly)
{
  r.write(spi1 + cr2, cr2_value | ssoe | frxth);
  r.write(spi1 + crcpr, poly);
  r.write(spi1 + cr1, control);
  r.write(spi1 + cr1, control | spe);
}

constexpr auto digits = [](std::size_t count) {
  auto out = std::vector<std::uint32_t>{};
  for(std::size_t index = 0; index < count; ++index)
  {
    out.push_back(0x31 + index);
  }
  return out;
};

TEST_CASE("RM0351 Table 270: CR2 0x0700, SR 0x0002, CRCPR 0x0007, CR1 0 on SPI1 to SPI3")
{
  rig r;
  r.f->target->reset();
  for(address const base : {spi1, address{0x4000'3800}, address{0x4000'3C00}})
  {
    CAPTURE(base);
    CHECK(r.read(base + cr1) == 0);
    CHECK(r.read(base + cr2) == 0x0700);
    CHECK(r.read(base + sr) == txe);
    CHECK(r.read(base + 0x10) == 7);
  }
}

TEST_CASE("RM0351 42.6.2: a DS write of 0000 to 0010 stores 0111, 0011 and above stay")
{
  rig r;
  for(std::uint32_t value = 0; value < 16; ++value)
  {
    CAPTURE(value);
    r.write(spi1 + cr2, value << 8);
    CHECK((r.read(spi1 + cr2) >> 8 & 0xFu) == (value < 3 ? 7 : value));
  }
}

TEST_CASE("RM0351 42.4.14: two DS 8 frames queued with CRCNEXT 1 send the CRC after both")
{
  rig r;
  r.write(spi1 + cr1, master | crcen);
  r.write(spi1 + cr1, master | crcen | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x01, 1);
  r.cpu_write(spi1 + dr, 0x02, 1);
  r.write(spi1 + cr1, master | crcen | crcnext | spe);
  r.idle_to(t0 + 4 * frame_ps(8));
  REQUIRE(r.device->frames.size() == 3);
  CHECK(r.device->outs[1] == 0x02);
  CHECK(r.device->outs[2] == (r.read(spi1 + txcrcr) & 0xFFu));
  CHECK(r.device->frames[2].at == t0 + 3 * frame_ps(8));
}

TEST_CASE("RM0351 42.4.10: with FRXTH 1 each frame sets RXNE, an 8-bit read pops one byte")
{
  rig r;
  r.write(spi1 + cr2, ds(8) | ssoe | frxth);
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x11, 1);
  r.cpu_write(spi1 + dr, 0x22, 1);
  r.idle_to(t0 + frame_ps(8) + frame_ps(8) / 2);
  CHECK((r.read(spi1 + sr) & rxne) == rxne);
  r.idle_to(t0 + 3 * frame_ps(8));
  CHECK(frlvl(r.read(spi1 + sr)) == 2);
  CHECK(r.cpu_read(spi1 + dr, 1) == 0x11);
  CHECK((r.read(spi1 + sr) & rxne) == rxne);
  CHECK(r.cpu_read(spi1 + dr, 1) == 0x22);
  CHECK((r.read(spi1 + sr) & rxne) == 0);
}

TEST_CASE("RM0351 Figure 466: a 16-bit write sends two DS 8 frames, FRXTH 0 reads both at once")
{
  rig r;
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x2211, 2);
  r.idle_to(t0 + frame_ps(8) + frame_ps(8) / 2);
  CHECK((r.read(spi1 + sr) & rxne) == 0);
  r.idle_to(t0 + 3 * frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x11, 0x22});
  CHECK((r.read(spi1 + sr) & rxne) == rxne);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x2211);
  CHECK(frlvl(r.read(spi1 + sr)) == 0);
}

TEST_CASE("inferred: a 32-bit DR write and read pack two DS 8 frames as the 16-bit access does")
{
  rig r;
  r.write(spi1 + cr1, master | spe);
  r.cpu_write(spi1 + dr, 0x4433'2211);
  r.idle_to(r.now_ps() + 3 * frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x11, 0x22});
  CHECK(r.cpu_read(spi1 + dr) == 0x2211);
  CHECK(frlvl(r.read(spi1 + sr)) == 0);
}

TEST_CASE("RM0351 42.4.9: the RXFIFO keeps its bytes across SPE 0, the TXFIFO too, inferred")
{
  rig r;
  r.write(spi1 + cr2, ds(8) | ssoe | frxth);
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  for(auto const byte : {0x11u, 0x22u, 0x33u})
  {
    r.cpu_write(spi1 + dr, byte, 1);
  }
  r.idle_to(t0 + frame_ps(8) + frame_ps(8) / 2);
  r.write(spi1 + cr1, master);
  r.idle_to(t0 + 4 * frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x11, 0x22});
  CHECK(ftlvl(r.read(spi1 + sr)) == 1);
  CHECK(frlvl(r.read(spi1 + sr)) == 2);
  r.write(spi1 + cr1, master | spe);
  r.idle_to(r.now_ps() + 3 * frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x11, 0x22, 0x33});
  CHECK(r.cpu_read(spi1 + dr, 1) == 0x11);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x3322);
}

TEST_CASE(
    "RM0351 42.4.9 note: an odd frame count ends with FRXTH 1 at FRLVL 01, FRLVL 11 at 3 "
    "bytes inferred")
{
  rig r;
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x2211, 2);
  r.cpu_write(spi1 + dr, 0x33, 1);
  r.idle_to(t0 + 4 * frame_ps(8));
  CHECK(frlvl(r.read(spi1 + sr)) == 3);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x2211);
  CHECK((r.read(spi1 + sr) & rxne) == 0);
  CHECK(frlvl(r.read(spi1 + sr)) == 1);
  r.write(spi1 + cr2, ds(8) | ssoe | frxth);
  CHECK((r.read(spi1 + sr) & rxne) == rxne);
  CHECK(r.cpu_read(spi1 + dr, 1) == 0x33);
}

TEST_CASE("RM0351 42.4.10: TXE is 1 up to 2 bytes queued, FTLVL reads 11 at 3 and 4 bytes")
{
  rig r;
  r.write(spi1 + cr1, master);
  for(std::uint32_t index = 0; index < 4; ++index)
  {
    CAPTURE(index);
    r.cpu_write(spi1 + dr, 0x10 + index, 1);
    CHECK((r.read(spi1 + sr) & txe) == (index < 2 ? txe : 0));
    CHECK(ftlvl(r.read(spi1 + sr)) == std::min(index + 1, 3u));
  }
}

TEST_CASE("inferred: a byte write to a full TXFIFO is dropped, logged once")
{
  rig r;
  r.write(spi1 + cr1, master);
  for(std::uint32_t index = 0; index < 6; ++index)
  {
    r.cpu_write(spi1 + dr, 0x10 + index, 1);
  }
  CHECK(r.warnings("does not fit the 4-byte TXFIFO") == 1);
  r.write(spi1 + cr1, master | spe);
  r.idle_to(r.now_ps() + 5 * frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x10, 0x11, 0x12, 0x13});
  CHECK(ftlvl(r.read(spi1 + sr)) == 0);
}

TEST_CASE(
    "inferred, RM0351 42.4.9: a 16-bit write over three DS 8 bytes is dropped whole, the "
    "stored frames are sent")
{
  rig r;
  r.write(spi1 + cr1, master);
  for(auto const byte : {0x01u, 0x02u, 0x03u})
  {
    r.cpu_write(spi1 + dr, byte, 1);
  }
  r.cpu_write(spi1 + dr, 0x0504, 2);
  CHECK(ftlvl(r.read(spi1 + sr)) == 3);
  CHECK(r.warnings("does not fit the 4-byte TXFIFO") == 1);
  r.write(spi1 + cr1, master | spe);
  r.idle_to(r.now_ps() + 4 * frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x01, 0x02, 0x03});
}

TEST_CASE(
    "inferred, RM0351 42.4.9: a DS 16 write with one byte of room is dropped whole, the "
    "frames stay aligned")
{
  rig r;
  r.write(spi1 + cr2, ds(16) | ssoe);
  r.write(spi1 + cr1, master);
  r.cpu_write(spi1 + dr, 0xBEEF, 2);
  r.cpu_write(spi1 + dr, 0xAD, 1);
  r.cpu_write(spi1 + dr, 0x1234, 2);
  CHECK(ftlvl(r.read(spi1 + sr)) == 3);
  r.write(spi1 + cr1, master | spe);
  r.idle_to(r.now_ps() + 2 * frame_ps(16));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0xBEEF});
  CHECK(ftlvl(r.read(spi1 + sr)) == 1);
  r.cpu_write(spi1 + dr, 0xDE, 1);
  r.idle_to(r.now_ps() + frame_ps(16) + 1);
  CHECK(r.device->outs == std::vector<std::uint32_t>{0xBEEF, 0xDEAD});
}

TEST_CASE("checklist 53 policy: under strict_peripherals a DR write that does not fit raises")
{
  rig strict{true};
  strict.write(spi1 + cr1, master);
  for(auto const byte : {0x01u, 0x02u, 0x03u})
  {
    strict.cpu_write(spi1 + dr, byte, 1);
  }
  CHECK_THROWS_AS(
      static_cast<void>(strict.services().write(spi1 + dr, 2, 0x0504, popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(ftlvl(strict.read(spi1 + sr)) == 3);
}

TEST_CASE("RM0351 42.4.10: a DS 16 frame takes two bytes of each FIFO")
{
  rig r;
  r.write(spi1 + cr2, ds(16) | ssoe);
  r.write(spi1 + cr1, master);
  r.cpu_write(spi1 + dr, 0xBEEF, 2);
  CHECK((r.read(spi1 + sr) & txe) == txe);
  CHECK(ftlvl(r.read(spi1 + sr)) == 2);
  r.cpu_write(spi1 + dr, 0xCAFE, 2);
  CHECK((r.read(spi1 + sr) & txe) == 0);
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.idle_to(t0 + 2 * frame_ps(16) + 1);
  CHECK(r.device->outs == std::vector<std::uint32_t>{0xBEEF, 0xCAFE});
  CHECK(r.device->frames[1].bits == 16);
  CHECK(r.device->frames[1].at == t0 + 2 * frame_ps(16));
  CHECK(frlvl(r.read(spi1 + sr)) == 3);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0xBEEF);
  CHECK(frlvl(r.read(spi1 + sr)) == 2);
}

TEST_CASE(
    "RM0351 42.4.10: with DS 16 a byte write waits for the second byte, inferred, BSY "
    "stays 0 until the frame is whole")
{
  rig r;
  r.write(spi1 + cr2, ds(16) | ssoe);
  r.write(spi1 + cr1, master | spe);
  r.cpu_write(spi1 + dr, 0xAD, 1);
  CHECK_FALSE(r.frame_due().has_value());
  CHECK((r.read(spi1 + sr) & bsy) == 0);
  r.cpu_write(spi1 + dr, 0xDE, 1);
  CHECK((r.read(spi1 + sr) & bsy) == bsy);
  r.idle_to(r.now_ps() + frame_ps(16) + 1);
  CHECK(r.device->outs == std::vector<std::uint32_t>{0xDEAD});
  CHECK((r.read(spi1 + sr) & bsy) == 0);
}

TEST_CASE("RM0351 42.4.10: OVR at the fifth DS 8 frame, the RXFIFO keeps the first four")
{
  rig r;
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  for(std::uint32_t index = 0; index < 5; ++index)
  {
    r.cpu_write(spi1 + dr, index + 1, 1);
    r.idle_to(t0 + index * frame_ps(8) + frame_ps(8) / 2);
    CHECK((r.read(spi1 + sr) & ovr) == 0);
  }
  r.idle_to(t0 + 6 * frame_ps(8));
  CHECK((r.read(spi1 + sr) & ovr) == ovr);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x0201);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x0403);
}

TEST_CASE("RM0351 42.4.11: a DR read, then an SR read clears OVR")
{
  rig r;
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  for(std::uint32_t index = 0; index < 5; ++index)
  {
    r.cpu_write(spi1 + dr, index + 1, 1);
    r.idle_to(t0 + (index + 1) * frame_ps(8) + 1);
  }
  CHECK(r.cpu_read(spi1 + sr) == (rxne | txe | ovr | 3u << 9));
  CHECK((r.read(spi1 + sr) & ovr) == ovr);
  CHECK(r.cpu_read(spi1 + dr, 1) == 0x01);
  CHECK(r.cpu_read(spi1 + sr) == (rxne | txe | ovr | 3u << 9));
  CHECK((r.read(spi1 + sr) & ovr) == 0);
}

TEST_CASE("RM0351 42.4.10: a DS 16 frame with one byte of room sets OVR")
{
  rig r;
  r.write(spi1 + cr2, ds(16) | ssoe);
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  for(std::uint32_t index = 0; index < 2; ++index)
  {
    r.cpu_write(spi1 + dr, 0x0101 * (index + 1), 2);
    r.idle_to(t0 + (index + 1) * frame_ps(16) + 1);
  }
  CHECK(r.cpu_read(spi1 + dr, 1) == 0x01);
  CHECK(frlvl(r.read(spi1 + sr)) == 3);
  r.cpu_write(spi1 + dr, 0x0303, 2);
  r.idle_to(r.now_ps() + frame_ps(16) + 1);
  CHECK((r.read(spi1 + sr) & ovr) == ovr);
  CHECK(frlvl(r.read(spi1 + sr)) == 3);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x0201);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x0002);
}

TEST_CASE("RM0351 42.6.2 DS: DS 4 and DS 12 frames are masked and due after DS + 1 bits")
{
  for(auto const& [bits, br] : {
          std::pair{4u,  1u},
          std::pair{12u, 1u},
          std::pair{8u,  3u}
  })
  {
    CAPTURE(bits);
    rig r;
    r.device->replies = {0xFFFF};
    r.write(spi1 + cr2, ds(bits) | ssoe | (bits <= 8 ? frxth : 0));
    r.write(spi1 + cr1, (master & ~(7u << 3)) | br << 3 | spe);
    auto const t0 = r.now_ps();
    r.cpu_write(spi1 + dr, bits <= 8 ? 0xBC : 0xFABC, bits <= 8 ? 1 : 2);
    CHECK(r.frame_due() == t0 + frame_ps(bits, br));
    r.idle_to(t0 + frame_ps(bits, br) + 1);
    REQUIRE(r.device->frames.size() == 1);
    CHECK(r.device->frames[0].bits == bits);
    CHECK(r.device->frames[0].out == (0xFABCu & ((1u << bits) - 1)));
    CHECK(r.cpu_read(spi1 + dr, 2) == (1u << bits) - 1);
  }
}

TEST_CASE("RM0351 42.4.10 note: controller receive-only keeps BSY 1 in RXONLY and BIDIMODE")
{
  rig r;
  r.write(spi1 + cr1, master | rxonly | spe);
  CHECK((r.read(spi1 + sr) & bsy) == bsy);
  r.write(spi1 + cr1, master);
  r.write(spi1 + cr1, master | bidimode | spe);
  auto const t0 = r.now_ps();
  r.idle_to(t0 + frame_ps(8) / 2);
  CHECK((r.read(spi1 + sr) & bsy) == bsy);
  r.write(spi1 + cr1, master);
  r.idle_to(t0 + 2 * frame_ps(8));
  CHECK((r.read(spi1 + sr) & bsy) == 0);
  r.cpu_write(spi1 + dr, 0x55, 1);
  CHECK((r.read(spi1 + sr) & bsy) == 0);
  r.write(spi1 + cr1, master | spe);
  CHECK((r.read(spi1 + sr) & bsy) == bsy);
}

TEST_CASE("inferred: a read of an empty RXFIFO slot returns 0, DR + 1 moves no FIFO byte")
{
  rig r;
  r.write(spi1 + cr2, ds(8) | ssoe | frxth);
  r.write(spi1 + cr1, master | spe);
  r.cpu_write(spi1 + dr + 1, 0x77, 1);
  CHECK(ftlvl(r.read(spi1 + sr)) == 0);
  CHECK_FALSE(r.frame_due().has_value());
  r.cpu_write(spi1 + dr, 0x5A, 1);
  r.idle_to(r.now_ps() + frame_ps(8) + 1);
  static_cast<void>(r.cpu_read(spi1 + dr + 1, 1));
  CHECK(frlvl(r.read(spi1 + sr)) == 1);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x5A);
  CHECK(frlvl(r.read(spi1 + sr)) == 0);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0);
}

TEST_CASE("RM0351 42.4.10: TXEIE, RXNEIE and the DMA requests follow TXE and RXNE of the FIFOs")
{
  rig r;
  r.write(spi1 + cr1, master);
  for(auto const byte : {0x01, 0x02, 0x03})
  {
    r.cpu_write(spi1 + dr, byte, 1);
  }
  r.write(spi1 + cr2, ds(8) | ssoe | txeie | txdmaen | rxdmaen);
  CHECK((r.read(ispr1) & spi1_irq) == 0);
  CHECK_FALSE(r.dma().level[tx_signal]);
  CHECK_FALSE(r.dma().level[rx_signal]);
  r.write(spi1 + cr1, master | spe);
  CHECK((r.read(ispr1) & spi1_irq) == spi1_irq);
  CHECK(r.dma().level[tx_signal]);
  r.write(spi1 + cr2, ds(8) | ssoe | rxneie | rxdmaen);
  r.write(ispr1 + 0x80, spi1_irq);
  auto const t0 = r.now_ps();
  r.idle_to(t0 + frame_ps(8) + frame_ps(8) / 2);
  CHECK((r.read(ispr1) & spi1_irq) == 0);
  CHECK_FALSE(r.dma().level[rx_signal]);
  CHECK_FALSE(r.dma().level[tx_signal]);
  r.idle_to(t0 + 2 * frame_ps(8) + frame_ps(8) / 2);
  CHECK((r.read(ispr1) & spi1_irq) == spi1_irq);
  CHECK(r.dma().level[rx_signal]);
}

TEST_CASE("a snapshot mid-frame restores the frame in flight and both FIFOs")
{
  rig r;
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x0201, 2);
  r.cpu_write(spi1 + dr, 0x03, 1);
  r.idle_to(t0 + frame_ps(8) + frame_ps(8) / 2);
  auto const saved = r.f->target->snapshot();
  rig        other;
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(frlvl(other.read(spi1 + sr)) == 1);
  CHECK(ftlvl(other.read(spi1 + sr)) == 1);
  for(auto const* one : {&r, &other})
  {
    one->idle_to(t0 + 4 * frame_ps(8));
    CHECK(one->cpu_read(spi1 + dr, 2) == 0x0201);
    CHECK(one->cpu_read(spi1 + dr, 1) == 0x03);
  }
  CHECK(other.device->outs == std::vector<std::uint32_t>{0x02, 0x03});
  CHECK(other.device->frames.back().at == t0 + 3 * frame_ps(8));
}

TEST_CASE("checklist 31: an SPI1RST pulse empties both FIFOs, a new frame keeps its own time")
{
  rig r;
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x0201, 2);
  r.cpu_write(spi1 + dr, 0x03, 1);
  r.idle_to(t0 + frame_ps(8) + frame_ps(8) / 2);
  r.write(apb2rstr, spi1rst);
  r.write(apb2rstr, 0);
  CHECK(r.read(spi1 + sr) == txe);
  CHECK(r.read(spi1 + cr2) == 0x0700);
  r.write(spi1 + cr2, ds(8) | ssoe | frxth);
  r.write(spi1 + cr1, master | spe);
  auto const t1 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x44, 1);
  r.idle_to(t0 + 2 * frame_ps(8) + 1);
  CHECK(r.device->frames.size() == 1);
  r.idle_to(t1 + frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x01, 0x44});
  CHECK(r.device->frames.back().at == t1 + frame_ps(8));
  CHECK(frlvl(r.read(spi1 + sr)) == 1);
  CHECK(r.cpu_read(spi1 + dr, 1) == 0x44);
}

TEST_CASE("checklist 27: a DR write at the frame end time queues behind the frame that ends")
{
  rig r;
  r.write(spi1 + cr2, ds(8) | ssoe | frxth);
  r.write(spi1 + cr1, master | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x01, 1);
  r.idle_to(t0 + frame_ps(8));
  r.cpu_write(spi1 + dr, 0x02, 1);
  CHECK(r.frame_due() == t0 + 2 * frame_ps(8));
  r.idle_to(t0 + 2 * frame_ps(8) + 1);
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x01, 0x02});
  CHECK(frlvl(r.read(spi1 + sr)) == 2);
}

TEST_CASE("checklist 39: the FIFO row needs CR2.FRXTH, CR2.DS, SR.FRLVL and SR.FTLVL")
{
  for(auto const& [reg, field] : {
          std::pair{"CR2", "FRXTH"},
          std::pair{"CR2", "DS"   },
          std::pair{"SR",  "FRLVL"},
          std::pair{"SR",  "FTLVL"}
  })
  {
    CAPTURE(field);
    auto const desc = without(reg, field);
    CHECK_THROWS_AS(rig(false, desc), popcorn::config_error);
  }
}

TEST_CASE("RM0351 42.6.2 FRF: the TI format logs once, raises model_error under strict")
{
  {
    rig r;
    r.write(spi1 + cr1, master | spe);
    r.write(spi1 + cr2, ds(8) | frf);
    r.write(spi1 + cr2, ds(8) | frf | ssoe);
    CHECK(r.warnings("TI frame format") == 1);
  }
  rig strict{true};
  strict.write(spi1 + cr1, master | spe);
  CHECK_THROWS_AS(
      static_cast<void>(
          strict.services().write(spi1 + cr2, 4, ds(8) | frf, popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(strict.warnings("TI frame format") == 1);
}

TEST_CASE("a snapshot whose TXFIFO or RXFIFO level is above 4 is refused at load")
{
  for(auto const key : {std::string_view{"tx_level"}, std::string_view{"rx_level"}})
  {
    CAPTURE(key);
    rig        r;
    auto       data = r.f->target->snapshot().data();
    auto const owner =
        std::ranges::find(data.models, std::string_view{"SPI1"}, &core::snapshot_part::owner);
    REQUIRE(owner != data.models.end());
    auto const bytes = std::span{owner->data};
    auto const found = std::ranges::search(
        bytes, key, [](std::byte one, char other) { return one == static_cast<std::byte>(other); });
    REQUIRE_FALSE(found.empty());
    auto const at = static_cast<std::size_t>(found.begin() - bytes.begin()) + key.size() + 1;
    REQUIRE(at < owner->data.size());
    owner->data[at] = std::byte{5};
    rig other;
    CHECK_THROWS_WITH_AS(
        other.f->target->restore(core::snapshot{data}), doctest::Contains("snapshot.SPI1.fifo"),
        popcorn::config_error);
  }
}
}

TEST_CASE("RM0351 42.4.14: CRC-8 over DS 8 frames of 123456789 is 0xF4, a bad frame sets CRCERR")
{
  for(auto const corrupt : {false, true})
  {
    CAPTURE(corrupt);
    rig r;
    crc_setup(r, ds(8), master | crcen, 7);
    if(corrupt)
    {
      r.device->replies = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x00, 0xF4};
    }
    auto const got = crc_block(r, digits(9), master | crcen, 8, 1);
    CHECK(r.device->outs.back() == 0xF4);
    CHECK(got.back() == 0xF4);
    CHECK(r.read(spi1 + txcrcr) == 0xF4);
    CHECK((r.read(spi1 + sr) & crcerr) == (corrupt ? crcerr : 0));
    CHECK((r.read(spi1 + cr1) & crcnext) == 0);
  }
}

TEST_CASE("RM0351 42.4.14: CRC-16 over DS 8 frames takes two CRC frames, high byte first inferred")
{
  rig r;
  crc_setup(r, ds(8), master | crcen | crcl, 0x1021);
  auto const got = crc_block(r, digits(9), master | crcen | crcl, 8, 2);
  CHECK(r.read(spi1 + txcrcr) == 0x31C3);
  CHECK(
      std::vector(r.device->outs.end() - 2, r.device->outs.end())
      == std::vector<std::uint32_t>{0x31, 0xC3});
  CHECK(std::vector(got.end() - 2, got.end()) == std::vector<std::uint32_t>{0x31, 0xC3});
  CHECK((r.read(spi1 + sr) & crcerr) == 0);
  CHECK(r.device->frames.back().at == r.device->frames[8].at + 2 * frame_ps(8));
}

TEST_CASE(
    "RM0351 42.4.14: DS 16 frames take a CRC-16 frame, with CRCL 0 a 16-bit frame holding the "
    "CRC-8 and CRCERR on its low 8 bits, inferred")
{
  auto const words = std::vector<std::uint32_t>{0x3132, 0x3334, 0x3536, 0x3738};
  for(auto const& [length, poly, expected] : {
          std::tuple{crcl, 0x1021u, 0x9015u},
          std::tuple{0u,   7u,      0xC7u  }
  })
  {
    CAPTURE(expected);
    rig r;
    crc_setup(r, ds(16), master | crcen | length, poly);
    if(length == 0)
    {
      r.device->replies = {0x3132, 0x3334, 0x3536, 0x3738, 0xAB00 | expected};
    }
    static_cast<void>(crc_block(r, words, master | crcen | length, 16, 1));
    CHECK(r.device->outs.size() == 5);
    CHECK(r.device->outs.back() == expected);
    CHECK(r.device->frames.back().bits == 16);
    CHECK((r.read(spi1 + sr) & crcerr) == 0);
  }
}

TEST_CASE("RM0351 42.4.14: with LSBFIRST 1 the CRC-16 phase of DS 8 frames sends [7:0] first")
{
  rig r;
  crc_setup(r, ds(8), master | crcen | crcl | lsbfirst, 0x1021);
  static_cast<void>(crc_block(r, digits(9), master | crcen | crcl | lsbfirst, 8, 2));
  auto const crc      = r.read(spi1 + txcrcr);
  auto const reversed = [](std::uint32_t byte) {
    auto out = std::uint32_t{};
    for(std::uint32_t bit = 0; bit < 8; ++bit)
    {
      out |= (byte >> bit & 1u) << (7 - bit);
    }
    return out;
  };
  REQUIRE((crc & 0xFF) != crc >> 8);
  CHECK(
      std::vector(r.device->outs.end() - 2, r.device->outs.end())
      == std::vector<std::uint32_t>{reversed(crc & 0xFF), reversed(crc >> 8)});
  CHECK((r.read(spi1 + sr) & crcerr) == 0);
}

TEST_CASE("RM0351 42.4.14: SPE 0 in a CRC-16 phase, then CRCEN 0 and 1 give the next block two")
{
  rig        r;
  auto const control = master | crcen | crcl;
  crc_setup(r, ds(8), control, 0x1021);
  for(auto const word : digits(9))
  {
    r.cpu_write(spi1 + dr, word, 1);
    if(word == 0x39)
    {
      r.write(spi1 + cr1, control | crcnext | spe);
    }
    r.idle_to(r.now_ps() + frame_ps(8));
    static_cast<void>(r.cpu_read(spi1 + dr, 1));
  }
  r.idle_to(r.now_ps() + frame_ps(8) / 2);
  r.write(spi1 + cr1, control);
  r.idle_to(r.now_ps() + 2 * frame_ps(8));
  REQUIRE(r.device->outs.size() == 10);
  r.write(spi1 + cr1, master | crcl);
  r.write(spi1 + cr1, control);
  r.write(spi1 + cr1, control | spe);
  while((r.read(spi1 + sr) & rxne) != 0)
  {
    static_cast<void>(r.cpu_read(spi1 + dr, 1));
  }
  static_cast<void>(crc_block(r, digits(9), control, 8, 2));
  CHECK(r.device->outs.size() == 21);
  CHECK(
      std::vector(r.device->outs.end() - 2, r.device->outs.end())
      == std::vector<std::uint32_t>{0x31, 0xC3});
  CHECK((r.read(spi1 + sr) & crcerr) == 0);
}

TEST_CASE("RM0351 42.4.14: a mode fault in the second CRC-16 frame leaves the next block two")
{
  rig        r;
  auto const control = master | crcen | crcl;
  crc_setup(r, ds(8), control, 0x1021);
  static_cast<void>(crc_block(r, digits(9), control, 8, 0));
  r.idle_to(r.now_ps() + frame_ps(8) + frame_ps(8) / 2);
  REQUIRE(r.device->outs.size() == 10);
  r.write(spi1 + cr1, (control | spe) & ~ssi);
  r.idle_to(r.now_ps() + 2 * frame_ps(8));
  REQUIRE(r.device->outs.size() == 10);
  static_cast<void>(r.cpu_read(spi1 + sr));
  r.write(spi1 + cr1, control);
  r.write(spi1 + cr1, control | spe);
  while((r.read(spi1 + sr) & rxne) != 0)
  {
    static_cast<void>(r.cpu_read(spi1 + dr, 1));
  }
  static_cast<void>(crc_block(r, digits(9), control, 8, 2));
  CHECK(r.device->outs.size() == 21);
}

TEST_CASE("RM0351 42.4.14: the data frame after a CRC phase clears TXCRCR and RXCRCR first")
{
  rig r;
  crc_setup(r, ds(8), master | crcen, 7);
  static_cast<void>(crc_block(r, digits(9), master | crcen, 8, 1));
  CHECK(r.read(spi1 + rxcrcr) == 0xF4);
  static_cast<void>(crc_block(r, digits(9), master | crcen, 8, 1));
  CHECK(r.device->outs.size() == 20);
  CHECK(r.device->outs.back() == 0xF4);
  CHECK((r.read(spi1 + sr) & crcerr) == 0);
}

TEST_CASE("checklist 53: CRCEN with DS 12 logs once, model_error under strict at the frame start")
{
  {
    rig r;
    crc_setup(r, ds(12), master | crcen, 7);
    r.cpu_write(spi1 + dr, 0x0ABC, 2);
    r.cpu_write(spi1 + dr, 0x0123, 2);
    r.idle_to(r.now_ps() + 3 * frame_ps(12));
    CHECK(r.device->outs == std::vector<std::uint32_t>{0xABC, 0x123});
    CHECK(r.warnings("CRCEN 1 with a CR2.DS other than 8 or 16") == 1);
  }
  rig strict{true};
  strict.write(spi1 + cr2, ds(12) | ssoe);
  strict.write(spi1 + cr1, master | crcen | spe);
  CHECK_THROWS_AS(
      static_cast<void>(strict.services().write(spi1 + dr, 2, 0x0ABC, popcorn::initiator::cpu)),
      popcorn::model_error);
}

TEST_CASE("checklist 53: the frame-start raise after a DS change keeps the landed frame's RXNE")
{
  rig r{true};
  r.write(spi1 + cr2, ds(8) | ssoe | frxth);
  r.write(spi1 + cr1, master | crcen | spe);
  r.cpu_write(spi1 + dr, 0x0201, 2);
  r.cpu_write(spi1 + dr, 0x0403, 2);
  r.write(spi1 + cr2, ds(12) | ssoe | frxth);
  CHECK_THROWS_AS(
      static_cast<void>(r.f->run_to_stop(
          {.max_instructions = icount{1'000'000}, .until_time = vtime{r.now_ps() + frame_ps(8)}})),
      popcorn::model_error);
  CHECK((r.read(spi1 + sr) & rxne) == rxne);
}

TEST_CASE("checklist 53: a target's frame-start raise after a DS change keeps RXNE, clears BSY")
{
  rig r{true, l476rc(), true};
  r.write(spi1 + cr2, ds(8) | frxth);
  r.write(spi1 + cr1, ssm | crcen | spe);
  std::vector<std::uint32_t> const frames{0x01, 0x02, 0x03};
  auto const start = r.port().transfer(vtime{r.now_ps() + 1'000'000}, frame_ps(8), 8, frames);
  r.idle_to(start.ps + frame_ps(8) / 2);
  REQUIRE((r.read(spi1 + sr) & bsy) == bsy);
  r.write(spi1 + cr2, ds(12) | frxth);
  CHECK_THROWS_AS(
      static_cast<void>(r.f->run_to_stop(
          {.max_instructions = icount{1'000'000},
           .until_time       = vtime{start.ps + 2 * frame_ps(8)}})),
      popcorn::model_error);
  CHECK((r.read(spi1 + sr) & (rxne | bsy)) == rxne);
}

TEST_CASE("RM0351 42.4.9: receive-only with CRCEN holds one frame, FRLVL reads 00")
{
  rig r;
  r.write(spi1 + cr1, master | rxonly | crcen);
  r.write(spi1 + cr1, master | rxonly | crcen | spe);
  auto const t0 = r.now_ps();
  r.idle_to(t0 + frame_ps(8) + frame_ps(8) / 2);
  CHECK((r.read(spi1 + sr) & (rxne | ovr)) == rxne);
  CHECK(frlvl(r.read(spi1 + sr)) == 0);
  r.idle_to(t0 + 2 * frame_ps(8) + frame_ps(8) / 2);
  CHECK((r.read(spi1 + sr) & ovr) == ovr);
  r.write(spi1 + cr1, master | rxonly | crcen);
  CHECK(r.cpu_read(spi1 + dr, 1) == 0xFF);
  CHECK((r.read(spi1 + sr) & rxne) == 0);
}

TEST_CASE("RM0351 42.4.9 Packing with DMA: LDMA_TX sends 7 frames from 4 half-word writes")
{
  for(auto const last : {0u, ldma_tx})
  {
    CAPTURE(last);
    rig r;
    r.write(spi1 + cr2, ds(8) | ssoe | frxth | txdmaen | last);
    r.write(spi1 + cr1, master | spe);
    for(std::uint32_t half : {0x0201u, 0x0403u, 0x0605u, 0x0807u})
    {
      if(half == 0x0807)
      {
        r.last_but_one(tx_signal);
      }
      r.cpu_write(spi1 + dr, half, 2);
      r.idle_to(r.now_ps() + 2 * frame_ps(8));
      static_cast<void>(r.cpu_read(spi1 + dr, 2));
    }
    auto expected = std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8};
    expected.resize(last != 0 ? 7 : 8);
    CHECK(r.device->outs == expected);
  }
}

TEST_CASE(
    "RM0351 42.4.9 Packing with DMA: LDMA_RX raises RXNE at one byte and pops it, 0 above inferred")
{
  for(auto const last : {0u, ldma_rx})
  {
    CAPTURE(last);
    rig r;
    r.write(spi1 + cr2, ds(8) | ssoe | rxdmaen | last);
    r.write(spi1 + cr1, master | spe);
    for(std::uint32_t half : {0x0201u, 0x0403u, 0x0605u})
    {
      r.cpu_write(spi1 + dr, half, 2);
      r.idle_to(r.now_ps() + 2 * frame_ps(8));
      CHECK(r.cpu_read(spi1 + dr, 2) == half);
    }
    r.last_but_one(rx_signal);
    r.cpu_write(spi1 + dr, 0x0807, 2);
    auto const t0 = r.now_ps();
    r.idle_to(t0 + frame_ps(8) + 1);
    CHECK((r.read(spi1 + sr) & rxne) == (last != 0 ? rxne : 0));
    CHECK(r.dma().level[rx_signal] == (last != 0));
    r.idle_to(t0 + 2 * frame_ps(8) + 1);
    CHECK(r.cpu_read(spi1 + dr, 2) == (last != 0 ? 0x07u : 0x0807u));
    CHECK(frlvl(r.read(spi1 + sr)) == (last != 0 ? 1u : 0u));
    CHECK((r.read(spi1 + sr) & ovr) == 0);
  }
}

TEST_CASE("inferred: an LDMA_TX flag lasts one DR write, needs DS 8 or less, SPE 0 clears it")
{
  for(auto const leg : {0, 1, 2})
  {
    CAPTURE(leg);
    rig r;
    r.write(spi1 + cr2, (leg == 0 ? ds(16) : ds(8)) | ssoe | frxth | txdmaen | ldma_tx);
    r.write(spi1 + cr1, master | spe);
    r.last_but_one(tx_signal);
    if(leg == 1)
    {
      r.cpu_write(spi1 + dr, 0x01, 1);
      r.idle_to(r.now_ps() + frame_ps(8));
      static_cast<void>(r.cpu_read(spi1 + dr, 1));
    }
    else if(leg == 0)
    {
      r.write(spi1 + cr2, ds(8) | ssoe | frxth | txdmaen | ldma_tx);
    }
    else
    {
      r.write(spi1 + cr1, master);
      r.write(spi1 + cr1, master | spe);
    }
    auto const before = r.device->outs.size();
    r.cpu_write(spi1 + dr, 0x0302, 2);
    r.idle_to(r.now_ps() + 3 * frame_ps(8));
    CHECK(r.device->outs.size() == before + 2);
  }
}

TEST_CASE("inferred: an LDMA_RX flag lasts one DR read, needs DS 8 or less, SPE 0 clears it")
{
  for(auto const leg : {0, 1, 2})
  {
    CAPTURE(leg);
    rig r;
    r.write(spi1 + cr2, (leg == 0 ? ds(16) : ds(8)) | ssoe | rxdmaen | ldma_rx);
    r.write(spi1 + cr1, master | spe);
    r.last_but_one(rx_signal);
    if(leg == 1)
    {
      r.cpu_write(spi1 + dr, 0x01, 1);
      r.idle_to(r.now_ps() + 2 * frame_ps(8));
      static_cast<void>(r.cpu_read(spi1 + dr, 1));
    }
    else if(leg == 0)
    {
      r.write(spi1 + cr2, ds(8) | ssoe | rxdmaen | ldma_rx);
    }
    else
    {
      r.write(spi1 + cr1, master);
      r.write(spi1 + cr1, master | spe);
    }
    r.cpu_write(spi1 + dr, 0x0302, 2);
    r.idle_to(r.now_ps() + 3 * frame_ps(8));
    CHECK(r.cpu_read(spi1 + dr, 2) == 0x0302);
  }
}

TEST_CASE("RM0351 42.4.12: NSSP pulses NSS high one SCK period between frames, inferred")
{
  rig r;
  r.write(spi1 + cr2, ds(8) | ssoe | frxth | nssp);
  r.write(spi1 + cr1, master | spe);
  auto const t0  = r.now_ps();
  auto const sck = frame_ps(8) / 8;
  r.cpu_write(spi1 + dr, 0x2211, 2);
  r.idle_to(t0 + frame_ps(8) + 1);
  CHECK(r.frame_due() == t0 + 2 * frame_ps(8) + sck);
  r.idle_to(t0 + 3 * frame_ps(8));
  auto const offset = r.device->frames.front().at - (t0 + frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x11, 0x22});
  CHECK(r.device->frames[1].at == t0 + 2 * frame_ps(8) + sck + offset);
  CHECK(
      r.device->selects
      == std::vector<std::pair<bool, std::uint64_t>>{
          {true,  t0 + offset                        },
          {false, t0 + frame_ps(8) + offset          },
          {true,  t0 + frame_ps(8) + sck + offset    },
          {false, t0 + 2 * frame_ps(8) + sck + offset}
  });
  CHECK((r.read(spi1 + cr1) & spe) == spe);
}

TEST_CASE("RM0351 42.6.2 NSSP: with CPHA 1 frames run back to back and NSS stays low")
{
  rig r;
  r.write(spi1 + cr2, ds(8) | ssoe | frxth | nssp);
  r.write(spi1 + cr1, master | cpha | spe);
  auto const t0 = r.now_ps();
  r.cpu_write(spi1 + dr, 0x2211, 2);
  r.idle_to(t0 + 3 * frame_ps(8));
  CHECK(r.device->outs == std::vector<std::uint32_t>{0x11, 0x22});
  CHECK(r.device->frames[1].at - r.device->frames[0].at == frame_ps(8));
  CHECK(r.device->selects.size() == 1);
}

TEST_CASE(
    "a target lands controller frames in the RXFIFO masked to DS, logged once, OVR at the "
    "fifth DS 8 frame")
{
  rig r{false, l476rc(), true};
  r.write(spi1 + cr2, ds(8) | frxth);
  r.write(spi1 + cr1, ssm | spe);
  r.cpu_write(spi1 + dr, 0xA5, 1);
  std::vector<std::uint32_t> const frames{0x1201, 0x1202, 0x1203, 0x1204, 0x1205};
  auto const start = r.port().transfer(vtime{r.now_ps() + 1'000'000}, frame_ps(16), 16, frames);
  r.idle_to(start.ps + 6 * frame_ps(16));
  CHECK(r.warnings("16-bit frames to a 8-bit DS") == 1);
  CHECK((r.read(spi1 + sr) & ovr) == ovr);
  CHECK(frlvl(r.read(spi1 + sr)) == 3);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x0201);
  CHECK(r.cpu_read(spi1 + dr, 2) == 0x0403);
}
