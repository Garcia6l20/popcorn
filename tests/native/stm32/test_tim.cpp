#include "dma.hpp"
#include "stm32_fixture.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

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

auto entered_on(
    popcorn::part_description desc,
    std::string_view          entry,
    core::machine_options     options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{std::move(desc), "tim_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("tim_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("tim_ll", entry));
  return f;
}

auto entered(
    std::string_view      entry,
    core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  return entered_on(u575(), entry, std::move(options));
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
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
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
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
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
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const field = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(field != found->fields.end());
    auto const mask = ((std::uint32_t{1} << field->width) - 1u) << field->lsb;
    write(peripheral, reg, (read(peripheral, reg) & ~mask) | ((value << field->lsb) & mask));
  }
};

class recorder final : public periph::model, public stm32::dma_controller
{
public:
  explicit recorder(periph::model_context& ctx, bool knows = true) : context{ctx}, knows{knows}
  {}

  periph::model_context&                      context;
  bool                                        knows;
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
    if(not knows)
    {
      return std::nullopt;
    }
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
  options.models.emplace("LPDMA1", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
    return std::make_shared<recorder>(ctx, false);
  });
  return options;
}

struct direct
{
  stm32_fixture f;
  bus           b{f};
  std::uint64_t start{};

  explicit direct(core::machine_options options = popcorn::testing::stm32_options())
      : direct{u575(), std::move(options)}
  {}

  explicit direct(
      popcorn::part_description desc,
      core::machine_options     options = popcorn::testing::stm32_options())
      : f{entered_on(std::move(desc), "idle", std::move(options))}
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

TEST_CASE("CCxDE and UDE raise their DMA requests, each drops only at its own acknowledge")
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
  auto const both = std::vector<std::pair<std::uint32_t, bool>>{
      {1, true},
      {2, true}
  };
  CHECK(dma->levels == both);
  auto const at = t.b.at("TIM1", "TIM1_CCR1");
  REQUIRE(dma->context.services.read(at, 4, popcorn::initiator::dma).has_value());
  REQUIRE(dma->context.services.write(at, 4, 3, popcorn::initiator::dma).has_value());
  CHECK(dma->levels == both);
  auto* const requester = dynamic_cast<stm32::dma_requester*>(t.f.target->model("TIM1"));
  REQUIRE(requester != nullptr);
  auto* const other = t.f.model_of<recorder>("LPDMA1");
  REQUIRE(other != nullptr);
  requester->acknowledged(*other, 1);
  requester->acknowledged(*other, 2);
  CHECK(dma->levels == both);
  CHECK(other->levels.empty());
  requester->acknowledged(*dma, 1);
  CHECK(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {1, true },
          {2, true },
          {1, false}
  });
  requester->acknowledged(*dma, 2);
  CHECK(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {1, true },
          {2, true },
          {1, false},
          {2, false}
  });
}

TEST_CASE("a TIM6 update request paces a channel to GPIOE BSRR at one word per update, firmware")
{
  auto f = entered("tim6_dma_bsrr");
  CHECK(finish(f) == std::vector<std::uint32_t>{8});
}

TEST_CASE("TIM2 UP and CC1 requests on two channels both complete, firmware")
{
  auto f = entered("tim2_two_requests");
  CHECK(finish(f) == std::vector<std::uint32_t>{4, 1, 1, 0, 0, 14, 24});
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

  explicit slaves(popcorn::part_description desc = u575())
      : f{entered_on(std::move(desc), "idle")},
        sink{f.target->add_log_sink(
            [this](core::log_record const& record) { logged.push_back(record); })}
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

TEST_CASE("TIM1RST of an MMS 001 master with CEN 1 drops TRGO and stops a gated slave")
{
  slaves t;
  t.base("TIM1", 0, 0xffff);
  t.master("TIM1", master_enable);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
  t.enable("TIM3");
  t.enable("TIM1");
  t.write("TIM3", "SR", 0);
  t.at(30);
  REQUIRE(t.count("TIM3") == 30);
  rcc_reset_pulse(t.f, "RCC_APB2RSTR", "TIM1RST");
  CHECK(t.read("TIM1", "CR1") == 0);
  CHECK(t.bits("TIM3", "SR", "TIF") == 1);
  t.at(60);
  CHECK(t.count("TIM3") == 30);
}

TEST_CASE("TIM1RST lowers the update and compare DMA requests on the controller")
{
  direct      t{with_recorder()};
  auto* const dma = t.f.model_of<recorder>("GPDMA1");
  REQUIRE(dma != nullptr);
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.b.write("TIM1", "TIM1_CCR1", 3);
  t.begin();
  t.b.write("TIM1", "TIM1_DIER", (1u << 9) | (1u << 8));
  t.at(13);
  REQUIRE(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {1, true},
          {2, true}
  });
  rcc_reset_pulse(t.f, "RCC_APB2RSTR", "TIM1RST");
  CHECK(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {1, true },
          {2, true },
          {2, false},
          {1, false}
  });
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
auto svd_named_timers() -> popcorn::part_description
{
  auto desc = u575();
  for(auto& unit : desc.peripherals)
  {
    auto const old_name = [&](std::string_view bare) -> std::string {
      auto const found = popcorn::find_register(unit, bare);
      return found != nullptr
                 and found->aliases
                 and not found->aliases->empty()
                 and found->aliases->front().starts_with("TIM1_")
               ? found->aliases->front()
               : std::string{bare};
    };
    for(auto& rule : unit.cross_rules)
    {
      rule.reg        = old_name(rule.reg);
      rule.target_reg = old_name(rule.target_reg);
    }
    std::vector<std::string> renamed;
    for(auto const& reg : unit.registers)
    {
      renamed.push_back(old_name(reg.name));
    }
    for(auto& reg : unit.registers)
    {
      if(reg.alternate_of)
      {
        reg.alternate_of = old_name(*reg.alternate_of);
      }
    }
    for(std::size_t index = 0; index < unit.registers.size(); ++index)
    {
      if(renamed[index] != unit.registers[index].name)
      {
        unit.registers[index].name    = renamed[index];
        unit.registers[index].aliases = std::nullopt;
      }
    }
  }
  return desc;
}

auto last_warning(std::vector<core::log_record> const& logged) -> core::log_record const&
{
  auto const found =
      std::ranges::find_last(logged, core::log_kind::model_warning, &core::log_record::kind);
  REQUIRE(not found.empty());
  return found.front();
}
}

TEST_CASE("TIM1 and TIM8 warnings name registers by their bare names")
{
  for(std::string_view const unit : {"TIM1", "TIM8"})
  {
    CAPTURE(unit);
    direct                        t;
    std::vector<core::log_record> logged;
    auto const                    sink =
        t.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    t.b.set("RCC", "RCC_APB2ENR", "TIM8EN", 1);
    t.b.write(unit, "SMCR", smcr(sms_external, ts_itr7));
    auto const& warning = last_warning(logged);
    CHECK(warning.peripheral == unit);
    CHECK(warning.reg == "SMCR");
  }
}

TEST_CASE("a description with only SVD names TIM1_CR1 and so on still runs TIM1")
{
  auto       desc = svd_named_timers();
  auto const tim1 = std::ranges::find(
      desc.peripherals, std::string_view{"TIM1"}, &popcorn::peripheral_desc::name);
  REQUIRE(tim1 != desc.peripherals.end());
  REQUIRE(popcorn::find_register(*tim1, "CR1") == nullptr);
  REQUIRE(popcorn::find_register(*tim1, "TIM1_CR1") != nullptr);
  direct                        t{std::move(desc)};
  std::vector<core::log_record> logged;
  auto const                    sink =
      t.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  t.b.write("TIM1", "TIM1_PSC", 3);
  t.b.write("TIM1", "TIM1_ARR", 99);
  t.b.write("TIM1", "TIM1_CCR1", 50);
  t.begin();
  CHECK(t.b.bits("TIM1", "TIM1_CR1", "CEN") == 1);
  t.at(4 * 50);
  CHECK(t.count() == 50);
  CHECK(t.flag("CC1IF") == 1);
  t.at(4 * 100);
  CHECK(t.count() == 0);
  CHECK(t.flag("UIF") == 1);
  t.b.write("TIM1", "TIM1_SMCR", smcr(sms_external, ts_itr7));
  CHECK(last_warning(logged).reg == "TIM1_SMCR");
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

TEST_CASE("TIM3 in encoder mode with no input edges holds, firmware")
{
  auto f = entered("tim3_encoder_idle");
  CHECK(finish(f) == std::vector<std::uint32_t>{0, 0});
}

TEST_CASE("without input edges every encoder SMS value, ECE 1 and SMS 0111 on a pin hold")
{
  constexpr std::uint32_t ece = 1u << 14;
  for(auto const word :
      {smcr(0b0001, 0), smcr(0b0010, 0), smcr(0b0011, 0), smcr(0b1010, 0), smcr(0b1011, 0),
       smcr(0b1100, 0), smcr(0b1101, 0), smcr(0b1110, 0), smcr(0b1111, 0), ece,
       smcr(sms_external, 5)})
  {
    CAPTURE(word);
    slaves t;
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", word);
    t.enable("TIM3");
    t.at(100);
    CHECK(t.count("TIM3") == 0);
  }
}

TEST_CASE("SMS back to 0000 with CEN 1 counts from the SMCR write")
{
  slaves t;
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(0b0011, 0));
  t.enable("TIM3");
  t.at(100);
  t.write("TIM3", "SMCR", 0);
  t.at(150);
  CHECK(t.count("TIM3") == 50);
}

TEST_CASE("an encoder index, gated TI1F_ED and ETRSEL each log once, pin inputs do not")
{
  auto const last = [](slaves const& t) {
    REQUIRE(not t.logged.empty());
    return t.logged.back().message;
  };
  SUBCASE("encoder inputs raise no input capture warning")
  {
    slaves t;
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "CCMR1_Input", 0x0101);
    t.write("TIM3", "SMCR", smcr(0b0011, 0));
    CHECK(t.warnings() == 0);
    t.enable("TIM3");
    t.enable("TIM3", false);
    t.enable("TIM3");
    CHECK(t.warnings() == 0);
    t.write("TIM3", "ECR", 1);
    CHECK(t.warnings() == 1);
    CHECK(last(t).contains("encoder index"));
    t.enable("TIM3", false);
    t.write("TIM3", "SMCR", 1u << 14);
    t.write("TIM3", "SMCR", smcr(sms_external, 5));
    t.write("TIM3", "CCMR1_Input", 0);
    t.enable("TIM3");
    CHECK(t.warnings() == 1);
    t.write("TIM3", "SMCR", smcr(sms_gated, 4));
    CHECK(t.warnings() == 2);
    CHECK(last(t).contains("TI1F_ED"));
    t.write("TIM3", "AF1", 1u << 14);
    CHECK(t.warnings() == 3);
    CHECK(last(t).contains("ETRSEL"));
  }
  SUBCASE("trigger mode on TI1FP1 with CEN 0 logs nothing")
  {
    slaves t;
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_trigger, 5));
    CHECK(t.warnings() == 0);
  }
  SUBCASE("an encoder SMCR write with CEN 1 logs nothing")
  {
    slaves t;
    t.base("TIM3", 0, 0xffff);
    t.enable("TIM3");
    t.write("TIM3", "SMCR", smcr(0b0011, 0));
    CHECK(t.warnings() == 0);
  }
}

TEST_CASE("TIM3 in encoder mode with no input edges logs nothing, firmware")
{
  auto                          f = entered("tim3_encoder_idle");
  std::vector<core::log_record> logged;
  auto const                    sink = f.target->add_log_sink([&](core::log_record const& record) {
    if(record.peripheral == "TIM3")
    {
      logged.push_back(record);
    }
  });
  static_cast<void>(finish(f));
  CHECK(logged.empty());
}

TEST_CASE("CPU writes to DMAR land in CCR1 to CCR3 through DBA, firmware")
{
  auto f = entered("tim5_burst_cpu");
  CHECK(finish(f) == std::vector<std::uint32_t>{11, 22, 33});
}

TEST_CASE("an update-paced channel to DMAR moves DBL + 1 words per update, firmware")
{
  auto f = entered("tim5_burst_dma");
  CHECK(finish(f) == std::vector<std::uint32_t>{1, 2, 3, 24, 4, 5, 6, 12, 7, 8, 9, 0, 0});
}

namespace
{
constexpr std::uint32_t dbss_update = 1u << 16;
constexpr std::uint32_t dbss_com    = 6u << 16;

constexpr auto dcr(std::uint32_t base, std::uint32_t transfers, std::uint32_t source)
    -> std::uint32_t
{
  return base | ((transfers - 1) << 8) | source;
}

struct burst
{
  direct        t{with_recorder()};
  recorder*     dma{t.f.model_of<recorder>("GPDMA1")};
  std::uint32_t dmar{};

  burst()
  {
    REQUIRE(dma != nullptr);
    t.b.set("RCC", "RCC_APB1ENR1", "TIM5EN", 1);
    dmar = static_cast<std::uint32_t>(t.b.at("TIM5", "DMAR"));
  }

  void cpu(std::uint32_t value, std::uint8_t size = 4, std::uint32_t lane = 0) const
  {
    REQUIRE(
        dma->context.services.write(dmar + lane, size, value, popcorn::initiator::cpu).has_value());
  }

  [[nodiscard]] auto cpu_read() const -> std::uint32_t
  {
    auto const value = dma->context.services.read(dmar, 4, popcorn::initiator::cpu);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto reg(std::string_view name) const -> std::uint32_t
  {
    return t.b.read("TIM5", name);
  }
};
}

TEST_CASE("a DMAR burst reprograms CCMR1 and CCMR2, which the model does not claim")
{
  burst b;
  b.t.b.write("TIM5", "DCR", dcr(6, 2, dbss_update));
  b.cpu(0x0000'6868);
  b.cpu(0x0000'7070);
  CHECK(b.reg("CCMR1_Output") == 0x6868);
  CHECK(b.reg("CCMR2_Output") == 0x7070);
}

TEST_CASE("the DMAR index wraps after DBL + 1 accesses and a DCR write resets it")
{
  burst b;
  b.t.b.write("TIM5", "DCR", dcr(13, 2, dbss_update));
  for(auto const value : {1u, 2u, 3u})
  {
    b.cpu(value);
  }
  CHECK(b.reg("CCR1") == 3);
  CHECK(b.reg("CCR2") == 2);
  CHECK(b.reg("CCR3") == 0);
  b.t.b.write("TIM5", "DCR", dcr(13, 2, dbss_update));
  b.cpu(5);
  CHECK(b.reg("CCR1") == 5);
  CHECK(b.reg("CCR2") == 2);
}

TEST_CASE("a debugger access to DMAR redirects without advancing the index")
{
  burst b;
  b.t.b.write("TIM5", "DCR", dcr(13, 3, dbss_update));
  b.t.b.write("TIM5", "CCR2", 77);
  b.cpu(1);
  CHECK(b.t.b.read("TIM5", "DMAR") == 77);
  b.t.b.write("TIM5", "DMAR", 88);
  CHECK(b.reg("CCR2") == 88);
  b.cpu(2);
  CHECK(b.reg("CCR2") == 2);
  CHECK(b.cpu_read() == 0);
  CHECK(b.reg("CCR1") == 1);
}

TEST_CASE("a half-word DMAR write lands in its byte lane of the target")
{
  burst b;
  b.t.b.write("TIM5", "DCR", dcr(13, 1, dbss_update));
  b.t.b.write("TIM5", "CCR1", 0x1234);
  b.cpu(0xabcd, 2, 2);
  CHECK(b.reg("CCR1") == 0xabcd'1234);
}

TEST_CASE("a DMAR access past the timer registers reads 0, writes nothing and logs once")
{
  burst                         b;
  std::vector<core::log_record> logged;
  auto const                    sink =
      b.t.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  b.t.b.write("TIM5", "DCR", dcr(0x11, 1, dbss_update));
  b.cpu(0x5555);
  CHECK(b.cpu_read() == 0);
  b.cpu(0x5555);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].reg == "DMAR");
  CHECK(b.t.f.target->peripheral("TIM5").peek(0x44) == 0);
}

TEST_CASE("the burst source holds its request across acknowledges until the index wraps")
{
  direct      t{with_recorder()};
  auto* const dma = t.f.model_of<recorder>("GPDMA1");
  REQUIRE(dma != nullptr);
  auto* const requester = dynamic_cast<stm32::dma_requester*>(t.f.target->model("TIM1"));
  REQUIRE(requester != nullptr);
  auto const dmar   = t.b.at("TIM1", "TIM1_DMAR");
  auto const access = [&] {
    REQUIRE(dma->context.services.write(dmar, 4, 9, popcorn::initiator::dma).has_value());
    requester->acknowledged(*dma, 2);
  };
  t.b.write("TIM1", "TIM1_ARR", 9);
  SUBCASE("update source")
  {
    t.b.write("TIM1", "TIM1_DCR", dcr(13, 3, dbss_update));
    t.begin();
    t.b.write("TIM1", "TIM1_DIER", 1u << 8);
    t.at(10);
    REQUIRE(
        dma->levels
        == std::vector<std::pair<std::uint32_t, bool>>{
            {2, true}
    });
    access();
    access();
    CHECK(dma->levels.size() == 1);
    access();
    CHECK(
        dma->levels
        == std::vector<std::pair<std::uint32_t, bool>>{
            {2, true },
            {2, false}
    });
  }
  SUBCASE("CC1 source")
  {
    t.b.write("TIM1", "TIM1_CCR1", 3);
    t.b.write("TIM1", "TIM1_DCR", dcr(13, 2, 2u << 16));
    t.begin();
    t.b.write("TIM1", "TIM1_DIER", 1u << 9);
    t.at(3);
    REQUIRE(
        dma->levels
        == std::vector<std::pair<std::uint32_t, bool>>{
            {1, true}
    });
    auto const cc1 = [&] {
      REQUIRE(dma->context.services.write(dmar, 4, 9, popcorn::initiator::dma).has_value());
      requester->acknowledged(*dma, 1);
    };
    cc1();
    CHECK(dma->levels.size() == 1);
    cc1();
    CHECK(
        dma->levels
        == std::vector<std::pair<std::uint32_t, bool>>{
            {1, true },
            {1, false}
    });
  }
  SUBCASE("COM source logs once and keeps one request per event")
  {
    std::vector<core::log_record> logged;
    auto const                    sink =
        t.f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    t.b.write("TIM1", "TIM1_DCR", dcr(13, 3, dbss_com));
    t.b.write("TIM1", "TIM1_DCR", dcr(13, 2, dbss_com));
    CHECK(logged.size() == 1);
    t.begin();
    t.b.write("TIM1", "TIM1_DIER", 1u << 8);
    t.at(10);
    access();
    CHECK(
        dma->levels
        == std::vector<std::pair<std::uint32_t, bool>>{
            {2, true },
            {2, false}
    });
  }
}

TEST_CASE("TIM16 and TIM17 DCR and DMAR reset to 0")
{
  slaves t;
  for(auto const* const bit : {"TIM16EN", "TIM17EN"})
  {
    t.b.set("RCC", "RCC_APB2ENR", bit, 1);
  }
  for(auto const* const tim : {"TIM16", "TIM17"})
  {
    CHECK(t.b.read(tim, "DCR") == 0);
    CHECK(t.b.read(tim, "DMAR") == 0);
  }
}

TEST_CASE("DBL 11010 runs 26 transfers, reserved DBL values run 26 and log once")
{
  for(auto const code : {26u, 31u})
  {
    CAPTURE(code);
    burst                         b;
    std::vector<core::log_record> logged;
    auto const                    sink = b.t.f.target->add_log_sink(
        [&](core::log_record const& record) { logged.push_back(record); });
    b.t.b.write("TIM5", "DCR", 6u | (code << 8) | dbss_update);
    CHECK(logged.size() == (code == 26 ? 0u : 1u));
    for(std::uint32_t access = 0; access < 26; ++access)
    {
      static_cast<void>(b.cpu_read());
    }
    b.cpu(0x0000'4848);
    CHECK(b.reg("CCMR1_Output") == 0x4848);
  }
}

namespace
{
constexpr std::uint64_t tim1_tick = 250'000;

struct held_tim
{
  direct        d;
  std::uint64_t slice;

  explicit held_tim(
      std::uint64_t         instructions,
      core::machine_options options = popcorn::testing::stm32_options())
      : d{std::move(options)}, slice{instructions}
  {}

  void to(std::uint64_t ticks) const
  {
    auto const until = vtime{d.start + ticks * tim1_tick};
    for(;;)
    {
      auto const result = d.f.run(
          {.max_instructions = icount{slice},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = until});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == stop_reason::time);
        return;
      }
    }
  }

  [[nodiscard]] auto block() const -> std::vector<std::uint32_t>
  {
    auto const&                regs = d.f.target->peripheral("TIM1");
    std::vector<std::uint32_t> out;
    for(std::uint32_t offset = 0; offset < regs.description().size; offset += 4)
    {
      out.push_back(regs.peek(offset));
    }
    return out;
  }
};
}

TEST_CASE("with DBG_TIM1_STOP a held TIM1 keeps its count and resumes on its phase")
{
  auto const run = [](std::uint64_t instructions) {
    held_tim t{instructions};
    t.d.begin();
    t.to(1000);
    CHECK(t.d.count() == 1000);
    popcorn::testing::set_debug_freeze(t.d.f, "DBG_TIM1_STOP", true);
    t.d.f.target->hold_core(true);
    t.to(6000);
    CHECK(t.d.count() == 1000);
    t.d.f.target->hold_core(false);
    t.to(7000);
    CHECK(t.d.count() == 2000);
    return t.block();
  };
  CHECK(run(997) == run(1'000'000'000));
}

TEST_CASE("TIM1RST during a DBG_TIM1_STOP hold keeps the timer frozen until the hold ends")
{
  held_tim t{1'000'000'000};
  t.d.begin();
  t.to(1000);
  popcorn::testing::set_debug_freeze(t.d.f, "DBG_TIM1_STOP", true);
  t.d.f.target->hold_core(true);
  rcc_reset_pulse(t.d.f, "RCC_APB2RSTR", "TIM1RST");
  t.d.begin();
  t.to(3000);
  CHECK(t.d.count() == 0);
  t.d.f.target->hold_core(false);
  t.to(4000);
  CHECK(t.d.count() == 1000);
}

TEST_CASE("without DBG_TIM1_STOP a held TIM1 counts on")
{
  held_tim t{1'000'000'000};
  t.d.begin();
  t.to(1000);
  t.d.f.target->hold_core(true);
  t.to(6000);
  CHECK(t.d.count() == 6000);
}

TEST_CASE("a TIM1 hold released at once mid-prescale leaves the counter as never held")
{
  auto const run = [](bool hold) {
    held_tim t{1'000'000'000};
    t.d.b.write("TIM1", "TIM1_PSC", 7);
    t.d.b.write("TIM1", "TIM1_ARR", 99);
    t.d.begin();
    popcorn::testing::set_debug_freeze(t.d.f, "DBG_TIM1_STOP", true);
    t.to(1003);
    if(hold)
    {
      t.d.f.target->hold_core(true);
      t.d.f.target->hold_core(false);
    }
    std::vector<std::uint32_t> seen;
    for(std::uint64_t ticks = 1004; ticks < 2600; ticks += 7)
    {
      t.to(ticks);
      seen.push_back(t.d.count());
      seen.push_back(t.d.flag("UIF"));
    }
    return seen;
  };
  CHECK(run(true) == run(false));
}

TEST_CASE("a core clock change and back during a frozen hold keeps the TIM1 phase")
{
  held_tim t{1'000'000'000};
  t.d.b.write("TIM1", "TIM1_PSC", 7);
  t.d.begin();
  popcorn::testing::set_debug_freeze(t.d.f, "DBG_TIM1_STOP", true);
  t.to(1003);
  t.d.f.target->hold_core(true);
  t.to(2000);
  auto const icscr1 = t.d.b.read("RCC", "RCC_ICSCR1");
  t.d.b.write("RCC", "RCC_ICSCR1", (icscr1 & ~(0xFu << 28)) | (1u << 28) | (1u << 23));
  t.to(3000);
  t.d.b.write("RCC", "RCC_ICSCR1", icscr1 | (1u << 23));
  t.to(4000);
  t.d.f.target->hold_core(false);
  t.to(4000 + 9000 - 1003);

  held_tim twin{1'000'000'000};
  twin.d.b.write("TIM1", "TIM1_PSC", 7);
  twin.d.begin();
  twin.to(9000);
  CHECK(t.d.count() == twin.d.count());
  CHECK(t.d.flag("UIF") == twin.d.flag("UIF"));
}

TEST_CASE("a frozen TIM1 restores frozen into another machine and resumes as the original")
{
  held_tim t{1'000'000'000};
  t.d.begin();
  popcorn::testing::set_debug_freeze(t.d.f, "DBG_TIM1_STOP", true);
  t.to(1000);
  t.d.f.target->hold_core(true);
  t.to(3000);
  auto const bytes = core::encode_snapshot(t.d.f.target->snapshot().data());
  t.d.f.target->hold_core(false);
  t.to(5000);

  held_tim other{1'000'000'000};
  other.d.f.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  other.d.start = t.d.start;
  CHECK(other.d.f.target->core_held());
  CHECK(other.d.count() == 1000);
  other.d.f.target->hold_core(false);
  other.to(5000);
  CHECK(other.d.count() == t.d.count());
  CHECK(other.d.count() == 3000);
}

TEST_CASE("a TIM1 hold with no freeze field to read warns once and the timer counts on")
{
  auto desc = popcorn::testing::u575();
  auto dbg  = std::ranges::find(
      desc.peripherals, std::string_view{"DBGMCU"}, &popcorn::peripheral_desc::name);
  REQUIRE(dbg != desc.peripherals.end());
  for(auto& reg : dbg->registers)
  {
    std::erase_if(
        reg.fields, [](popcorn::field_desc const& field) { return field.name == "DBG_TIM1_STOP"; });
  }
  stm32_fixture f{desc, "tim_ll", popcorn::testing::stm32_options()};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("tim_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("tim_ll", "idle"));
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  bus b{f};
  b.set("RCC", "RCC_APB2ENR", "TIM1EN", 1);
  b.write("TIM1", "TIM1_EGR", 1);
  b.set("TIM1", "TIM1_CR1", "CEN", 1);
  f.target->hold_core(true);
  f.target->hold_core(false);
  f.target->hold_core(true);
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000},
                     .until_time = vtime{f.target->virtual_clock().time().ps + 1000 * tim1_tick}})
          .reason
      == stop_reason::time);
  CHECK((b.read("TIM1", "TIM1_CNT") & 0xffffu) >= 1000);
  auto const warnings = std::ranges::count_if(logged, [](core::log_record const& record) {
    return record.kind == core::log_kind::model_warning and record.peripheral == "TIM1";
  });
  CHECK(warnings == 1);
}

TEST_CASE("a frozen TIM1 with updates due during the hold takes none until the release")
{
  held_tim t{1'000'000'000};
  t.d.b.write("TIM1", "TIM1_ARR", 99);
  t.d.begin();
  popcorn::testing::set_debug_freeze(t.d.f, "DBG_TIM1_STOP", true);
  t.to(1050);
  t.d.b.write("TIM1", "TIM1_SR", 0);
  t.d.f.target->hold_core(true);
  t.to(3050);
  CHECK(t.d.count() == 50);
  t.to(6050);
  CHECK(t.d.flag("UIF") == 0);
  CHECK(t.d.count() == 50);
  t.d.f.target->hold_core(false);
  t.to(6099);
  CHECK(t.d.flag("UIF") == 0);
  t.to(6100);
  CHECK(t.d.flag("UIF") == 1);
  CHECK(t.d.count() == 0);
}

TEST_CASE("an update held at a barrier when the core is held raises its DMA request at the hold")
{
  direct      t{with_recorder()};
  auto* const dma = t.f.model_of<recorder>("GPDMA1");
  REQUIRE(dma != nullptr);
  t.b.write("TIM1", "TIM1_ARR", 9);
  t.begin();
  t.b.write("TIM1", "TIM1_DIER", 1u << 8);
  popcorn::testing::set_debug_freeze(t.f, "DBG_TIM1_STOP", true);
  auto const h      = vtime{t.start + 10 * tim1_tick};
  auto const result = t.f.run(
      {.max_instructions = icount{1'000'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until_time       = h,
       .hold_from_until  = true});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::time);
  CHECK(dma->levels.empty());
  t.f.target->hold_core(true);
  CHECK(
      dma->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {2, true}
  });
}

TEST_CASE("a hold on a machine without DBGMCU warns once for the machine, not per model")
{
  auto desc = popcorn::testing::u575();
  std::erase_if(
      desc.peripherals, [](popcorn::peripheral_desc const& unit) { return unit.name == "DBGMCU"; });
  stm32_fixture f{desc, "tim_ll", popcorn::testing::stm32_options()};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("tim_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("tim_ll", "idle"));
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  bus b{f};
  b.set("RCC", "RCC_APB2ENR", "TIM1EN", 1);
  b.set("RCC", "RCC_APB1ENR1", "TIM2EN", 1);
  b.write("TIM1", "TIM1_EGR", 1);
  b.set("TIM1", "TIM1_CR1", "CEN", 1);
  b.write("TIM2", "EGR", 1);
  b.set("TIM2", "CR1", "CEN", 1);
  b.write("IWDG", "KR", 0xCCCC);
  f.target->hold_core(true);
  f.target->hold_core(false);
  f.target->hold_core(true);
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000},
                     .until_time = vtime{f.target->virtual_clock().time().ps + 1000 * tim1_tick}})
          .reason
      == stop_reason::time);
  CHECK((b.read("TIM1", "TIM1_CNT") & 0xffffu) >= 1000);
  auto const warnings = std::ranges::count_if(logged, [](core::log_record const& record) {
    return record.kind == core::log_kind::model_warning and record.message.contains("no DBGMCU");
  });
  CHECK(warnings == 1);
  CHECK(
      std::ranges::count_if(
          logged,
          [](core::log_record const& record) {
            return record.kind == core::log_kind::model_warning;
          })
      == 1);
}

TEST_CASE("slave edges reaching a frozen TIM3 apply at the time of the hold")
{
  auto const hold = [](slaves& t, std::uint64_t from, std::uint64_t to) {
    t.at(from);
    popcorn::testing::set_debug_freeze(t.f, "DBG_TIM3_STOP", true);
    t.f.target->hold_core(true);
    t.at(to);
    t.f.target->hold_core(false);
  };
  SUBCASE("reset mode")
  {
    slaves t;
    t.base("TIM1", 0, 9);
    t.master("TIM1", master_update);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_reset, ts_itr0));
    t.enable("TIM3");
    t.enable("TIM1");
    t.at(103);
    CHECK(t.count("TIM3") == 3);
    hold(t, 103, 147);
    CHECK(t.count("TIM3") == 0);
    t.at(149);
    CHECK(t.count("TIM3") == 2);
  }
  SUBCASE("trigger mode")
  {
    slaves t;
    t.base("TIM1", 0, 9);
    t.master("TIM1", master_update);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_trigger, ts_itr0));
    t.enable("TIM1");
    hold(t, 5, 47);
    CHECK(t.bits("TIM3", "CR1", "CEN") == 1);
    CHECK(t.count("TIM3") == 0);
    t.at(49);
    CHECK(t.count("TIM3") == 2);
  }
  SUBCASE("external clock mode 1")
  {
    slaves t;
    t.base("TIM1", 0, 9);
    t.master("TIM1", master_update);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_external, ts_itr0));
    t.enable("TIM3");
    t.enable("TIM1");
    hold(t, 103, 147);
    CHECK(t.count("TIM3") == 14);
    t.at(149);
    CHECK(t.count("TIM3") == 14);
    t.at(155);
    CHECK(t.count("TIM3") == 15);
  }
  SUBCASE("gated mode")
  {
    slaves t;
    t.base("TIM1", 0, 0xffff);
    t.master("TIM1", master_enable);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "SMCR", smcr(sms_gated, ts_itr0));
    t.enable("TIM3");
    t.at(100);
    popcorn::testing::set_debug_freeze(t.f, "DBG_TIM3_STOP", true);
    t.f.target->hold_core(true);
    t.at(120);
    t.enable("TIM1");
    t.at(147);
    CHECK(t.count("TIM3") == 0);
    t.f.target->hold_core(false);
    t.at(150);
    CHECK(t.count("TIM3") == 3);
  }
}

TEST_CASE("a frozen reset slave dates its TRGO at the current time for a sink that runs on")
{
  slaves t;
  t.base("TIM1", 0, 9);
  t.master("TIM1", master_update);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "SMCR", smcr(sms_reset, ts_itr0));
  t.master("TIM3", master_update);
  t.base("TIM4", 0, 0xffff);
  t.write("TIM4", "SMCR", smcr(sms_reset, ts_itr2));
  t.enable("TIM4");
  t.enable("TIM3");
  t.enable("TIM1");
  t.at(103);
  CHECK(t.count("TIM4") == 3);
  popcorn::testing::set_debug_freeze(t.f, "DBG_TIM3_STOP", true);
  t.f.target->hold_core(true);
  t.at(147);
  CHECK(t.count("TIM3") == 0);
  CHECK(t.count("TIM4") == 7);
  t.f.target->hold_core(false);
  t.at(149);
  CHECK(t.count("TIM3") == 2);
  CHECK(t.count("TIM4") == 9);
}

TEST_CASE(
    "a frozen TIM3 switched to MMS 001 during the hold opens a running gated sink at the write")
{
  slaves t;
  t.base("TIM3", 0, 0xffff);
  t.base("TIM4", 0, 0xffff);
  t.write("TIM4", "SMCR", smcr(sms_gated, ts_itr2));
  t.enable("TIM4");
  t.enable("TIM3");
  t.at(100);
  CHECK(t.count("TIM4") == 0);
  popcorn::testing::set_debug_freeze(t.f, "DBG_TIM3_STOP", true);
  t.f.target->hold_core(true);
  t.at(120);
  t.master("TIM3", master_enable);
  t.at(147);
  CHECK(t.count("TIM3") == 100);
  CHECK(t.count("TIM4") == 27);
  t.f.target->hold_core(false);
}

TEST_CASE("a frozen TIM5 burst is finished by a GPDMA channel that runs on, then resumes on phase")
{
  constexpr popcorn::address words = 0x2001'0000;
  auto const                 setup = [&](slaves& t) {
    t.b.set("RCC", "RCC_AHB1ENR", "GPDMA1EN", 1);
    for(std::uint32_t index = 0; index < 9; ++index)
    {
      REQUIRE(t.f.target->mem().write(words + 4 * index, 4, index + 1));
    }
    auto* const controller = dynamic_cast<stm32::dma_controller*>(t.f.target->model("GPDMA1"));
    REQUIRE(controller != nullptr);
    auto const request = controller->request_of("tim5_upd_dma");
    REQUIRE(request.has_value());
    t.b.set("GPDMA1", "GPDMA_C0TR1", "SDW_LOG2", 2);
    t.b.set("GPDMA1", "GPDMA_C0TR1", "DDW_LOG2", 2);
    t.b.set("GPDMA1", "GPDMA_C0TR1", "SINC", 1);
    t.b.set("GPDMA1", "GPDMA_C0TR2", "REQSEL", *request);
    t.b.write("GPDMA1", "GPDMA_C0BR1", 36);
    t.b.write("GPDMA1", "GPDMA_C0SAR", static_cast<std::uint32_t>(words));
    t.b.write("GPDMA1", "GPDMA_C0DAR", static_cast<std::uint32_t>(t.b.at("TIM5", "DMAR")));
    t.b.set("GPDMA1", "GPDMA_C0CR", "EN", 1);
    t.base("TIM5", 0, 99);
    t.write("TIM5", "DCR", dcr(13, 3, dbss_update));
    t.write("TIM5", "DIER", 1u << 8);
    t.enable("TIM5");
  };
  auto const seen = [](slaves const& t) {
    return std::vector<std::uint32_t>{
        t.count("TIM5"), t.read("TIM5", "CCR1"), t.read("TIM5", "CCR2"), t.read("TIM5", "CCR3"),
        t.b.bits("GPDMA1", "GPDMA_C0BR1", "BNDT")};
  };
  slaves twin;
  setup(twin);
  slaves t;
  setup(t);
  t.at(105);
  REQUIRE(seen(t)[1] == 1);
  REQUIRE(seen(t)[3] == 0);
  popcorn::testing::set_debug_freeze(t.f, "DBG_TIM5_STOP", true);
  t.f.target->hold_core(true);
  t.at(505);
  CHECK(seen(t) == std::vector<std::uint32_t>{5, 1, 2, 3, 24});
  t.f.target->hold_core(false);
  for(std::uint64_t tick : {550u, 615u, 705u, 815u})
  {
    t.at(tick);
    twin.at(tick - 400);
    CHECK(seen(t) == seen(twin));
  }
  CHECK(seen(t) == std::vector<std::uint32_t>{15, 7, 8, 9, 0});
}

TEST_CASE("a pin edge due at a TIM update instant applies after it however early it was posted")
{
  std::vector<std::uint32_t> seen;
  for(auto const early : {true, false})
  {
    CAPTURE(early);
    direct d;
    d.b.set("RCC", "RCC_AHB2ENR1", "GPIOCEN", 1);
    d.b.set("GPIOC", "GPIO_MODER", "MODE13", 0);
    auto* const pc = d.f.model_of<stm32::gpio_port>("GPIOC");
    REQUIRE(pc != nullptr);
    d.b.write("TIM1", "TIM1_ARR", 99);
    auto const    update  = d.f.target->virtual_clock().time().ps + 100 * 250'000;
    std::uint32_t flagged = 2;
    auto          sub =
        pc->on_change([&](std::uint8_t, bool) { flagged = d.f.field("TIM1", "TIM1_SR", "UIF"); });
    if(early)
    {
      pc->drive_at(13, true, vtime{update});
    }
    d.begin();
    REQUIRE(d.start + 100 * 250'000 == update);
    if(not early)
    {
      pc->drive_at(13, true, vtime{update});
    }
    d.at(101);
    seen.push_back(flagged);
  }
  CHECK(seen == std::vector<std::uint32_t>{1, 1});
}

namespace
{
constexpr std::uint64_t edge_gap = 10 * us;

struct inputs : slaves
{
  stm32::gpio_port* porta{};
  std::uint64_t     next{};
  bool              ti1{};
  bool              ti2{};

  explicit inputs(popcorn::part_description desc = u575()) : slaves{std::move(desc)}
  {
    b.set("RCC", "RCC_AHB2ENR1", "GPIOAEN", 1);
    porta = f.model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(porta != nullptr);
    next = start;
  }

  void route(std::uint8_t pin, std::uint8_t af) const
  {
    auto const n = std::to_string(pin);
    b.set("GPIOA", "GPIO_MODER", "MODE" + n, 2);
    b.set("GPIOA", pin < 8 ? "GPIO_AFRL" : "GPIO_AFRH", "AFSEL" + n, af);
  }

  void encoder(std::uint32_t mode, std::uint32_t reload = 0xffff) const
  {
    route(6, 2);
    route(7, 2);
    base("TIM3", 0, reload);
    write("TIM3", "CCMR1_Input", 0x0101);
    write("TIM3", "SMCR", smcr(mode, 0));
    enable("TIM3");
  }

  void edge(std::uint8_t pin, bool level)
  {
    next += edge_gap;
    porta->drive_at(pin, level, vtime{next});
    (pin == 6 ? ti1 : ti2) = level;
  }

  void quadrature(int steps)
  {
    for(int step = 0; step < (steps < 0 ? -steps : steps); ++step)
    {
      auto const forward = steps > 0;
      if(ti1 == ti2)
      {
        forward ? edge(6, not ti1) : edge(7, not ti2);
      }
      else
      {
        forward ? edge(7, not ti2) : edge(6, not ti1);
      }
    }
  }

  void settle() const
  {
    auto const until = vtime{next + edge_gap};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = until}).reason
        == stop_reason::time);
  }
};
}

TEST_CASE("TIM3 encoder mode 3 on PA6 and PA7 counts 40 edges forward then 10 back, DIR follows")
{
  SUBCASE("no wrap")
  {
    inputs t;
    t.porta->drive(6, false);
    t.porta->drive(7, false);
    t.encoder(0b0011);
    t.quadrature(40);
    t.settle();
    CHECK(t.count("TIM3") == 40);
    CHECK(t.bits("TIM3", "CR1", "DIR") == 0);
    CHECK(t.bits("TIM3", "SR", "DIRF") == 0);
    t.quadrature(-10);
    t.settle();
    CHECK(t.count("TIM3") == 30);
    CHECK(t.bits("TIM3", "CR1", "DIR") == 1);
    CHECK(t.bits("TIM3", "SR", "DIRF") == 1);
    CHECK(t.bits("TIM3", "SR", "UIF") == 0);
  }
  SUBCASE("wrapping at ARR 24")
  {
    inputs t;
    t.encoder(0b0011, 24);
    t.quadrature(40);
    t.settle();
    CHECK(t.count("TIM3") == 15);
    CHECK(t.bits("TIM3", "SR", "UIF") == 1);
    t.write("TIM3", "SR", 0);
    t.quadrature(-16);
    t.settle();
    CHECK(t.count("TIM3") == 24);
    CHECK(t.bits("TIM3", "SR", "UIF") == 1);
  }
}

TEST_CASE("each quadrature encoder mode counts its edges of 8 forward and 8 backward steps")
{
  struct expected
  {
    std::uint32_t mode;
    std::uint32_t forward;
  };
  for(auto const [mode, forward] : {
          expected{0b0001, 4},
          expected{0b0010, 4},
          expected{0b0011, 8},
          expected{0b1110, 2},
          expected{0b1111, 2}
  })
  {
    CAPTURE(mode);
    inputs t;
    t.encoder(mode);
    t.write("TIM3", "CNT", 100);
    t.quadrature(8);
    t.settle();
    CHECK(t.count("TIM3") == 100 + forward);
    t.quadrature(-8);
    t.settle();
    CHECK(t.count("TIM3") == 100);
  }
}

TEST_CASE("CC1P inverts tim_ti1fp1, reversing the quadrature count")
{
  inputs t;
  t.encoder(0b0011);
  t.write("TIM3", "CCER", 1u << 1);
  t.write("TIM3", "CNT", 100);
  t.quadrature(8);
  t.settle();
  CHECK(t.count("TIM3") == 92);
}

TEST_CASE("clock plus direction and directional clock modes count TI2 or each line")
{
  SUBCASE("1010 counts both TI2 edges, up while TI1 is high")
  {
    inputs t;
    t.encoder(0b1010);
    t.write("TIM3", "CNT", 100);
    t.edge(6, true);
    for(int pulse = 0; pulse < 3; ++pulse)
    {
      t.edge(7, true);
      t.edge(7, false);
    }
    t.edge(6, false);
    t.edge(7, true);
    t.settle();
    CHECK(t.count("TIM3") == 105);
    CHECK(t.bits("TIM3", "CR1", "DIR") == 1);
  }
  SUBCASE("1011 counts TI2 rising edges only")
  {
    inputs t;
    t.encoder(0b1011);
    t.write("TIM3", "CNT", 100);
    t.edge(6, true);
    for(int pulse = 0; pulse < 3; ++pulse)
    {
      t.edge(7, true);
      t.edge(7, false);
    }
    t.settle();
    CHECK(t.count("TIM3") == 103);
  }
  SUBCASE("1100 counts down on TI1 and up on TI2 edges while the other idles high")
  {
    inputs t;
    t.porta->drive(6, true);
    t.porta->drive(7, true);
    t.ti1 = t.ti2 = true;
    t.encoder(0b1100);
    t.write("TIM3", "CNT", 100);
    for(int pulse = 0; pulse < 3; ++pulse)
    {
      t.edge(7, false);
      t.edge(7, true);
    }
    t.edge(6, false);
    t.edge(6, true);
    t.settle();
    CHECK(t.count("TIM3") == 104);
    CHECK(t.bits("TIM3", "CR1", "DIR") == 1);
  }
  SUBCASE("1101 counts falling edges only")
  {
    inputs t;
    t.porta->drive(6, true);
    t.porta->drive(7, true);
    t.ti1 = t.ti2 = true;
    t.encoder(0b1101);
    t.write("TIM3", "CNT", 100);
    for(int pulse = 0; pulse < 3; ++pulse)
    {
      t.edge(7, false);
      t.edge(7, true);
    }
    t.settle();
    CHECK(t.count("TIM3") == 103);
  }
}

TEST_CASE("a TI1SEL other than 0 leaves the encoder without TI1 edges and logs once")
{
  inputs t;
  t.encoder(0b0001);
  t.write("TIM3", "TISEL", 1);
  CHECK(t.warnings() == 1);
  CHECK(t.logged.back().message.contains("TI1SEL"));
  t.write("TIM3", "TISEL", 2);
  CHECK(t.warnings() == 1);
  t.quadrature(8);
  t.settle();
  CHECK(t.count("TIM3") == 0);
}

TEST_CASE("an encoder ignores pin edges while CEN is 0 and a pin not in its AF")
{
  inputs t;
  t.encoder(0b0001);
  t.enable("TIM3", false);
  t.quadrature(4);
  t.settle();
  CHECK(t.count("TIM3") == 0);
  t.enable("TIM3");
  t.b.set("GPIOA", "GPIO_AFRL", "AFSEL6", 1);
  t.quadrature(4);
  t.settle();
  CHECK(t.count("TIM3") == 0);
  t.b.set("GPIOA", "GPIO_AFRL", "AFSEL6", 2);
  t.quadrature(4);
  t.settle();
  CHECK(t.count("TIM3") == 2);
}

TEST_CASE("a snapshot between encoder edges restores the count and the pending edges")
{
  inputs t;
  t.encoder(0b0011);
  t.quadrature(6);
  auto const mid = t.next - 3 * edge_gap + edge_gap / 2;
  REQUIRE(
      t.f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{mid}}).reason
      == stop_reason::time);
  CHECK(t.count("TIM3") == 3);
  auto const saved = t.f.target->snapshot();
  t.settle();
  CHECK(t.count("TIM3") == 6);
  t.f.target->restore(saved);
  CHECK(t.count("TIM3") == 3);
  t.settle();
  CHECK(t.count("TIM3") == 6);
}

TEST_CASE("a pin entering its timer AF while high gives the timer a rising edge")
{
  inputs t;
  t.porta->drive(6, true);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CCMR1_Input", 0x0101);
  t.write("TIM3", "SMCR", smcr(0b0011, 0));
  t.enable("TIM3");
  t.route(7, 2);
  CHECK(t.count("TIM3") == 0);
  t.route(6, 2);
  CHECK(t.count("TIM3") == 1);
  t.b.set("GPIOA", "GPIO_MODER", "MODE6", 0);
  CHECK(t.count("TIM3") == 0);
}

TEST_CASE("CR2.TI1S feeds tim_ti1 with TI1 xor TI2 xor TI3")
{
  inputs t;
  t.encoder(0b0001);
  t.write("TIM3", "CR2", 1u << 7);
  t.edge(7, true);
  t.edge(7, false);
  t.settle();
  CHECK(t.count("TIM3") == 2);
  t.write("TIM3", "CR2", 0);
  t.edge(7, true);
  t.edge(7, false);
  t.settle();
  CHECK(t.count("TIM3") == 2);
}

TEST_CASE("after a restore, routing a pin already high into its timer AF gives the timer an edge")
{
  inputs t;
  t.porta->drive(6, true);
  t.b.set("GPIOA", "GPIO_MODER", "MODE6", 0);
  t.b.set("GPIOA", "GPIO_AFRL", "AFSEL6", 2);
  t.route(7, 2);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CCMR1_Input", 0x0101);
  t.write("TIM3", "SMCR", smcr(0b0011, 0));
  t.enable("TIM3");
  auto const saved = t.f.target->snapshot();
  t.b.set("GPIOA", "GPIO_MODER", "MODE6", 2);
  CHECK(t.count("TIM3") == 1);
  t.f.target->restore(saved);
  CHECK(t.count("TIM3") == 0);
  t.b.set("GPIOA", "GPIO_MODER", "MODE6", 2);
  CHECK(t.count("TIM3") == 1);
  t.b.set("GPIOA", "GPIO_MODER", "MODE6", 3);
  CHECK(t.count("TIM3") == 0);
}

namespace
{
void pulses(inputs& t, std::uint8_t pin, int count)
{
  for(int pulse = 0; pulse < count; ++pulse)
  {
    t.edge(pin, true);
    t.edge(pin, false);
  }
}

auto etr_clocked(std::uint32_t smcr_word) -> inputs
{
  inputs t;
  t.route(5, 2);
  t.base("TIM2", 0, 0xffff'ffff);
  t.write("TIM2", "SMCR", smcr_word);
  t.enable("TIM2");
  return t;
}
}

TEST_CASE("TIM2 in external clock mode 2 counts ETR rising edges through ETPS and ETP")
{
  constexpr std::uint32_t ece = 1u << 14;
  for(auto const& [prescale, counted] : {
          std::pair{0u, 12u},
          {1u, 6u },
          {2u, 3u },
          {3u, 1u }
  })
  {
    CAPTURE(prescale);
    auto t = etr_clocked(ece | (prescale << 12));
    pulses(t, 5, 12);
    t.settle();
    CHECK(t.count("TIM2") == counted);
    CHECK(t.bits("TIM2", "SR", "TIF") == 0);
  }
  SUBCASE("ETP counts falling edges")
  {
    auto t = etr_clocked(ece | (1u << 15));
    t.edge(5, true);
    t.settle();
    CHECK(t.count("TIM2") == 0);
    t.edge(5, false);
    t.settle();
    CHECK(t.count("TIM2") == 1);
  }
  SUBCASE("ETRSEL other than 0 reads low")
  {
    auto t = etr_clocked(ece);
    t.write("TIM2", "AF1", 1u << 14);
    pulses(t, 5, 4);
    t.settle();
    CHECK(t.count("TIM2") == 0);
  }
}

TEST_CASE("external clock mode 1 on ETRF, TS 00111, counts each ETR rising edge once")
{
  for(auto const ece : {0u, 1u << 14})
  {
    CAPTURE(ece);
    auto t = etr_clocked(ece | smcr(sms_external, 7));
    pulses(t, 5, 5);
    t.settle();
    CHECK(t.count("TIM2") == 5);
    CHECK(t.bits("TIM2", "SR", "TIF") == 1);
  }
}

TEST_CASE("trigger mode on TI1FP1, TS 00101, starts the counter at the pin edge")
{
  inputs t;
  t.route(6, 2);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CCMR1_Input", 0x0001);
  t.write("TIM3", "SMCR", smcr(sms_trigger, 5));
  t.edge(6, true);
  auto const edge = t.next;
  t.settle();
  CHECK(t.bits("TIM3", "CR1", "CEN") == 1);
  CHECK(t.bits("TIM3", "SR", "TIF") == 1);
  CHECK(t.count("TIM3") == (t.next + edge_gap - edge) / tick_ps);
}

TEST_CASE("TI1FP1 polarity picks the active edge, both edges with CC1NP and CC1P")
{
  struct expected
  {
    std::uint32_t ccer;
    std::uint32_t ticks;
  };
  for(auto const [ccer, ticks] : {
          expected{0b0000, 2},
          expected{0b0010, 2},
          expected{0b1010, 4}
  })
  {
    CAPTURE(ccer);
    inputs t;
    t.route(6, 2);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "CCMR1_Input", 0x0001);
    t.write("TIM3", "CCER", ccer);
    t.write("TIM3", "SMCR", smcr(sms_external, 5));
    t.enable("TIM3");
    pulses(t, 6, 2);
    t.settle();
    CHECK(t.count("TIM3") == ticks);
  }
}

TEST_CASE("reset mode on TI2FP2 and gated mode on TI1FP1 follow the pins")
{
  SUBCASE("reset")
  {
    inputs t;
    t.route(7, 2);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "CCMR1_Input", 0x0100);
    t.write("TIM3", "SMCR", smcr(sms_reset, 6));
    t.enable("TIM3");
    t.next = t.start + 100 * tick_ps;
    t.edge(7, true);
    auto const edge = t.next;
    t.settle();
    CHECK(t.count("TIM3") == (t.next + edge_gap - edge) / tick_ps);
  }
  SUBCASE("gated")
  {
    inputs t;
    t.route(6, 2);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "CCMR1_Input", 0x0001);
    t.write("TIM3", "SMCR", smcr(sms_gated, 5));
    t.enable("TIM3");
    t.edge(6, true);
    t.edge(6, false);
    t.settle();
    CHECK(t.count("TIM3") == edge_gap / tick_ps);
    t.write("TIM3", "SR", 0);
    t.porta->drive(6, true);
    CHECK(t.bits("TIM3", "SR", "TIF") == 1);
  }
}

TEST_CASE("TS 00100 triggers on every TI1F transition")
{
  inputs t;
  t.route(6, 2);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CCMR1_Input", 0x0001);
  t.write("TIM3", "CCER", 0b0010);
  t.write("TIM3", "SMCR", smcr(sms_external, 4));
  t.enable("TIM3");
  pulses(t, 6, 3);
  t.settle();
  CHECK(t.count("TIM3") == 6);
}

TEST_CASE("external clock mode 2 counts only while a gate on TI1FP1 is open")
{
  constexpr std::uint32_t ece = 1u << 14;
  auto                    t   = etr_clocked(ece | smcr(sms_gated, 5));
  t.route(0, 1);
  t.write("TIM2", "CCMR1_Input", 0x0001);
  pulses(t, 5, 4);
  t.settle();
  CHECK(t.count("TIM2") == 0);
  t.edge(0, true);
  pulses(t, 5, 4);
  t.settle();
  CHECK(t.count("TIM2") == 4);
  t.edge(0, false);
  pulses(t, 5, 4);
  t.settle();
  CHECK(t.count("TIM2") == 4);
}

TEST_CASE("with ECE, external clock mode 1 on TI1FP1 counts tim_etrf only")
{
  constexpr std::uint32_t ece = 1u << 14;
  auto                    t   = etr_clocked(ece | smcr(sms_external, 5));
  t.route(0, 1);
  t.write("TIM2", "CCMR1_Input", 0x0001);
  pulses(t, 0, 3);
  pulses(t, 5, 5);
  t.settle();
  CHECK(t.count("TIM2") == 5);
  CHECK(t.bits("TIM2", "SR", "TIF") == 1);
}

TEST_CASE("an ETP write that flips tim_etr_in is an edge")
{
  constexpr std::uint32_t ece = 1u << 14;
  auto                    t   = etr_clocked(ece);
  t.write("TIM2", "SMCR", ece | (1u << 15));
  CHECK(t.count("TIM2") == 1);
  t.edge(5, true);
  t.settle();
  CHECK(t.count("TIM2") == 1);
  t.edge(5, false);
  t.settle();
  CHECK(t.count("TIM2") == 2);
}

TEST_CASE("TS 00111 on TIM15, which has no ETR, logs once")
{
  slaves t;
  t.base("TIM15", 0, 0xffff);
  t.write("TIM15", "SMCR", smcr(sms_trigger, 7));
  CHECK(t.warnings() == 1);
  CHECK(t.logged.back().message.contains("not modelled"));
}

TEST_CASE("TS 00111 drives reset, trigger and gated modes from tim_etrf")
{
  SUBCASE("trigger")
  {
    auto t = etr_clocked(smcr(sms_trigger, 7));
    t.enable("TIM2", false);
    t.edge(5, true);
    auto const edge = t.next;
    t.settle();
    CHECK(t.bits("TIM2", "CR1", "CEN") == 1);
    CHECK(t.count("TIM2") == (t.next + edge_gap - edge) / tick_ps);
  }
  SUBCASE("reset")
  {
    auto t = etr_clocked(smcr(sms_reset, 7));
    t.next = t.start + 100 * tick_ps;
    t.edge(5, true);
    auto const edge = t.next;
    t.settle();
    CHECK(t.count("TIM2") == (t.next + edge_gap - edge) / tick_ps);
  }
  SUBCASE("gated")
  {
    auto t = etr_clocked(smcr(sms_gated, 7));
    t.edge(5, true);
    t.edge(5, false);
    t.settle();
    CHECK(t.count("TIM2") == edge_gap / tick_ps);
  }
}

TEST_CASE("the ETPS count survives a snapshot")
{
  constexpr std::uint32_t ece = 1u << 14;
  auto                    t   = etr_clocked(ece | (3u << 12));
  pulses(t, 5, 3);
  t.settle();
  auto const saved = t.f.target->snapshot();
  t.f.target->restore(saved);
  pulses(t, 5, 5);
  t.settle();
  CHECK(t.count("TIM2") == 1);
}

TEST_CASE("after a restore, routing an ETR pin already high gives external clock mode 2 an edge")
{
  constexpr std::uint32_t ece = 1u << 14;
  inputs                  t;
  t.porta->drive(5, true);
  t.b.set("GPIOA", "GPIO_AFRL", "AFSEL5", 2);
  t.base("TIM2", 0, 0xffff'ffff);
  t.write("TIM2", "SMCR", ece);
  t.enable("TIM2");
  auto const saved = t.f.target->snapshot();
  t.f.target->restore(saved);
  t.b.set("GPIOA", "GPIO_MODER", "MODE5", 2);
  CHECK(t.count("TIM2") == 1);
}

TEST_CASE("gated on tim_etrf with ETPS /2 stays open for half the divided period")
{
  auto t = etr_clocked(smcr(sms_gated, 7) | (1u << 12));
  for(int pulse = 0; pulse < 3; ++pulse)
  {
    t.next += 15 * tick_ps;
    t.porta->drive_at(5, true, vtime{t.next});
    t.next += 5 * tick_ps;
    t.porta->drive_at(5, false, vtime{t.next});
  }
  t.settle();
  CHECK(t.count("TIM2") == 20);
}

TEST_CASE("on a busy core a pin edge reaches the timer at its own time, not at the dispatch")
{
  constexpr address loop = 0x2000'8000;
  inputs            t;
  for(std::uint32_t nop = 0; nop < 32; ++nop)
  {
    REQUIRE(t.f.target->mem().write(loop + 2 * nop, 2, 0xbf00).has_value());
  }
  REQUIRE(t.f.target->mem().write(loop + 64, 2, 0xe7de).has_value());
  t.f.set("pc", loop);
  t.route(6, 2);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CCMR1_Input", 0x0001);
  t.write("TIM3", "SMCR", smcr(sms_trigger, 5));
  auto const edge = t.start + 10 * us + 2 * tick_ps + tick_ps / 3;
  t.porta->drive_at(6, true, vtime{edge});
  REQUIRE(
      t.f.run_to_stop(
             {.max_instructions = icount{1'000'000'000}, .until_time = vtime{edge + 30 * us}})
          .reason
      == stop_reason::time);
  auto const now = t.f.target->virtual_clock().time().ps;
  CHECK(t.count("TIM3") == (now - edge) / tick_ps);
}

namespace
{
auto capturing(std::uint32_t ccmr, std::uint32_t ccer = 0b0001) -> inputs
{
  inputs t;
  t.route(6, 2);
  t.route(7, 2);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CCMR1_Input", ccmr);
  t.write("TIM3", "CCER", ccer);
  t.enable("TIM3");
  return t;
}

auto ticks_at(inputs const& t, std::uint64_t at) -> std::uint32_t
{
  return static_cast<std::uint32_t>((at - t.start) / tick_ps);
}
}

TEST_CASE(
    "CH1 captures the count at each rising edge of TI1, CC1OF on an uncleared one, debugger reads "
    "keep CC1IF")
{
  auto t = capturing(0x0001);
  t.edge(6, true);
  auto const first = t.next;
  t.edge(6, false);
  t.settle();
  CHECK(t.bits("TIM3", "SR", "CC1IF") == 1);
  CHECK(t.bits("TIM3", "SR", "CC1OF") == 0);
  CHECK(t.read("TIM3", "CCR1") == ticks_at(t, first));
  CHECK(t.bits("TIM3", "SR", "CC1IF") == 1);
  t.write("TIM3", "SR", 0);
  t.edge(6, true);
  t.edge(6, false);
  t.edge(6, true);
  auto const third = t.next;
  t.settle();
  CHECK(t.bits("TIM3", "SR", "CC1OF") == 1);
  CHECK(t.read("TIM3", "CCR1") == ticks_at(t, third));
}

TEST_CASE("IC1PSC captures every fourth active edge, the count reset by CC1E 0")
{
  auto                       t = capturing(0x0001 | (2u << 2));
  std::vector<std::uint64_t> rising;
  for(int pulse = 0; pulse < 9; ++pulse)
  {
    t.edge(6, true);
    rising.push_back(t.next);
    t.edge(6, false);
    if(pulse == 3)
    {
      t.settle();
      CHECK(t.read("TIM3", "CCR1") == ticks_at(t, rising[3]));
    }
  }
  t.settle();
  CHECK(t.read("TIM3", "CCR1") == ticks_at(t, rising[7]));
  t.write("TIM3", "CCER", 0);
  t.write("TIM3", "CCER", 0b0001);
  for(int pulse = 0; pulse < 3; ++pulse)
  {
    t.edge(6, true);
    t.edge(6, false);
  }
  t.settle();
  CHECK(t.read("TIM3", "CCR1") == ticks_at(t, rising[7]));
}

TEST_CASE("TIM1 IC1PSC on PA8 captures every fourth edge with SVD names and with bare names")
{
  auto const fourth_edge = [](popcorn::part_description desc) {
    inputs t{std::move(desc)};
    t.route(8, 1);
    t.base("TIM1", 0, 0xffff);
    t.write("TIM1", "CCMR1", 0x0001 | (2u << 2));
    t.write("TIM1", "CCER", 0b0001);
    t.enable("TIM1");
    std::vector<std::uint64_t> rising;
    for(int pulse = 0; pulse < 4; ++pulse)
    {
      t.edge(8, true);
      rising.push_back(t.next);
      t.edge(8, false);
      if(pulse == 2)
      {
        t.settle();
        CHECK(t.bits("TIM1", "SR", "CC1IF") == 0);
      }
    }
    t.settle();
    CHECK(t.bits("TIM1", "SR", "CC1IF") == 1);
    CHECK(t.read("TIM1", "CCR1") == ticks_at(t, rising[3]));
  };
  SUBCASE("SVD names, a description written before bare names")
  {
    auto       desc = svd_named_timers();
    auto const tim1 = std::ranges::find(
        desc.peripherals, std::string_view{"TIM1"}, &popcorn::peripheral_desc::name);
    REQUIRE(tim1 != desc.peripherals.end());
    REQUIRE(popcorn::find_register(*tim1, "CCMR1") == nullptr);
    REQUIRE(popcorn::find_register(*tim1, "TIM1_CCMR1") != nullptr);
    fourth_edge(std::move(desc));
  }
  SUBCASE("bare names")
  {
    fourth_edge(u575());
  }
}

TEST_CASE("capture polarity, the neighbour input and CC1E 0")
{
  SUBCASE("falling")
  {
    auto t = capturing(0x0001, 0b0011);
    t.edge(6, true);
    t.edge(6, false);
    auto const fall = t.next;
    t.settle();
    CHECK(t.read("TIM3", "CCR1") == ticks_at(t, fall));
  }
  SUBCASE("both edges")
  {
    auto t = capturing(0x0001 | (1u << 2), 0b1011);
    t.edge(6, true);
    t.edge(6, false);
    auto const fall = t.next;
    t.settle();
    CHECK(t.read("TIM3", "CCR1") == ticks_at(t, fall));
  }
  SUBCASE("CC2S 10 captures TI1 into CCR2")
  {
    auto t = capturing(0x0200, 0b0001'0000);
    t.edge(6, true);
    auto const rise = t.next;
    t.settle();
    CHECK(t.bits("TIM3", "SR", "CC2IF") == 1);
    CHECK(t.read("TIM3", "CCR2") == ticks_at(t, rise));
  }
  SUBCASE("disabled")
  {
    auto t = capturing(0x0001, 0);
    t.edge(6, true);
    t.settle();
    CHECK(t.bits("TIM3", "SR", "CC1IF") == 0);
  }
}

TEST_CASE("IC1F with 8 samples drops a 5-sample pulse and passes a 10-sample one 8 samples late")
{
  constexpr std::uint64_t sample = tick_ps;
  auto                    t      = capturing(0x0001 | (3u << 4));
  t.next += edge_gap;
  t.porta->drive_at(6, true, vtime{t.next});
  t.porta->drive_at(6, false, vtime{t.next + 5 * sample});
  t.next += edge_gap;
  auto const pulse = t.next;
  t.porta->drive_at(6, true, vtime{pulse});
  t.porta->drive_at(6, false, vtime{pulse + 10 * sample});
  t.settle();
  CHECK(t.bits("TIM3", "SR", "CC1OF") == 0);
  CHECK(t.read("TIM3", "CCR1") == ticks_at(t, pulse + 8 * sample));
}

TEST_CASE("ETF delays the ETR edges ECE counts by its sample count on fDTS")
{
  constexpr std::uint32_t ece = 1u << 14;
  auto                    t   = etr_clocked(ece | (4u << 8));
  t.write("TIM2", "CR1", t.read("TIM2", "CR1") | (1u << 8));
  t.edge(5, true);
  auto const rise = t.next;
  auto const due  = rise + 6 * 4 * tick_ps;
  REQUIRE(
      t.f.run_to_stop(
             {.max_instructions = icount{1'000'000'000}, .until_time = vtime{due - tick_ps}})
          .reason
      == stop_reason::time);
  CHECK(t.count("TIM2") == 0);
  t.settle();
  CHECK(t.count("TIM2") == 1);
}

TEST_CASE("a snapshot with a filtered edge pending restores it")
{
  auto t = capturing(0x0001 | (3u << 4));
  t.edge(6, true);
  auto const rise = t.next;
  REQUIRE(
      t.f.run_to_stop(
             {.max_instructions = icount{1'000'000'000}, .until_time = vtime{rise + tick_ps}})
          .reason
      == stop_reason::time);
  auto const saved = t.f.target->snapshot();
  t.settle();
  auto const captured = t.read("TIM3", "CCR1");
  CHECK(captured == ticks_at(t, rise + 8 * tick_ps));
  t.f.target->restore(saved);
  CHECK(t.bits("TIM3", "SR", "CC1IF") == 0);
  t.settle();
  CHECK(t.read("TIM3", "CCR1") == captured);
}

TEST_CASE("TIM3RST drops a pending filtered edge and takes the held pin level with no capture")
{
  auto t = capturing(0x0001 | (3u << 4));
  t.edge(6, true);
  auto const rise = t.next;
  REQUIRE(
      t.f.run_to_stop(
             {.max_instructions = icount{1'000'000'000}, .until_time = vtime{rise + tick_ps}})
          .reason
      == stop_reason::time);
  auto const filtering = [&] {
    std::vector<std::uint64_t> out;
    for(auto const& event : t.f.target->snapshot().data().events.pending)
    {
      if(event.due_ps > rise and event.due_ps <= rise + 8 * tick_ps)
      {
        out.push_back(event.due_ps);
      }
    }
    return out;
  };
  REQUIRE_FALSE(filtering().empty());
  t.b.set("RCC", "RCC_APB1RSTR1", "TIM3RST", 1);
  t.b.set("RCC", "RCC_APB1RSTR1", "TIM3RST", 0);
  CHECK(filtering().empty());
  CHECK(t.read("TIM3", "CCER") == 0);
  t.write("TIM3", "CCMR1_Input", 0x0001);
  t.write("TIM3", "CCER", 0b0001);
  t.enable("TIM3");
  t.settle();
  CHECK(t.bits("TIM3", "SR", "CC1IF") == 0);
}

TEST_CASE("a CCxP write that flips the polarised input is an edge")
{
  SUBCASE("encoder")
  {
    inputs t;
    t.encoder(0b0011);
    t.edge(6, true);
    t.settle();
    t.write("TIM3", "CNT", 100);
    t.write("TIM3", "CCER", 1u << 1);
    CHECK(t.count("TIM3") == 99);
  }
  SUBCASE("trigger on TI1FP1")
  {
    inputs t;
    t.route(6, 2);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "CCMR1_Input", 0x0001);
    t.write("TIM3", "SMCR", smcr(sms_trigger, 5));
    t.write("TIM3", "CCER", 1u << 1);
    CHECK(t.bits("TIM3", "CR1", "CEN") == 1);
    CHECK(t.bits("TIM3", "SR", "TIF") == 1);
  }
  SUBCASE("capture")
  {
    auto t = capturing(0x0001);
    t.write("TIM3", "CCER", 0b0011);
    CHECK(t.bits("TIM3", "SR", "CC1IF") == 1);
  }
}

namespace
{
auto model_read(inputs const& t, std::string_view reg, popcorn::initiator by) -> std::uint32_t
{
  auto&      regs  = t.f.target->peripheral("TIM3");
  auto const found = popcorn::find_register(regs.description(), reg);
  REQUIRE(found != nullptr);
  auto const value = t.f.target->model("TIM3")->on_read(
      regs, found->offset, 4, {.pc = 0, .now = icount{0}, .by = by});
  REQUIRE(value.has_value());
  return *value;
}

void run_to(inputs const& t, std::uint64_t ps)
{
  REQUIRE(
      t.f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
      == stop_reason::time);
}
}

TEST_CASE("ETF passes prescaled ETR pulses as the divider output holds them")
{
  constexpr std::uint32_t ece = 1u << 14;
  auto                    t   = etr_clocked(ece | (3u << 12) | (3u << 8));
  for(int pulse = 0; pulse < 16; ++pulse)
  {
    t.next += 19 * us;
    t.porta->drive_at(5, true, vtime{t.next});
    t.next += us;
    t.porta->drive_at(5, false, vtime{t.next});
  }
  t.settle();
  CHECK(t.count("TIM2") == 2);
}

TEST_CASE(
    "a filtered edge maturing in a debug freeze applies at the hold, a glitch in it stays out")
{
  auto const trigger_on_slow_filter = [](inputs& t) {
    t.route(6, 2);
    t.base("TIM3", 0, 0xffff);
    t.write("TIM3", "CCMR1_Input", 0x0001 | (15u << 4));
    t.write("TIM3", "SMCR", smcr(sms_trigger, 5));
    popcorn::testing::set_debug_freeze(t.f, "DBG_TIM3_STOP", true);
  };
  SUBCASE("edge before the hold")
  {
    inputs t;
    trigger_on_slow_filter(t);
    auto const edge = t.start + 10 * us;
    t.porta->drive_at(6, true, vtime{edge});
    run_to(t, edge + 10 * us);
    t.f.target->hold_core(true);
    run_to(t, edge + 100 * us);
    t.f.target->hold_core(false);
    run_to(t, edge + 200 * us);
    CHECK(t.bits("TIM3", "CR1", "CEN") == 1);
    CHECK(t.count("TIM3") == 100 * us / tick_ps);
  }
  SUBCASE("glitch inside the hold")
  {
    inputs t;
    trigger_on_slow_filter(t);
    run_to(t, t.start + 10 * us);
    t.f.target->hold_core(true);
    auto const glitch = t.start + 110 * us;
    t.porta->drive_at(6, true, vtime{glitch});
    t.porta->drive_at(6, false, vtime{glitch + 5 * us});
    run_to(t, t.start + 1'010 * us);
    t.f.target->hold_core(false);
    run_to(t, t.start + 1'100 * us);
    CHECK(t.bits("TIM3", "CR1", "CEN") == 0);
  }
}

TEST_CASE("a DMA read of CCR1, direct or through DMAR, clears CC1IF, a debugger read does not")
{
  for(auto const burst : {false, true})
  {
    CAPTURE(burst);
    auto t = capturing(0x0001);
    t.write("TIM3", "DCR", 13u);
    t.edge(6, true);
    t.settle();
    REQUIRE(t.bits("TIM3", "SR", "CC1IF") == 1);
    static_cast<void>(model_read(t, burst ? "DMAR" : "CCR1", popcorn::initiator::debugger));
    CHECK(t.bits("TIM3", "SR", "CC1IF") == 1);
    static_cast<void>(model_read(t, burst ? "DMAR" : "CCR1", popcorn::initiator::dma));
    CHECK(t.bits("TIM3", "SR", "CC1IF") == 0);
  }
}

TEST_CASE("a CCxNP write that moves the gated TI1FP1 level is a gate edge")
{
  inputs t;
  t.route(6, 2);
  t.base("TIM3", 0, 0xffff);
  t.write("TIM3", "CCMR1_Input", 0x0001);
  t.write("TIM3", "CCER", 1u << 1);
  t.write("TIM3", "SMCR", smcr(sms_gated, 5));
  t.enable("TIM3");
  run_to(t, t.start + 10 * us);
  auto const counted = t.count("TIM3");
  CHECK(counted > 0);
  t.write("TIM3", "CCER", (1u << 1) | (1u << 3));
  run_to(t, t.start + 20 * us);
  CHECK(t.count("TIM3") == counted);
}

TEST_CASE("a glitching filtered input keeps one filter event queued")
{
  auto       t      = capturing(0x0001 | (15u << 4));
  auto const events = t.f.target->pending_events();
  for(int glitch = 0; glitch < 1000; ++glitch)
  {
    t.next += tick_ps;
    t.porta->drive_at(6, glitch % 2 == 0, vtime{t.next});
  }
  run_to(t, t.next);
  CHECK(t.f.target->pending_events() <= events + 1);
  CHECK(t.bits("TIM3", "SR", "CC1IF") == 0);
}

namespace
{
auto timer_register(popcorn::part_description& desc, std::string_view unit, std::string_view reg)
    -> popcorn::register_desc&
{
  auto const found =
      std::ranges::find(desc.peripherals, std::string{unit}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  auto const named =
      std::ranges::find(found->registers, std::string{reg}, &popcorn::register_desc::name);
  REQUIRE(named != found->registers.end());
  return *named;
}

void drop_field(popcorn::register_desc& reg, std::string_view field)
{
  REQUIRE(
      std::erase_if(reg.fields, [&](popcorn::field_desc const& bits) { return bits.name == field; })
      == 1);
}
}

TEST_CASE("a U575 timer without its renamed SMS_1, TS_1 or 4-bit MMS_1 throws at construction")
{
  for(std::string_view const unit : {"TIM1", "TIM2", "TIM3", "TIM4", "TIM5", "TIM8"})
  {
    CAPTURE(unit);
    auto desc = u575();
    drop_field(timer_register(desc, unit, "CR2"), "MMS_1");
    CHECK_THROWS_AS(stm32_fixture(std::move(desc), "tim_ll"), popcorn::config_error);
  }
  for(std::string_view const field : {"SMS_1", "TS_1"})
  {
    CAPTURE(field);
    auto desc = u575();
    drop_field(timer_register(desc, "TIM2", "SMCR"), field);
    CHECK_THROWS_AS(stm32_fixture(std::move(desc), "tim_ll"), popcorn::config_error);
  }
}

TEST_CASE("a U575 timer whose MMS has 3 bits or none throws when the description says otherwise")
{
  SUBCASE("TIM15 given an MMS_1")
  {
    auto desc = u575();
    timer_register(desc, "TIM15", "CR2").fields.push_back({.name = "MMS_1", .lsb = 25, .width = 1});
    CHECK_THROWS_AS(stm32_fixture(std::move(desc), "tim_ll"), popcorn::config_error);
  }
  SUBCASE("TIM6 without its MMS")
  {
    auto desc = u575();
    drop_field(timer_register(desc, "TIM6", "CR2"), "MMS");
    CHECK_THROWS_AS(stm32_fixture(std::move(desc), "tim_ll"), popcorn::config_error);
  }
  SUBCASE("TIM16 given an MMS")
  {
    auto desc = u575();
    timer_register(desc, "TIM16", "CR2").fields.push_back({.name = "MMS", .lsb = 4, .width = 3});
    CHECK_THROWS_AS(stm32_fixture(std::move(desc), "tim_ll"), popcorn::config_error);
  }
  SUBCASE("the shipped description")
  {
    CHECK_NOTHROW(stm32_fixture(u575(), "tim_ll"));
  }
}
