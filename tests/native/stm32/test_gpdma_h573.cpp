#include "dma_signal.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware  = "h573_rcc_ll";
constexpr popcorn::address source    = 0x2001'0000;
constexpr popcorn::address target    = 0x2001'4000;
constexpr std::uint64_t    ps_per_ms = 1'000'000'000;
constexpr std::uint32_t    spi4_tx   = 48;

class requester final : public periph::model, public stm32::dma_requester
{
public:
  explicit requester(periph::model_context& ctx) : tx{ctx.peers, "spi4_tx_dma"}
  {}

  stm32::dma_signal tx;

  void transfer_completed(stm32::dma_controller const&, std::uint32_t) override
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {
    tx.resolve();
  }
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
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

auto machine(bool with_requester = false) -> stm32_fixture
{
  auto options = popcorn::testing::stm32_options();
  if(with_requester)
  {
    options.models.emplace(
        "SPI4",
        [](periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> { return std::make_shared<requester>(ctx); });
  }
  stm32_fixture f{h573(), firmware, std::move(options)};
  f.set("pc", sym(firmware, "idle"));
  return f;
}

auto source_byte(std::uint32_t index) -> std::uint32_t
{
  return (index * 13u + 5u) & 0xffu;
}

void fill(stm32_fixture const& f, std::uint32_t length)
{
  for(std::uint32_t index = 0; index < length; ++index)
  {
    REQUIRE(f.target->mem().write(source + index, 1, source_byte(index)).has_value());
    REQUIRE(f.target->mem().write(target + index, 1, 0).has_value());
  }
}

auto desc_of(stm32_fixture const& f, std::string_view unit) -> popcorn::peripheral_desc const&
{
  return f.target->peripheral(unit).description();
}

auto bit(stm32_fixture const& f, std::string_view unit, std::string_view reg, std::string_view name)
    -> std::uint32_t
{
  auto const& desc  = desc_of(f, unit);
  auto const* found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  auto const field = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
  REQUIRE(field != found->fields.end());
  return std::uint32_t{1} << field->lsb;
}

void write(stm32_fixture const& f, std::string_view unit, std::string_view reg, std::uint32_t value)
{
  auto const& desc  = desc_of(f, unit);
  auto const* found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  REQUIRE(f.target->mem().write(desc.base + found->offset, 4, value).has_value());
}

auto word(stm32_fixture const& f, popcorn::address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto byte_at(stm32_fixture const& f, popcorn::address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 1);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto channel_reg(std::uint32_t channel, std::string_view reg) -> std::string
{
  return std::format("GPDMA_C{}{}", channel, reg);
}

void copy(
    stm32_fixture const& f,
    std::string_view     unit,
    std::uint32_t        channel,
    std::uint32_t        bytes,
    popcorn::address     from         = source,
    popcorn::address     to           = target,
    std::uint32_t        repeat       = 0,
    std::uint32_t        block_offset = 0)
{
  auto const tr1 = channel_reg(channel, "TR1");
  auto const cr  = channel_reg(channel, "CR");
  write(f, unit, tr1, bit(f, unit, tr1, "SINC") | bit(f, unit, tr1, "DINC"));
  write(f, unit, channel_reg(channel, "TR2"), bit(f, unit, channel_reg(channel, "TR2"), "SWREQ"));
  if(block_offset != 0)
  {
    write(
        f, unit, channel_reg(channel, "BR2"),
        block_offset * bit(f, unit, channel_reg(channel, "BR2"), "BRDAO"));
  }
  write(
      f, unit, channel_reg(channel, "BR1"),
      bytes | (repeat == 0 ? 0u : repeat * bit(f, unit, channel_reg(channel, "BR1"), "BRC")));
  write(f, unit, channel_reg(channel, "SAR"), static_cast<std::uint32_t>(from));
  write(f, unit, channel_reg(channel, "DAR"), static_cast<std::uint32_t>(to));
  write(f, unit, cr, bit(f, unit, cr, "EN"));
}

void idle_for(stm32_fixture const& f, std::uint64_t ps)
{
  auto const until = vtime{f.target->virtual_clock().time().ps + ps};
  for(;;)
  {
    auto const result = f.run(
        {.max_instructions = icount{1'000'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = until});
    if(result.stopped)
    {
      REQUIRE(result.stopped->reason == stop_reason::time);
      return;
    }
  }
}

auto left(stm32_fixture const& f, std::string_view unit, std::uint32_t channel) -> std::uint32_t
{
  return f.field(unit, channel_reg(channel, "BR1"), "BNDT");
}

auto done(stm32_fixture const& f, std::string_view unit, std::uint32_t channel) -> bool
{
  return f.field(unit, channel_reg(channel, "SR"), "TCF") == 1;
}

void run_until_left(
    stm32_fixture const& f,
    std::string_view     unit,
    std::uint32_t        channel,
    std::uint32_t        bytes)
{
  for(int step = 0; step < 100'000 and left(f, unit, channel) > bytes; ++step)
  {
    idle_for(f, 100'000);
  }
  REQUIRE(left(f, unit, channel) <= bytes);
}

void request_copy(stm32_fixture const& f, std::uint32_t request, std::uint32_t bytes)
{
  auto const tr1 = channel_reg(0, "TR1");
  auto const tr2 = channel_reg(0, "TR2");
  write(f, "GPDMA1", tr1, bit(f, "GPDMA1", tr1, "SINC") | bit(f, "GPDMA1", tr1, "DINC"));
  write(
      f, "GPDMA1", tr2, request * bit(f, "GPDMA1", tr2, "REQSEL") | bit(f, "GPDMA1", tr2, "DREQ"));
  write(f, "GPDMA1", channel_reg(0, "BR1"), bytes);
  write(f, "GPDMA1", channel_reg(0, "SAR"), static_cast<std::uint32_t>(source));
  write(f, "GPDMA1", channel_reg(0, "DAR"), static_cast<std::uint32_t>(target));
  write(f, "GPDMA1", channel_reg(0, "CR"), bit(f, "GPDMA1", channel_reg(0, "CR"), "EN"));
}

auto pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  return (word(f, 0xe000'e200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
}
}

TEST_CASE("the H573 has GPDMA1 and GPDMA2 of 8 channels, none past channel 7")
{
  auto f = machine();
  for(std::string_view const unit : {"GPDMA1", "GPDMA2"})
  {
    CAPTURE(unit);
    CHECK(dynamic_cast<stm32::dma_controller*>(f.target->model(unit)) != nullptr);
    auto const& desc = desc_of(f, unit);
    CHECK(popcorn::find_register(desc, "GPDMA_C7CR") != nullptr);
    CHECK(popcorn::find_register(desc, "GPDMA_C8CR") == nullptr);
  }
}

TEST_CASE("a copy on each 2D channel, GPDMA1 channel 6 and GPDMA2 channel 7, runs its two blocks")
{
  for(auto const& [unit, channel] : {
          std::pair{"GPDMA1", 6u},
          std::pair{"GPDMA2", 7u}
  })
  {
    CAPTURE(unit);
    auto f = machine();
    fill(f, 16);
    copy(f, unit, channel, 8, source, target, 1, 8);
    idle_for(f, 50 * ps_per_ms / 1000);
    CHECK(done(f, unit, channel));
    CHECK(f.field(unit, channel_reg(channel, "CR"), "EN") == 0);
    for(std::uint32_t index = 0; index < 8; ++index)
    {
      CHECK(byte_at(f, target + index) == source_byte(index));
      CHECK(byte_at(f, target + 8 + index) == 0);
      CHECK(byte_at(f, target + 16 + index) == source_byte(8 + index));
    }
  }
}

TEST_CASE("the transfer complete of GPDMA1 channel 0 and GPDMA2 channel 7 raise IRQ 27 and 97")
{
  auto f = machine();
  fill(f, 32);
  for(auto const& [unit, channel, irq] : {
          std::tuple{"GPDMA1", 0u, 27u},
          std::tuple{"GPDMA2", 7u, 97u}
  })
  {
    CAPTURE(unit);
    CHECK_FALSE(pending(f, irq));
    write(f, unit, channel_reg(channel, "CR"), bit(f, unit, channel_reg(channel, "CR"), "TCIE"));
    copy(f, unit, channel, 8);
    write(
        f, unit, channel_reg(channel, "CR"),
        bit(f, unit, channel_reg(channel, "CR"), "EN")
            | bit(f, unit, channel_reg(channel, "CR"), "TCIE"));
    idle_for(f, 50 * ps_per_ms / 1000);
    CHECK(pending(f, irq));
  }
}

TEST_CASE("the two instances run independent copies at once")
{
  auto f = machine();
  fill(f, 64);
  for(std::uint32_t index = 0; index < 64; ++index)
  {
    REQUIRE(f.target->mem().write(source + 0x100 + index, 1, 0xff - index).has_value());
  }
  copy(f, "GPDMA1", 0, 32);
  copy(f, "GPDMA2", 0, 32, source + 0x100, target + 0x100);
  idle_for(f, 50 * ps_per_ms / 1000);
  CHECK(done(f, "GPDMA1", 0));
  CHECK(done(f, "GPDMA2", 0));
  for(std::uint32_t index = 0; index < 32; ++index)
  {
    CHECK(byte_at(f, target + index) == source_byte(index));
    CHECK(byte_at(f, target + 0x100 + index) == 0xff - index);
  }
}

TEST_CASE("SPI4 TX on GPDMA1 channel 0 moves bytes with REQSEL 48 only while the request is high")
{
  auto f = machine(true);
  fill(f, 16);
  request_copy(f, 48, 16);
  idle_for(f, 20 * ps_per_ms / 1000);
  CHECK(left(f, "GPDMA1", 0) == 16);
  auto* const spi4 = f.model_of<requester>("SPI4");
  REQUIRE(spi4 != nullptr);
  spi4->tx.set(true);
  idle_for(f, 40 * ps_per_ms / 1000);
  CHECK(left(f, "GPDMA1", 0) == 0);
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    CHECK(byte_at(f, target + index) == source_byte(index));
  }
}

TEST_CASE("a request number past 127 selects its channel, REQSEL is 8 bits wide")
{
  auto f = machine();
  fill(f, 16);
  request_copy(f, 129, 16);
  auto* const controller = dynamic_cast<stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  idle_for(f, 20 * ps_per_ms / 1000);
  CHECK(left(f, "GPDMA1", 0) == 16);
  controller->on_request(129, true);
  idle_for(f, 40 * ps_per_ms / 1000);
  CHECK(left(f, "GPDMA1", 0) == 0);
}

TEST_CASE("GPDMA1RST keeps a request line of 128 or more that its requester holds")
{
  auto f = machine();
  fill(f, 16);
  auto* const controller = dynamic_cast<stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  controller->on_request(129, true);
  popcorn::testing::rcc_reset_pulse(f, "RCC_AHB1RSTR", "GPDMA1RST");
  request_copy(f, 129, 16);
  idle_for(f, 40 * ps_per_ms / 1000);
  CHECK(left(f, "GPDMA1", 0) == 0);
}

TEST_CASE("a request line of 128 or more held at a snapshot is held after the restore")
{
  auto f = machine();
  fill(f, 16);
  auto* const controller = dynamic_cast<stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  controller->on_request(129, true);
  auto const saved = f.target->snapshot();
  auto       other = machine();
  fill(other, 16);
  other.target->restore(
      popcorn::core::snapshot{
          popcorn::core::decode_snapshot(popcorn::core::encode_snapshot(saved.data()))});
  request_copy(other, 129, 16);
  idle_for(other, 40 * ps_per_ms / 1000);
  CHECK(left(other, "GPDMA1", 0) == 0);
}

namespace
{
auto frozen_copy(bool freeze, std::string_view field_name) -> stm32_fixture
{
  auto f = machine();
  fill(f, 1024);
  if(freeze)
  {
    popcorn::testing::set_debug_freeze(f, field_name, true);
  }
  copy(f, "GPDMA1", 0, 1024);
  run_until_left(f, "GPDMA1", 0, 768);
  REQUIRE(left(f, "GPDMA1", 0) > 0);
  return f;
}
}

TEST_CASE("DBG_GPDMA1_0_STOP holds channel 0 of GPDMA1 for the length of a core hold")
{
  auto f = frozen_copy(true, "DBG_GPDMA1_0_STOP");
  f.target->hold_core(true);
  auto const kept = left(f, "GPDMA1", 0);
  idle_for(f, ps_per_ms);
  CHECK(left(f, "GPDMA1", 0) == kept);
  f.target->hold_core(false);
  idle_for(f, ps_per_ms);
  CHECK(done(f, "GPDMA1", 0));
}

TEST_CASE("a copy runs on during a core hold without its DBG_GPDMA1_0_STOP bit")
{
  auto f = frozen_copy(false, "");
  f.target->hold_core(true);
  idle_for(f, ps_per_ms);
  CHECK(done(f, "GPDMA1", 0));
}

TEST_CASE("DBG_GPDMA1_0_STOP leaves channel 0 of GPDMA2 running during a core hold")
{
  auto f = machine();
  fill(f, 1024);
  popcorn::testing::set_debug_freeze(f, "DBG_GPDMA1_0_STOP", true);
  copy(f, "GPDMA2", 0, 1024);
  run_until_left(f, "GPDMA2", 0, 768);
  f.target->hold_core(true);
  idle_for(f, ps_per_ms);
  CHECK(done(f, "GPDMA2", 0));
}

TEST_CASE("DBG_GPDMA2_3_STOP holds channel 3 of GPDMA2 only")
{
  auto f = machine();
  fill(f, 1024);
  popcorn::testing::set_debug_freeze(f, "DBG_GPDMA2_3_STOP", true);
  copy(f, "GPDMA2", 3, 1024);
  copy(f, "GPDMA2", 2, 1024, source, target + 0x1000);
  run_until_left(f, "GPDMA2", 3, 768);
  f.target->hold_core(true);
  auto const kept = left(f, "GPDMA2", 3);
  idle_for(f, ps_per_ms);
  CHECK(left(f, "GPDMA2", 3) == kept);
  CHECK(done(f, "GPDMA2", 2));
}

namespace
{
auto reset_mid_copy(bool wait) -> std::pair<std::uint64_t, std::vector<std::uint32_t>>
{
  auto f = machine();
  fill(f, 1024);
  copy(f, "GPDMA2", 0, 1024);
  run_until_left(f, "GPDMA2", 0, 768);
  auto const now       = f.target->virtual_clock().time().ps;
  auto       stale_due = std::numeric_limits<std::uint64_t>::max();
  for(auto const& event : f.target->snapshot().data().events.pending)
  {
    if(event.id.kind == 0 and (event.payload >> 32) == 0 and event.due_ps > now)
    {
      stale_due = std::min(stale_due, event.due_ps);
    }
  }
  REQUIRE(stale_due != std::numeric_limits<std::uint64_t>::max());
  constexpr std::uint64_t late_wait = 5 * ps_per_ms / 1000;
  if(wait)
  {
    REQUIRE(stale_due <= now + late_wait);
  }
  popcorn::testing::rcc_reset_pulse(f, "RCC_AHB1RSTR", "GPDMA2RST");
  if(wait)
  {
    idle_for(f, late_wait);
  }
  fill(f, 32);
  copy(f, "GPDMA2", 0, 32);
  auto const start = f.target->virtual_clock().time().ps;
  for(int step = 0; step < 100'000 and not done(f, "GPDMA2", 0); ++step)
  {
    idle_for(f, 10'000);
  }
  REQUIRE(done(f, "GPDMA2", 0));
  std::vector<std::uint32_t> bytes;
  for(std::uint32_t index = 0; index < 32; ++index)
  {
    bytes.push_back(byte_at(f, target + index));
  }
  return {f.target->virtual_clock().time().ps - start, bytes};
}
}

TEST_CASE(
    "a channel of GPDMA2 re-enabled after GPDMA2RST before the stale burst's due time runs alone")
{
  auto const [early, early_bytes] = reset_mid_copy(false);
  auto const [late, late_bytes]   = reset_mid_copy(true);
  CHECK(early == late);
  CHECK(early_bytes == late_bytes);
  for(std::uint32_t index = 0; index < 32; ++index)
  {
    CHECK(early_bytes[index] == source_byte(index));
  }
}
