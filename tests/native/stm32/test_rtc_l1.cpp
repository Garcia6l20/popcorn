#include "../support/inert_model.hpp"
#include "factories.hpp"
#include "stm32_fixture.hpp"
#include "variants.hpp"

#include <popcorn/core/coverage.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
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
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::part_description;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware     = "l152xe_l1_rtc_ll";
constexpr std::string_view firmware_b   = "l152xb_l1_rtc_ll";
constexpr std::string_view idle_image   = "l152xe_l1_exti_ll";
constexpr std::string_view idle_image_b = "l152xb_l1_exti_ll";
constexpr std::uint64_t    us           = 1'000'000;
constexpr std::uint64_t    ms           = 1'000 * us;
constexpr std::uint64_t    second       = 1'000 * ms;
constexpr std::uint64_t    rtc_cycle    = 1'000'000'000'000 / 32'768;
constexpr std::uint32_t    init_bit     = 1u << 7;
constexpr std::uint32_t    initf_bit    = 1u << 6;
constexpr std::uint32_t    rsf_bit      = 1u << 5;
constexpr std::uint32_t    feb_28       = 0x26'C228;
constexpr std::uint32_t    march_1      = 0x26'E301;

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(popcorn::periph::model_context& ctx) : context{ctx}
  {}

  popcorn::periph::model_context& context;
};

struct rig
{
  part_description const&        part;
  std::unique_ptr<stm32_fixture> f;
  std::string                    elf;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  explicit rig(
      part_description const& desc   = l152re(),
      std::string_view        image  = idle_image,
      bool                    strict = false)
      : part{desc}, elf{image}
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories({}, desc);
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "DAC", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f = std::make_unique<stm32_fixture>(desc, image, std::move(options));
    f->set("pc", sym(elf, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
  }

  [[nodiscard]] auto reg(std::string_view unit, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const* found =
        popcorn::find_register(f->target->peripheral(unit).description(), std::string{name});
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto at(std::string_view unit, std::string_view name) const -> popcorn::address
  {
    return f->target->peripheral(unit).description().base + reg(unit, name).offset;
  }

  [[nodiscard]] auto read(std::string_view name, std::string_view unit = "RTC") const
      -> std::uint32_t
  {
    auto const value = probe().services.read(at(unit, name), 4, popcorn::initiator::debugger);
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::string_view name, std::uint32_t value, std::string_view unit = "RTC") const
  {
    REQUIRE(
        probe().services.write(at(unit, name), 4, value, popcorn::initiator::debugger).has_value());
  }

  [[nodiscard]] auto probe() const -> popcorn::periph::model_context&
  {
    auto* const found = f->model_of<bus_probe>("DAC");
    REQUIRE(found != nullptr);
    return found->context;
  }

  [[nodiscard]] auto cpu_read_at(popcorn::address address) const -> std::uint32_t
  {
    auto const value = probe().services.read(address, 4, popcorn::initiator::cpu);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto cpu_read(std::string_view name) const -> std::uint32_t
  {
    return cpu_read_at(at("RTC", name));
  }

  void cpu_write(std::string_view name, std::uint32_t value) const
  {
    CHECK(probe().services.write(at("RTC", name), 4, value, popcorn::initiator::cpu).has_value());
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    for(;;)
    {
      auto const result = f->run(
          {.max_instructions = icount{1'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{ps}});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == stop_reason::time);
        return;
      }
    }
  }

  void later(std::uint64_t ps) const
  {
    run_until(now() + ps);
  }

  void start_clock() const
  {
    write("CSR", read("CSR", "RCC") | (1u << 8), "RCC");
    for(int step = 0; step < 5'000 and f->field("RCC", "CSR", "LSERDY") == 0; ++step)
    {
      later(ms);
    }
    REQUIRE(f->field("RCC", "CSR", "LSERDY") == 1);
    write("CSR", read("CSR", "RCC") | (1u << 16) | (1u << 22), "RCC");
  }

  void unlock() const
  {
    write("WPR", 0xCA);
    write("WPR", 0x53);
  }

  void enter_init() const
  {
    unlock();
    write("ISR", ~0u);
    later(3 * rtc_cycle);
    REQUIRE((read("ISR") & initf_bit) != 0);
  }

  [[nodiscard]] auto isr(std::uint32_t bit) const -> std::uint32_t
  {
    return (read("ISR") & bit) != 0 ? 1 : 0;
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
  }
};

auto entered(part_description const& desc, std::string_view name, std::string_view entry)
    -> stm32_fixture
{
  stm32_fixture f{desc, name};
  f.set("pc", sym(name, entry));
  return f;
}

auto run_to_idle(stm32_fixture const& f) -> void
{
  auto const stopped = f.run_to_stop({.max_instructions = icount{20'000}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::idle);
}

auto finish(stm32_fixture const& f, std::uint64_t cap = 10'000) -> std::vector<std::uint32_t>
{
  auto const stopped = f.run_to_stop({.max_instructions = icount{cap}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  return f.log();
}

void run_until(stm32_fixture const& f, std::uint64_t ps)
{
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
      == stop_reason::time);
}

auto without_register(part_description desc, std::string_view name) -> part_description
{
  auto& rtc = *std::ranges::find(
      desc.peripherals, std::string_view{"RTC"}, &popcorn::peripheral_desc::name);
  std::erase_if(rtc.registers, [&](popcorn::register_desc const& r) { return r.name == name; });
  return desc;
}

auto with_version(part_description desc, std::string_view version) -> part_description
{
  auto& rtc = *std::ranges::find(
      desc.peripherals, std::string_view{"RTC"}, &popcorn::peripheral_desc::name);
  rtc.ip_version = std::string{version};
  return desc;
}

auto make_machine(part_description desc) -> std::unique_ptr<core::machine>
{
  stm32::register_builtin_models();
  return std::make_unique<core::machine>(
      std::move(desc), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options());
}
}

TEST_CASE("RM0038 20.3.5: the LL init sets 2026-02-28 23:59:58 and 3 s later reads March 1st")
{
  for(auto const cat1 : {false, true})
  {
    CAPTURE(cat1);
    auto const name = cat1 ? firmware_b : firmware;
    auto       f    = entered(cat1 ? l152rb() : l152re(), name, "calendar");
    run_to_idle(f);
    auto const log = f.log();
    REQUIRE(log.size() == (cat1 ? 6u : 7u));
    CHECK(std::vector(log.begin(), log.begin() + 4) == std::vector<std::uint32_t>{0, 0, 0, 1});
    CHECK(std::vector(log.end() - 2, log.end()) == std::vector<std::uint32_t>{0x23'5958, feb_28});
    if(not cat1)
    {
      CHECK(log[4] <= 255);
    }
    run_until(f, f.target->virtual_clock().time().ps + 3 * second);
    f.set("pc", sym(name, "read_calendar"));
    auto const after = finish(f);
    REQUIRE(after.size() == log.size() + (cat1 ? 2u : 3u));
    CHECK(
        std::vector(after.end() - 2, after.end())
        == std::vector<std::uint32_t>{0x00'0001, march_1});
  }
}

TEST_CASE("RM0038 20.3.5: TR, DR, PRER, CALIBR and CR.FMT take writes under the key with INITF set")
{
  rig r;
  r.start_clock();
  r.write("TR", 0x12'3456);
  r.write("PRER", 0x7F'0010);
  r.write("CR", 1u << 6);
  CHECK(r.read("TR") == 0);
  CHECK(r.read("PRER") == 0x7F'00FF);
  CHECK(r.read("CR") == 0);
  r.unlock();
  r.write("CR", 1u << 6);
  r.write("CALIBR", 0x85);
  CHECK(r.read("CR") == 0);
  CHECK(r.read("CALIBR") == 0);
  r.write("ISR", init_bit);
  r.write("TR", 0x12'3456);
  CHECK(r.read("TR") == 0);
  r.later(3 * rtc_cycle);
  r.write("TR", 0x12'3456);
  r.write("DR", feb_28);
  r.write("PRER", 0x7F'00FF);
  r.write("CALIBR", 0x85);
  r.write("CR", 1u << 6);
  CHECK(r.read("TR") == 0x52'3456);
  CHECK(r.read("DR") == feb_28);
  CHECK(r.read("CALIBR") == 0x85);
  CHECK(r.read("CR") == 1u << 6);
  r.write("ISR", 0);
  r.later(2 * second + 10 * ms);
  CHECK(r.read("TR") == 0x52'3458);
}

TEST_CASE(
    "RM0038 20.3.5: INITF sets at the second RTCCLK edge after INIT, counting at the fourth after")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.write("ISR", init_bit);
  auto const entered_at = r.now();
  r.run_until(entered_at + rtc_cycle - us);
  CHECK(r.isr(initf_bit) == 0);
  r.run_until(entered_at + rtc_cycle + us);
  CHECK(r.isr(initf_bit) == 0);
  r.run_until(entered_at + 2 * rtc_cycle + us);
  CHECK(r.isr(initf_bit) == 1);
  r.write("PRER", 0x00'0000);
  r.write("PRER", 0x00'0000);
  r.write("ISR", 0);
  CHECK(r.isr(initf_bit) == 0);
  auto const exited = r.now();
  r.run_until(exited + 4 * rtc_cycle + rtc_cycle / 2);
  CHECK(r.read("SSR") == 0);
  CHECK(r.read("TR") == 0x00'0000);
  r.run_until(exited + 5 * rtc_cycle + rtc_cycle / 2);
  CHECK(r.read("TR") == 0x00'0001);
}

TEST_CASE("RM0038 20.3.5: a key write leaves the stored WPR word at zero")
{
  rig r;
  r.start_clock();
  r.write("WPR", 0xCA);
  CHECK(r.f->target->peripheral("RTC").peek(r.reg("RTC", "WPR").offset) == 0);
}

TEST_CASE("RM0038 20.3.5: 0xCA then 0x53 unlocks, the reverse order or a wrong key locks")
{
  rig r;
  r.start_clock();
  r.write("WPR", 0x53);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
  r.write("WPR", 0x53);
  r.write("WPR", 0xCA);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
  r.unlock();
  r.write("WPR", 0xFF);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
  r.unlock();
  CHECK(r.read("WPR") == 0);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 1);
}

TEST_CASE("RM0038 20.3.5: a system reset keeps the key state, a power-on locks again")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 1);
  r.f->target->reset(reset_kind::power_on);
  r.f->set("pc", sym(r.elf, "idle"));
  r.start_clock();
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
}

TEST_CASE("RM0038 20.6.4: RSF sets at the second RTCCLK edge after a clear under the key")
{
  rig r;
  r.start_clock();
  r.later(ms);
  REQUIRE(r.isr(rsf_bit) == 1);
  r.write("ISR", ~rsf_bit);
  CHECK(r.isr(rsf_bit) == 1);
  r.unlock();
  r.write("ISR", ~rsf_bit & ~init_bit);
  auto const cleared = r.now();
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(cleared + rtc_cycle - us);
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(cleared + 2 * rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE("RM0038 20.6.4: init mode clears RSF, set at the second edge after counting restarts")
{
  rig r;
  r.start_clock();
  r.later(ms);
  REQUIRE(r.isr(rsf_bit) == 1);
  r.enter_init();
  CHECK(r.isr(rsf_bit) == 0);
  r.later(ms);
  CHECK(r.isr(rsf_bit) == 0);
  r.write("ISR", 0);
  auto const exited = r.now();
  r.run_until(exited + 5 * rtc_cycle - us);
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(exited + 6 * rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE("RM0038 20.6.4: a 0 clears ALRAF to TAMP1F without the key, TAMP2F and TAMP3F with it")
{
  rig        r;
  auto&      regs   = r.f->target->peripheral("RTC");
  auto const offset = r.reg("RTC", "ISR").offset;
  auto const flags  = 0x3Fu << 8;
  auto const keyed  = 0x3u << 14;
  regs.poke(offset, regs.peek(offset) | flags | keyed);
  r.write("ISR", 0);
  CHECK((r.read("ISR") & flags) == 0);
  CHECK((r.read("ISR") & keyed) == keyed);
  CHECK((r.read("ISR") & 0x7u) == 0x7u);
  r.write("ISR", ~0u);
  CHECK((r.read("ISR") & flags) == 0);
  r.unlock();
  r.write("ISR", 0);
  CHECK((r.read("ISR") & keyed) == 0);
  CHECK((r.read("ISR") & 0x7u) == 0x7u);
}

TEST_CASE("RM0038 20.3.6: an SSR or TR read holds TR and DR until DR is read")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("TR", 0x23'5959);
  r.write("DR", feb_28);
  r.write("ISR", 0);
  r.later(ms);
  REQUIRE(r.isr(rsf_bit) == 1);
  r.later(second - 2 * ms - 4 * rtc_cycle);
  CHECK(r.cpu_read("TR") == 0x23'5959);
  r.later(10 * ms);
  CHECK(r.cpu_read("TR") == 0x23'5959);
  CHECK(r.cpu_read("DR") == feb_28);
  CHECK(r.cpu_read("SSR") <= 255);
  CHECK(r.cpu_read("TR") == 0);
  CHECK(r.cpu_read("DR") == march_1);
  CHECK(r.read("TR") == 0);
}

TEST_CASE("RM0038 20.6.1 and 20.6.2: after a system reset TR and DR read reset values until RSF")
{
  for(auto const cat1 : {false, true})
  {
    CAPTURE(cat1);
    auto const name = cat1 ? firmware_b : firmware;
    auto       f    = entered(cat1 ? l152rb() : l152re(), name, "calendar");
    run_to_idle(f);
    auto const before = f.log().size();
    f.target->reset(reset_kind::software);
    f.set("pc", sym(name, "read_after_reset"));
    auto const log  = finish(f);
    auto const tail = std::vector(log.begin() + static_cast<std::ptrdiff_t>(before), log.end());
    if(cat1)
    {
      CHECK(tail == std::vector<std::uint32_t>{0, 0, 0x2101, 0x23'5958, feb_28});
    }
    else
    {
      REQUIRE(tail.size() == 7);
      CHECK(
          std::vector(tail.begin(), tail.begin() + 4)
          == std::vector<std::uint32_t>{0, 0, 0, 0x2101});
      CHECK(tail[5] == 0x23'5958);
      CHECK(tail[6] == feb_28);
    }
  }
}

TEST_CASE(
    "RM0038 20.6.3: BYPSHAD reads the counters with RSF clear, RSF sets at the second edge after")
{
  rig r;
  r.start_clock();
  r.later(ms);
  r.unlock();
  r.write("CR", 1u << 5);
  CHECK(r.isr(rsf_bit) == 0);
  r.later(ms);
  CHECK(r.isr(rsf_bit) == 0);
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  CHECK(r.cpu_read("DR") == 0x00'2101);
  CHECK(r.cpu_read("TR") == r.read("TR"));
  r.later(second);
  CHECK(r.cpu_read("TR") == 0x00'0001);
  r.write("CR", 0);
  auto const cleared = r.now();
  r.run_until(cleared + rtc_cycle - us);
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(cleared + 2 * rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE("RM0038 20.3.1: SSR counts PREDIV_S down at RTCCLK / (PREDIV_A + 1)")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("PRER", 0x07'0FFF);
  r.write("ISR", 0);
  auto const tick = r.now() + (4 + 8) * rtc_cycle;
  r.run_until(tick - us);
  CHECK(r.read("SSR") == 0xFFF);
  r.run_until(tick + us);
  CHECK(r.read("SSR") == 0xFFE);
  r.run_until(tick + 4095 * 8 * rtc_cycle + us);
  CHECK(r.read("SSR") == 0xFFF);
  CHECK(r.read("TR") == 0x00'0001);
}

TEST_CASE("RM0038 20.6.5: PREDIV_S has 13 bits on Cat.1 and 15 from Cat.2")
{
  for(auto const cat1 : {false, true})
  {
    CAPTURE(cat1);
    rig r{cat1 ? l152rb() : l152re(), cat1 ? idle_image_b : idle_image};
    r.start_clock();
    r.enter_init();
    r.write("PRER", 0x7F'FFFF);
    CHECK((r.read("PRER") & 0xFFFFu) == (cat1 ? 0x1FFFu : 0x7FFFu));
  }
}

TEST_CASE(
    "RM0038 20.6.11: Cat.1 has no RTC_SSR, a CPU read of offset 0x28 logs once, strict raises")
{
  {
    rig        r{l152rb(), idle_image_b};
    auto const at = r.f->target->peripheral("RTC").description().base + 0x28;
    CHECK(r.cpu_read_at(at) == 0);
    CHECK(r.cpu_read_at(at) == 0);
    CHECK(r.warnings() == 1);
  }
  rig        r{l152rb(), idle_image_b, true};
  auto const at          = r.f->target->peripheral("RTC").description().base + 0x28;
  auto const by_debugger = r.probe().services.read(at, 4, popcorn::initiator::debugger);
  REQUIRE(by_debugger.has_value());
  CHECK(*by_debugger == 0);
  CHECK(r.warnings() == 0);
  CHECK_THROWS_AS(
      static_cast<void>(r.probe().services.read(at, 4, popcorn::initiator::cpu)),
      popcorn::model_error);
  CHECK(r.warnings() == 1);
}

TEST_CASE("checklist 39: each RTC variant refuses a description that disagrees with it")
{
  CHECK_NOTHROW(static_cast<void>(make_machine(l152re())));
  CHECK_NOTHROW(static_cast<void>(make_machine(l152rb())));
  CHECK_THROWS_AS(
      static_cast<void>(make_machine(without_register(l152re(), "SSR"))), popcorn::config_error);
  CHECK_THROWS_AS(
      static_cast<void>(make_machine(without_register(l152re(), "CALR"))), popcorn::config_error);
  CHECK_THROWS_AS(
      static_cast<void>(make_machine(with_version(l152re(), "STM32L1_rtc_cat1"))),
      popcorn::config_error);
  CHECK_THROWS_AS(
      static_cast<void>(make_machine(with_version(l152rb(), "rtc2_v2_5_Cube"))),
      popcorn::config_error);
}

TEST_CASE(
    "RM0038 20.3.7: BKP0R and BKP19R keep their value through a system reset, RTCRST clears them")
{
  for(auto const cat1 : {false, true})
  {
    CAPTURE(cat1);
    auto const name = cat1 ? firmware_b : firmware;
    auto       f    = entered(cat1 ? l152rb() : l152re(), name, "backup_write");
    run_to_idle(f);
    f.target->reset(reset_kind::software);
    f.set("pc", sym(name, "backup_read"));
    CHECK(finish(f) == std::vector<std::uint32_t>{0xCAFE'1234, 0x5EED'0019});
    f.set("pc", sym(name, "backup_reset"));
    CHECK(finish(f) == std::vector<std::uint32_t>{0xCAFE'1234, 0x5EED'0019, 0, 0});
  }
}

TEST_CASE("RM0038 6.1.2: a power-on and a brown-out reset the calendar and the backup registers")
{
  for(auto const kind : {reset_kind::power_on, reset_kind::brown_out})
  {
    CAPTURE(kind);
    rig r;
    r.start_clock();
    r.enter_init();
    r.write("DR", feb_28);
    r.write("ISR", 0);
    r.write("BKP3R", 0x1234);
    r.f->target->reset(kind);
    r.f->set("pc", sym(r.elf, "idle"));
    CHECK(r.read("BKP3R") == 0);
    CHECK(r.read("DR") == 0x00'2101);
    CHECK(r.read("ISR") == 0x7);
  }
}

TEST_CASE("RM0038 20.3.5: the backup registers take CPU and debugger writes with DBP 0, key locked")
{
  rig r;
  REQUIRE(r.f->field("PWR", "CR", "DBP") == 0);
  r.cpu_write("BKP1R", 0xA5A5'0001);
  r.write("BKP2R", 0xA5A5'0002);
  CHECK(r.cpu_read("BKP1R") == 0xA5A5'0001);
  CHECK(r.read("BKP2R") == 0xA5A5'0002);
}

TEST_CASE("RM0038 20.6.3: ADD1H adds an hour, SUB1H subtracts one except at hour 0, both read 0")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("TR", 0x00'3000);
  r.write("DR", feb_28);
  r.write("ISR", 0);
  r.later(ms);
  r.write("CR", 1u << 17);
  CHECK(r.read("TR") == 0x00'3000);
  r.write("CR", 1u << 16);
  CHECK(r.read("TR") == 0x01'3000);
  CHECK(r.read("CR") == 0);
  r.write("CR", 1u << 17);
  CHECK(r.read("TR") == 0x00'3000);
}

TEST_CASE(
    "each unmodelled RTC function logs once and raises model_error at the CPU write under strict")
{
  struct row
  {
    std::string_view reg;
    std::uint32_t    value;
    bool             init;
    bool             cat1;
  };
  for(auto const& [reg, value, init, cat1] : {
          row{"CR",     1u << 11, false, true },
          row{"CR",     1u << 7,  true,  true },
          row{"CR",     1u << 23, false, true },
          row{"CR",     1u << 21, false, true },
          row{"CR",     1u << 4,  true,  true },
          row{"TAFCR",  1u << 0,  false, true },
          row{"TAFCR",  1u << 3,  false, false},
          row{"TAFCR",  1u << 5,  false, false},
          row{"CALR",   0x100,    false, false},
          row{"SHIFTR", 0x10,     false, false}
  })
  {
    for(auto const part : {false, true})
    {
      if(part and not cat1)
      {
        continue;
      }
      CAPTURE(reg);
      CAPTURE(value);
      CAPTURE(part);
      auto const& desc  = part ? l152rb() : l152re();
      auto const  image = part ? idle_image_b : idle_image;
      {
        rig r{desc, image};
        r.start_clock();
        init ? r.enter_init() : r.unlock();
        auto const before = r.warnings();
        r.cpu_write(reg, value);
        r.cpu_write(reg, value);
        CHECK(r.warnings() == before + 1);
        CHECK(r.read(reg) == (reg == "SHIFTR" ? 0 : value));
      }
      rig r{desc, image, true};
      r.start_clock();
      init ? r.enter_init() : r.unlock();
      auto const old = r.read(reg);
      CHECK_THROWS_AS(
          static_cast<void>(
              r.probe().services.write(r.at("RTC", reg), 4, value, popcorn::initiator::cpu)),
          popcorn::model_error);
      CHECK(r.read(reg) == old);
      r.write(reg, value);
      CHECK(r.read(reg) == (reg == "SHIFTR" ? 0 : value));
    }
  }
}

TEST_CASE("with DBG_RTC_STOP a held core stops the calendar until the release")
{
  rig r;
  r.start_clock();
  r.later(ms);
  popcorn::testing::set_debug_freeze(*r.f, "DBG_RTC_STOP", true);
  auto const at_hold = r.read("SSR");
  r.f->target->hold_core(true);
  r.later(2 * second);
  CHECK(r.read("SSR") == at_hold);
  CHECK(r.read("TR") == 0);
  r.f->target->hold_core(false);
  r.later(second);
  CHECK(r.read("TR") == 0x00'0001);
}

TEST_CASE("a reset of a held core after DBG_RTC_STOP is cleared releases the calendar")
{
  rig r;
  r.start_clock();
  r.later(ms);
  popcorn::testing::set_debug_freeze(*r.f, "DBG_RTC_STOP", true);
  r.f->target->hold_core(true);
  r.later(2 * second);
  CHECK(r.read("TR") == 0);
  popcorn::testing::set_debug_freeze(*r.f, "DBG_RTC_STOP", false);
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  r.later(2 * second);
  CHECK(r.read("TR") == 0x00'0002);
}

TEST_CASE("Cat.1 has no DBG_RTC_STOP: a held core under strict leaves the calendar counting")
{
  rig r{l152rb(), idle_image_b, true};
  r.start_clock();
  r.later(ms);
  auto const before = r.warnings();
  r.f->target->hold_core(true);
  r.later(2 * second);
  CHECK(r.read("TR") == 0x00'0002);
  CHECK(r.warnings() == before);
  r.f->target->hold_core(false);
}

TEST_CASE("RM0038 20.6.5: a system reset in init mode restarts the calendar on the PRER it holds")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("PRER", 0x7F'007F);
  REQUIRE(r.read("PRER") == 0x7F'007F);
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  CHECK(r.isr(init_bit) == 0);
  r.later(600 * ms);
  CHECK(r.read("SSR") < 128);
  CHECK(r.read("TR") == 0x00'0001);
}

TEST_CASE(
    "RM0038 20.3.6: after a system reset CPU reads of TR, DR and SSR give reset values until RSF")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("TR", 0x12'3456);
  r.write("DR", feb_28);
  r.write("ISR", 0);
  r.later(ms);
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  REQUIRE(r.isr(rsf_bit) == 0);
  CHECK(r.cpu_read("SSR") == 0);
  CHECK(r.cpu_read("TR") == 0);
  CHECK(r.cpu_read("DR") == 0x00'2101);
  r.later(3 * rtc_cycle);
  REQUIRE(r.isr(rsf_bit) == 1);
  CHECK(r.cpu_read("TR") == 0x12'3456);
  CHECK(r.cpu_read("DR") == feb_28);
}

TEST_CASE("RM0038 20.3.7 and 6.1.3: BKP0R keeps its value through a system reset, RTCRST clears it")
{
  rig r;
  r.cpu_write("BKP0R", 0xCAFE'0000);
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  CHECK(r.cpu_read("BKP0R") == 0xCAFE'0000);
  r.write("CSR", r.read("CSR", "RCC") | (1u << 23), "RCC");
  r.write("CSR", r.read("CSR", "RCC") & ~(1u << 23), "RCC");
  CHECK(r.cpu_read("BKP0R") == 0);
}

TEST_CASE("coverage reports CPU reads of TSTR, CALIBR and TAFCR as storage_only and TR as served")
{
  rig r;
  r.f->target->enable_coverage({}, true);
  for(auto const* name : {"TSTR", "CALIBR", "TAFCR", "TR"})
  {
    static_cast<void>(r.cpu_read(name));
  }
  auto const               found = r.f->target->coverage().unmodelled;
  std::vector<std::string> names;
  for(auto const& entry : found)
  {
    CHECK(entry.peripheral == "RTC");
    CHECK(entry.reason == popcorn::core::unmodelled_reason::storage_only);
    names.push_back(entry.reg);
  }
  std::ranges::sort(names);
  CHECK(names == std::vector<std::string>{"CALIBR", "TAFCR", "TSTR"});
}

namespace
{
constexpr std::uint32_t alraf      = 1u << 8;
constexpr std::uint32_t alrbf      = 1u << 9;
constexpr std::uint32_t wutf       = 1u << 10;
constexpr std::uint32_t alrawf     = 1u << 0;
constexpr std::uint32_t alrbwf     = 1u << 1;
constexpr std::uint32_t wutwf      = 1u << 2;
constexpr std::uint32_t alrae      = 1u << 8;
constexpr std::uint32_t alrbe      = 1u << 9;
constexpr std::uint32_t wute       = 1u << 10;
constexpr std::uint32_t alraie     = 1u << 12;
constexpr std::uint32_t alrbie     = 1u << 13;
constexpr std::uint32_t wutie      = 1u << 14;
constexpr std::uint32_t date_any   = 1u << 31;
constexpr std::uint32_t hour_any   = 1u << 23;
constexpr std::uint32_t minute_any = 1u << 15;
constexpr std::uint32_t second_any = 1u << 7;
constexpr std::uint32_t exti_base  = 0x4001'0400;
constexpr std::uint32_t alarm_irq  = 41;
constexpr std::uint32_t wakeup_irq = 3;

void exti_write(rig const& r, std::uint32_t offset, std::uint32_t value)
{
  REQUIRE(r.probe()
              .services.write(exti_base + offset, 4, value, popcorn::initiator::debugger)
              .has_value());
}

auto exti_read(rig const& r, std::uint32_t offset) -> std::uint32_t
{
  auto const value = r.probe().services.read(exti_base + offset, 4, popcorn::initiator::debugger);
  REQUIRE(value.has_value());
  return *value;
}

auto nvic_pending(rig const& r, std::uint32_t irq) -> bool
{
  auto const value =
      r.probe().services.read(0xE000'E200u + 4 * (irq / 32), 4, popcorn::initiator::debugger);
  REQUIRE(value.has_value());
  return (*value >> (irq % 32) & 1u) != 0;
}

void exti_rising(rig const& r, std::uint8_t line)
{
  exti_write(r, 0x00, exti_read(r, 0x00) | (1u << line));
  exti_write(r, 0x08, exti_read(r, 0x08) | (1u << line));
}

void midnight(rig const& r)
{
  r.start_clock();
  r.enter_init();
  r.write("TR", 0);
  r.write("DR", feb_28);
}
}

TEST_CASE(
    "RM0038 20.3.3 and 20.5: alarm A or B on 00:00:05 sets ALRxF and raises RTC_Alarm on EXTI 17, "
    "strict or not")
{
  for(auto const b : {false, true})
  {
    for(auto const strict : {false, true})
    {
      for(auto const unmasked : {false, true})
      {
        CAPTURE(b);
        CAPTURE(strict);
        CAPTURE(unmasked);
        auto const flag = b ? alrbf : alraf;
        rig        r{l152re(), idle_image, strict};
        midnight(r);
        r.write(b ? "ALRMBR" : "ALRMAR", date_any | 0x05);
        if(unmasked)
        {
          exti_rising(r, 17);
        }
        r.cpu_write("CR", b ? alrbe | alrbie : alrae | alraie);
        r.write("ISR", 0);
        auto const exited = r.now();
        auto const cycles = std::uint64_t{5} * 256 * 128;
        r.run_until(exited + (3 + cycles) * rtc_cycle - us);
        CHECK_FALSE(nvic_pending(r, alarm_irq));
        CHECK((exti_read(r, 0x14) & (1u << 17)) == 0);
        CHECK((r.read("ISR") & (alraf | alrbf)) == 0);
        r.run_until(exited + (4 + cycles) * rtc_cycle + us);
        CHECK(nvic_pending(r, alarm_irq) == unmasked);
        CHECK((exti_read(r, 0x14) & (1u << 17)) == (unmasked ? 1u << 17 : 0));
        CHECK((r.read("ISR") & (alraf | alrbf)) == flag);
        CHECK(r.read("TR") == 0x00'0005);
        CHECK(r.warnings() == 0);
      }
    }
  }
}

TEST_CASE("RM0038 20.3.3: alarm B with every field masked sets ALRBF every second")
{
  rig r;
  midnight(r);
  r.write("ALRMBR", date_any | hour_any | minute_any | second_any);
  r.write("CR", alrbe);
  r.write("ISR", 0);
  auto const exited = r.now();
  for(std::uint64_t second = 1; second <= 3; ++second)
  {
    CAPTURE(second);
    r.run_until(exited + (3 + second * 256 * 128) * rtc_cycle - us);
    CHECK((r.read("ISR") & alrbf) == 0);
    r.run_until(exited + (4 + second * 256 * 128) * rtc_cycle + us);
    CHECK((r.read("ISR") & alrbf) == alrbf);
    r.write("ISR", ~alrbf & ~init_bit);
  }
}

TEST_CASE("RM0038 20.3.3: an alarm on seconds 30 with minutes masked fires once a minute")
{
  rig r;
  midnight(r);
  r.write("ALRMAR", date_any | minute_any | 0x30);
  r.write("CR", alrae);
  r.write("ISR", 0);
  auto const exited = r.now();
  r.run_until(exited + (4 + 30ULL * 256 * 128) * rtc_cycle + us);
  CHECK((r.read("ISR") & alraf) == alraf);
  r.write("ISR", ~alraf & ~init_bit);
  r.run_until(exited + (3 + 90ULL * 256 * 128) * rtc_cycle - us);
  CHECK((r.read("ISR") & alraf) == 0);
  r.run_until(exited + (4 + 90ULL * 256 * 128) * rtc_cycle + us);
  CHECK((r.read("ISR") & alraf) == alraf);
  CHECK(r.read("TR") == 0x00'0130);
}

TEST_CASE("RM0038 20.6.18: alarm A with MASKSS 8 compares SS[7:0] inside a matching second")
{
  rig r;
  midnight(r);
  r.write("ALRMAR", date_any | 0x02);
  r.write("ALRMASSR", (8u << 24) | 0xC0);
  r.write("CR", alrae);
  r.write("ISR", 0);
  auto const exited = r.now();
  auto const tick   = 2ULL * 256 + (255 - 0xC0);
  r.run_until(exited + (3 + tick * 128) * rtc_cycle - us);
  CHECK((r.read("ISR") & alraf) == 0);
  r.run_until(exited + (4 + tick * 128) * rtc_cycle + us);
  CHECK((r.read("ISR") & alraf) == alraf);
  CHECK(r.read("SSR") == 0xC0);
}

TEST_CASE("RM0038 20.6.18: MASKSS 15 with an SS above PREDIV_S never matches")
{
  rig r;
  midnight(r);
  r.write("ALRMAR", date_any | hour_any | minute_any | second_any);
  r.write("ALRMASSR", (15u << 24) | 0x1000);
  r.write("CR", alrae);
  r.write("ISR", 0);
  r.later(3 * second);
  CHECK((r.read("ISR") & alraf) == 0);
}

TEST_CASE("RM0038 20.6.18: Cat.2 to Cat.6 keep ALRxWF 1, ALRMxSSR refuses writes with ALRxE 1")
{
  for(auto const b : {false, true})
  {
    CAPTURE(b);
    auto const alarm   = b ? "ALRMBR" : "ALRMAR";
    auto const sub     = b ? "ALRMBSSR" : "ALRMASSR";
    auto const written = b ? alrbwf : alrawf;
    rig        r;
    r.start_clock();
    r.unlock();
    r.write("CR", b ? alrbe : alrae);
    CHECK((r.read("ISR") & written) == written);
    r.write(alarm, 0x12);
    CHECK(r.read(alarm) == 0x12);
    r.write(sub, 0x05);
    CHECK(r.read(sub) == 0);
    r.write(b ? "ALRMASSR" : "ALRMBSSR", 0x05);
    CHECK(r.read(b ? "ALRMASSR" : "ALRMBSSR") == 0x05);
    r.write("CR", 0);
    r.write(sub, 0x05);
    CHECK(r.read(sub) == 0x05);
  }
}

TEST_CASE(
    "RM0038 20.3.5, board check 76 inferred: on Cat.1 ALRxWF clears with ALRxE and in init mode, "
    "sets 2 cycles after")
{
  for(auto const b : {false, true})
  {
    CAPTURE(b);
    auto const alarm   = b ? "ALRMBR" : "ALRMAR";
    auto const written = b ? alrbwf : alrawf;
    rig        r{l152rb(), idle_image_b};
    r.start_clock();
    r.unlock();
    CHECK((r.read("ISR") & written) == written);
    r.write("CR", b ? alrbe : alrae);
    CHECK((r.read("ISR") & written) == 0);
    CHECK((r.read("ISR") & (alrawf | alrbwf)) == (alrawf | alrbwf) - written);
    r.write(alarm, 0x12);
    CHECK(r.read(alarm) == 0);
    r.write("CR", 0);
    auto const cleared = r.now();
    r.run_until(cleared + rtc_cycle - us);
    CHECK((r.read("ISR") & written) == 0);
    r.run_until(cleared + 2 * rtc_cycle + us);
    CHECK((r.read("ISR") & written) == written);
    r.write(alarm, 0x12);
    CHECK(r.read(alarm) == 0x12);
    r.write("ISR", ~0u);
    r.later(3 * rtc_cycle);
    CHECK((r.read("ISR") & written) == 0);
    r.write(alarm, 0x34);
    CHECK(r.read(alarm) == 0x34);
  }
}

TEST_CASE(
    "RM0038 20.6.4, board check 151 inferred: a system reset 1 cycle after WUTE or a Cat.1 ALRxE "
    "clears keeps the WF")
{
  struct leg
  {
    bool          cat1;
    std::uint32_t enable;
    std::uint32_t written;
  };
  for(auto const& [cat1, enable, written] : {
          leg{false, wute,  wutwf },
          leg{true,  wute,  wutwf },
          leg{true,  alrae, alrawf},
          leg{true,  alrbe, alrbwf}
  })
  {
    CAPTURE(cat1);
    CAPTURE(enable);
    rig r{cat1 ? l152rb() : l152re(), cat1 ? idle_image_b : idle_image};
    r.start_clock();
    r.unlock();
    r.write("CR", enable);
    r.later(3 * rtc_cycle);
    REQUIRE((r.read("ISR") & written) == 0);
    r.write("CR", 0);
    auto const cleared = r.now();
    r.run_until(cleared + rtc_cycle - us);
    r.f->target->reset(reset_kind::software);
    r.f->set("pc", sym(r.elf, "idle"));
    auto const reset_at = r.now();
    CHECK((r.read("ISR") & written) == 0);
    r.run_until(reset_at + rtc_cycle - 2 * us);
    CHECK((r.read("ISR") & written) == 0);
    r.run_until(reset_at + rtc_cycle + 2 * us);
    CHECK((r.read("ISR") & written) == written);
  }
}

TEST_CASE(
    "RM0038 20.3.5, board check 150 inferred: on Cat.1 a system reset in init mode sets ALRxWF "
    "for a disabled alarm")
{
  rig r{l152rb(), idle_image_b};
  r.start_clock();
  r.unlock();
  r.write("CR", alrbe);
  r.enter_init();
  REQUIRE((r.read("ISR") & (alrawf | alrbwf)) == 0);
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  CHECK((r.read("ISR") & init_bit) == 0);
  CHECK((r.read("ISR") & (alrawf | alrbwf)) == alrawf);
  r.write("ALRMAR", 0x12);
  CHECK(r.read("ALRMAR") == 0x12);
  r.write("ALRMBR", 0x12);
  CHECK(r.read("ALRMBR") == 0);
}

TEST_CASE(
    "RM0038 20.3.4, board check 150 inferred: a ck_spre wakeup counts its period from the restart "
    "of an init exit or a system reset in init mode")
{
  for(auto const reset : {false, true})
  {
    CAPTURE(reset);
    rig r;
    r.start_clock();
    r.unlock();
    r.write("WUTR", 3);
    r.write("CR", 4);
    r.write("CR", 4u | wute);
    r.later(2 * second);
    r.enter_init();
    if(reset)
    {
      r.f->target->reset(reset_kind::software);
      r.f->set("pc", sym(r.elf, "idle"));
    }
    else
    {
      r.write("ISR", 0);
    }
    auto const restarted = r.now();
    r.run_until(restarted + 4 * second - 20 * ms);
    CHECK((r.read("ISR") & wutf) == 0);
    r.run_until(restarted + 4 * second + 20 * ms);
    CHECK((r.read("ISR") & wutf) == wutf);
  }
}

TEST_CASE(
    "RM0038 20.3.4, board check 77 inferred: wakeup on RTCCLK / 16 sets WUTF every 16 x (WUT + 1) "
    "cycles, EXTI 20, strict or not")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{l152re(), idle_image, strict};
    r.start_clock();
    r.unlock();
    exti_rising(r, 20);
    r.write("WUTR", 99);
    r.cpu_write("CR", wute | wutie);
    auto const enabled = r.now();
    for(std::uint64_t n = 1; n <= 2; ++n)
    {
      CAPTURE(n);
      r.run_until(enabled + (n * 1600 - 1) * rtc_cycle - us);
      CHECK_FALSE(nvic_pending(r, wakeup_irq));
      CHECK((r.read("ISR") & wutf) == 0);
      r.run_until(enabled + n * 1600 * rtc_cycle + us);
      CHECK(nvic_pending(r, wakeup_irq));
      CHECK((r.read("ISR") & wutf) == wutf);
      r.write("ISR", ~wutf & ~init_bit);
      exti_write(r, 0x14, 1u << 20);
      REQUIRE(r.probe()
                  .services.write(0xE000'E280u, 4, 1u << wakeup_irq, popcorn::initiator::debugger)
                  .has_value());
    }
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE("RM0038 20.3.4: a ck_spre wakeup with WUCKSEL 110 adds 2^16 to WUT")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.write("WUTR", 1);
  r.write("CR", 6);
  r.write("CR", 6u | wute);
  r.later((0x1'0002 - 1) * second);
  CHECK((r.read("ISR") & wutf) == 0);
  r.later(second + 10 * ms);
  CHECK((r.read("ISR") & wutf) == wutf);
}

TEST_CASE(
    "RM0038 20.6.4: WUTWF clears 1 to 2 cycles after WUTE sets, WUTR and WUCKSEL refuse writes "
    "until it sets")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.write("CR", wute);
  auto const enabled = r.now();
  CHECK((r.read("ISR") & wutwf) == wutwf);
  r.run_until(enabled + 2 * rtc_cycle + us);
  CHECK((r.read("ISR") & wutwf) == 0);
  r.write("WUTR", 7);
  r.write("CR", wute | 3);
  CHECK(r.read("WUTR") == 0xFFFF);
  CHECK((r.read("CR") & 7u) == 0);
  r.write("CR", 0);
  r.write("WUTR", 7);
  CHECK(r.read("WUTR") == 0xFFFF);
  auto const disabled = r.now();
  r.run_until(disabled + rtc_cycle - us);
  CHECK((r.read("ISR") & wutwf) == 0);
  r.run_until(disabled + 2 * rtc_cycle + us);
  CHECK((r.read("ISR") & wutwf) == wutwf);
  r.write("WUTR", 7);
  r.write("CR", 3);
  CHECK(r.read("WUTR") == 7);
  CHECK((r.read("CR") & 7u) == 3);
}

TEST_CASE(
    "RM0038 20.5, board check 74 inferred: ALRAF with ALRAIE 0 leaves EXTI 17 low, an ALRAIE "
    "write raises it")
{
  rig r;
  midnight(r);
  exti_rising(r, 17);
  r.write("ALRMAR", date_any | 0x01);
  r.write("CR", alrae);
  r.write("ISR", 0);
  r.later(2 * second);
  REQUIRE((r.read("ISR") & alraf) == alraf);
  CHECK((exti_read(r, 0x14) & (1u << 17)) == 0);
  CHECK_FALSE(nvic_pending(r, alarm_irq));
  r.write("CR", alrae | alraie);
  CHECK((exti_read(r, 0x14) & (1u << 17)) == 1u << 17);
  CHECK(nvic_pending(r, alarm_irq));
}

TEST_CASE("RM0038 10.2.4 and 20.5: clearing WUTF is a falling edge on EXTI 20")
{
  rig r;
  r.start_clock();
  r.unlock();
  exti_write(r, 0x00, 1u << 20);
  exti_write(r, 0x0C, 1u << 20);
  r.write("WUTR", 0);
  r.write("CR", wute | wutie);
  r.later(32 * rtc_cycle);
  REQUIRE((r.read("ISR") & wutf) == wutf);
  CHECK((exti_read(r, 0x14) & (1u << 20)) == 0);
  r.write("CR", wutie);
  r.write("ISR", ~wutf & ~init_bit);
  CHECK((exti_read(r, 0x14) & (1u << 20)) == 1u << 20);
}

TEST_CASE("RM0038 20.3.4 and 20.3.7: a system reset keeps the wakeup timer on its period")
{
  for(auto const reset : {false, true})
  {
    CAPTURE(reset);
    rig r;
    r.start_clock();
    r.unlock();
    r.write("WUTR", 99);
    r.write("CR", wute);
    auto const enabled = r.now();
    r.later(500 * rtc_cycle);
    auto const reset_at = r.now();
    if(reset)
    {
      r.f->target->reset(reset_kind::software);
      r.f->set("pc", sym(r.elf, "idle"));
    }
    auto const due = enabled + 1600 * rtc_cycle - (reset ? reset_at : 0);
    r.run_until(due - rtc_cycle - us);
    CHECK((r.read("ISR") & wutf) == 0);
    r.run_until(due + us);
    CHECK((r.read("ISR") & wutf) == wutf);
  }
}

TEST_CASE("RM0038 6.3.14: RTCRST drops a pending wakeup, a new one fires on its own reload only")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.write("WUTR", 99);
  r.write("CR", wute);
  r.later(100 * rtc_cycle);
  r.write("CSR", r.read("CSR", "RCC") | (1u << 23), "RCC");
  r.write("CSR", r.read("CSR", "RCC") & ~(1u << 23), "RCC");
  r.start_clock();
  r.unlock();
  r.write("WUTR", 199);
  r.write("CR", wute);
  auto const enabled = r.now();
  r.run_until(enabled + 3199 * rtc_cycle - us);
  CHECK((r.read("ISR") & wutf) == 0);
  r.run_until(enabled + 3200 * rtc_cycle + us);
  CHECK((r.read("ISR") & wutf) == wutf);
}

TEST_CASE("with DBG_RTC_STOP a wakeup due during a held span comes one held span later")
{
  rig r;
  r.start_clock();
  popcorn::testing::set_debug_freeze(*r.f, "DBG_RTC_STOP", true);
  r.unlock();
  r.write("WUTR", 99);
  r.write("CR", wute);
  auto const enabled = r.now();
  r.later(800 * rtc_cycle);
  r.f->target->hold_core(true);
  r.later(1000 * rtc_cycle);
  r.f->target->hold_core(false);
  r.run_until(enabled + 2599 * rtc_cycle - us);
  CHECK((r.read("ISR") & wutf) == 0);
  r.run_until(enabled + 2601 * rtc_cycle + us);
  CHECK((r.read("ISR") & wutf) == wutf);
}

TEST_CASE("RM0038 20.5: LL alarm A on 00:00:01 runs RTC_Alarm_IRQHandler through EXTI line 17")
{
  for(auto const cat1 : {false, true})
  {
    CAPTURE(cat1);
    auto const name = cat1 ? firmware_b : firmware;
    auto       f    = entered(cat1 ? l152rb() : l152re(), name, "alarm");
    CHECK(finish(f, 20'000'000) == std::vector<std::uint32_t>{0, 0, 0, 0, 0x00'0001, march_1, 1});
  }
}

TEST_CASE(
    "RM0038 20.5: LL ck_spre wakeup every second runs RTC_WKUP_IRQHandler through EXTI line 20")
{
  for(auto const cat1 : {false, true})
  {
    CAPTURE(cat1);
    auto const name = cat1 ? firmware_b : firmware;
    auto       f    = entered(cat1 ? l152rb() : l152re(), name, "wakeup");
    auto const log  = finish(f, 20'000'000);
    REQUIRE(log.size() == (cat1 ? 6u : 7u));
    CHECK(std::vector(log.begin(), log.begin() + 3) == std::vector<std::uint32_t>{0, 0, 0});
    CHECK(
        std::vector(log.end() - 3, log.end()) == std::vector<std::uint32_t>{0x00'0001, march_1, 3});
  }
}
