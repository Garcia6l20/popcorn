#include "stm32_fixture.hpp"

#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t ms          = 1'000'000'000;
constexpr std::uint64_t second      = 1'000 * ms;
constexpr std::uint64_t instruction = 250'000;

auto entered(
    std::string_view      entry,
    core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{u575(), "rtc_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("rtc_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("rtc_ll", entry));
  return f;
}

struct calls
{
  std::vector<std::uint64_t> at;
  core::intercept_handle     hook;

  calls(stm32_fixture& f, std::string_view function)
      : hook{
            f.target->add_intercept(sym("rtc_ll", function), [this](core::intercept_call const& c) {
              at.push_back(c.target().virtual_clock().time().ps);
              return core::intercept_result::proceed();
            })}
  {}
};

auto finish(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  auto const stopped = f.run_to_stop({.max_instructions = icount{1'000'000'000}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  return f.log();
}

auto run_until(stm32_fixture const& f, std::uint64_t ps) -> void
{
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
      == stop_reason::time);
}

auto read(stm32_fixture const& f, std::string_view peripheral, std::string_view reg)
    -> std::uint32_t
{
  auto const& desc  = f.target->peripheral(peripheral).description();
  auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != desc.registers.end());
  auto const value = f.target->mem().read(desc.base + found->offset, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void write(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::uint32_t        value)
{
  auto const& desc  = f.target->peripheral(peripheral).description();
  auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != desc.registers.end());
  REQUIRE(f.target->mem().write(desc.base + found->offset, 4, value).has_value());
}

auto cpu_read(stm32_fixture& f, std::string_view reg) -> std::uint32_t
{
  auto&       regs  = f.target->peripheral("RTC");
  auto const& desc  = regs.description();
  auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
  REQUIRE(found != desc.registers.end());
  auto const value = f.target->model("RTC")->on_read(
      regs, found->offset, 4, {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
  REQUIRE(value.has_value());
  return *value;
}

auto first_wakeup(stm32_fixture const& f) -> std::uint64_t
{
  REQUIRE(
      f.run_to_stop(
           {.max_instructions = icount{1'000'000'000}, .until = sym("rtc_ll", "RTC_IRQHandler")})
          .reason
      == stop_reason::address);
  return f.target->virtual_clock().time().ps;
}

auto booted(std::chrono::sys_seconds at) -> stm32_fixture
{
  static std::deque<stm32::board> boards;
  auto&                           board = boards.emplace_back();
  board.rtc_initial                     = at;
  auto options                          = popcorn::testing::stm32_options();
  options.models                        = stm32::factories(board, u575());
  auto f                                = entered("clock_only", std::move(options));
  REQUIRE(finish(f) == std::vector<std::uint32_t>{1});
  f.set("pc", sym("rtc_ll", "idle"));
  return f;
}

void unlock(stm32_fixture const& f)
{
  write(f, "RTC", "WPR", 0xCA);
  write(f, "RTC", "WPR", 0x53);
}

auto later(stm32_fixture const& f, std::uint64_t ps) -> void
{
  run_until(f, f.target->virtual_clock().time().ps + ps);
}

auto flag(stm32_fixture const& f, unsigned bit) -> std::uint32_t
{
  return read(f, "RTC", "SR") >> bit & 1u;
}

constexpr std::uint64_t rtc_cycle = 1'000'000'000'000 / 32'768;

constexpr std::uint32_t one_second_past_midnight = 0x00'0001;
constexpr std::uint32_t march_first_2026_sunday  = 0x26'E301;

auto gaps_are_one_second(std::vector<std::uint64_t> const& at, std::uint64_t slack) -> bool
{
  for(std::size_t index = 1; index < at.size(); ++index)
  {
    auto const gap = at[index] - at[index - 1];
    if(gap + slack < second or gap > second + slack)
    {
      return false;
    }
  }
  return true;
}
}

TEST_CASE("the calendar rolls from 2026-02-28 23:59:58 to March 1st and wakes every second")
{
  std::vector<std::uint64_t> first;
  for(int run = 0; run < 2; ++run)
  {
    auto  f = entered("calendar");
    calls irq{f, "RTC_IRQHandler"};
    CHECK(
        finish(f)
        == std::vector<std::uint32_t>{one_second_past_midnight, march_first_2026_sunday, 5});
    REQUIRE(irq.at.size() == 5);
    CHECK(gaps_are_one_second(irq.at, instruction));
    if(run == 0)
    {
      first = irq.at;
    }
    else
    {
      CHECK(irq.at == first);
    }
  }
}

TEST_CASE("alarm A on 00:00:01 fires once")
{
  auto f = entered("alarm");
  CHECK(finish(f) == std::vector<std::uint32_t>{one_second_past_midnight, 1});
}

TEST_CASE("a board calendar at power-on counts through a leap day with INITS set")
{
  stm32::board board{};
  board.rtc_initial = std::chrono::sys_days{std::chrono::year{2028} / 2 / 28}
                    + std::chrono::hours{23}
                    + std::chrono::minutes{59}
                    + std::chrono::seconds{59};
  auto options      = popcorn::testing::stm32_options();
  options.models    = stm32::factories(board, u575());
  auto f            = entered("clock_only", std::move(options));
  CHECK(finish(f) == std::vector<std::uint32_t>{1});
  f.set("pc", sym("rtc_ll", "idle"));
  run_until(f, f.target->virtual_clock().time().ps + 1'500 * ms);
  CHECK(read(f, "RTC", "DR") == 0x28'4229);
  CHECK(read(f, "RTC", "TR") == 0);
}

TEST_CASE("shadow registers read through TR hold the date until DR is read")
{
  auto       f = entered("calendar");
  calls      irq{f, "RTC_IRQHandler"};
  auto const first    = first_wakeup(f);
  auto const midnight = first + second;
  run_until(f, midnight - 10 * ms);
  CHECK(cpu_read(f, "TR") == 0x23'5959);
  run_until(f, midnight + 10 * ms);
  CHECK(cpu_read(f, "DR") == 0x26'C228);
  CHECK(cpu_read(f, "TR") == 0);
  CHECK(cpu_read(f, "DR") == march_first_2026_sunday);
}

TEST_CASE("TR, DR and PRER ignore writes outside init mode and while write-protected")
{
  auto f = entered("clock_only");
  CHECK(finish(f) == std::vector<std::uint32_t>{0});
  f.set("pc", sym("rtc_ll", "idle"));
  auto const initf = [&] { return read(f, "RTC", "ICSR") >> 6 & 1u; };
  write(f, "RTC", "ICSR", 1u << 7);
  run_until(f, f.target->virtual_clock().time().ps + ms);
  CHECK(initf() == 0);
  write(f, "RTC", "WPR", 0x53);
  write(f, "RTC", "ICSR", 1u << 7);
  run_until(f, f.target->virtual_clock().time().ps + ms);
  CHECK(initf() == 0);
  write(f, "RTC", "WPR", 0xCA);
  write(f, "RTC", "WPR", 0x53);
  write(f, "RTC", "PRER", 0x7F'0010);
  CHECK(read(f, "RTC", "PRER") == 0x7F'00FF);
  write(f, "RTC", "TR", 0x05'0505);
  CHECK(read(f, "RTC", "TR") != 0x05'0505);
  write(f, "RTC", "ICSR", 1u << 7);
  run_until(f, f.target->virtual_clock().time().ps + ms);
  CHECK(initf() == 1);
  write(f, "RTC", "TR", 0x12'3456);
  write(f, "RTC", "PRER", 0x7F'00FF);
  CHECK(read(f, "RTC", "PRER") == 0x7F'00FF);
  run_until(f, f.target->virtual_clock().time().ps + 2'000 * ms);
  CHECK(read(f, "RTC", "TR") == 0x12'3456);
  write(f, "RTC", "WPR", 0xFF);
  write(f, "RTC", "TR", 0x01'0101);
  CHECK(read(f, "RTC", "TR") == 0x12'3456);
  write(f, "RTC", "WPR", 0xCA);
  write(f, "RTC", "WPR", 0x53);
  write(f, "RTC", "ICSR", 0);
  auto const              exited = f.target->virtual_clock().time().ps;
  constexpr std::uint64_t cycle  = 1'000'000'000'000 / 32'768;
  auto const              tick   = exited + (4 + 128) * cycle;
  run_until(f, tick - cycle - 1'000'000);
  CHECK(read(f, "RTC", "SSR") == 255);
  run_until(f, tick + 1'000'000);
  CHECK(read(f, "RTC", "SSR") == 254);
  CHECK(read(f, "RTC", "TR") == 0x12'3456);
}

TEST_CASE("a core clock change keeps the wake-up interrupts one second apart")
{
  auto       f = entered("backup");
  calls      irq{f, "RTC_IRQHandler"};
  auto const first = first_wakeup(f);
  run_until(f, first + 500 * ms);
  auto const was = read(f, "RCC", "RCC_ICSCR1") & ~(0xFu << 28);
  write(f, "RCC", "RCC_ICSCR1", was | (7u << 28) | (1u << 23));
  run_until(f, first + 3'500 * ms);
  REQUIRE(irq.at.size() == 4);
  CAPTURE(irq.at);
  CHECK(gaps_are_one_second(irq.at, 20'000'000));
  CHECK(irq.at[3] - irq.at[1] == 2 * second);
}

TEST_CASE("the calendar stops while RTCEN is clear")
{
  auto f = entered("clock_only");
  CHECK(finish(f) == std::vector<std::uint32_t>{0});
  f.set("pc", sym("rtc_ll", "idle"));
  run_until(f, f.target->virtual_clock().time().ps + 1'500 * ms);
  CHECK(read(f, "RTC", "TR") == 1);
  write(f, "RCC", "RCC_BDCR", read(f, "RCC", "RCC_BDCR") & ~(1u << 15));
  run_until(f, f.target->virtual_clock().time().ps + 2'000 * ms);
  CHECK(read(f, "RTC", "TR") == 1);
}

TEST_CASE("an alarm on midnight enabled one second before fires at midnight, not at once")
{
  auto       f     = entered("calendar");
  auto const first = first_wakeup(f);
  write(f, "RTC", "WPR", 0xCA);
  write(f, "RTC", "WPR", 0x53);
  write(f, "RTC", "ALRMAR", 1u << 31);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 8));
  f.set("pc", sym("rtc_ll", "idle"));
  run_until(f, first + second - ms);
  CHECK((read(f, "RTC", "SR") & 1u) == 0);
  run_until(f, first + second + ms);
  CHECK((read(f, "RTC", "SR") & 1u) == 1);
}

TEST_CASE("a snapshot with the wake-up timer running restores to the same interrupts and log")
{
  auto  f = entered("calendar");
  calls irq{f, "RTC_IRQHandler"};
  run_until(f, f.target->virtual_clock().time().ps + 1'500 * ms);
  auto const                       saved  = f.target->snapshot();
  auto const                       before = irq.at.size();
  auto const                       log    = finish(f);
  std::vector<std::uint64_t> const after{
      irq.at.begin() + static_cast<std::ptrdiff_t>(before), irq.at.end()};

  f.target->restore(saved);
  irq.at.resize(before);
  CHECK(finish(f) == log);
  CHECK(
      std::vector<std::uint64_t>{irq.at.begin() + static_cast<std::ptrdiff_t>(before), irq.at.end()}
      == after);

  auto  other = entered("calendar");
  calls again{other, "RTC_IRQHandler"};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(finish(other) == log);
  CHECK(again.at == after);
}

TEST_CASE("backup registers survive a software reset and clear at power-on")
{
  auto       f = entered("backup");
  calls      irq{f, "RTC_IRQHandler"};
  auto const first = first_wakeup(f);
  run_until(f, first + 100 * ms);
  CHECK(read(f, "TAMP", "TAMP_BKP0R") == 0xCAFE'1234);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(read(f, "TAMP", "TAMP_BKP0R") == 0xCAFE'1234);
  f.target->reset(popcorn::reset_kind::power_on);
  CHECK(read(f, "TAMP", "TAMP_BKP0R") == 0);
  CHECK(read(f, "RTC", "DR") == 0x2101);
  CHECK(read(f, "RTC", "TR") == 0);
}

TEST_CASE("12-hour format counts PM into AM and matches an alarm's PM bit")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1});
  unlock(f);
  write(f, "RTC", "ICSR", read(f, "RTC", "ICSR") | (1u << 7));
  later(f, ms);
  REQUIRE((read(f, "RTC", "ICSR") >> 6 & 1u) == 1);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 6));
  write(f, "RTC", "TR", (1u << 22) | 0x11'5959);
  write(f, "RTC", "ICSR", read(f, "RTC", "ICSR") & ~(1u << 7));
  later(f, 1'500 * ms);
  CHECK(read(f, "RTC", "TR") == 0x12'0000);

  write(f, "RTC", "ICSR", read(f, "RTC", "ICSR") | (1u << 7));
  later(f, ms);
  write(f, "RTC", "TR", (1u << 22) | 0x12'5959);
  write(f, "RTC", "ICSR", read(f, "RTC", "ICSR") & ~(1u << 7));
  write(f, "RTC", "ALRMAR", (1u << 31) | (1u << 22) | 0x01'0000);
  write(f, "RTC", "ALRMBR", (1u << 31) | 0x01'0000);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 8) | (1u << 9));
  later(f, 1'500 * ms);
  CHECK(read(f, "RTC", "TR") == ((1u << 22) | 0x01'0000));
  CHECK(flag(f, 0) == 1);
  CHECK(flag(f, 1) == 0);
}

TEST_CASE("alarm B alone matches a weekday selected with WDSEL")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1} + hours{23} + minutes{59} + seconds{59});
  unlock(f);
  write(f, "RTC", "ALRMBR", (1u << 30) | (5u << 24));
  write(f, "RTC", "ALRMAR", (1u << 30) | (6u << 24));
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 9) | (1u << 8));
  later(f, 500 * ms);
  CHECK(flag(f, 1) == 0);
  later(f, second);
  CHECK(flag(f, 1) == 1);
  CHECK(flag(f, 0) == 0);
}

TEST_CASE("the wake-up timer on RTCCLK / 16 fires every 16 x (WUT + 1) RTCCLK cycles")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1});
  unlock(f);
  write(f, "RTC", "WUTR", 99);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 10));
  auto const enabled = f.target->virtual_clock().time().ps;
  auto const period  = 16 * 100 * rtc_cycle;
  run_until(f, enabled + period - 2 * rtc_cycle);
  CHECK(flag(f, 2) == 0);
  run_until(f, enabled + period + rtc_cycle);
  CHECK(flag(f, 2) == 1);
  write(f, "RTC", "SCR", 1u << 2);
  run_until(f, enabled + 2 * period - 2 * rtc_cycle);
  CHECK(flag(f, 2) == 0);
  run_until(f, enabled + 2 * period + rtc_cycle);
  CHECK(flag(f, 2) == 1);
}

TEST_CASE("the 17-bit ck_spre wake-up adds 2^16 seconds to the reload")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1});
  unlock(f);
  write(f, "RTC", "WUTR", 0);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | 6u);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 10));
  auto const long_run = [&](std::uint64_t ps) {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000'000'000},
                       .until_time       = vtime{f.target->virtual_clock().time().ps + ps}})
            .reason
        == stop_reason::time);
  };
  long_run(65'536 * second);
  CHECK(flag(f, 2) == 0);
  long_run(2 * second);
  CHECK(flag(f, 2) == 1);
}

TEST_CASE("WUTR and WUCKSEL refuse writes while WUTWF is clear")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1});
  unlock(f);
  write(f, "RTC", "WUTR", 99);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 10));
  later(f, ms);
  REQUIRE((read(f, "RTC", "ICSR") >> 2 & 1u) == 0);
  write(f, "RTC", "WUTR", 5);
  CHECK((read(f, "RTC", "WUTR") & 0xFFFFu) == 99);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | 3u);
  CHECK((read(f, "RTC", "CR") & 7u) == 0);
}

TEST_CASE("the calendar wraps from 2099-12-31 to 2000-01-01")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2099} / 12 / 31} + hours{23} + minutes{59} + seconds{59});
  later(f, 1'500 * ms);
  CHECK(read(f, "RTC", "DR") == 0x00'A101);
  CHECK(read(f, "RTC", "TR") == 0);
}

TEST_CASE("ADD1H and SUB1H move the calendar by an hour, SUB1H does nothing at hour 0")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1} + hours{10});
  unlock(f);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 16));
  CHECK(read(f, "RTC", "TR") == 0x11'0000);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 17));
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 17));
  CHECK(read(f, "RTC", "TR") == 0x09'0000);

  auto midnight = booted(sys_days{year{2026} / 1 / 1} + minutes{30});
  unlock(midnight);
  write(midnight, "RTC", "CR", read(midnight, "RTC", "CR") | (1u << 17));
  CHECK(read(midnight, "RTC", "TR") == 0x00'3000);
  CHECK(read(midnight, "RTC", "DR") == 0x26'8101);
}

TEST_CASE(
    "after a software reset at 2.4 s the calendar continues and WUTF keeps its 1 s boundaries")
{
  auto       f     = entered("backup");
  auto const first = first_wakeup(f);
  run_until(f, first + 1'400 * ms);
  auto const mono = [&] { return f.target->virtual_clock().since_power_on().ps; };
  auto const at   = [&](std::uint64_t since_power_on) {
    run_until(f, since_power_on - (mono() - f.target->virtual_clock().time().ps));
  };
  REQUIRE(mono() == first + 1'400 * ms);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(mono() == first + 1'400 * ms);
  CHECK(read(f, "TAMP", "TAMP_BKP0R") == 0xCAFE'1234);
  CHECK((read(f, "RTC", "ICSR") >> 5 & 1u) == 0);
  at(first + 1'401 * ms);
  CHECK((read(f, "RTC", "ICSR") >> 5 & 1u) == 1);
  at(first + 2 * second - ms);
  CHECK(flag(f, 2) == 0);
  CHECK(read(f, "RTC", "TR") == 0);
  at(first + 2 * second + ms);
  CHECK(flag(f, 2) == 1);
  CHECK(read(f, "RTC", "TR") == one_second_past_midnight);
  CHECK(read(f, "RTC", "DR") == march_first_2026_sunday);
  write(f, "RTC", "SCR", 1u << 2);
  at(first + 3 * second + ms);
  CHECK(flag(f, 2) == 1);
}

TEST_CASE("an alarm with a digit the hardware never matches never fires and costs no search")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1});
  unlock(f);
  write(f, "RTC", "ALRMAR", (1u << 31) | (1u << 23) | (1u << 15) | 0x79);
  auto const began = steady_clock::now();
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 8));
  CHECK(steady_clock::now() - began < milliseconds{50});
  later(f, 3 * second);
  CHECK(flag(f, 0) == 0);

  write(f, "RTC", "CR", read(f, "RTC", "CR") & ~(1u << 8));
  write(f, "RTC", "ALRMAR", (1u << 31) | (1u << 23) | (1u << 15) | 0x0A);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 8));
  later(f, 61 * second);
  CHECK(flag(f, 0) == 0);
}

TEST_CASE("a debugger read of the calendar leaves the model state unchanged")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1});
  later(f, 1'500 * ms);
  std::vector<std::byte> before;
  f.target->model("RTC")->save(before);
  (void)read(f, "RTC", "TR");
  (void)read(f, "RTC", "DR");
  std::vector<std::byte> after;
  f.target->model("RTC")->save(after);
  CHECK(before == after);
}

TEST_CASE("a backup domain reset clears the calendar, the protection and the backup registers")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 5 / 5} + hours{10});
  write(f, "TAMP", "TAMP_BKP0R", 0x1234);
  write(f, "TAMP", "TAMP_CR2", 1u << 16);
  unlock(f);
  write(f, "RTC", "ALRMAR", 0x12'3456);
  later(f, 1'500 * ms);
  auto const bdcr = read(f, "RCC", "RCC_BDCR");
  write(f, "RCC", "RCC_BDCR", bdcr | (1u << 16));
  write(f, "RCC", "RCC_BDCR", 0);
  CHECK(read(f, "TAMP", "TAMP_BKP0R") == 0);
  CHECK(read(f, "TAMP", "TAMP_CR2") == 0);
  CHECK(read(f, "RTC", "DR") == 0x2101);
  CHECK(read(f, "RTC", "TR") == 0);
  CHECK(read(f, "RTC", "ALRMAR") == 0);
  write(f, "RTC", "ICSR", 1u << 7);
  later(f, ms);
  CHECK((read(f, "RTC", "ICSR") >> 6 & 1u) == 0);
}

TEST_CASE("backup-domain registers the model only stores survive a software reset")
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 5 / 5});
  write(f, "TAMP", "TAMP_CR2", 1u << 16);
  write(f, "RTC", "ALRABINR", 0x55);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(read(f, "TAMP", "TAMP_CR2") == 1u << 16);
  CHECK(read(f, "RTC", "ALRABINR") == 0x55);
}

namespace
{
std::uint64_t rtc_slicing = 1'000'000'000;

auto run_sliced(stm32_fixture const& f, vtime until, bool hold_from_until = false) -> popcorn::stop
{
  for(;;)
  {
    auto const result = f.run(
        {.max_instructions = icount{rtc_slicing},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = until,
         .hold_from_until  = hold_from_until});
    if(result.stopped)
    {
      return *result.stopped;
    }
  }
}

void held_later(stm32_fixture const& f, std::uint64_t ps)
{
  REQUIRE(
      run_sliced(f, vtime{f.target->virtual_clock().time().ps + ps}).reason == stop_reason::time);
}

auto calendar_of(stm32_fixture const& f) -> std::vector<std::uint32_t>
{
  return {read(f, "RTC", "SSR"), read(f, "RTC", "TR"), read(f, "RTC", "DR"), read(f, "RTC", "SR")};
}

auto frozen_rtc() -> stm32_fixture
{
  using namespace std::chrono;
  auto f = booted(sys_days{year{2026} / 1 / 1} + hours{10});
  popcorn::testing::set_debug_freeze(f, "DBG_RTC_STOP", true);
  return f;
}

void enable_wakeup(stm32_fixture const& f)
{
  unlock(f);
  write(f, "RTC", "WUTR", 99);
  write(f, "RTC", "CR", read(f, "RTC", "CR") | (1u << 10));
}
}

TEST_CASE("with DBG_RTC_STOP a held RTC keeps its calendar and resumes on its phase")
{
  auto const run = [](std::uint64_t instructions) {
    rtc_slicing = instructions;
    auto f      = frozen_rtc();
    auto twin   = frozen_rtc();
    held_later(f, 300 * ms + rtc_cycle / 3);
    held_later(twin, 300 * ms + rtc_cycle / 3);
    auto const at_hold = calendar_of(f);
    f.target->hold_core(true);
    held_later(f, 2 * second);
    CHECK(calendar_of(f) == at_hold);
    f.target->hold_core(false);
    held_later(f, second + 7 * ms);
    held_later(twin, second + 7 * ms);
    CHECK(calendar_of(f) == calendar_of(twin));
    rtc_slicing = 1'000'000'000;
    return calendar_of(f);
  };
  CHECK(run(997) == run(1'000'000'000));
}

TEST_CASE("without DBG_RTC_STOP a held RTC counts on")
{
  using namespace std::chrono;
  auto f    = booted(sys_days{year{2026} / 1 / 1} + hours{10});
  auto twin = booted(sys_days{year{2026} / 1 / 1} + hours{10});
  f.target->hold_core(true);
  held_later(f, 2 * second);
  held_later(twin, 2 * second);
  CHECK(calendar_of(f) == calendar_of(twin));
}

TEST_CASE("a wake-up due during a frozen hold comes one held span later")
{
  auto f    = frozen_rtc();
  auto twin = frozen_rtc();
  enable_wakeup(f);
  enable_wakeup(twin);
  auto const enabled = f.target->virtual_clock().time().ps;
  auto const period  = 16 * 100 * rtc_cycle;
  run_until(f, enabled + period / 2);
  f.target->hold_core(true);
  run_until(f, enabled + period / 2 + second);
  CHECK(flag(f, 2) == 0);
  f.target->hold_core(false);
  run_until(f, enabled + period + second - 2 * rtc_cycle);
  CHECK(flag(f, 2) == 0);
  run_until(f, enabled + period + second + rtc_cycle);
  CHECK(flag(f, 2) == 1);
  run_until(twin, enabled + period + rtc_cycle);
  CHECK(calendar_of(twin)[0] == calendar_of(f)[0]);
}

TEST_CASE("an RTC hold released at once leaves the calendar and the wake-up as never held")
{
  auto const run = [](bool hold) {
    auto f = frozen_rtc();
    enable_wakeup(f);
    held_later(f, 7 * ms + rtc_cycle / 3);
    if(hold)
    {
      f.target->hold_core(true);
      f.target->hold_core(false);
    }
    std::vector<std::uint32_t> seen;
    for(int step = 0; step < 40; ++step)
    {
      held_later(f, 3 * ms + 17 * instruction);
      auto const now = calendar_of(f);
      seen.insert(seen.end(), now.begin(), now.end());
    }
    return seen;
  };
  CHECK(run(true) == run(false));
}

TEST_CASE("a frozen RTC restores frozen into another machine and resumes as the original")
{
  auto f = frozen_rtc();
  held_later(f, 300 * ms);
  f.target->hold_core(true);
  held_later(f, second);
  auto const bytes = core::encode_snapshot(f.target->snapshot().data());
  auto const held  = calendar_of(f);
  f.target->hold_core(false);
  held_later(f, second);

  auto other = frozen_rtc();
  other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(other.target->core_held());
  CHECK(calendar_of(other) == held);
  other.target->hold_core(false);
  held_later(other, second);
  CHECK(calendar_of(other) == calendar_of(f));
}

TEST_CASE("a wake-up held back at a barrier when the core is held fires at the hold")
{
  auto probe = frozen_rtc();
  enable_wakeup(probe);
  auto const enabled = probe.target->virtual_clock().time().ps;
  auto const period  = 16 * 100 * rtc_cycle;
  auto       due     = enabled + period - 2 * rtc_cycle;
  run_until(probe, due);
  while(flag(probe, 2) == 0)
  {
    due += instruction;
    run_until(probe, due);
  }

  auto f = frozen_rtc();
  enable_wakeup(f);
  REQUIRE(f.target->virtual_clock().time().ps == enabled);
  REQUIRE(run_sliced(f, vtime{due}, true).reason == stop_reason::time);
  CHECK(flag(f, 2) == 0);
  f.target->hold_core(true);
  CHECK(flag(f, 2) == 1);
}

TEST_CASE("a wake-up timer enabled during a frozen hold starts at the release")
{
  auto f = frozen_rtc();
  f.target->hold_core(true);
  enable_wakeup(f);
  auto const held   = f.target->virtual_clock().time().ps;
  auto const period = 16 * 100 * rtc_cycle;
  run_until(f, held + second);
  CHECK(flag(f, 2) == 0);
  f.target->hold_core(false);
  run_until(f, held + second + period - 2 * rtc_cycle);
  CHECK(flag(f, 2) == 0);
  run_until(f, held + second + period + 2 * rtc_cycle);
  CHECK(flag(f, 2) == 1);
}

TEST_CASE("a software reset of a held core keeps the RTC frozen")
{
  auto f    = frozen_rtc();
  auto twin = frozen_rtc();
  held_later(f, 300 * ms);
  held_later(twin, 300 * ms);
  f.target->hold_core(true);
  held_later(f, 500 * ms);
  f.target->reset(popcorn::reset_kind::software);
  CHECK(popcorn::testing::debug_freeze_set(f, "DBG_RTC_STOP"));
  held_later(f, 500 * ms);
  f.target->hold_core(false);
  held_later(f, second);
  held_later(twin, second);
  CHECK(read(f, "RTC", "TR") == read(twin, "RTC", "TR"));
  CHECK(read(f, "RTC", "SSR") == read(twin, "RTC", "SSR"));
}

TEST_CASE("a brown-out of a held core clears DBG_RTC_STOP and the RTC runs on from it")
{
  auto f    = frozen_rtc();
  auto twin = frozen_rtc();
  held_later(f, 300 * ms);
  held_later(twin, 300 * ms);
  f.target->hold_core(true);
  held_later(f, 500 * ms);
  f.target->reset(popcorn::reset_kind::brown_out);
  CHECK_FALSE(popcorn::testing::debug_freeze_set(f, "DBG_RTC_STOP"));
  CHECK(f.target->core_held());
  held_later(f, 500 * ms);
  f.target->hold_core(false);
  held_later(f, second);
  held_later(twin, 1'500 * ms);
  CHECK(read(f, "RTC", "TR") == read(twin, "RTC", "TR"));
  CHECK(read(f, "RTC", "SSR") == read(twin, "RTC", "SSR"));
}

TEST_CASE("a backup domain reset of a frozen RTC leaves it frozen until the release")
{
  auto f = frozen_rtc();
  held_later(f, 300 * ms);
  f.target->hold_core(true);
  auto const bdcr = read(f, "RCC", "RCC_BDCR");
  write(f, "RCC", "RCC_BDCR", bdcr | (1u << 16));
  write(f, "RCC", "RCC_BDCR", bdcr & ~(1u << 16));
  auto const reset = calendar_of(f);
  held_later(f, 2 * second);
  CHECK(calendar_of(f) == reset);
  f.target->hold_core(false);
  held_later(f, 2 * second);
  CHECK(read(f, "RTC", "TR") != reset[1]);
}
