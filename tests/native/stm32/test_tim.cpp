#include "dma.hpp"
#include "stm32_fixture.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <meta>
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
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t ms = 1'000'000'000;
constexpr std::uint64_t us = 1'000'000;

auto entered(
    std::string_view      entry,
    core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{u575(), "tim_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("tim_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("tim_ll", entry));
  return f;
}

struct entries
{
  std::vector<std::uint64_t> at;
  core::intercept_handle     hook;

  entries(stm32_fixture& f, std::string_view handler)
      : hook{f.target->add_intercept(sym("tim_ll", handler), [this](core::intercept_call const& c) {
          at.push_back(c.target().virtual_clock().time().ps);
          return core::intercept_result::proceed();
        })}
  {}

  [[nodiscard]] auto gaps() const -> std::vector<std::uint64_t>
  {
    std::vector<std::uint64_t> out;
    for(std::size_t index = 1; index < at.size(); ++index)
    {
      out.push_back(at[index] - at[index - 1]);
    }
    return out;
  }
};

auto finish(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  auto const stopped = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  return f.log();
}

struct bus
{
  stm32_fixture& f;

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    return desc.base + found->offset;
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(at(peripheral, reg), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(peripheral, reg), 4, value).has_value());
  }

  [[nodiscard]] auto
      bits(std::string_view peripheral, std::string_view reg, std::string_view name) const
      -> std::uint32_t
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    auto const field = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(field != found->fields.end());
    return (read(peripheral, reg) >> field->lsb) & ((std::uint32_t{1} << field->width) - 1u);
  }

  void
      set(std::string_view peripheral,
          std::string_view reg,
          std::string_view name,
          std::uint32_t    value) const
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    auto const field = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(field != found->fields.end());
    auto const mask = ((std::uint32_t{1} << field->width) - 1u) << field->lsb;
    write(peripheral, reg, (read(peripheral, reg) & ~mask) | ((value << field->lsb) & mask));
  }
};

class recorder final : public periph::model, public stm32::dma_controller
{
public:
  explicit recorder(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context&                      context;
  std::vector<std::pair<std::uint32_t, bool>> levels;

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
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
  void on_request(std::uint32_t request, bool level) override
  {
    levels.emplace_back(request, level);
  }
  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    if(signal == "tim1_cc1_dma")
    {
      return 1;
    }
    if(signal == "tim1_upd_dma")
    {
      return 2;
    }
    return std::nullopt;
  }
};

auto with_recorder() -> core::machine_options
{
  auto options = popcorn::testing::stm32_options();
  options.models.emplace("GPDMA1", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
    return std::make_shared<recorder>(ctx);
  });
  return options;
}

struct direct
{
  stm32_fixture f;
  bus           b{f};
  std::uint64_t start{};

  explicit direct(core::machine_options options = popcorn::testing::stm32_options())
      : f{entered("idle", std::move(options))}
  {
    b.set("RCC", "RCC_APB2ENR", "TIM1EN", 1);
  }

  void cr1(std::uint32_t value) const
  {
    b.write("TIM1", "TIM1_CR1", value);
  }

  void begin()
  {
    b.write("TIM1", "TIM1_EGR", 1);
    b.write("TIM1", "TIM1_SR", 0);
    b.set("TIM1", "TIM1_CR1", "CEN", 1);
    start = f.target->virtual_clock().time().ps;
  }

  void at(std::uint64_t ticks, std::uint64_t tick_ps = 250'000) const
  {
    auto const until = vtime{start + ticks * tick_ps};
    auto const stopped =
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = until});
    REQUIRE(stopped.reason == stop_reason::time);
    REQUIRE(f.target->virtual_clock().time() == until);
  }

  [[nodiscard]] auto count() const -> std::uint32_t
  {
    return b.read("TIM1", "TIM1_CNT") & 0xffffu;
  }

  [[nodiscard]] auto flag(std::string_view name) const -> std::uint32_t
  {
    return b.bits("TIM1", "TIM1_SR", name);
  }
};

auto pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  auto const word = f.target->mem().read(0xE000'E200 + 4 * (irq / 32), 4);
  REQUIRE(word.has_value());
  return ((*word >> (irq % 32)) & 1u) != 0;
}
}

TEST_CASE("TIM16 at 1 kHz on 160 MHz raises 100 update IRQs exactly 1 ms apart")
{
  std::vector<std::uint64_t> first;
  std::vector<std::uint32_t> first_block;
  for(int run = 0; run < 2; ++run)
  {
    auto    f = entered("tim16_updates");
    entries irq{f, "TIM16_IRQHandler"};
    CHECK(finish(f) == std::vector<std::uint32_t>{100});
    REQUIRE(irq.at.size() == 100);
    CHECK(std::ranges::all_of(irq.gaps(), [](std::uint64_t gap) { return gap == ms; }));
    auto const&                regs = f.target->peripheral("TIM16");
    std::vector<std::uint32_t> block;
    for(std::uint32_t offset = 0; offset < regs.description().size; offset += 4)
    {
      block.push_back(regs.peek(offset));
    }
    if(run == 0)
    {
      first       = irq.at;
      first_block = block;
    }
    else
    {
      CHECK(irq.at == first);
      CHECK(block == first_block);
    }
  }
}

TEST_CASE("TIM16 CNT follows the virtual time between updates")
{
  auto       f = entered("tim16_updates");
  entries    irq{f, "TIM16_IRQHandler"};
  bus        b{f};
  auto const until = [&](std::uint64_t ps) {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  };
  until(f.target->virtual_clock().time().ps + 50 * ms + ms / 2);
  REQUIRE(irq.at.size() >= 50);
  auto const entry = irq.at.back();
  until(entry + 300 * us + us / 2);
  CHECK((b.read("TIM16", "CNT") & 0xffffu) == 300);
  until(entry + 400 * us + us / 2);
  CHECK((b.read("TIM16", "CNT") & 0xffffu) == 400);
  until(entry + 999 * us + us / 2);
  CHECK((b.read("TIM16", "CNT") & 0xffffu) == 999);
}

TEST_CASE("a snapshot with a pending update restores to the same IRQ times and log")
{
  auto       f = entered("tim16_updates");
  entries    irq{f, "TIM16_IRQHandler"};
  auto const middle = vtime{f.target->virtual_clock().time().ps + 50 * ms + ms / 2};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = middle}).reason
      == stop_reason::time);
  auto const                       saved  = f.target->snapshot();
  auto const                       before = irq.at.size();
  auto const                       log    = finish(f);
  std::vector<std::uint64_t> const after{
      irq.at.begin() + static_cast<std::ptrdiff_t>(before), irq.at.end()};
  REQUIRE(after.size() == 100 - before);

  f.target->restore(saved);
  irq.at.resize(before);
  CHECK(finish(f) == log);
  CHECK(
      std::vector<std::uint64_t>{irq.at.begin() + static_cast<std::ptrdiff_t>(before), irq.at.end()}
      == after);

  auto    other = entered("tim16_updates");
  entries again{other, "TIM16_IRQHandler"};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(finish(other) == log);
  CHECK(again.at == after);
}

TEST_CASE("one-pulse mode stops the counter at its first update")
{
  auto f = entered("tim16_one_pulse");
  CHECK(finish(f) == std::vector<std::uint32_t>{1, 0, 0});
}

TEST_CASE("TIM2 raises CC1IF once, when the 32-bit counter reaches CCR1")
{
  auto                       f = entered("tim2_compare");
  bus                        b{f};
  std::vector<std::uint32_t> at_entry;
  auto const                 hook =
      f.target->add_intercept(sym("tim_ll", "TIM2_IRQHandler"), [&](core::intercept_call const&) {
        at_entry.push_back(b.read("TIM2", "CNT"));
        return core::intercept_result::proceed();
      });
  auto const log = finish(f);
  REQUIRE(log.size() == 2);
  REQUIRE(at_entry.size() == 1);
  CHECK(at_entry[0] >= 40'000);
  CHECK(at_entry[0] < 40'000 + 32);
  CHECK(log[0] > at_entry[0]);
  CHECK(log[1] == 1);
}

TEST_CASE("PSC and a preloaded ARR written mid-run apply at the next update, not before")
{
  auto    f = entered("tim6_preload");
  entries irq{f, "TIM6_IRQHandler"};
  CHECK(finish(f) == std::vector<std::uint32_t>{10});
  CHECK(irq.gaps() == std::vector<std::uint64_t>{ms, ms, ms, 2 * ms, 2 * ms, 2 * ms, ms, ms, ms});
}

TEST_CASE("a clock change through RCC mid-count continues the count at the new rate")
{
  auto    f = entered("tim6_clock");
  entries irq{f, "TIM6_IRQHandler"};
  CHECK(finish(f) == std::vector<std::uint32_t>{5});
  auto const gaps = irq.gaps();
  REQUIRE(gaps.size() == 4);
  CHECK(gaps[0] == ms);
  CHECK(gaps[1] > 3 * ms / 2 - 10 * us);
  CHECK(gaps[1] < 3 * ms / 2 + 10 * us);
  CHECK(gaps[2] == 2 * ms);
  CHECK(gaps[3] == 2 * ms);
}

TEST_CASE("the TIM6 update DMA request moves one word per update")
{
  auto f = entered("tim6_dma");
  CHECK(finish(f) == std::vector<std::uint32_t>{4, 4});
}

TEST_CASE("polling UIF with no interrupt enabled waits on a timer event, not a spin")
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 10'000;
  auto       f           = entered("tim6_poll", std::move(options));
  auto const stopped     = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{2});
}

TEST_CASE("up and down counting wrap at ARR with the prescaler")
{
  direct t;
  t.b.write("TIM1", "TIM1_PSC", 3);
  t.b.write("TIM1", "TIM1_ARR", 99);
  t.b.write("TIM1", "TIM1_CCR1", 50);
  t.begin();
  t.at(4 * 50);
  CHECK(t.count() == 50);
  CHECK(t.flag("UIF") == 0);
  CHECK(t.flag("CC1IF") == 1);
  t.b.write("TIM1", "TIM1_SR", 0);
  t.at(4 * 100 - 1);
  CHECK(t.count() == 99);
  CHECK(t.flag("UIF") == 0);
  CHECK(t.flag("CC1IF") == 0);
  t.at(4 * 100);
  CHECK(t.count() == 0);
  CHECK(t.flag("UIF") == 1);

  direct down;
  down.b.write("TIM1", "TIM1_PSC", 3);
  down.b.write("TIM1", "TIM1_ARR", 99);
  down.cr1(1u << 4);
  down.begin();
  CHECK(down.count() == 99);
  down.at(4 * 30);
  CHECK(down.count() == 69);
  down.at(4 * 100 - 1);
  CHECK(down.count() == 0);
  CHECK(down.flag("UIF") == 0);
  down.at(4 * 100);
  CHECK(down.count() == 99);
  CHECK(down.flag("UIF") == 1);
}

TEST_CASE("center-aligned mode 1 counts up and down and flags compares only counting down")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 10);
  t.b.write("TIM1", "TIM1_CCR1", 4);
  t.cr1(1u << 5);
  t.begin();
  t.at(5);
  CHECK(t.count() == 5);
  CHECK(t.b.bits("TIM1", "TIM1_CR1", "DIR") == 0);
  CHECK(t.flag("CC1IF") == 0);
  t.at(10);
  CHECK(t.count() == 10);
  CHECK(t.flag("UIF") == 1);
  t.b.write("TIM1", "TIM1_SR", 0);
  t.at(13);
  CHECK(t.count() == 7);
  CHECK(t.b.bits("TIM1", "TIM1_CR1", "DIR") == 1);
  t.at(15);
  CHECK(t.flag("CC1IF") == 0);
  t.at(16);
  CHECK(t.flag("CC1IF") == 1);
  CHECK(t.flag("UIF") == 0);
  t.at(20);
  CHECK(t.count() == 0);
  CHECK(t.flag("UIF") == 1);
  CHECK(t.b.bits("TIM1", "TIM1_CR1", "DIR") == 0);
}

TEST_CASE("the repetition counter spaces updates by RCR + 1 overflows")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.b.write("TIM1", "TIM1_RCR", 2);
  t.begin();
  t.at(29);
  CHECK(t.flag("UIF") == 0);
  t.at(30);
  CHECK(t.flag("UIF") == 1);
  t.b.write("TIM1", "TIM1_SR", 0);
  t.at(59);
  CHECK(t.flag("UIF") == 0);
  t.at(60);
  CHECK(t.flag("UIF") == 1);
}

TEST_CASE("UDIS suppresses updates, URS keeps UG from setting UIF, a null ARR blocks the counter")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.b.write("TIM1", "TIM1_CR1", 1u << 2);
  t.b.write("TIM1", "TIM1_EGR", 1);
  CHECK(t.flag("UIF") == 0);
  t.b.write("TIM1", "TIM1_CR1", 0);
  t.b.write("TIM1", "TIM1_EGR", 1);
  CHECK(t.flag("UIF") == 1);
  t.b.write("TIM1", "TIM1_SR", 0);
  t.b.write("TIM1", "TIM1_CR1", 1u << 1);
  t.b.set("TIM1", "TIM1_CR1", "CEN", 1);
  t.start = t.f.target->virtual_clock().time().ps;
  t.at(25);
  CHECK(t.count() == 5);
  CHECK(t.flag("UIF") == 0);

  direct blocked;
  blocked.b.write("TIM1", "TIM1_ARR", 0);
  blocked.begin();
  blocked.at(100);
  CHECK(blocked.count() == 0);
  CHECK(blocked.flag("UIF") == 0);
}

TEST_CASE("TIM1 raises its update, compare and break IRQs on their own lines")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.b.write("TIM1", "TIM1_CCR1", 3);
  t.begin();
  t.b.write("TIM1", "TIM1_DIER", 0b1000'0011);
  CHECK_FALSE(pending(t.f, 42));
  t.at(4);
  CHECK(pending(t.f, 44));
  CHECK_FALSE(pending(t.f, 42));
  t.at(10);
  CHECK(pending(t.f, 42));
  CHECK_FALSE(pending(t.f, 41));
  t.b.write("TIM1", "TIM1_EGR", 1u << 7);
  CHECK(t.flag("BIF") == 1);
  CHECK(pending(t.f, 41));
  t.b.write("TIM1", "TIM1_SR", ~std::uint32_t{0b10});
  CHECK(t.flag("UIF") == 1);
  CHECK(t.flag("CC1IF") == 0);
  CHECK(t.flag("COMIF") == 0);
  CHECK(t.flag("BIF") == 1);
}

TEST_CASE("an ARR below the count lets the counter run to its maximum and wrap without an update")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 99);
  t.begin();
  t.at(50);
  t.b.write("TIM1", "TIM1_ARR", 20);
  t.at(0x1'0000);
  CHECK(t.count() == 0);
  CHECK(t.flag("UIF") == 0);
  t.at(0x1'0000 + 20);
  CHECK(t.flag("UIF") == 0);
  t.at(0x1'0000 + 21);
  CHECK(t.count() == 0);
  CHECK(t.flag("UIF") == 1);
}

TEST_CASE("a timer clock whose period is not a whole number of picoseconds updates on time")
{
  direct t;
  t.b.set("RCC", "RCC_ICSCR1", "MSISRANGE", 1);
  t.b.set("RCC", "RCC_ICSCR1", "MSIRGSEL", 1);
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.begin();
  t.b.write("TIM1", "TIM1_DIER", 1);
  constexpr std::uint64_t hz = 24'000'000;
  for(std::uint64_t update = 1; update <= 5; ++update)
  {
    auto const due     = (10 * update * 1'000'000'000'000 + hz - 1) / hz;
    auto const stopped = t.f.run_to_stop(
        {.max_instructions = icount{10'000'000}, .until_time = vtime{t.start + due}});
    REQUIRE(stopped.reason == stop_reason::time);
    CHECK(t.flag("UIF") == 1);
    t.b.write("TIM1", "TIM1_SR", 0);
  }
  CHECK(pending(t.f, 42));
}

TEST_CASE("a long idle stretch jumps to the same count, flags and next updates as small steps")
{
  std::uint32_t cr1       = 1u << 7;
  bool          no_update = false;
  SUBCASE("edge up")
  {}
  SUBCASE("edge down")
  {
    cr1 |= 1u << 4;
  }
  SUBCASE("edge up with UDIS")
  {
    no_update = true;
  }
  SUBCASE("center-aligned mode 1")
  {
    cr1 |= 1u << 5;
  }
  SUBCASE("center-aligned mode 2")
  {
    cr1 |= 2u << 5;
  }
  SUBCASE("center-aligned mode 3")
  {
    cr1 |= 3u << 5;
  }
  auto const configure = [&](direct& t) {
    t.b.write("TIM1", "TIM1_PSC", 2);
    t.b.write("TIM1", "TIM1_ARR", 13);
    t.b.write("TIM1", "TIM1_RCR", 3);
    t.b.write("TIM1", "TIM1_CCR1", 5);
    t.b.write("TIM1", "TIM1_CCR2", 13);
    t.b.write("TIM1", "TIM1_CCR3", 0);
    t.b.write("TIM1", "TIM1_CCR4", 20);
    t.b.write("TIM1", "TIM1_CCMR1_ALTERNATE1", 1u << 3);
    t.cr1(cr1);
    t.begin();
    t.b.write("TIM1", "TIM1_PSC", 1);
    t.b.write("TIM1", "TIM1_ARR", 11);
    t.b.write("TIM1", "TIM1_CCR1", 7);
    if(no_update)
    {
      t.cr1(cr1 | 3u);
    }
  };
  direct stepped;
  configure(stepped);
  direct jumped;
  configure(jumped);
  constexpr std::uint64_t far = 3 * 10'007;
  for(std::uint64_t tick = 7; tick < far; tick += 7)
  {
    stepped.at(tick);
    (void)stepped.count();
  }
  stepped.at(far);
  jumped.at(far);
  CHECK(stepped.count() == jumped.count());
  CHECK(stepped.b.read("TIM1", "TIM1_SR") == jumped.b.read("TIM1", "TIM1_SR"));
  CHECK(stepped.b.read("TIM1", "TIM1_CR1") == jumped.b.read("TIM1", "TIM1_CR1"));
  std::vector<std::uint32_t> seen_stepped;
  std::vector<std::uint32_t> seen_jumped;
  stepped.b.write("TIM1", "TIM1_SR", 0);
  jumped.b.write("TIM1", "TIM1_SR", 0);
  for(std::uint64_t tick = far + 1; tick < far + 200; ++tick)
  {
    stepped.at(tick);
    jumped.at(tick);
    seen_stepped.push_back(stepped.b.read("TIM1", "TIM1_SR") | (stepped.count() << 16));
    seen_jumped.push_back(jumped.b.read("TIM1", "TIM1_SR") | (jumped.count() << 16));
    stepped.b.write("TIM1", "TIM1_SR", 0);
    jumped.b.write("TIM1", "TIM1_SR", 0);
  }
  CHECK(seen_stepped == seen_jumped);
  auto const updates =
      std::ranges::count(seen_jumped, 1u, [](std::uint32_t sr) { return sr & 1u; });
  CHECK((updates > 0) != no_update);
}

TEST_CASE("time spent with a null ARR is not credited once ARR is written")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 0);
  t.begin();
  t.at(105);
  CHECK(t.count() == 0);
  t.b.write("TIM1", "TIM1_ARR", 50);
  t.at(107);
  CHECK(t.count() == 2);
}

TEST_CASE("a clock change mid-tick keeps the prescaler progress")
{
  direct t;
  t.b.write("TIM1", "TIM1_PSC", 3);
  t.b.write("TIM1", "TIM1_ARR", 99);
  t.begin();
  t.at(10);
  CHECK(t.count() == 2);
  t.b.set("RCC", "RCC_CFGR2", "HPRE", 8);
  t.at(12);
  CHECK(t.count() == 2);
  t.at(14);
  CHECK(t.count() == 3);
  t.at(16);
  t.b.set("RCC", "RCC_APB2ENR", "TIM1EN", 0);
  t.at(18);
  t.b.set("RCC", "RCC_APB2ENR", "TIM1EN", 1);
  t.at(22);
  CHECK(t.count() == 3);
  t.at(24);
  CHECK(t.count() == 4);
}

TEST_CASE("one-pulse mode with a repetition counter stops at the update after RCR + 1 overflows")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.b.write("TIM1", "TIM1_CR1", 1u << 3);
  t.b.write("TIM1", "TIM1_RCR", 2);
  t.begin();
  t.at(29);
  CHECK(t.b.bits("TIM1", "TIM1_CR1", "CEN") == 1);
  t.at(30);
  CHECK(t.b.bits("TIM1", "TIM1_CR1", "CEN") == 0);
  CHECK(t.count() == 0);
  t.at(40);
  CHECK(t.count() == 0);
}

TEST_CASE("a center-aligned count written equal to ARR turns down at the next tick")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 10);
  t.cr1(1u << 5);
  t.begin();
  t.b.write("TIM1", "TIM1_CNT", 10);
  t.at(3);
  CHECK(t.count() == 7);
  CHECK(t.b.bits("TIM1", "TIM1_CR1", "DIR") == 1);
}

TEST_CASE("UIFREMAP copies UIF into CNT bit 31")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.cr1(1u << 11);
  t.begin();
  CHECK((t.b.read("TIM1", "TIM1_CNT") >> 31) == 0);
  t.at(10);
  CHECK((t.b.read("TIM1", "TIM1_CNT") >> 31) == 1);
  t.b.write("TIM1", "TIM1_SR", 0);
  CHECK((t.b.read("TIM1", "TIM1_CNT") >> 31) == 0);
}

TEST_CASE("CCxG on an input channel captures the count and sets CCxOF on the second capture")
{
  direct t;
  t.b.write("TIM1", "TIM1_ARR", 99);
  t.b.write("TIM1", "TIM1_CCMR1", 1);
  t.begin();
  t.at(17);
  t.b.write("TIM1", "TIM1_EGR", 1u << 1);
  CHECK(t.b.read("TIM1", "TIM1_CCR1") == 17);
  CHECK(t.flag("CC1IF") == 1);
  CHECK(t.flag("CC1OF") == 0);
  t.at(40);
  t.b.write("TIM1", "TIM1_EGR", 1u << 1);
  CHECK(t.b.read("TIM1", "TIM1_CCR1") == 40);
  CHECK(t.flag("CC1OF") == 1);
}

TEST_CASE("CCxDE and UDE raise their DMA requests, a DMA access to the timer releases them")
{
  direct      t{with_recorder()};
  auto* const dma = t.f.model_of<recorder>("GPDMA1");
  REQUIRE(dma != nullptr);
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.b.write("TIM1", "TIM1_CCR1", 3);
  t.begin();
  t.b.write("TIM1", "TIM1_DIER", (1u << 9) | (1u << 8));
  t.at(2);
  CHECK(dma->levels.empty());
  t.at(3);
  CHECK(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {1, true}
  });
  t.at(13);
  CHECK(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {1, true},
          {2, true}
  });
  auto const at = t.b.at("TIM1", "TIM1_CCR1");
  REQUIRE(dma->context.services.read(at, 4, popcorn::initiator::dma).has_value());
  CHECK(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {1, true },
          {2, true },
          {2, false},
          {1, false}
  });
}

TEST_CASE("selecting DMA burst mode logs once")
{
  direct                        t;
  std::vector<core::log_record> logged;
  auto const                    sink =
      t.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  t.b.write("TIM1", "TIM1_DCR", 3u << 8);
  t.b.write("TIM1", "TIM1_DCR", 1u << 8);
  auto const warnings =
      std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind);
  CHECK(warnings == 1);
}

namespace
{
class trigger_probe final : public periph::model, public stm32::trigger_sink
{
public:
  explicit trigger_probe(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context&     context;
  std::string                watched{"tim15_trgo"};
  bool                       listening{};
  std::vector<std::uint64_t> pulses;
  std::vector<std::uint64_t> falls;

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
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override
  {
    return listening and signal == watched;
  }
  void triggered(std::string_view, stm32::trigger_edge edge, std::uint64_t at_ps) override
  {
    (edge == stm32::trigger_edge::rising ? pulses : falls).push_back(at_ps);
  }

  void listen(bool on)
  {
    static_cast<void>(stm32::interest_changed(context.peers, watched));
    listening = on;
    CHECK(stm32::interest_changed(context.peers, watched));
  }
};

auto with_probe() -> core::machine_options
{
  auto options = popcorn::testing::stm32_options();
  options.models.emplace("DAC1", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
    return std::make_shared<trigger_probe>(ctx);
  });
  return options;
}

constexpr std::uint32_t master_reset   = 0;
constexpr std::uint32_t master_enable  = 1;
constexpr std::uint32_t master_update  = 2;
constexpr std::uint32_t master_compare = 3;
constexpr std::uint64_t tim15_period   = 500 * ms;
constexpr std::uint64_t tim15_clock_ps = 250'000;

auto later(std::vector<std::uint64_t> times, std::uint64_t by) -> std::vector<std::uint64_t>
{
  for(auto& at : times)
  {
    at += by;
  }
  return times;
}

struct trgo
{
  stm32_fixture  f;
  bus            b{f};
  trigger_probe* sink{};
  std::uint64_t  start{};

  explicit trgo(std::uint32_t mode) : f{entered("idle", with_probe())}
  {
    sink = f.model_of<trigger_probe>("DAC1");
    REQUIRE(sink != nullptr);
    b.set("RCC", "RCC_APB2ENR", "TIM15EN", 1);
    b.write("TIM15", "PSC", 124);
    b.write("TIM15", "ARR", 15'999);
    b.write("TIM15", "EGR", 1);
    b.write("TIM15", "SR", 0);
    b.set("TIM15", "CR2", "MMS", mode);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  void begin()
  {
    start = now();
    b.set("TIM15", "CR1", "CEN", 1);
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto updates(std::uint64_t first, std::uint64_t last) const
      -> std::vector<std::uint64_t>
  {
    std::vector<std::uint64_t> out;
    for(auto k = first; k <= last; ++k)
    {
      out.push_back(start + k * tim15_period);
    }
    return out;
  }
};
}

TEST_CASE("TIM15 MMS 010 pulses a listening sink at each update's own time, UIF left set")
{
  trgo t{master_update};
  t.sink->listen(true);
  t.begin();
  t.run_until(t.start + 2'600 * ms);
  CHECK(t.sink->pulses == t.updates(1, 5));
  CHECK(t.sink->falls == later(t.updates(1, 5), tim15_clock_ps));
  CHECK(t.b.bits("TIM15", "SR", "UIF") == 1);
}

TEST_CASE("TIM15 MMS 010 with no sink listening schedules nothing after its first update")
{
  trgo       t{master_update};
  auto const idle = t.f.target->pending_events();
  t.begin();
  t.run_until(t.start + 600 * ms);
  CHECK(t.b.bits("TIM15", "SR", "UIF") == 1);
  CHECK(t.f.target->pending_events() == idle);
  t.sink->listen(true);
  CHECK(t.f.target->pending_events() == idle + 1);
  t.run_until(t.start + 1'100 * ms);
  CHECK(t.sink->pulses == t.updates(2, 2));
}

TEST_CASE("a sink listening mid-period gets the next update, no earlier one is replayed")
{
  trgo t{master_update};
  t.begin();
  t.run_until(t.start + 1'250 * ms);
  CHECK(t.b.bits("TIM15", "SR", "UIF") == 1);
  t.sink->listen(true);
  t.run_until(t.start + 2'600 * ms);
  CHECK(t.sink->pulses == t.updates(3, 5));
  t.sink->listen(false);
  t.run_until(t.start + 3'600 * ms);
  CHECK(t.sink->pulses == t.updates(3, 5));
}

TEST_CASE("a TIM15 read 3 us after an update it did not schedule pulses once, at the update")
{
  trgo t{master_update};
  t.begin();
  t.run_until(t.start + 1'250 * ms);
  static_cast<void>(t.b.read("TIM15", "CNT"));
  t.sink->listening = true;
  t.run_until(t.start + 3 * tim15_period + 3 * us);
  CHECK(t.sink->pulses.empty());
  static_cast<void>(t.b.read("TIM15", "CNT"));
  CHECK(t.sink->pulses == t.updates(3, 3));
  t.run_until(t.start + 2'100 * ms);
  CHECK(t.sink->pulses == t.updates(3, 4));
}

TEST_CASE("MMS 000 pulses at each UG write, MMS 001 at the CEN rising edge")
{
  SUBCASE("reset")
  {
    trgo t{master_reset};
    t.sink->listen(true);
    t.begin();
    t.run_until(t.start + 700 * ms);
    CHECK(t.sink->pulses.empty());
    t.b.write("TIM15", "EGR", 1);
    t.run_until(t.start + 900 * ms);
    t.b.write("TIM15", "EGR", 1);
    t.run_until(t.start + 2'000 * ms);
    CHECK(t.sink->pulses == std::vector<std::uint64_t>{t.start + 700 * ms, t.start + 900 * ms});
  }
  SUBCASE("enable")
  {
    trgo t{master_enable};
    t.sink->listen(true);
    t.begin();
    t.run_until(t.start + 1'200 * ms);
    CHECK(t.sink->pulses == std::vector<std::uint64_t>{t.start});
    CHECK(t.sink->falls.empty());
    t.b.set("TIM15", "CR1", "CEN", 0);
    CHECK(t.sink->falls == std::vector<std::uint64_t>{t.start + 1'200 * ms});
    t.b.set("TIM15", "CR1", "OPM", 1);
    t.b.set("TIM15", "CR1", "CEN", 1);
    t.run_until(t.start + 2'000 * ms);
    CHECK(t.b.bits("TIM15", "CR1", "CEN") == 0);
    auto const restart = t.start + 1'200 * ms;
    CHECK(t.sink->pulses == std::vector<std::uint64_t>{t.start, restart});
    CHECK(t.sink->falls == std::vector<std::uint64_t>{restart, t.start + 1'500 * ms});
  }
}

TEST_CASE("MMS 011 pulses at each CC1 match, CC1IF left set")
{
  trgo t{master_compare};
  t.b.write("TIM15", "CCR1", 4'000);
  t.sink->listen(true);
  t.begin();
  t.run_until(t.start + 1'200 * ms);
  CHECK(
      t.sink->pulses
      == std::vector<std::uint64_t>{t.start + 125 * ms, t.start + 625 * ms, t.start + 1'125 * ms});
  CHECK(t.b.bits("TIM15", "SR", "CC1IF") == 1);
}

TEST_CASE("MMS 100 logs one warning and pulses nothing")
{
  trgo                          t{master_reset};
  std::vector<core::log_record> logged;
  auto const                    log_sink =
      t.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  t.b.set("TIM15", "CR2", "MMS", 4);
  t.b.set("TIM15", "CR2", "MMS", 5);
  CHECK(t.b.bits("TIM15", "CR2", "MMS") == 5);
  t.sink->listen(true);
  t.begin();
  t.b.write("TIM15", "EGR", 1);
  t.run_until(t.start + 1'200 * ms);
  CHECK(t.sink->pulses.empty());
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].peripheral == "TIM15");
  CHECK(logged[0].reg == "CR2");
}

TEST_CASE("a snapshot between two pulses restores to the same pulse times in a fresh machine")
{
  trgo t{master_update};
  t.sink->listen(true);
  t.begin();
  t.run_until(t.start + 1'250 * ms);
  REQUIRE(t.sink->pulses == t.updates(1, 2));
  auto const saved = t.f.target->snapshot();
  t.run_until(t.start + 2'600 * ms);
  CHECK(t.sink->pulses == t.updates(1, 5));

  trgo other{master_update};
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.sink->listening = true;
  other.run_until(t.start + 2'600 * ms);
  CHECK(other.sink->pulses == t.updates(3, 5));
}

TEST_CASE("MMS 010 pulses on UG with URS set, UDIS suppresses the UG and overflow pulses")
{
  trgo t{master_update};
  t.b.set("TIM15", "CR1", "URS", 1);
  t.sink->listen(true);
  t.begin();
  t.run_until(t.start + 100 * ms);
  t.b.write("TIM15", "EGR", 1);
  CHECK(t.b.bits("TIM15", "SR", "UIF") == 0);
  t.run_until(t.start + 700 * ms);
  CHECK(
      t.sink->pulses
      == std::vector<std::uint64_t>{t.start + 100 * ms, t.start + 100 * ms + tim15_period});
  t.b.set("TIM15", "CR1", "UDIS", 1);
  t.b.write("TIM15", "EGR", 1);
  t.run_until(t.start + 2'000 * ms);
  CHECK(t.sink->pulses.size() == 2);
}

namespace
{
constexpr std::uint64_t tick_ps = 250'000;

constexpr auto smcr(std::uint32_t mode, std::uint32_t select) -> std::uint32_t
{
  return (mode & 7u) | ((mode >> 3) << 16) | ((select & 7u) << 4) | ((select >> 3) << 20);
}

constexpr std::uint32_t sms_reset         = 0b0100;
constexpr std::uint32_t sms_gated         = 0b0101;
constexpr std::uint32_t sms_trigger       = 0b0110;
constexpr std::uint32_t sms_external      = 0b0111;
constexpr std::uint32_t sms_reset_trigger = 0b1000;
constexpr std::uint32_t sms_gated_reset   = 0b1001;
constexpr std::uint32_t ts_itr0           = 0;
constexpr std::uint32_t ts_itr1           = 1;
constexpr std::uint32_t ts_itr2           = 2;
constexpr std::uint32_t ts_itr3           = 3;
constexpr std::uint32_t ts_itr7           = 11;

struct slaves
{
  stm32_fixture                 f;
  bus                           b{f};
  std::uint64_t                 start{};
  std::vector<core::log_record> logged;
  core::hook_handle             sink;

  slaves()
      : f{entered("idle")}, sink{f.target->add_log_sink([this](core::log_record const& record) {
          logged.push_back(record);
        })}
  {
    for(auto const* const bit : {"TIM1EN", "TIM15EN"})
    {
      b.set("RCC", "RCC_APB2ENR", bit, 1);
    }
    for(auto const* const bit : {"TIM2EN", "TIM3EN", "TIM4EN", "TIM5EN"})
    {
      b.set("RCC", "RCC_APB1ENR1", bit, 1);
    }
    start = f.target->virtual_clock().time().ps;
  }

  [[nodiscard]] static auto reg(std::string_view tim, std::string_view name) -> std::string
  {
    return tim == "TIM1" ? std::string{"TIM1_"} + std::string{name} : std::string{name};
  }

  void write(std::string_view tim, std::string_view name, std::uint32_t value) const
  {
    b.write(tim, reg(tim, name), value);
  }

  [[nodiscard]] auto read(std::string_view tim, std::string_view name) const -> std::uint32_t
  {
    return b.read(tim, reg(tim, name));
  }

  [[nodiscard]] auto bits(std::string_view tim, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    return b.bits(tim, reg(tim, name), field);
  }

  void base(std::string_view tim, std::uint32_t prescaler, std::uint32_t reload) const
  {
    write(tim, "PSC", prescaler);
    write(tim, "ARR", reload);
    write(tim, "EGR", 1);
    write(tim, "SR", 0);
  }

  void master(std::string_view tim, std::uint32_t mode) const
  {
    write(tim, "CR2", mode << 4);
  }

  void enable(std::string_view tim, bool on = true) const
  {
    write(tim, "CR1", (read(tim, "CR1") & ~1u) | (on ? 1u : 0u));
  }

  [[nodiscard]] auto count(std::string_view tim) const -> std::uint32_t
  {
    return read(tim, "CNT");
  }

  void at(std::uint64_t ticks) const
  {
    auto const until = vtime{start + ticks * tick_ps};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = until}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
  }
};
}

TEST_CASE("TIM4 update events start TIM5 in trigger mode through ITR3, firmware")
{
  auto       f   = entered("tim4_triggers_tim5");
  auto const log = finish(f);
  REQUIRE(log.size() == 3);
  CHECK(log[0] == 1);
  CHECK(log[1] == 1);
  CHECK(log[2] >= 900);
  CHECK(log[2] < 1'000);
}

TEST_CASE("TIM15 in external clock mode 1 on ITR0 counts TIM1 updates, firmware")
{
  auto f = entered("tim1_clocks_tim15");
  CHECK(finish(f) == std::vector<std::uint32_t>{20});
}

TEST_CASE("TIM15 in external clock mode 1 on a stopped TIM1 holds, firmware")
{
  auto f = entered("tim15_unclocked");
  CHECK(finish(f) == std::vector<std::uint32_t>{0});
}

TEST_CASE("an SMCR write with CEN 1 reaches the model: SMS 0111 stops the internal clock at once")
{
  slaves t;
  t.base("TIM3", 0, 0xffff);
  t.enable("TIM3");
  t.at(100);
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
  t.at(200);
  CHECK(t.count("TIM3") == 100);
}

TEST_CASE("external clock mode 1 counts one tick per PSC + 1 master updates")
{
  slaves t;
  t.base("TIM1", 0, 9);
  t.master("TIM1", master_update);
  t.base("TIM3", 2, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(85);
  CHECK(t.count("TIM3") == 2);
  CHECK(t.bits("TIM3", "SR", "TIF") == 1);
  t.at(95);
  CHECK(t.count("TIM3") == 3);
}

TEST_CASE("external clock mode 1 with ARR 4 updates once per 5 master updates")
{
  slaves t;
  t.base("TIM1", 0, 9);
  t.master("TIM1", master_update);
  t.base("TIM3", 0, 4);
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(45);
  CHECK(t.count("TIM3") == 4);
  CHECK(t.bits("TIM3", "SR", "UIF") == 0);
  t.at(75);
  CHECK(t.count("TIM3") == 2);
  CHECK(t.bits("TIM3", "SR", "UIF") == 1);
}

TEST_CASE("UG clears the external clock prescaler progress")
{
  slaves t;
  t.base("TIM1", 0, 9);
  t.master("TIM1", master_update);
  t.base("TIM3", 2, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(25);
  t.write("TIM3", "EGR", 1);
  t.at(35);
  CHECK(t.count("TIM3") == 0);
  t.at(55);
  CHECK(t.count("TIM3") == 1);
}

TEST_CASE("an external clock slave counts only its own master, whatever is read in between")
{
  auto const run = [](bool reads) {
    slaves t;
    t.base("TIM1", 0, 9);
    t.master("TIM1", master_update);
    t.base("TIM4", 0, 6);
    t.master("TIM4", master_update);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
    t.enable("TIM3");
    t.enable("TIM4");
    t.enable("TIM1");
    for(std::uint64_t ticks = 3; ticks < 200; ticks += 7)
    {
      t.at(ticks);
      if(reads)
      {
        static_cast<void>(t.count("TIM1"));
        static_cast<void>(t.count("TIM4"));
      }
    }
    t.at(205);
    return t.count("TIM3");
  };
  CHECK(run(false) == 20);
  CHECK(run(true) == 20);
}

TEST_CASE("trigger mode starts TIM5 at the master's update and counts from that time")
{
  slaves t;
  t.base("TIM4", 0, 99);
  t.master("TIM4", master_update);
  t.base("TIM5", 0, 0xffff'ffff);
  t.write("TIM5", "SMCR", smcr(sms_trigger, ts_itr3));
  t.enable("TIM4");
  t.at(150);
  CHECK(t.bits("TIM5", "CR1", "CEN") == 1);
  CHECK(t.bits("TIM5", "SR", "TIF") == 1);
  CHECK(t.count("TIM5") == 50);
}

TEST_CASE("reset mode clears the count and sets TIF at the master's update")
{
  slaves t;
  t.base("TIM1", 0, 99);
  t.master("TIM1", master_update);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_reset, ts_itr0));
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(130);
  CHECK(t.count("TIM3") == 30);
  CHECK(t.bits("TIM3", "SR", "TIF") == 1);
  CHECK(t.bits("TIM3", "SR", "UIF") == 1);
}

TEST_CASE("reset + trigger mode reinitialises and starts a stopped counter")
{
  slaves t;
  t.base("TIM1", 0, 99);
  t.master("TIM1", master_update);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CNT", 500);
  t.write("TIM3", "SMCR", smcr(sms_reset_trigger, ts_itr0));
  t.enable("TIM1");
  t.at(130);
  CHECK(t.bits("TIM3", "CR1", "CEN") == 1);
  CHECK(t.count("TIM3") == 30);
  CHECK(t.bits("TIM3", "SR", "TIF") == 1);
}

TEST_CASE("gated mode on an MMS 001 master counts only while the master's CEN is 1")
{
  SUBCASE("edges")
  {
    slaves t;
    t.base("TIM1", 0, 0xffff);
    t.master("TIM1", master_enable);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
    t.enable("TIM3");
    t.at(20);
    CHECK(t.count("TIM3") == 0);
    t.enable("TIM1");
    CHECK(t.bits("TIM3", "SR", "TIF") == 1);
    t.write("TIM3", "SR", 0);
    t.at(50);
    CHECK(t.count("TIM3") == 30);
    t.enable("TIM1", false);
    CHECK(t.bits("TIM3", "SR", "TIF") == 1);
    t.at(80);
    CHECK(t.count("TIM3") == 30);
  }
  SUBCASE("master already running at the SMCR write")
  {
    slaves t;
    t.base("TIM1", 0, 0xffff);
    t.master("TIM1", master_enable);
    t.enable("TIM1");
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
    t.enable("TIM3");
    t.at(10);
    CHECK(t.count("TIM3") == 0);
    t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
    t.at(40);
    CHECK(t.count("TIM3") == 30);
  }
  SUBCASE("the master leaving MMS 001 closes the gate")
  {
    slaves t;
    t.base("TIM1", 0, 0xffff);
    t.master("TIM1", master_enable);
    t.enable("TIM1");
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
    t.enable("TIM3");
    t.at(30);
    t.write("TIM3", "SR", 0);
    t.master("TIM1", master_update);
    CHECK(t.bits("TIM3", "SR", "TIF") == 1);
    t.at(60);
    CHECK(t.count("TIM3") == 30);
  }
}

TEST_CASE(
    "gated mode entered at a one-pulse master's last update reads the level after that update")
{
  slaves t;
  t.base("TIM1", 0, 99);
  t.master("TIM1", master_enable);
  t.write("TIM1", "CR1", 1u << 3);
  t.enable("TIM1");
  t.base("TIM3", 0, 0xffff);
  t.at(100);
  t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
  t.enable("TIM3");
  t.at(150);
  CHECK(t.bits("TIM1", "CR1", "CEN") == 0);
  CHECK(t.count("TIM3") == 0);
}

TEST_CASE("gated mode on an MMS 010 master holds, sets no TIF and logs once")
{
  slaves t;
  t.base("TIM1", 0, 9);
  t.master("TIM1", master_update);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(105);
  CHECK(t.count("TIM3") == 0);
  CHECK(t.bits("TIM3", "SR", "TIF") == 0);
  CHECK(t.warnings() == 1);
}

TEST_CASE("gated + reset mode clears the count when the gate closes")
{
  slaves t;
  t.base("TIM1", 0, 0xffff);
  t.master("TIM1", master_enable);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_gated_reset, ts_itr0));
  t.enable("TIM3");
  t.at(20);
  t.enable("TIM1");
  t.at(50);
  CHECK(t.count("TIM3") == 30);
  t.enable("TIM1", false);
  t.at(80);
  CHECK(t.count("TIM3") == 0);
  t.enable("TIM1");
  t.at(90);
  CHECK(t.count("TIM3") == 10);
}

TEST_CASE(
    "a TIM2 and TIM3 reset mode cycle terminates, each timer logs its dropped edge once"
    * doctest::timeout(10))
{
  slaves t;
  for(auto const* const tim : {"TIM2", "TIM3"})
  {
    t.base(tim, 0, 99);
    t.master(tim, master_update);
  }
  t.write("TIM2", "SMCR", smcr(sms_reset, ts_itr2));
  t.write("TIM3", "SMCR", smcr(sms_reset, ts_itr1));
  t.enable("TIM2");
  t.at(50);
  t.enable("TIM3");
  t.at(1'050);
  CHECK(t.count("TIM2") == 50);
  CHECK(t.count("TIM3") == 50);
  REQUIRE(t.warnings() == 2);
  CHECK(t.logged[0].peripheral == "TIM2");
  CHECK(t.logged[1].peripheral == "TIM3");
}

TEST_CASE("a TIM2 and TIM3 external clock cycle drops the closing edge and keeps the UG")
{
  slaves t;
  for(auto const* const tim : {"TIM2", "TIM3"})
  {
    t.base(tim, 0, 1);
    t.master(tim, master_update);
    t.write(tim, "CNT", 1);
  }
  t.write("TIM2", "SMCR", smcr(sms_external, ts_itr2));
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr1));
  t.enable("TIM2");
  t.enable("TIM3");
  t.write("TIM2", "EGR", 1);
  CHECK(t.count("TIM2") == 0);
  CHECK(t.count("TIM3") == 0);
  CHECK(t.bits("TIM3", "SR", "UIF") == 1);
  CHECK(t.warnings() == 1);
}

TEST_CASE("external clock mode 1 on ITR7, tim16_oc1, holds and logs once")
{
  slaves t;
  t.base("TIM1", 0, 9);
  t.master("TIM1", master_update);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr7));
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr7));
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(100);
  CHECK(t.count("TIM3") == 0);
  CHECK(t.warnings() == 1);
}

namespace
{
struct tim_state_fields
{
  std::uint64_t                anchor_ps{};
  std::uint64_t                anchor_frac{};
  std::uint64_t                hz{};
  std::uint64_t                due_ps{};
  std::uint64_t                carry{};
  std::uint32_t                generation{};
  std::uint32_t                count{};
  std::uint32_t                repetition{};
  std::uint32_t                prescaler{};
  std::uint32_t                reload{};
  std::uint32_t                edges{};
  std::array<std::uint32_t, 4> compare{};
  std::array<std::uint8_t, 4>  compare_request{};
  std::uint8_t                 update_request{};
  std::uint8_t                 down{};
  std::uint8_t                 scheduled{};
  std::uint8_t                 gate{};
};

struct tim_state_without_edges
{
  std::uint64_t                anchor_ps{};
  std::uint64_t                anchor_frac{};
  std::uint64_t                hz{};
  std::uint64_t                due_ps{};
  std::uint64_t                carry{};
  std::uint32_t                generation{};
  std::uint32_t                count{};
  std::uint32_t                repetition{};
  std::uint32_t                prescaler{};
  std::uint32_t                reload{};
  std::array<std::uint32_t, 4> compare{};
  std::array<std::uint8_t, 4>  compare_request{};
  std::uint8_t                 update_request{};
  std::uint8_t                 down{};
  std::uint8_t                 scheduled{};
  std::uint8_t                 gate{};
};

struct tim_state_without_gate
{
  std::uint64_t                anchor_ps{};
  std::uint64_t                anchor_frac{};
  std::uint64_t                hz{};
  std::uint64_t                due_ps{};
  std::uint64_t                carry{};
  std::uint32_t                generation{};
  std::uint32_t                count{};
  std::uint32_t                repetition{};
  std::uint32_t                prescaler{};
  std::uint32_t                reload{};
  std::uint32_t                edges{};
  std::array<std::uint32_t, 4> compare{};
  std::array<std::uint8_t, 4>  compare_request{};
  std::uint8_t                 update_request{};
  std::uint8_t                 down{};
  std::uint8_t                 scheduled{};
};

template <typename To, typename From> auto narrowed(From const& from) -> To
{
  To             to{};
  constexpr auto access = std::meta::access_context::unchecked();
  template for(constexpr auto out :
               std::define_static_array(std::meta::nonstatic_data_members_of(^^To, access)))
  {
    template for(constexpr auto in :
                 std::define_static_array(std::meta::nonstatic_data_members_of(^^From, access)))
    {
      if constexpr(std::meta::identifier_of(out) == std::meta::identifier_of(in))
      {
        to.[:out:] = from.[:in:];
      }
    }
  }
  return to;
}

auto saved_state(stm32_fixture const& f, std::string_view tim) -> tim_state_fields
{
  std::vector<std::byte> bytes;
  f.target->model(tim)->save(bytes);
  return reflex::serde::bson::deserializer{std::span<std::byte const>{bytes}}
      .load<tim_state_fields>();
}
}

TEST_CASE("a TIM state saved before the external clock prescaler count loads it as 0")
{
  slaves t;
  t.base("TIM1", 0, 9);
  t.master("TIM1", master_update);
  t.base("TIM3", 2, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(25);
  auto const now = saved_state(t.f, "TIM3");
  CHECK(now.edges == 2);
  std::vector<std::byte> old;
  reflex::serde::bson::serializer{old}.dump(narrowed<tim_state_without_edges>(now));
  t.f.target->model("TIM3")->load(old);
  CHECK(saved_state(t.f, "TIM3").edges == 0);
  t.at(45);
  CHECK(t.count("TIM3") == 0);
}

TEST_CASE("a TIM state saved before the gate loads it closed")
{
  slaves t;
  t.base("TIM1", 0, 0xffff);
  t.master("TIM1", master_enable);
  t.enable("TIM1");
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
  t.enable("TIM3");
  t.at(10);
  auto const now = saved_state(t.f, "TIM3");
  CHECK(now.gate == 1);
  std::vector<std::byte> old;
  reflex::serde::bson::serializer{old}.dump(narrowed<tim_state_without_gate>(now));
  t.f.target->model("TIM3")->load(old);
  CHECK(saved_state(t.f, "TIM3").gate == 0);
}

TEST_CASE("a gated MMS 001 master gives its gate to its own gated slave")
{
  slaves t;
  t.base("TIM1", 0, 0xffff);
  t.master("TIM1", master_enable);
  t.base("TIM2", 0, 0xffff);
  t.write("TIM2", "SMCR", smcr(sms_gated, ts_itr0));
  t.master("TIM2", master_enable);
  t.enable("TIM2");
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr1));
  t.enable("TIM3");
  t.at(20);
  CHECK(t.count("TIM3") == 0);
  t.enable("TIM1");
  t.at(50);
  CHECK(t.count("TIM2") == 30);
  CHECK(t.count("TIM3") == 30);
  t.enable("TIM1", false);
  t.at(80);
  CHECK(t.count("TIM2") == 30);
  CHECK(t.count("TIM3") == 30);
}

TEST_CASE("a gated slave samples its master again when its clock comes back")
{
  for(auto const mode : {sms_gated, sms_gated_reset})
  {
    CAPTURE(mode);
    slaves t;
    t.base("TIM1", 0, 0xffff);
    t.master("TIM1", master_enable);
    t.enable("TIM1");
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(mode, ts_itr0));
    t.enable("TIM3");
    t.at(20);
    t.b.set("RCC", "RCC_APB1ENR1", "TIM3EN", 0);
    t.at(30);
    t.enable("TIM1", false);
    t.at(40);
    t.b.set("RCC", "RCC_APB1ENR1", "TIM3EN", 1);
    t.at(70);
    CHECK(t.count("TIM3") == (mode == sms_gated ? 20u : 0u));
  }
}

TEST_CASE("a past edge starts or resets the counter at the edge's time")
{
  SUBCASE("trigger")
  {
    slaves t;
    t.base("TIM4", 0, 99);
    t.master("TIM4", master_update);
    t.base("TIM5", 0, 0xffff'ffff);
    t.write("TIM5", "SMCR", smcr(sms_trigger, ts_itr3));
    t.at(150);
    auto* const sink = dynamic_cast<stm32::trigger_sink*>(t.f.target->model("TIM5"));
    REQUIRE(sink != nullptr);
    sink->triggered("tim4_trgo", stm32::trigger_edge::rising, t.start + 100 * tick_ps);
    CHECK(t.bits("TIM5", "CR1", "CEN") == 1);
    CHECK(t.count("TIM5") == 50);
  }
  SUBCASE("reset")
  {
    slaves t;
    t.base("TIM1", 0, 0xffff);
    t.master("TIM1", master_update);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_reset, ts_itr0));
    t.enable("TIM3");
    t.at(150);
    auto* const sink = dynamic_cast<stm32::trigger_sink*>(t.f.target->model("TIM3"));
    REQUIRE(sink != nullptr);
    sink->triggered("tim1_trgo", stm32::trigger_edge::rising, t.start + 100 * tick_ps);
    CHECK(t.count("TIM3") == 50);
  }
}

TEST_CASE("a gated slave with CEN 0 still follows its master, so a later CEN finds the gate shut")
{
  slaves t;
  t.base("TIM1", 0, 0xffff);
  t.master("TIM1", master_enable);
  t.enable("TIM1");
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
  t.at(10);
  t.enable("TIM1", false);
  t.at(20);
  t.enable("TIM3");
  t.at(50);
  CHECK(t.count("TIM3") == 0);
}

TEST_CASE("a gated slave configured before its master's MMS logs nothing and counts")
{
  slaves t;
  t.base("TIM1", 0, 0xffff);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
  t.enable("TIM3");
  t.master("TIM1", master_enable);
  t.at(10);
  t.enable("TIM1");
  t.at(30);
  CHECK(t.count("TIM3") == 20);
  CHECK(t.warnings() == 0);
}

TEST_CASE("a CEN clear and an SMS change clear the external clock prescaler progress")
{
  for(auto const via_sms : {false, true})
  {
    CAPTURE(via_sms);
    slaves t;
    t.base("TIM1", 0, 9);
    t.master("TIM1", master_update);
    t.base("TIM3", 2, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
    t.enable("TIM3");
    t.enable("TIM1");
    t.at(25);
    if(via_sms)
    {
      t.write("TIM3", "SMCR", 0);
      t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
    }
    else
    {
      t.enable("TIM3", false);
      t.enable("TIM3");
    }
    t.at(35);
    CHECK(t.count("TIM3") == 0);
    t.at(55);
    CHECK(t.count("TIM3") == 1);
  }
}
