#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include "../fake_services.hpp"
#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;

struct probe_state
{
  std::uint32_t generation{};
  std::uint64_t anchor{};
  std::uint32_t level{};

  friend auto operator==(probe_state const&, probe_state const&) -> bool = default;
};

class probe_model final : public stm32::stm32_model<probe_state>
{
public:
  probe_model(
      periph::model_context&          ctx,
      popcorn::peripheral_desc const& desc,
      std::string_view                reg_name,
      std::string_view                field_name)
      : stm32_model{ctx, desc, "probe"}, word{reg(reg_name)}, bits{field(reg_name, field_name)}
  {}

  stm32::reg_ref   word;
  stm32::field_ref bits;

  auto line(std::string_view name) const -> periph::irq_line&
  {
    return irq(name);
  }

  auto state() -> probe_state&
  {
    return state_;
  }

  void arm(icount at)
  {
    ++state_.generation;
    state_.anchor = at.value;
    ctx_.events.schedule(at, 1, state_.generation);
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
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
};

class no_peers final : public periph::peer_directory
{
public:
  [[nodiscard]] auto find(std::string_view) const noexcept -> periph::model* override
  {
    return nullptr;
  }
  [[nodiscard]] auto all() const noexcept -> std::span<periph::model* const> override
  {
    return {};
  }
};

struct bench
{
  popcorn::part_description part = popcorn::testing::description_from("POPCORN_STM32_BASE_DESC");
  popcorn::peripheral_desc const& rcc = part.peripherals.front();
  periph::register_file           regs{rcc};
  core::clock                     clock{4'000'000};
  core::scheduler                 queue;
  core::event_port                events{queue, 0};
  popcorn::testing::fake_services services;
  no_peers                        peers;
  periph::irq_line                nmi;
  periph::model_context           ctx{
      .regs     = regs,
      .irqs     = {},
      .nmi      = nmi,
      .dma      = {},
      .events   = events,
      .clock    = clock,
      .services = services,
      .peers    = peers,
      .part     = part};

  auto probe(std::string_view reg, std::string_view field) -> probe_model
  {
    return probe_model{ctx, rcc, reg, field};
  }
};

auto error_of(bench& b, std::string_view reg, std::string_view field) -> popcorn::config_error
{
  try
  {
    probe_model const made{b.ctx, b.rcc, reg, field};
  }
  catch(popcorn::config_error const& e)
  {
    return e;
  }
  FAIL("no config_error");
  return popcorn::config_error{"", 0, "", ""};
}
}

TEST_CASE("registers and fields resolve by name from the description")
{
  bench       b;
  probe_model hsion{b.ctx, b.rcc, "RCC_CR", "HSION"};
  CHECK(hsion.word.offset == 0);
  CHECK(hsion.bits.offset == 0);
  CHECK(hsion.bits.lsb == 8);
  CHECK(hsion.bits.width == 1);
  CHECK(hsion.bits.mask() == 0x100u);
  probe_model sws{b.ctx, b.rcc, "RCC_CFGR1", "SWS"};
  CHECK(sws.word.offset == 0x1c);
  CHECK(sws.bits.lsb == 2);
  CHECK(sws.bits.width == 2);
  CHECK(sws.bits.mask() == 0xcu);
}

TEST_CASE("a model resolves a register by its bare name and by its SVD alias")
{
  bench b;
  auto& cr = *std::ranges::find(
      b.part.peripherals.front().registers, std::string_view{"RCC_CR"},
      &popcorn::register_desc::name);
  cr.name    = "CR";
  cr.aliases = std::vector<std::string>{"RCC_CR"};
  probe_model const bare{b.ctx, b.rcc, "CR", "HSION"};
  probe_model const aliased{b.ctx, b.rcc, "RCC_CR", "HSION"};
  CHECK(bare.word.offset == 0);
  CHECK(aliased.word.offset == 0);
  CHECK(aliased.bits.lsb == 8);
  CHECK(aliased.bits.mask() == bare.bits.mask());
  CHECK(error_of(b, "RCC_CTRL", "HSION").key() == "peripherals.RCC.registers.RCC_CTRL");
}

TEST_CASE("a register in the upper half of its word resolves to that half's bits")
{
  bench       b;
  probe_model half{b.ctx, b.rcc, "RCC_HALF", "LOW"};
  CHECK(half.word.offset == 0x20);
  CHECK(half.word.lsb == 16);
  CHECK(half.word.width == 16);
  CHECK(half.bits.offset == 0x20);
  CHECK(half.bits.lsb == 16);
  CHECK(half.bits.mask() == 0x000f'0000u);
  b.regs.poke(0x20, 0x0000'beefu);
  stm32::update(b.regs, half.bits, 0xa);
  CHECK(b.regs.peek(0x20) == 0x000a'beefu);
  CHECK(stm32::get(b.regs, half.bits) == 0xa);
}

TEST_CASE("a name absent from the description is a config error naming part, path and model")
{
  bench      b;
  auto const field = error_of(b, "RCC_CR", "NOPE");
  CHECK(field.file() == "STM32BASE");
  CHECK(field.key() == "peripherals.RCC.registers.RCC_CR.NOPE");
  CHECK(field.expected() == "a register the probe model needs");
  auto const reg = error_of(b, "RCC_NOPE", "HSION");
  CHECK(reg.key() == "peripherals.RCC.registers.RCC_NOPE");
}

TEST_CASE("field helpers read and replace only the field's bits")
{
  bench       b;
  probe_model sws{b.ctx, b.rcc, "RCC_CFGR1", "SWS"};
  b.regs.poke(0x1c, 0xffff'fff3u);
  CHECK(stm32::get(b.regs, sws.bits) == 0);
  stm32::update(b.regs, sws.bits, 3);
  CHECK(b.regs.peek(0x1c) == 0xffff'ffffu);
  stm32::update(b.regs, sws.bits, 0x6);
  CHECK(b.regs.peek(0x1c) == 0xffff'fffbu);
  CHECK(stm32::get(b.regs, sws.bits) == 2);
  constexpr stm32::field_ref whole{.offset = 0, .lsb = 0, .width = 32};
  static_assert(whole.mask() == 0xffff'ffffu);
  static_assert(whole.with(0x1234'5678u, 0xcafe'f00du) == 0xcafe'f00du);
}

TEST_CASE("state round-trips through BSON exactly and load schedules nothing")
{
  bench       b;
  probe_model model{b.ctx, b.rcc, "RCC_CR", "HSION"};
  model.arm(icount{400});
  model.arm(icount{900});
  model.state().level = 3;
  std::vector<std::byte> saved;
  model.save(saved);
  auto const                              expected = model.state();
  std::vector<popcorn::core::event> const before{
      b.queue.pending().begin(), b.queue.pending().end()};
  auto const next_seq = b.queue.next_seq();

  model.state() = {};
  model.load(saved);
  CHECK(model.state() == expected);
  CHECK(model.state().generation == 2);
  CHECK(model.state().anchor == 900);
  CHECK(
      std::vector<popcorn::core::event>{b.queue.pending().begin(), b.queue.pending().end()}
      == before);
  CHECK(b.queue.next_seq() == next_seq);
}

TEST_CASE("built-in registration runs once and factories cover only served peripherals")
{
  stm32::register_builtin_models();
  stm32::register_builtin_models();
  auto const         part = popcorn::testing::description_from("POPCORN_STM32_BASE_DESC");
  stm32::board const board{};
  auto const         made = stm32::factories(board, part);
  CHECK(made.contains("RCC"));
  for(auto const& [name, factory] : made)
  {
    CHECK(factory);
    CHECK(
        std::ranges::find(part.peripherals, name, &popcorn::peripheral_desc::name)
        != part.peripherals.end());
  }
}

namespace
{
class table_probe final : public stm32::dispatch_model<table_probe, probe_state>
{
public:
  table_probe(
      periph::model_context&                           ctx,
      popcorn::peripheral_desc const&                  desc,
      std::span<stm32::write_entry<table_probe> const> table)
      : dispatch_model{ctx, desc, "table", table}
  {}

  std::uint32_t last{};

  void written(std::uint32_t, std::uint32_t now)
  {
    last = now;
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  void on_event(std::uint32_t, std::uint64_t) override
  {}
};
}

TEST_CASE("a claimed register without fields is writable in full unless it is read-only")
{
  bench                                                b;
  std::array<stm32::write_entry<table_probe>, 2> const table{
      {
       {"RCC_RAW", &table_probe::written},
       {"RCC_WO", &table_probe::written},
       }
  };
  table_probe model{b.ctx, b.rcc, table};
  CHECK(model.claims() == std::vector<std::string>{"RCC_RAW", "RCC_WO"});
  REQUIRE(model.on_write(b.regs, 0x40, 4, 0, 0xdead'beefu, {.pc = 0, .now = icount{0}}));
  CHECK(model.last == 0xdead'beefu);
  CHECK(model.on_read(b.regs, 0x40, 4, {.pc = 0, .now = icount{0}}) == 0xdead'beefu);
  REQUIRE(model.on_write(b.regs, 0x48, 4, 0, 0x1ffu, {.pc = 0, .now = icount{0}}));
  CHECK(b.regs.peek(0x48) == 0x101u);
  CHECK(model.on_read(b.regs, 0x48, 4, {.pc = 0, .now = icount{0}}) == 0x100u);
  std::array<stm32::write_entry<table_probe>, 1> const read_only{
      {{"RCC_RO", &table_probe::written}}};
  try
  {
    table_probe const refused{b.ctx, b.rcc, read_only};
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& e)
  {
    CHECK(e.key() == "peripherals.RCC.registers.RCC_RO.fields");
  }
}

TEST_CASE("an IRQ resolves by its description name, a missing one is a config error")
{
  bench                           b;
  probe_model                     model{b.ctx, b.rcc, "RCC_CR", "HSION"};
  std::array<periph::irq_line, 1> lines{};
  b.ctx.irqs = lines;
  CHECK(&model.line("RCC_IRQn") == &lines[0]);
  try
  {
    std::ignore = model.line("NOPE_IRQn");
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& e)
  {
    CHECK(e.key() == "peripherals.RCC.irqs.NOPE_IRQn");
    CHECK(e.expected() == "an IRQ the probe model raises");
  }
}
