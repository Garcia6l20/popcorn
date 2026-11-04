#include "../support/inert_model.hpp"
#include "factories.hpp"
#include "stm32_fixture.hpp"
#include "variants.hpp"

#include <popcorn/core/coverage.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
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
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view idle_image = "l476_l4_exti_ll";
constexpr std::string_view firmware   = "l476_l4_rtc_ll";
constexpr std::uint64_t    us         = 1'000'000;
constexpr std::uint64_t    ms         = 1'000 * us;
constexpr std::uint64_t    second     = 1'000 * ms;
constexpr std::uint64_t    rtc_cycle  = 1'000'000'000'000 / 32'768;
constexpr std::uint32_t    init_bit   = 1u << 7;
constexpr std::uint32_t    initf_bit  = 1u << 6;
constexpr std::uint32_t    rsf_bit    = 1u << 5;
constexpr std::uint32_t    feb_28     = 0x26'C228;
constexpr std::uint32_t    march_1    = 0x26'E301;

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(popcorn::periph::model_context& ctx) : context{ctx}
  {}

  popcorn::periph::model_context& context;
};

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  explicit rig(bool strict = false)
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories({}, l476rc());
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "DAC1", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f = std::make_unique<stm32_fixture>(l476rc(), idle_image, std::move(options));
    f->set("pc", sym(idle_image, "idle"));
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

  [[nodiscard]] auto probe() const -> popcorn::periph::model_context&
  {
    auto* const found = f->model_of<bus_probe>("DAC1");
    REQUIRE(found != nullptr);
    return found->context;
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

  [[nodiscard]] auto cpu_read(std::string_view name) const -> std::uint32_t
  {
    auto const value = probe().services.read(at("RTC", name), 4, popcorn::initiator::cpu);
    REQUIRE(value.has_value());
    return *value;
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

  void idle_again() const
  {
    f->set("pc", sym(idle_image, "idle"));
  }

  void start_clock() const
  {
    write("BDCR", read("BDCR", "RCC") | 1u, "RCC");
    for(int step = 0; step < 5'000 and f->field("RCC", "BDCR", "LSERDY") == 0; ++step)
    {
      later(ms);
    }
    REQUIRE(f->field("RCC", "BDCR", "LSERDY") == 1);
    write("BDCR", read("BDCR", "RCC") | (1u << 8) | (1u << 15), "RCC");
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

auto entered(std::string_view entry) -> stm32_fixture
{
  stm32_fixture f{l476rc(), firmware};
  f.set("pc", sym(firmware, entry));
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

auto rtc_of(part_description& desc) -> popcorn::peripheral_desc&
{
  return *std::ranges::find(
      desc.peripherals, std::string_view{"RTC"}, &popcorn::peripheral_desc::name);
}

auto without_register(part_description desc, std::string_view name) -> part_description
{
  std::erase_if(
      rtc_of(desc).registers, [&](popcorn::register_desc const& r) { return r.name == name; });
  return desc;
}

auto without_field(part_description desc, std::string_view reg, std::string_view name)
    -> part_description
{
  auto& fields =
      std::ranges::find(rtc_of(desc).registers, reg, &popcorn::register_desc::name)->fields;
  std::erase_if(fields, [&](popcorn::field_desc const& f) { return f.name == name; });
  return desc;
}

auto make_machine(part_description desc, std::optional<stm32::rtc_classic_variant> variant = {})
    -> std::unique_ptr<core::machine>
{
  stm32::register_builtin_models();
  auto options = popcorn::testing::stm32_options();
  if(variant)
  {
    options.models = stm32::factories({}, desc);
    options.models.insert_or_assign("RTC", stm32::rtc_classic_factory(std::nullopt, *variant));
  }
  return std::make_unique<core::machine>(
      std::move(desc), popcorn::arch::armv8m::factory(), std::move(options));
}
}

TEST_CASE(
    "RM0351 38.3.7: TR, DR, PRER and CR.FMT take writes under the key with INITF set, "
    "2026-02-28 23:59:58 reads March 1st 3 s later")
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
  CHECK(r.read("CR") == 0);
  r.write("ISR", init_bit);
  r.write("TR", 0x12'3456);
  CHECK(r.read("TR") == 0);
  r.later(3 * rtc_cycle);
  r.write("TR", 0x23'5958);
  r.write("DR", feb_28);
  r.write("PRER", 0x7F'00FF);
  r.write("CR", 1u << 6);
  CHECK(r.read("TR") == 0x51'5958);
  CHECK(r.read("DR") == feb_28);
  CHECK(r.read("CR") == 1u << 6);
  r.write("CR", 0);
  CHECK(r.read("TR") == 0x23'5958);
  r.write("ISR", 0);
  r.later(3 * second + 10 * ms);
  CHECK(r.read("TR") == 0x00'0001);
  CHECK(r.read("DR") == march_1);
  CHECK(r.isr(1u << 4) == 1);
}

TEST_CASE(
    "RM0351 38.3.7: INITF sets at the second RTCCLK edge after INIT, counting at the fourth after")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.write("ISR", init_bit);
  auto const entered_at = r.now();
  r.run_until(entered_at + rtc_cycle + us);
  CHECK(r.isr(initf_bit) == 0);
  r.run_until(entered_at + 2 * rtc_cycle + us);
  CHECK(r.isr(initf_bit) == 1);
  r.write("PRER", 0);
  r.write("PRER", 0);
  r.write("ISR", 0);
  CHECK(r.isr(initf_bit) == 0);
  auto const exited = r.now();
  r.run_until(exited + 4 * rtc_cycle + rtc_cycle / 2);
  CHECK(r.read("TR") == 0x00'0000);
  r.run_until(exited + 5 * rtc_cycle + rtc_cycle / 2);
  CHECK(r.read("TR") == 0x00'0001);
}

TEST_CASE(
    "RM0351 38.3.7: 0xCA then 0x53 unlocks, a wrong key locks, a system reset keeps the key "
    "state, a power-on locks")
{
  rig r;
  r.start_clock();
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
  r.f->target->reset(reset_kind::software);
  r.idle_again();
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 1);
  r.f->target->reset(reset_kind::power_on);
  r.idle_again();
  r.start_clock();
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
}

TEST_CASE(
    "RM0351 38.3.4 and 38.3.8, inferred: RSF sets at the first RTCCLK edge after a "
    "clear under the key")
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
  r.run_until(cleared + rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE(
    "RM0351 38.6.4, inferred: init mode clears RSF, set at the first edge after "
    "counting restarts")
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
  r.run_until(exited + 4 * rtc_cycle - us);
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(exited + 5 * rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE(
    "RM0351 38.6.3, inferred: BYPSHAD reads the counters with RSF clear, RSF sets "
    "at the first edge after its clear")
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
  r.idle_again();
  CHECK(r.cpu_read("DR") == 0x00'2101);
  r.later(second);
  CHECK(r.cpu_read("TR") == 0x00'0001);
  r.write("CR", 0);
  auto const cleared = r.now();
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(cleared + rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE("RM0351 38.3.7: a 0 clears ALRAF to TAMP3F without the key, INIT and RSF need it")
{
  rig        r;
  auto&      regs   = r.f->target->peripheral("RTC");
  auto const offset = r.reg("RTC", "ISR").offset;
  auto const flags  = 0xFFu << 8;
  regs.poke(offset, regs.peek(offset) | flags | rsf_bit);
  r.write("ISR", 0);
  CHECK((r.read("ISR") & flags) == 0);
  CHECK(r.isr(rsf_bit) == 1);
  CHECK((r.read("ISR") & 0x7u) == 0x7u);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
}

TEST_CASE("RM0351 38.3.8: an SSR or TR read holds TR and DR until DR is read")
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
}

TEST_CASE(
    "RM0351 38.3.8: after a system reset CPU reads of TR, DR and SSR give reset values until RSF")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("TR", 0x12'3456);
  r.write("DR", feb_28);
  r.write("ISR", 0);
  r.later(ms);
  r.f->target->reset(reset_kind::software);
  r.idle_again();
  REQUIRE(r.isr(rsf_bit) == 0);
  CHECK(r.cpu_read("SSR") == 0);
  CHECK(r.cpu_read("TR") == 0);
  CHECK(r.cpu_read("DR") == 0x00'2101);
  r.later(rtc_cycle + us);
  REQUIRE(r.isr(rsf_bit) == 1);
  CHECK(r.cpu_read("TR") == 0x12'3456);
  CHECK(r.cpu_read("DR") == feb_28);
}

TEST_CASE(
    "RM0351 6.1.3: a brown-out keeps the calendar and the backup registers, a power-on resets "
    "them")
{
  for(auto const kind : {reset_kind::brown_out, reset_kind::power_on})
  {
    CAPTURE(kind);
    rig r;
    r.start_clock();
    r.enter_init();
    r.write("DR", feb_28);
    r.write("ISR", 0);
    r.write("BKP3R", 0x1234);
    r.f->target->reset(kind);
    r.idle_again();
    auto const kept = kind == reset_kind::brown_out;
    CHECK(r.read("BKP3R") == (kept ? 0x1234u : 0u));
    CHECK(r.read("DR") == (kept ? feb_28 : 0x00'2101u));
  }
}

TEST_CASE("RM0351 38.3.7: RTC_TAMPCR, RTC_OR and the backup registers take writes, key locked")
{
  rig r;
  r.cpu_write("TAMPCR", 3u << 11);
  r.cpu_write("OR", 0x3);
  r.cpu_write("BKP31R", 0xA5A5'0031);
  CHECK(r.read("TAMPCR") == 3u << 11);
  CHECK(r.read("OR") == 0x3);
  CHECK(r.read("BKP31R") == 0xA5A5'0031);
  CHECK(r.warnings() == 0);
}

TEST_CASE(
    "each unmodelled L4 RTC function logs once and raises model_error at the CPU write under "
    "strict")
{
  struct row
  {
    std::string_view reg;
    std::uint32_t    value;
    bool             init;
  };
  for(auto const& [reg, value, init] : {
          row{"CR",     1u << 24, false},
          row{"CR",     1u << 23, false},
          row{"CR",     1u << 21, false},
          row{"CR",     1u << 4,  true },
          row{"CALR",   0x100,    false},
          row{"SHIFTR", 0x10,     false}
  })
  {
    CAPTURE(reg);
    CAPTURE(value);
    {
      rig r;
      r.start_clock();
      init ? r.enter_init() : r.unlock();
      auto const before = r.warnings();
      r.cpu_write(reg, value);
      r.cpu_write(reg, value);
      CHECK(r.warnings() == before + 1);
      CHECK(r.read(reg) == (reg == "SHIFTR" ? 0 : value));
    }
    rig r{true};
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

TEST_CASE("RM0351 38.6.3: CR bit 7 has no DCE, a CPU write in init mode under strict logs nothing")
{
  rig r{true};
  r.start_clock();
  r.enter_init();
  auto const before = r.warnings();
  r.cpu_write("CR", 1u << 7);
  CHECK(r.warnings() == before);
}

TEST_CASE("coverage reports CPU reads of TAMPCR and OR as storage_only, TSTR and TR as served")
{
  rig r;
  r.f->target->enable_coverage({}, true);
  for(auto const* name : {"TSTR", "TAMPCR", "OR", "TR"})
  {
    static_cast<void>(r.cpu_read(name));
  }
  std::vector<std::string> names;
  for(auto const& entry : r.f->target->coverage().unmodelled)
  {
    CHECK(entry.peripheral == "RTC");
    CHECK(entry.reason == popcorn::core::unmodelled_reason::storage_only);
    names.push_back(entry.reg);
  }
  std::ranges::sort(names);
  CHECK(names == std::vector<std::string>{"OR", "TAMPCR"});
}

TEST_CASE("RM0351 48.16.4: with DBG_RTC_STOP of DBGMCU_APB1FZR1 a held core stops the calendar")
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

TEST_CASE("checklist 39: the L4 RTC variant refuses a description that disagrees with it")
{
  CHECK_NOTHROW(static_cast<void>(make_machine(l476rc())));
  for(auto const name : {"OR", "TAMPCR", "SSR", "CALR"})
  {
    CAPTURE(name);
    CHECK_THROWS_AS(
        static_cast<void>(make_machine(without_register(l476rc(), name))), popcorn::config_error);
  }
  for(auto const name : {"ITSE", "BYPSHAD"})
  {
    CAPTURE(name);
    CHECK_THROWS_AS(
        static_cast<void>(make_machine(without_field(l476rc(), "CR", name))),
        popcorn::config_error);
  }
  CHECK_THROWS_AS(
      static_cast<void>(make_machine(without_field(l476rc(), "TAMPCR", "TAMP3E"))),
      popcorn::config_error);
  CHECK_THROWS_AS(static_cast<void>(make_machine(l476rc(), stm32::l1_rtc)), popcorn::config_error);
}

TEST_CASE("RM0351 38.3.7: the LL init sets 2026-02-28 23:59:58 and 3 s later reads March 1st")
{
  auto f = entered("calendar");
  run_to_idle(f);
  auto const log = f.log();
  REQUIRE(log.size() == 7);
  CHECK(std::vector(log.begin(), log.begin() + 4) == std::vector<std::uint32_t>{0, 0, 0, 1});
  CHECK(log[4] <= 255);
  CHECK(std::vector(log.end() - 2, log.end()) == std::vector<std::uint32_t>{0x23'5958, feb_28});
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000},
                     .until_time       = vtime{f.target->virtual_clock().time().ps + 3 * second}})
          .reason
      == stop_reason::time);
  f.set("pc", sym(firmware, "read_calendar"));
  auto const after = finish(f);
  REQUIRE(after.size() == log.size() + 3);
  CHECK(
      std::vector(after.end() - 2, after.end()) == std::vector<std::uint32_t>{0x00'0001, march_1});
}

TEST_CASE(
    "RM0351 38.3.8: after a system reset the LL reads reset values until RSF, then the calendar")
{
  auto f = entered("calendar");
  run_to_idle(f);
  auto const before = f.log().size();
  f.target->reset(reset_kind::software);
  f.set("pc", sym(firmware, "read_after_reset"));
  auto const log  = finish(f);
  auto const tail = std::vector(log.begin() + static_cast<std::ptrdiff_t>(before), log.end());
  REQUIRE(tail.size() == 7);
  CHECK(std::vector(tail.begin(), tail.begin() + 4) == std::vector<std::uint32_t>{0, 0, 0, 0x2101});
  CHECK(tail[5] == 0x23'5958);
  CHECK(tail[6] == feb_28);
}

TEST_CASE(
    "RM0351 38.6.20 and 6.1.3: BKP0R and BKP31R keep their value through a system reset, "
    "BDRST clears them")
{
  auto f = entered("backup_write");
  run_to_idle(f);
  f.target->reset(reset_kind::software);
  f.set("pc", sym(firmware, "backup_read"));
  CHECK(finish(f) == std::vector<std::uint32_t>{0xCAFE'1234, 0x5EED'0031});
  f.set("pc", sym(firmware, "backup_reset"));
  CHECK(finish(f) == std::vector<std::uint32_t>{0xCAFE'1234, 0x5EED'0031, 0, 0});
}

namespace
{
constexpr std::uint32_t flash_key1  = 0x4567'0123;
constexpr std::uint32_t flash_key2  = 0xcdef'89ab;
constexpr std::uint32_t option_key1 = 0x0819'2a3b;
constexpr std::uint32_t option_key2 = 0x4c5d'6e7f;
constexpr std::uint32_t optstrt     = 1u << 17;

void program_rdp(rig const& r, std::uint32_t rdp)
{
  r.write("KEYR", flash_key1, "FLASH");
  r.write("KEYR", flash_key2, "FLASH");
  r.write("OPTKEYR", option_key1, "FLASH");
  r.write("OPTKEYR", option_key2, "FLASH");
  REQUIRE(r.f->field("FLASH", "CR", "OPTLOCK") == 0);
  r.write("OPTR", (r.read("OPTR", "FLASH") & ~0xffu) | rdp, "FLASH");
  r.write("CR", optstrt, "FLASH");
  REQUIRE(r.f->field("FLASH", "CR", "OPTSTRT") == 0);
  r.f->target->reset(reset_kind::option_load);
  r.idle_again();
  REQUIRE(r.f->field("FLASH", "OPTR", "RDP") == rdp);
}
}

TEST_CASE(
    "RM0351 3.5.1: the RDP level 1 to level 0 regression erases every backup register, the "
    "calendar kept")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("DR", feb_28);
  r.write("ISR", 0);
  r.write("BKP0R", 0x1111'0000);
  r.write("BKP31R", 0x1111'0031);
  program_rdp(r, 0xbb);
  CHECK(r.read("BKP0R") == 0x1111'0000);
  CHECK(r.read("BKP31R") == 0x1111'0031);
  program_rdp(r, 0xaa);
  for(auto index = 0; index < 32; ++index)
  {
    CAPTURE(index);
    CHECK(r.read(std::format("BKP{}R", index)) == 0);
  }
  CHECK(r.read("DR") == feb_28);
}

namespace
{
constexpr std::uint32_t alraf       = 1u << 8;
constexpr std::uint32_t alrbf       = 1u << 9;
constexpr std::uint32_t wutf        = 1u << 10;
constexpr std::uint32_t alrawf      = 1u << 0;
constexpr std::uint32_t alrbwf      = 1u << 1;
constexpr std::uint32_t wutwf       = 1u << 2;
constexpr std::uint32_t alrae       = 1u << 8;
constexpr std::uint32_t alrbe       = 1u << 9;
constexpr std::uint32_t wute        = 1u << 10;
constexpr std::uint32_t alraie      = 1u << 12;
constexpr std::uint32_t alrbie      = 1u << 13;
constexpr std::uint32_t wutie       = 1u << 14;
constexpr std::uint32_t date_any    = 1u << 31;
constexpr std::uint32_t hour_any    = 1u << 23;
constexpr std::uint32_t minute_any  = 1u << 15;
constexpr std::uint32_t second_any  = 1u << 7;
constexpr std::uint32_t exti_base   = 0x4001'0400;
constexpr std::uint32_t alarm_line  = 18;
constexpr std::uint32_t wakeup_line = 20;
constexpr std::uint32_t alarm_irq   = 41;
constexpr std::uint32_t wakeup_irq  = 3;

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

void exti_rising(rig const& r, std::uint32_t line)
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
    "RM0351 38.3.5 and Table 62: alarm A or B on 00:00:05 sets ALRxF and raises RTC_Alarm on "
    "EXTI 18, strict or not")
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
        rig        r{strict};
        midnight(r);
        r.write(b ? "ALRMBR" : "ALRMAR", date_any | 0x05);
        if(unmasked)
        {
          exti_rising(r, alarm_line);
        }
        r.cpu_write("CR", b ? alrbe | alrbie : alrae | alraie);
        r.write("ISR", 0);
        auto const exited = r.now();
        auto const cycles = std::uint64_t{5} * 256 * 128;
        r.run_until(exited + (3 + cycles) * rtc_cycle - us);
        CHECK_FALSE(nvic_pending(r, alarm_irq));
        CHECK((exti_read(r, 0x14) & (1u << alarm_line)) == 0);
        CHECK((r.read("ISR") & (alraf | alrbf)) == 0);
        r.run_until(exited + (4 + cycles) * rtc_cycle + us);
        CHECK(nvic_pending(r, alarm_irq) == unmasked);
        CHECK((exti_read(r, 0x14) & (1u << alarm_line)) == (unmasked ? 1u << alarm_line : 0));
        CHECK((r.read("ISR") & (alraf | alrbf)) == flag);
        CHECK(r.read("TR") == 0x00'0005);
        CHECK(r.warnings() == 0);
      }
    }
  }
}

TEST_CASE("RM0351 38.3.5: alarm B with every field masked sets ALRBF every second")
{
  rig r;
  midnight(r);
  r.write("ALRMBR", date_any | hour_any | minute_any | second_any);
  r.write("CR", alrbe);
  r.write("ISR", 0);
  auto const exited = r.now();
  for(std::uint64_t n = 1; n <= 3; ++n)
  {
    CAPTURE(n);
    r.run_until(exited + (3 + n * 256 * 128) * rtc_cycle - us);
    CHECK((r.read("ISR") & alrbf) == 0);
    r.run_until(exited + (4 + n * 256 * 128) * rtc_cycle + us);
    CHECK((r.read("ISR") & alrbf) == alrbf);
    r.write("ISR", ~alrbf & ~init_bit);
  }
}

TEST_CASE(
    "RM0351 38.6.17: alarm A with MASKSS 8 compares SS[7:0] inside a matching second, SS 0x40")
{
  rig r;
  midnight(r);
  r.write("ALRMAR", date_any | 0x02);
  r.write("ALRMASSR", (8u << 24) | 0x40);
  r.write("CR", alrae);
  r.write("ISR", 0);
  auto const exited = r.now();
  auto const tick   = 2ULL * 256 + (255 - 0x40);
  r.run_until(exited + (3 + tick * 128) * rtc_cycle - us);
  CHECK((r.read("ISR") & alraf) == 0);
  r.run_until(exited + (4 + tick * 128) * rtc_cycle + us);
  CHECK((r.read("ISR") & alraf) == alraf);
  CHECK(r.read("SSR") == 0x40);
}

TEST_CASE(
    "RM0351 38.3.7 and 38.6.17, inferred: ALRxWF reads 1, ALRMxR takes writes with ALRxE 1, "
    "ALRMxSSR refuses them")
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
    r.later(3 * rtc_cycle);
    CHECK((r.read("ISR") & written) == written);
    r.write(alarm, 0x12);
    CHECK(r.read(alarm) == 0x12);
    r.write(sub, 0x05);
    CHECK(r.read(sub) == 0);
    r.write("CR", 0);
    r.write(sub, 0x05);
    CHECK(r.read(sub) == 0x05);
  }
}

TEST_CASE(
    "RM0351 38.3.6 and Table 62: wakeup on RTCCLK / 16 sets WUTF every 16 x (WUT + 1) cycles, "
    "EXTI 20, strict or not")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{strict};
    r.start_clock();
    r.unlock();
    exti_rising(r, wakeup_line);
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
      exti_write(r, 0x14, 1u << wakeup_line);
      REQUIRE(r.probe()
                  .services.write(0xE000'E280u, 4, 1u << wakeup_irq, popcorn::initiator::debugger)
                  .has_value());
    }
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE("RM0351 38.3.6: a ck_spre wakeup with WUCKSEL 110 adds 2^16 to WUT")
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
    "RM0351 38.6.4: WUTWF clears 2 cycles after WUTE sets, WUTR and WUCKSEL refuse writes "
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
    "RM0351 Table 230 and 14.3.4, inferred: ALRAF with ALRAIE 0 leaves EXTI 18 low, an ALRAIE "
    "write raises it, clearing WUTF is a falling edge on EXTI 20")
{
  rig r;
  midnight(r);
  exti_rising(r, alarm_line);
  r.write("ALRMAR", date_any | 0x01);
  r.write("CR", alrae);
  r.write("ISR", 0);
  r.later(2 * second);
  REQUIRE((r.read("ISR") & alraf) == alraf);
  CHECK((exti_read(r, 0x14) & (1u << alarm_line)) == 0);
  CHECK_FALSE(nvic_pending(r, alarm_irq));
  r.write("CR", alrae | alraie);
  CHECK((exti_read(r, 0x14) & (1u << alarm_line)) == 1u << alarm_line);
  CHECK(nvic_pending(r, alarm_irq));
  exti_write(r, 0x00, exti_read(r, 0x00) | (1u << wakeup_line));
  exti_write(r, 0x0C, 1u << wakeup_line);
  r.write("WUTR", 0);
  r.write("CR", alrae | alraie | wute | wutie);
  r.later(32 * rtc_cycle);
  REQUIRE((r.read("ISR") & wutf) == wutf);
  CHECK((exti_read(r, 0x14) & (1u << wakeup_line)) == 0);
  r.write("CR", wutie);
  r.write("ISR", ~wutf & ~init_bit);
  CHECK((exti_read(r, 0x14) & (1u << wakeup_line)) == 1u << wakeup_line);
}

TEST_CASE("RM0351 38.5: LL alarm A on 00:00:01 runs RTC_Alarm_IRQHandler through EXTI line 18")
{
  auto f = entered("alarm");
  CHECK(finish(f, 20'000'000) == std::vector<std::uint32_t>{0, 0, 0, 0, 0x00'0001, march_1, 1});
}

TEST_CASE(
    "RM0351 38.5: LL ck_spre wakeup every second runs RTC_WKUP_IRQHandler through EXTI line 20")
{
  auto       f   = entered("wakeup");
  auto const log = finish(f, 20'000'000);
  REQUIRE(log.size() == 7u);
  CHECK(std::vector(log.begin(), log.begin() + 3) == std::vector<std::uint32_t>{0, 0, 0});
  CHECK(std::vector(log.end() - 3, log.end()) == std::vector<std::uint32_t>{0x00'0001, march_1, 3});
}

namespace
{
constexpr std::uint32_t tse         = 1u << 11;
constexpr std::uint32_t tsedge      = 1u << 3;
constexpr std::uint32_t tsie        = 1u << 15;
constexpr std::uint32_t tsf         = 1u << 11;
constexpr std::uint32_t tsovf       = 1u << 12;
constexpr std::uint32_t stamp_line  = 19;
constexpr std::uint32_t stamp_irq   = 2;
constexpr std::uint64_t stamp_delay = 2 * 128;

auto pc(rig const& r) -> stm32::gpio_port&
{
  auto* const found = r.f->model_of<stm32::gpio_port>("GPIOC");
  REQUIRE(found != nullptr);
  return *found;
}

void running_at_midnight(rig const& r)
{
  midnight(r);
  r.write("ISR", 0);
  r.later(1'500 * ms);
}
}

TEST_CASE(
    "RM0351 38.3.13, inferred: a rising PC13 edge with TSE sets TSF 2 ck_apre cycles later, TSTR, "
    "TSDR and TSSSR hold the calendar of the edge, TSIE raises TAMP_STAMP on EXTI 19")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{strict};
    running_at_midnight(r);
    exti_rising(r, stamp_line);
    r.cpu_write("CR", tse | tsie);
    auto const moder = r.f->target->peripheral("GPIOC").peek(r.reg("GPIOC", "MODER").offset);
    REQUIRE((moder >> 26 & 3u) == 3u);
    auto const subsecond = r.read("SSR");
    auto const edge      = r.now();
    pc(r).drive(13, true);
    r.run_until(edge + (stamp_delay - 1) * rtc_cycle - us);
    CHECK((r.read("ISR") & tsf) == 0);
    CHECK(r.read("TSTR") == 0);
    CHECK_FALSE(nvic_pending(r, stamp_irq));
    r.run_until(edge + stamp_delay * rtc_cycle + us);
    CHECK((r.read("ISR") & (tsf | tsovf)) == tsf);
    CHECK(r.read("TSTR") == 0x00'0001);
    CHECK(r.read("TSDR") == (feb_28 & 0xFF3Fu));
    CHECK(r.read("TSSSR") == subsecond);
    CHECK((exti_read(r, 0x14) & (1u << stamp_line)) == 1u << stamp_line);
    CHECK(nvic_pending(r, stamp_irq));
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE("RM0351 38.6.3: TSEDGE 1 stamps on a falling PC13 edge only, TSE 0 stamps nothing")
{
  rig r;
  running_at_midnight(r);
  pc(r).drive(13, true);
  pc(r).drive(13, false);
  r.later(10 * ms);
  CHECK((r.read("ISR") & tsf) == 0);
  r.write("CR", tse | tsedge);
  pc(r).drive(13, true);
  r.later(10 * ms);
  CHECK((r.read("ISR") & tsf) == 0);
  pc(r).drive(13, false);
  r.later(10 * ms);
  CHECK((r.read("ISR") & tsf) == tsf);
}

TEST_CASE(
    "RM0351 38.3.13 and 38.6.13: an edge while TSF is set or pending sets TSOVF and keeps the "
    "stamp, a TSF clear clears TSTR, TSDR and TSSSR")
{
  rig r;
  running_at_midnight(r);
  r.write("CR", tse);
  pc(r).drive(13, true);
  pc(r).drive(13, false);
  r.later(ms);
  pc(r).drive(13, true);
  CHECK((r.read("ISR") & (tsf | tsovf)) == tsovf);
  r.later(10 * ms);
  CHECK((r.read("ISR") & (tsf | tsovf)) == (tsf | tsovf));
  auto const first = r.read("TSSSR");
  r.later(second);
  pc(r).drive(13, false);
  pc(r).drive(13, true);
  r.later(10 * ms);
  CHECK(r.read("TSTR") == 0x00'0001);
  CHECK(r.read("TSSSR") == first);
  r.write("ISR", ~(tsf | tsovf) & ~init_bit);
  CHECK(r.read("TSTR") == 0);
  CHECK(r.read("TSDR") == 0);
  CHECK(r.read("TSSSR") == 0);
  pc(r).drive(13, false);
  pc(r).drive(13, true);
  r.later(10 * ms);
  CHECK((r.read("ISR") & (tsf | tsovf)) == tsf);
  CHECK(r.read("TSTR") == 0x00'0002);
}

TEST_CASE("RM0351 38.3.13: TSE is modelled, a CPU write of it under strict logs nothing")
{
  rig r{true};
  r.start_clock();
  r.unlock();
  auto const before = r.warnings();
  r.cpu_write("CR", tse);
  CHECK(r.warnings() == before);
  CHECK(r.read("CR") == tse);
}

TEST_CASE("RM0351 38.5: LL timestamp on a PC13 rise runs TAMP_STAMP_IRQHandler through EXTI 19")
{
  auto f = entered("timestamp");
  run_to_idle(f);
  auto* const port = f.model_of<stm32::gpio_port>("GPIOC");
  REQUIRE(port != nullptr);
  port->drive_at(13, true, vtime{f.target->virtual_clock().time().ps + 1'500 * ms});
  CHECK(
      finish(f, 2'000'000'000)
      == std::vector<std::uint32_t>{0, 0, 0, 0x23'5959, feb_28 & 0xFF3Fu, 1});
}

namespace
{
constexpr std::uint32_t tamp1e       = 1u << 0;
constexpr std::uint32_t tamp1trg     = 1u << 1;
constexpr std::uint32_t tampie       = 1u << 2;
constexpr std::uint32_t tamp2e       = 1u << 3;
constexpr std::uint32_t tamp2trg     = 1u << 4;
constexpr std::uint32_t tamp3e       = 1u << 5;
constexpr std::uint32_t tamp3trg     = 1u << 6;
constexpr std::uint32_t tampts       = 1u << 7;
constexpr std::uint32_t freq_256     = 7u << 8;
constexpr std::uint32_t filter_2     = 1u << 11;
constexpr std::uint32_t tamppudis    = 1u << 15;
constexpr std::uint32_t tamp1ie      = 1u << 16;
constexpr std::uint32_t tamp1noerase = 1u << 17;
constexpr std::uint32_t tamp1mf      = 1u << 18;
constexpr std::uint32_t tamp2noerase = 1u << 20;
constexpr std::uint32_t tamp1f       = 1u << 13;
constexpr std::uint32_t tamp2f       = 1u << 14;
constexpr std::uint32_t tamp3f       = 1u << 15;
constexpr std::uint64_t tamper_delay = 3 * 128;

auto pa(rig const& r) -> stm32::gpio_port&
{
  auto* const found = r.f->model_of<stm32::gpio_port>("GPIOA");
  REQUIRE(found != nullptr);
  return *found;
}

void fill_backups(rig const& r)
{
  r.write("BKP0R", 0x1111'0000);
  r.write("BKP31R", 0x1111'0031);
}

auto backups_kept(rig const& r) -> bool
{
  return r.read("BKP0R") == 0x1111'0000 and r.read("BKP31R") == 0x1111'0031;
}
}

TEST_CASE(
    "RM0351 38.3.14: a rising PC13 edge with TAMP1E sets TAMP1F at once, erases the backup "
    "registers and with TAMPIE raises TAMP_STAMP on EXTI 19, strict or not")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{strict};
    r.start_clock();
    fill_backups(r);
    exti_rising(r, stamp_line);
    r.cpu_write("TAMPCR", tamp1e | tampie);
    CHECK((r.read("ISR") & tamp1f) == 0);
    pc(r).drive(13, true);
    CHECK((r.read("ISR") & tamp1f) == tamp1f);
    CHECK(r.read("BKP0R") == 0);
    CHECK(r.read("BKP31R") == 0);
    CHECK(nvic_pending(r, stamp_irq));
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE(
    "RM0351 38.6.16: TAMPxTRG 1 detects the falling edge, TAMPxNOERASE keeps the backup "
    "registers, a set TAMPxF blocks the next event until cleared")
{
  rig r;
  r.start_clock();
  pa(r).drive(0, true);
  fill_backups(r);
  r.write("TAMPCR", tamp2e | tamp2trg | tamp2noerase);
  pa(r).drive(0, false);
  CHECK((r.read("ISR") & (tamp1f | tamp2f | tamp3f)) == tamp2f);
  CHECK(backups_kept(r));
  r.write("TAMPCR", tamp2e | tamp2trg);
  pa(r).drive(0, true);
  pa(r).drive(0, false);
  CHECK(backups_kept(r));
  r.write("ISR", ~tamp2f & ~init_bit);
  CHECK((r.read("ISR") & tamp2f) == 0);
  pa(r).drive(0, true);
  CHECK((r.read("ISR") & tamp2f) == 0);
  pa(r).drive(0, false);
  CHECK((r.read("ISR") & tamp2f) == tamp2f);
  CHECK(r.read("BKP0R") == 0);
}

TEST_CASE("RM0351 38.6.16: TAMPxMF masks TAMPxF and keeps the backup registers")
{
  rig r;
  r.start_clock();
  fill_backups(r);
  r.write("TAMPCR", tamp1e | tamp1mf);
  pc(r).drive(13, true);
  r.later(10 * ms);
  CHECK((r.read("ISR") & tamp1f) == 0);
  CHECK(backups_kept(r));
}

TEST_CASE(
    "RM0351 38.3.14, inferred: TAMPTS stamps the calendar of the edge, TSF and TAMP2F set 3 "
    "ck_apre cycles later, TSE 0")
{
  rig r;
  running_at_midnight(r);
  r.write("TAMPCR", tamp2e | tamp2noerase | tampts);
  auto const subsecond = r.read("SSR");
  auto const edge      = r.now();
  pa(r).drive(0, true);
  r.run_until(edge + (tamper_delay - 1) * rtc_cycle - us);
  CHECK((r.read("ISR") & (tsf | tamp2f)) == 0);
  r.run_until(edge + tamper_delay * rtc_cycle + us);
  CHECK((r.read("ISR") & (tsf | tamp2f)) == (tsf | tamp2f));
  CHECK(r.read("TSTR") == 0x00'0001);
  CHECK(r.read("TSSSR") == subsecond);
}

TEST_CASE(
    "RM0351 38.3.14, inferred: TAMPFLT 1 detects after 2 samples at the active level, sampled at "
    "TAMPFREQ 7, TAMPxF 3 ck_apre cycles after the second sample")
{
  rig r;
  r.start_clock();
  fill_backups(r);
  pa(r).drive(0, true);
  r.write("TAMPCR", tamp2e | tamp2trg | filter_2 | freq_256);
  auto const written = r.now();
  r.run_until(written + (256 + tamper_delay) * rtc_cycle - us);
  CHECK((r.read("ISR") & tamp2f) == 0);
  CHECK(backups_kept(r));
  r.run_until(written + (2 * 256 + tamper_delay + 1) * rtc_cycle + us);
  CHECK((r.read("ISR") & tamp2f) == tamp2f);
  CHECK(r.read("BKP0R") == 0);
}

TEST_CASE(
    "RM0351 38.3.14: the precharge pulls an undriven tamper input high before each sample, "
    "TAMPPUDIS 1 leaves it at its PUPDR pull")
{
  for(auto const precharge : {true, false})
  {
    CAPTURE(precharge);
    rig r;
    r.start_clock();
    r.write("TAMPCR", tamp2e | tamp2trg | filter_2 | freq_256 | (precharge ? 0 : tamppudis));
    r.later(4 * 256 * rtc_cycle);
    CHECK((r.read("ISR") & tamp2f) == (precharge ? tamp2f : 0));
  }
}

TEST_CASE(
    "DS10198 Table 16: RTC_TAMP3 on PE6 never fires on the 64-pin L476RC, where PE6 is not "
    "bonded, while RTC_TAMP2 on PA0 does")
{
  rig         r;
  auto* const porte = r.f->model_of<stm32::gpio_port>("GPIOE");
  REQUIRE(porte != nullptr);
  REQUIRE(porte->bonded_pins() == 0);
  r.start_clock();
  r.write("TAMPCR", tamp2e | tamp2trg | tamp3e | tamp3trg | filter_2 | freq_256 | tamp2noerase);
  r.later(4 * 256 * rtc_cycle);
  CHECK((r.read("ISR") & (tamp2f | tamp3f)) == tamp2f);
}

TEST_CASE(
    "ES0250 2.19.1: clearing TAMPxE of a falling-edge tamper with the pin high sets TAMPxF and "
    "erases, unless an earlier write set TAMPxNOERASE, an undriven pin reads low")
{
  enum class leg : std::uint8_t
  {
    plain,
    earlier,
    same_write,
    undriven,
  };
  for(auto const which : {leg::plain, leg::earlier, leg::same_write, leg::undriven})
  {
    CAPTURE(static_cast<int>(which));
    rig r;
    r.start_clock();
    fill_backups(r);
    r.write("TAMPCR", tamp1e | tamp1trg);
    if(which == leg::undriven)
    {
      r.write("TAMPCR", 0);
      CHECK((r.read("ISR") & tamp1f) == 0);
      CHECK(backups_kept(r));
      continue;
    }
    pc(r).drive(13, true);
    CHECK((r.read("ISR") & tamp1f) == 0);
    if(which == leg::earlier)
    {
      r.write("TAMPCR", tamp1e | tamp1trg | tamp1noerase);
    }
    r.write("TAMPCR", which == leg::same_write ? tamp1noerase : 0);
    CHECK((r.read("ISR") & tamp1f) == tamp1f);
    CHECK(backups_kept(r) == (which == leg::earlier));
  }
}

TEST_CASE("RM0351 38.6.16: TAMP1IE alone raises the tamper line, TAMPIE 0")
{
  rig r;
  r.start_clock();
  exti_rising(r, stamp_line);
  r.write("TAMPCR", tamp1e | tamp1ie | tamp1noerase);
  pc(r).drive(13, true);
  CHECK((exti_read(r, 0x14) & (1u << stamp_line)) == 1u << stamp_line);
  CHECK(nvic_pending(r, stamp_irq));
}

TEST_CASE("RM0351 38.5: LL tamper 2 on a PA0 rise runs TAMP_STAMP_IRQHandler, BKP0R erased")
{
  auto f = entered("tamper");
  run_to_idle(f);
  auto* const port = f.model_of<stm32::gpio_port>("GPIOA");
  REQUIRE(port != nullptr);
  port->drive_at(0, true, vtime{f.target->virtual_clock().time().ps + 10 * ms});
  CHECK(finish(f, 200'000'000) == std::vector<std::uint32_t>{0xCAFE'1234, 0, 1});
}

namespace
{
auto restored(rig const& from) -> std::unique_ptr<rig>
{
  auto const saved = from.f->target->snapshot();
  auto       to    = std::make_unique<rig>();
  to->f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  return to;
}
}

TEST_CASE(
    "checklist 66: a snapshot between the PC13 edge and TSF restores the pending stamp, TSF at "
    "the same cycle, TSOVF on a second edge")
{
  rig r;
  running_at_midnight(r);
  r.write("CR", tse);
  auto const subsecond = r.read("SSR");
  auto const edge      = r.now();
  pc(r).drive(13, true);
  r.run_until(edge + 100 * rtc_cycle);
  auto const copy = restored(r);
  REQUIRE(copy->now() == r.now());
  copy->run_until(edge + (stamp_delay - 1) * rtc_cycle - us);
  CHECK((copy->read("ISR") & (tsf | tsovf)) == 0);
  pc(*copy).drive(13, false);
  pc(*copy).drive(13, true);
  CHECK((copy->read("ISR") & (tsf | tsovf)) == tsovf);
  copy->run_until(edge + stamp_delay * rtc_cycle + us);
  CHECK((copy->read("ISR") & (tsf | tsovf)) == (tsf | tsovf));
  CHECK(copy->read("TSTR") == 0x00'0001);
  CHECK(copy->read("TSSSR") == subsecond);
}

TEST_CASE(
    "checklist 66: a snapshot after the first of two tamper samples restores the count, TAMP2F "
    "and the erase in the same window")
{
  rig r;
  r.start_clock();
  fill_backups(r);
  pa(r).drive(0, true);
  r.write("TAMPCR", tamp2e | tamp2trg | filter_2 | freq_256);
  auto const written = r.now();
  r.run_until(written + 257 * rtc_cycle);
  auto const copy = restored(r);
  copy->run_until(written + (256 + tamper_delay) * rtc_cycle - us);
  CHECK((copy->read("ISR") & tamp2f) == 0);
  CHECK(backups_kept(*copy));
  copy->run_until(written + (2 * 256 + tamper_delay + 1) * rtc_cycle + us);
  CHECK((copy->read("ISR") & tamp2f) == tamp2f);
  CHECK(copy->read("BKP0R") == 0);
}

TEST_CASE(
    "checklist 66: a snapshot inside the TAMPTS latency restores the pending tamper and stamp, "
    "TSF and TAMP2F at the same cycle")
{
  rig r;
  running_at_midnight(r);
  r.write("TAMPCR", tamp2e | tampts);
  fill_backups(r);
  auto const subsecond = r.read("SSR");
  auto const edge      = r.now();
  pa(r).drive(0, true);
  r.run_until(edge + 100 * rtc_cycle);
  auto const copy = restored(r);
  copy->run_until(edge + (tamper_delay - 1) * rtc_cycle - us);
  CHECK((copy->read("ISR") & (tsf | tamp2f)) == 0);
  CHECK(backups_kept(*copy));
  copy->run_until(edge + tamper_delay * rtc_cycle + us);
  CHECK((copy->read("ISR") & (tsf | tamp2f)) == (tsf | tamp2f));
  CHECK(copy->read("TSSSR") == subsecond);
  CHECK(copy->read("BKP0R") == 0);
}
