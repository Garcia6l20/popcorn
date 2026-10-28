#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint32_t exti13_mark = 0xe013;
constexpr std::uint32_t exti0_mark  = 0xe000;
constexpr std::uint32_t resumed     = 0xb3;

auto port(stm32_fixture const& f, std::string_view name) -> stm32::gpio_port&
{
  auto* const found = f.model_of<stm32::gpio_port>(name);
  REQUIRE(found != nullptr);
  return *found;
}

auto at_breakpoint() -> stm32_fixture
{
  stm32_fixture f{u575(), "gpio_ll"};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.log() == std::vector<std::uint32_t>{1, 0, 1, 1, 1});
  return f;
}

auto finished(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f.log();
}

void write(stm32_fixture const& f, std::string_view reg, std::uint32_t value)
{
  auto&      regs  = f.target->peripheral("GPIOA");
  auto const found = popcorn::find_register(regs.description(), reg);
  REQUIRE(found != nullptr);
  REQUIRE(
      f.target->model("GPIOA")
          ->on_write(
              regs, found->offset, 4, regs.peek(found->offset), value, {.pc = 0, .now = icount{0}})
          .has_value());
}

auto read_register(stm32_fixture const& f, std::string_view peripheral, std::string_view reg)
    -> std::uint32_t
{
  auto const& desc  = f.target->peripheral(peripheral).description();
  auto const  found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  auto const value = f.target->mem().read(desc.base + found->offset, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void write_register(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::uint32_t        value)
{
  auto const& desc  = f.target->peripheral(peripheral).description();
  auto const  found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  REQUIRE(f.target->mem().write(desc.base + found->offset, 4, value).has_value());
}

auto count(std::vector<std::uint32_t> const& log, std::uint32_t mark) -> long
{
  return std::ranges::count(log, mark);
}
}

TEST_CASE(
    "LL drives PA5, a BSRR set wins over a reset, PC13 pulls up, a falling edge enters EXTI13")
{
  auto f = at_breakpoint();
  CHECK(port(f, "GPIOA").level(5));
  CHECK(port(f, "GPIOA").mode(5) == stm32::pin_mode::output);
  CHECK(port(f, "GPIOC").mode(13) == stm32::pin_mode::input);
  port(f, "GPIOA").drive(13, true);
  port(f, "GPIOA").drive(13, false);
  CHECK(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 0);
  port(f, "GPIOC").drive(13, false);
  CHECK(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 1);
  auto const log = finished(f);
  CHECK(
      log
      == std::vector<std::uint32_t>{1, 0, 1, 1, 1, exti13_mark, resumed, 1, 1, exti0_mark, 0x5e1});
  CHECK(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 0);
  port(f, "GPIOC").drive(13, {});
  CHECK(port(f, "GPIOC").level(13));
  CHECK(f.field("EXTI", "EXTI_RPR1", "RPIF13") == 0);
  CHECK(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 0);

  auto again = at_breakpoint();
  port(again, "GPIOC").drive(13, false);
  CHECK(finished(again) == log);
}

TEST_CASE("a pulse between two slices is one falling edge and one EXTI13 entry")
{
  auto f = at_breakpoint();
  port(f, "GPIOC").drive(13, false);
  port(f, "GPIOC").drive(13, true);
  CHECK(count(finished(f), exti13_mark) == 1);
}

TEST_CASE("an external drive does not override a push-pull output, it does an open-drain one")
{
  auto  f  = at_breakpoint();
  auto& pa = port(f, "GPIOA");
  CHECK(pa.level(5));
  pa.drive(5, false);
  CHECK(pa.level(5));
  write(f, "GPIO_MODER", (f.target->peripheral("GPIOA").peek(0) & ~(3u << 12)) | (1u << 12));
  write(f, "GPIO_OTYPER", 1u << 6);
  write(f, "GPIO_ODR", (1u << 5) | (1u << 6));
  CHECK_FALSE(pa.level(6));
  pa.drive(6, true);
  CHECK(pa.level(6));
  pa.drive(6, false);
  CHECK_FALSE(pa.level(6));
  write(f, "GPIO_ODR", 1u << 5);
  pa.drive(6, true);
  CHECK_FALSE(pa.level(6));
}

TEST_CASE("listeners hear each level change once, and a destroyed handle stops its listener")
{
  auto                                       f  = at_breakpoint();
  auto&                                      pc = port(f, "GPIOC");
  std::vector<std::pair<std::uint8_t, bool>> first;
  std::vector<std::pair<std::uint8_t, bool>> second;
  auto one = pc.on_change([&](std::uint8_t pin, bool level) { first.emplace_back(pin, level); });
  {
    auto two = pc.on_change([&](std::uint8_t pin, bool level) { second.emplace_back(pin, level); });
    pc.drive(13, false);
    pc.drive(13, false);
  }
  pc.drive(13, true);
  CHECK(
      first
      == std::vector<std::pair<std::uint8_t, bool>>{
          {13, false},
          {13, true }
  });
  CHECK(
      second
      == std::vector<std::pair<std::uint8_t, bool>>{
          {13, false}
  });
  one.reset();
  pc.drive(13, false);
  CHECK(first.size() == 2);
}

TEST_CASE("the LCKR key sequence freezes the pin mode until reset")
{
  auto f = at_breakpoint();
  port(f, "GPIOC").drive(13, false);
  auto const log = finished(f);
  CHECK(log[7] == 1);
  CHECK(log[8] == 1);
  CHECK(f.field("GPIOA", "GPIO_LCKR", "LCKK") == 1);
  f.target->reset();
  CHECK(f.field("GPIOA", "GPIO_LCKR", "LCKK") == 0);
}

TEST_CASE("a snapshot with a pending FPR restores to one handler entry, drives and listeners kept")
{
  auto  f     = at_breakpoint();
  auto& pc    = port(f, "GPIOC");
  int   heard = 0;
  auto  sub   = pc.on_change([&](std::uint8_t, bool) { ++heard; });
  pc.drive(13, false);
  REQUIRE(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 1);
  auto const saved    = f.target->snapshot();
  auto const expected = finished(f);
  CHECK(count(expected, exti13_mark) == 1);
  pc.drive(13, true);
  f.target->restore(saved);
  CHECK_FALSE(pc.level(13));
  CHECK(finished(f) == expected);
  auto const before = heard;
  pc.drive(13, true);
  CHECK(heard == before + 1);
}

TEST_CASE("EXTI keeps a masked line off the NVIC and ignores edges it does not select")
{
  auto       f      = at_breakpoint();
  auto&      exti   = f.target->peripheral("EXTI");
  auto const offset = [&](std::string_view reg) {
    return popcorn::find_register(exti.description(), reg)->offset;
  };
  auto const pending = [&] {
    auto const word = f.target->mem().read(0xe000'e200u, 4);
    REQUIRE(word.has_value());
    return (*word >> 24 & 1u) != 0;
  };
  auto const write_exti = [&](std::string_view reg, std::uint32_t value) {
    REQUIRE(
        f.target->model("EXTI")
            ->on_write(
                exti, offset(reg), 4, exti.peek(offset(reg)), value, {.pc = 0, .now = icount{0}})
            .has_value());
  };
  write_exti("EXTI_IMR1", 0);
  port(f, "GPIOC").drive(13, false);
  CHECK(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 1);
  CHECK_FALSE(pending());
  write_exti("EXTI_IMR1", 1u << 13);
  CHECK(pending());
  write_exti("EXTI_FPR1", 1u << 13);
  exti.poke(offset("EXTI_FTSR1"), 0);
  port(f, "GPIOC").drive(13, true);
  port(f, "GPIOC").drive(13, false);
  CHECK(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 0);
  CHECK(f.field("EXTI", "EXTI_RPR1", "RPIF13") == 0);
}

namespace
{
void write_port(
    stm32_fixture const& f,
    std::string_view     name,
    std::string_view     reg,
    std::uint32_t        value)
{
  auto&      regs  = f.target->peripheral(name);
  auto const found = popcorn::find_register(regs.description(), reg);
  REQUIRE(found != nullptr);
  REQUIRE(
      f.target->model(name)
          ->on_write(
              regs, found->offset, 4, regs.peek(found->offset), value, {.pc = 0, .now = icount{0}})
          .has_value());
}
}

TEST_CASE("a listener removed or added during a notification is not called for that change")
{
  auto                f        = at_breakpoint();
  auto&               pc       = port(f, "GPIOC");
  bool                b_called = false;
  bool                c_called = false;
  stm32::subscription b;
  stm32::subscription c;
  auto                a = pc.on_change([&](std::uint8_t, bool) {
    b.reset();
    c = pc.on_change([&](std::uint8_t, bool) { c_called = true; });
  });
  b                     = pc.on_change([&](std::uint8_t, bool) { b_called = true; });
  pc.drive(13, false);
  CHECK_FALSE(b_called);
  CHECK_FALSE(c_called);
}

struct death_flag
{
  explicit death_flag(bool& dead) : dead_{&dead}
  {}

  death_flag(death_flag const&)                    = delete;
  auto operator=(death_flag const&) -> death_flag& = delete;

  death_flag(death_flag&& other) noexcept : dead_{std::exchange(other.dead_, nullptr)}
  {}

  auto operator=(death_flag&&) -> death_flag& = delete;

  ~death_flag()
  {
    if(dead_ != nullptr)
    {
      *dead_ = true;
    }
  }

private:
  bool* dead_;
};

TEST_CASE("a move-only listener that cancels itself lives to the end of its call")
{
  auto                f    = at_breakpoint();
  auto&               pc   = port(f, "GPIOC");
  bool                dead = false;
  std::vector<bool>   dead_in_call;
  stm32::subscription self;
  self = pc.on_change([&, flag = death_flag{dead}](std::uint8_t, bool) {
    self.reset();
    dead_in_call.push_back(dead);
  });
  pc.drive(13, false);
  pc.drive(13, true);
  CHECK(dead_in_call == std::vector<bool>{false});
  CHECK(dead);
}

TEST_CASE("a listener that drives a pin the same change moved reports each final level once")
{
  auto       f     = at_breakpoint();
  auto&      pc    = port(f, "GPIOC");
  auto const moder = f.target->peripheral("GPIOC").peek(0);
  write_port(f, "GPIOC", "GPIO_MODER", moder & ~(3u << 28));
  write_port(f, "GPIOC", "GPIO_PUPDR", (1u << 26) | (1u << 28));
  REQUIRE(pc.level(13));
  REQUIRE(pc.level(14));
  std::vector<std::pair<std::uint8_t, bool>> heard;
  auto                                       sub = pc.on_change([&](std::uint8_t pin, bool level) {
    heard.emplace_back(pin, level);
    if(pin == 13)
    {
      pc.drive(14, true);
    }
  });
  write_port(f, "GPIOC", "GPIO_PUPDR", (2u << 26) | (2u << 28));
  CHECK_FALSE(pc.level(13));
  CHECK(pc.level(14));
  CHECK(
      heard
      == std::vector<std::pair<std::uint8_t, bool>>{
          {13, false}
  });
}

TEST_CASE("a machine restored from saved bytes without a reset forwards a pin edge to EXTI")
{
  auto       f     = at_breakpoint();
  auto const bytes = popcorn::core::encode_snapshot(f.target->snapshot().data());
  auto       fresh = popcorn::testing::stm32_fixture{u575(), "gpio_ll"};
  auto       other = std::make_unique<popcorn::core::machine>(
      u575(), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options());
  other->load(popcorn::testing::elf("gpio_ll"));
  other->restore(popcorn::core::snapshot{popcorn::core::decode_snapshot(bytes)});
  fresh.target = std::move(other);

  port(fresh, "GPIOC").drive(13, false);
  CHECK(fresh.field("EXTI", "EXTI_FPR1", "FPIF13") == 1);
  CHECK(count(finished(fresh), exti13_mark) == 1);
}

namespace
{
constexpr std::uint64_t us_ps = 1'000'000;

struct timed_change
{
  std::uint8_t  pin{};
  bool          level{};
  std::uint64_t at{};

  friend auto operator==(timed_change const&, timed_change const&) -> bool = default;
};

auto idling() -> stm32_fixture
{
  stm32_fixture f{u575(), "tim_ll"};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("tim_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("tim_ll", "idle"));
  auto moder = f.target->peripheral("GPIOC").peek(0);
  write_port(f, "GPIOC", "GPIO_MODER", moder & ~(3u << 26));
  return f;
}

void exti_falling_pc13(stm32_fixture const& f)
{
  auto const& desc = f.target->peripheral("EXTI").description();
  auto const  at   = [&](std::string_view reg) {
    auto const found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  };
  REQUIRE(f.target->mem().write(at("EXTI_EXTICR4"), 4, 2u << 8).has_value());
  REQUIRE(f.target->mem().write(at("EXTI_FTSR1"), 4, 1u << 13).has_value());
}

void run_until(stm32_fixture const& f, std::uint64_t until, std::uint64_t slice)
{
  while(f.target->virtual_clock().time().ps < until)
  {
    auto const result = f.run(
        {.max_instructions = icount{slice},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = vtime{until}});
    if(result.stopped)
    {
      REQUIRE(result.stopped->reason == stop_reason::time);
    }
  }
}

auto edges_seen(std::uint64_t slice) -> std::pair<std::vector<timed_change>, std::uint64_t>
{
  auto  f  = idling();
  auto& pc = port(f, "GPIOC");
  exti_falling_pc13(f);
  pc.drive(13, true);
  auto const                start = f.target->virtual_clock().time().ps;
  std::vector<timed_change> seen;
  std::uint32_t             falling = 0;
  auto                      sub     = pc.on_change([&](std::uint8_t pin, bool level) {
    seen.push_back({pin, level, f.target->virtual_clock().time().ps});
    falling = f.field("EXTI", "EXTI_FPR1", "FPIF13");
  });
  pc.drive_at(13, false, vtime{start + 10 * us_ps});
  pc.drive_at(13, true, vtime{start + 20 * us_ps});
  pc.drive_at(13, false, vtime{start + 30 * us_ps});
  CHECK(pc.pending_edges() == 3);
  run_until(f, start + 25 * us_ps, slice);
  CHECK(pc.pending_edges() == 1);
  CHECK(f.field("EXTI", "EXTI_FPR1", "FPIF13") == 1);
  run_until(f, start + 40 * us_ps, slice);
  CHECK(pc.pending_edges() == 0);
  CHECK(falling == 1);
  return {seen, start};
}
}

TEST_CASE("three timed edges reach IDR, listeners and EXTI at their exact times at any slicing")
{
  auto const [parked, start] = edges_seen(1'000'000'000);
  CHECK(
      parked
      == std::vector<timed_change>{
          {13, false, start + 10 * us_ps},
          {13, true,  start + 20 * us_ps},
          {13, false, start + 30 * us_ps}
  });
  for(auto const slice : {1u, 7u, 1000u})
  {
    CAPTURE(slice);
    auto const [sliced, sliced_start] = edges_seen(slice);
    CHECK(sliced_start == start);
    CHECK(sliced == parked);
  }
}

TEST_CASE("an edge before the machine time is refused, a full port refuses the next edge")
{
  auto       f   = idling();
  auto&      pc  = port(f, "GPIOC");
  auto const now = f.target->virtual_clock().time().ps;
  run_until(f, now + us_ps, 1'000'000);
  CHECK_THROWS_AS(pc.drive_at(13, true, vtime{now}), std::invalid_argument);
  CHECK(pc.pending_edges() == 0);
  auto const later = f.target->virtual_clock().time().ps + us_ps;
  for(std::size_t index = 0; index < stm32::max_pending_edges; ++index)
  {
    pc.drive_at(13, index % 2 == 0, vtime{later + index});
  }
  CHECK_THROWS_AS(pc.drive_at(13, true, vtime{later}), popcorn::config_error);
  CHECK(pc.pending_edges() == stm32::max_pending_edges);
}

TEST_CASE("a snapshot with pending edges restores them, a drive leaves them in place")
{
  auto       f     = idling();
  auto&      pc    = port(f, "GPIOC");
  auto const start = f.target->virtual_clock().time().ps;
  pc.drive_at(13, true, vtime{start + 10 * us_ps});
  pc.drive_at(13, false, vtime{start + 20 * us_ps});
  auto const saved  = f.target->snapshot();
  auto const events = f.target->pending_events();
  run_until(f, start + 15 * us_ps, 1'000'000);
  CHECK(pc.level(13));
  pc.drive(13, false);
  CHECK(pc.pending_edges() == 1);
  f.target->restore(saved);
  CHECK(pc.pending_edges() == 2);
  CHECK(f.target->pending_events() == events);
  std::vector<timed_change> seen;
  auto                      sub = pc.on_change([&](std::uint8_t pin, bool level) {
    seen.push_back({pin, level, f.target->virtual_clock().time().ps});
  });
  run_until(f, start + 30 * us_ps, 1'000'000);
  CHECK(
      seen
      == std::vector<timed_change>{
          {13, true,  start + 10 * us_ps},
          {13, false, start + 20 * us_ps}
  });
}

TEST_CASE("a system or power-on reset after a system reset keeps each pending edge at its instant")
{
  for(auto const kind : {popcorn::reset_kind::software, popcorn::reset_kind::power_on})
  {
    CAPTURE(kind);
    auto  f  = idling();
    auto& pc = port(f, "GPIOC");
    run_until(f, f.target->virtual_clock().time().ps + 5 * us_ps, 1'000'000);
    auto const first = f.target->virtual_clock().time().ps;
    f.target->reset(popcorn::reset_kind::software);
    f.set("pc", sym("tim_ll", "idle"));
    auto const start = f.target->virtual_clock().time().ps;
    pc.drive_at(13, true, vtime{start + 30 * us_ps});
    run_until(f, start + 10 * us_ps, 1'000'000);
    auto const reset_at = f.target->virtual_clock().time().ps;
    f.target->reset(kind);
    f.set("pc", sym("tim_ll", "idle"));
    CHECK(pc.pending_edges() == 1);
    static_cast<void>(first);
    auto const due = start + 30 * us_ps - reset_at;
    run_until(f, due - us_ps, 1'000'000);
    CHECK(pc.pending_edges() == 1);
    run_until(f, due, 1'000'000);
    CHECK(pc.pending_edges() == 0);
  }
}

TEST_CASE("edges of one instant keep their order across a reset")
{
  auto       f     = idling();
  auto&      pc    = port(f, "GPIOC");
  auto const start = f.target->virtual_clock().time().ps;
  pc.drive(13, false);
  pc.drive_at(13, true, vtime{start + 20 * us_ps});
  pc.drive_at(13, false, vtime{start + 20 * us_ps});
  run_until(f, start + 5 * us_ps, 1'000'000);
  auto const reset_at = f.target->virtual_clock().time().ps;
  f.target->reset(popcorn::reset_kind::software);
  f.set("pc", sym("tim_ll", "idle"));
  write_port(f, "GPIOC", "GPIO_MODER", f.target->peripheral("GPIOC").peek(0) & ~(3u << 26));
  std::vector<bool> heard;
  auto              sub = pc.on_change([&](std::uint8_t, bool level) { heard.push_back(level); });
  run_until(f, start + 30 * us_ps - reset_at, 1'000'000);
  CHECK(heard == std::vector<bool>{true, false});
  CHECK_FALSE(pc.level(13));
}

TEST_CASE("GPIOCRST returns pins to analog with their edges, the board drive comes back as input")
{
  auto  f  = idling();
  auto& pc = port(f, "GPIOC");
  pc.drive(13, true);
  REQUIRE(pc.level(13));
  auto const moder = f.target->peripheral("GPIOC").peek(0);
  write_port(f, "GPIOC", "GPIO_MODER", (moder & ~(3u << 14)) | (1u << 14));
  write_port(f, "GPIOC", "GPIO_ODR", 1u << 7);
  REQUIRE(pc.level(7));
  std::vector<std::pair<std::uint8_t, bool>> heard;
  auto sub = pc.on_change([&](std::uint8_t pin, bool level) { heard.emplace_back(pin, level); });
  rcc_reset_pulse(f, "RCC_AHB2RSTR1", "GPIOCRST");
  CHECK(f.target->peripheral("GPIOC").peek(0) == 0xFFFF'FFFF);
  CHECK_FALSE(pc.level(7));
  CHECK_FALSE(pc.level(13));
  CHECK(
      heard
      == std::vector<std::pair<std::uint8_t, bool>>{
          {7,  false},
          {13, false}
  });
  write_port(f, "GPIOC", "GPIO_MODER", 0xFFFF'FFFF & ~(3u << 26));
  CHECK(pc.level(13));
}

TEST_CASE("GPIOARST and GPIOBRST return the debug pins to AF0 with their pulls, the rest to analog")
{
  auto  f  = idling();
  auto& pa = port(f, "GPIOA");
  auto& pb = port(f, "GPIOB");
  write_port(f, "GPIOA", "GPIO_MODER", 0x5555'5555);
  write_port(f, "GPIOB", "GPIO_MODER", 0x5555'5555);
  rcc_reset_pulse(f, "RCC_AHB2RSTR1", "GPIOARST");
  rcc_reset_pulse(f, "RCC_AHB2RSTR1", "GPIOBRST");
  CHECK(f.target->peripheral("GPIOA").peek(0) == 0xABFF'FFFF);
  CHECK(f.target->peripheral("GPIOB").peek(0) == 0xFFFF'FEBF);
  for(std::uint8_t const pin : {13, 14, 15})
  {
    CAPTURE(pin);
    CHECK(pa.mode(pin) == stm32::pin_mode::alternate);
  }
  CHECK(pa.level(13));
  CHECK_FALSE(pa.level(14));
  CHECK(pa.level(15));
  CHECK(pb.mode(3) == stm32::pin_mode::alternate);
  CHECK(pb.mode(4) == stm32::pin_mode::alternate);
  CHECK(pb.level(4));
  CHECK(pa.mode(0) == stm32::pin_mode::analog);
}

TEST_CASE("a board edge queued before GPIOCRST applies at its time after the reset")
{
  auto       f   = idling();
  auto&      pc  = port(f, "GPIOC");
  auto const due = f.target->virtual_clock().time().ps + 1'000'000;
  pc.drive_at(13, true, popcorn::vtime{due});
  rcc_reset_pulse(f, "RCC_AHB2RSTR1", "GPIOCRST");
  write_port(f, "GPIOC", "GPIO_MODER", 0xFFFF'FFFF & ~(3u << 26));
  CHECK_FALSE(pc.level(13));
  REQUIRE(
      f.run_to_stop(
           {.max_instructions = icount{100'000'000}, .until_time = popcorn::vtime{due + 1}})
          .reason
      == stop_reason::time);
  CHECK(pc.level(13));
}

TEST_CASE("RM0456 13.4.1: GPIOI of the STM32U575 has pins 0 to 7, as read back on silicon")
{
  stm32_fixture const f{u575(), "gpio_ll"};
  write_register(f, "GPIOI", "MODER", 0xffff'ffff);
  CHECK(read_register(f, "GPIOI", "MODER") == 0x0000'ffff);
  write_register(f, "GPIOI", "OSPEEDR", 0xffff'ffff);
  CHECK(read_register(f, "GPIOI", "OSPEEDR") == 0x0000'ffff);
  write_register(f, "GPIOI", "AFRH", 0xffff'ffff);
  CHECK(read_register(f, "GPIOI", "AFRH") == 0);
  write_register(f, "GPIOI", "AFRH", 0x0000'd5a1);
  CHECK(read_register(f, "GPIOI", "AFRH") == 0);
  write_register(f, "GPIOH", "AFRH", 0xffff'ffff);
  CHECK(read_register(f, "GPIOH", "AFRH") == 0xffff'ffff);
  write_register(f, "GPIOH", "MODER", 0xffff'ffff);
  CHECK(read_register(f, "GPIOH", "MODER") == 0xffff'ffff);
}

TEST_CASE("driving GPIOI pin 8 of the STM32U575 throws config_error, pin 7 and GPIOH pin 8 drive")
{
  stm32_fixture const f{u575(), "gpio_ll"};
  auto&               gpioi = port(f, "GPIOI");
  CHECK(gpioi.pin_count() == 8);
  CHECK(port(f, "GPIOH").pin_count() == 16);
  CHECK_THROWS_AS(gpioi.drive(8, true), popcorn::config_error);
  CHECK_THROWS_AS(gpioi.drive_at(8, true, vtime{0}), popcorn::config_error);
  CHECK_THROWS_AS(gpioi.drive(15, true), popcorn::config_error);
  CHECK(gpioi.pending_edges() == 0);
  write_register(f, "GPIOI", "MODER", 0xffff'0000);
  CHECK_NOTHROW(gpioi.drive(7, true));
  CHECK(gpioi.level(7));
  write_register(f, "GPIOH", "MODER", 0xfffe'ffff);
  CHECK_NOTHROW(port(f, "GPIOH").drive(8, true));
  CHECK(port(f, "GPIOH").level(8));
}
