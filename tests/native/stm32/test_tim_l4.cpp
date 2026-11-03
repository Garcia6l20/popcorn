#include "../support/inert_model.hpp"
#include "factories.hpp"
#include "stm32_fixture.hpp"
#include "trigger_probe.hpp"
#include "variants.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
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
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::uint64_t    us            = 1'000'000;
constexpr std::string_view firmware      = "l476_l4_rcc_ll";
constexpr std::string_view tim_firmware  = "l476_l4_tim_ll";
constexpr std::uint64_t    ms            = 1'000 * us;
constexpr std::uint64_t    ps_per_tick   = 250'000;
constexpr std::uint32_t    sms_trigger   = 6;
constexpr std::uint32_t    sms_external  = 7;
constexpr std::uint32_t    sms_reset_trg = 1u << 16;
constexpr std::uint32_t    master_update = 2;
constexpr std::uint32_t    ece           = 1u << 14;
constexpr std::uint32_t    uifremap      = 1u << 11;
constexpr std::uint32_t    dir_down      = 1u << 4;
constexpr std::uint32_t    pwm1          = 6u << 4;
constexpr std::uint32_t    sms_reset     = 4;

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(popcorn::periph::model_context& ctx) : context{ctx}
  {}

  popcorn::periph::model_context& context;
};

auto options(part_description const& desc, bool strict, bool trigger = false)
    -> core::machine_options
{
  auto out               = popcorn::testing::stm32_options();
  out.models             = stm32::factories({}, desc);
  out.strict_peripherals = strict;
  out.models.insert_or_assign(
      "TSC",
      [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<popcorn::periph::model> { return std::make_shared<bus_probe>(ctx); });
  if(trigger)
  {
    out.models.insert_or_assign(
        "DAC1", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<popcorn::testing::trigger_probe>(ctx);
        });
  }
  return out;
}

struct rig
{
  part_description const&        part;
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;
  std::uint64_t                  start{};

  explicit rig(bool strict = false, part_description const& desc = l476rc(), bool trigger = false)
      : part{desc}
  {
    f = std::make_unique<stm32_fixture>(desc, firmware, options(desc, strict, trigger));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    start = now();
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

  [[nodiscard]] auto read(std::string_view unit, std::string_view name) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at(unit, name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view unit, std::string_view name, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at(unit, name), 4, value).has_value());
  }

  void cpu_write(std::string_view unit, std::string_view name, std::uint32_t value) const
  {
    auto* const probe = f->model_of<bus_probe>("TSC");
    REQUIRE(probe != nullptr);
    auto const written =
        probe->context.services.write(at(unit, name), 4, value, popcorn::initiator::cpu);
    CHECK(written.has_value());
  }

  [[nodiscard]] auto get(std::string_view unit, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    auto const& fields = reg(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    auto const mask =
        found->width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << found->width) - 1u;
    return (read(unit, name) >> found->lsb) & mask;
  }

  void
      set(std::string_view unit,
          std::string_view name,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const& fields = reg(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    auto const mask =
        found->width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << found->width) - 1u;
    write(unit, name, (read(unit, name) & ~(mask << found->lsb)) | ((value & mask) << found->lsb));
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

  void at_tick(std::uint64_t ticks) const
  {
    run_until(start + ticks * ps_per_tick);
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
  }

  void clock(std::string_view unit) const
  {
    auto const apb2 = std::ranges::contains(
        std::array<std::string_view, 5>{"TIM1", "TIM8", "TIM15", "TIM16", "TIM17"}, unit);
    set("RCC", apb2 ? "APB2ENR" : "APB1ENR1", std::string{unit} + "EN", 1);
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    auto const word = f->target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }

  void base(std::string_view unit, std::uint32_t reload) const
  {
    clock(unit);
    write(unit, "PSC", 0);
    write(unit, "ARR", reload);
    write(unit, "EGR", 1);
    write(unit, "SR", 0);
  }

  void enable(std::string_view unit) const
  {
    write(unit, "CR1", read(unit, "CR1") | 1u);
  }

  [[nodiscard]] auto port(std::string_view name, std::uint8_t pin, std::uint32_t af) const
      -> stm32::gpio_port&
  {
    set("RCC", "AHB2ENR", std::string{name} + "EN", 1);
    auto* const found = f->model_of<stm32::gpio_port>(name);
    REQUIRE(found != nullptr);
    found->drive(pin, false);
    auto const n = std::to_string(pin);
    set(name, "MODER", "MODER" + n, 2);
    set(name, pin < 8 ? "AFRL" : "AFRH", (pin < 8 ? "AFRL" : "AFRH") + n, af);
    return *found;
  }
};

struct firmware_run
{
  std::unique_ptr<stm32_fixture>      f;
  std::vector<std::uint64_t>          updates;
  std::vector<std::uint64_t>          compares;
  std::vector<core::intercept_handle> hooks;

  explicit firmware_run(std::string_view entry, bool trigger = false)
  {
    f = std::make_unique<stm32_fixture>(l476rc(), tim_firmware, options(l476rc(), false, trigger));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(tim_firmware, "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym(tim_firmware, entry));
    record("on_update", updates);
    record("on_compare", compares);
  }

  void record(std::string_view symbol, std::vector<std::uint64_t>& into)
  {
    hooks.push_back(
        f->target->add_intercept(sym(tim_firmware, symbol), [&into](core::intercept_call const& c) {
          into.push_back(c.target().virtual_clock().time().ps);
          return core::intercept_result::proceed();
        }));
  }

  [[nodiscard]] auto started() const -> std::uint64_t
  {
    auto const stopped = f->run_to_stop(
        {.max_instructions = icount{1'000'000}, .until = sym(tim_firmware, "on_started")});
    REQUIRE(stopped.reason == stop_reason::address);
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto finish(std::uint64_t instructions = 2'000'000) const
      -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{instructions}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }

  [[nodiscard]] auto probe(std::string_view signal) const -> popcorn::testing::trigger_probe&
  {
    auto* const found = f->model_of<popcorn::testing::trigger_probe>("DAC1");
    REQUIRE(found != nullptr);
    found->watched = signal;
    return *found;
  }
};

auto gaps(std::vector<std::uint64_t> const& at) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> out;
  for(std::size_t index = 1; index < at.size(); ++index)
  {
    out.push_back(at[index] - at[index - 1]);
  }
  return out;
}

auto timer_of(part_description& desc, std::string_view unit) -> popcorn::peripheral_desc&
{
  auto const timer =
      std::ranges::find(desc.peripherals, std::string{unit}, &popcorn::peripheral_desc::name);
  REQUIRE(timer != desc.peripherals.end());
  return *timer;
}

auto register_of(popcorn::peripheral_desc& timer, std::string_view reg) -> popcorn::register_desc&
{
  auto const found =
      std::ranges::find(timer.registers, std::string{reg}, &popcorn::register_desc::name);
  REQUIRE(found != timer.registers.end());
  return *found;
}

void constructs_throw(part_description const& desc)
{
  CHECK_THROWS_AS(stm32_fixture(desc, firmware, options(desc, false)), popcorn::config_error);
}
}

TEST_CASE("RM0351 30.1 to 33.1: TIM1 to TIM8 and TIM15 to TIM17 have a model on STM32L476RCTx")
{
  rig const r;
  for(auto const* unit :
      {"TIM1", "TIM2", "TIM3", "TIM4", "TIM5", "TIM6", "TIM7", "TIM8", "TIM15", "TIM16", "TIM17"})
  {
    CAPTURE(unit);
    CHECK(r.f->target->model(unit) != nullptr);
  }
}

TEST_CASE("RM0351 31.4.15: ARR resets to 0xFFFFFFFF, TIM2 and TIM5 count on 32 bits, TIM3, TIM4 16")
{
  rig r;
  for(auto const* unit : {"TIM2", "TIM3", "TIM4", "TIM5"})
  {
    CAPTURE(unit);
    CHECK(r.read(unit, "ARR") == 0xffff'ffffu);
    r.clock(unit);
    r.write(unit, "CCR1", 0x1'0005);
    r.enable(unit);
  }
  r.at_tick(70'000);
  for(auto const* unit : {"TIM2", "TIM5"})
  {
    CAPTURE(unit);
    CHECK(r.read(unit, "CNT") == 70'000);
    CHECK(r.get(unit, "SR", "UIF") == 0);
    CHECK(r.get(unit, "SR", "CC1IF") == 1);
    CHECK(r.read(unit, "CCR1") == 0x1'0005);
  }
  for(auto const* unit : {"TIM3", "TIM4"})
  {
    CAPTURE(unit);
    CHECK(r.read(unit, "CNT") == 70'000 - 65'536);
    CHECK(r.get(unit, "SR", "UIF") == 1);
    CHECK(r.read(unit, "CCR1") == 0x0005);
  }
}

TEST_CASE("TIM3 and TIM4 ARR bits 31:16 keep their reset ones after a write, inferred")
{
  rig r;
  for(auto const* unit : {"TIM3", "TIM4"})
  {
    CAPTURE(unit);
    r.write(unit, "ARR", 0x0001'2345);
    CHECK(r.read(unit, "ARR") == 0xffff'2345u);
    r.write(unit, "ARR", 0);
    CHECK(r.read(unit, "ARR") == 0xffff'0000u);
  }
  r.write("TIM2", "ARR", 0x0001'2345);
  CHECK(r.read("TIM2", "ARR") == 0x0001'2345u);
}

TEST_CASE("every entry of RM0351 Tables 207, 202 and 210 reaches its own source and no other")
{
  struct row
  {
    std::string_view slave;
    std::uint32_t    select;
    std::uint32_t    remap;
    std::string_view source;
  };
  constexpr std::array<row, 29> table{
      {{"TIM2", 0, 0, "TIM1"},       {"TIM2", 1, 0, "TIM8"},      {"TIM2", 1, 1, "OTG_FS"},
       {"TIM2", 2, 0, "TIM3"},       {"TIM2", 3, 0, "TIM4"},      {"TIM3", 0, 0, "TIM1"},
       {"TIM3", 1, 0, "TIM2"},       {"TIM3", 2, 0, "TIM15"},     {"TIM3", 3, 0, "TIM4"},
       {"TIM4", 0, 0, "TIM1"},       {"TIM4", 1, 0, "TIM2"},      {"TIM4", 2, 0, "TIM3"},
       {"TIM4", 3, 0, "TIM8"},       {"TIM5", 0, 0, "TIM2"},      {"TIM5", 1, 0, "TIM3"},
       {"TIM5", 2, 0, "TIM4"},       {"TIM5", 3, 0, "TIM8"},      {"TIM1", 0, 0, "TIM15"},
       {"TIM1", 1, 0, "TIM2"},       {"TIM1", 2, 0, "TIM3"},      {"TIM1", 3, 0, "TIM4"},
       {"TIM8", 0, 0, "TIM1"},       {"TIM8", 1, 0, "TIM2"},      {"TIM8", 2, 0, "TIM4"},
       {"TIM8", 3, 0, "TIM5"},       {"TIM15", 0, 0, "TIM1"},     {"TIM15", 1, 0, "TIM3"},
       {"TIM15", 2, 0, "TIM16_OC1"}, {"TIM15", 3, 0, "TIM17_OC1"}}
  };
  constexpr std::array<std::string_view, 7> masters{"TIM1", "TIM2", "TIM3", "TIM4",
                                                    "TIM5", "TIM8", "TIM15"};
  for(auto const& [slave, select, remap, source] : table)
  {
    CAPTURE(slave);
    CAPTURE(select);
    CAPTURE(remap);
    rig              r;
    auto const       oc1        = source.ends_with("_OC1");
    auto const       has_source = oc1 or std::ranges::contains(masters, source);
    std::string_view bystander{};
    for(auto const unit : masters)
    {
      if(unit != slave and unit != source)
      {
        bystander = unit;
      }
    }
    r.base(bystander, 99);
    r.write(bystander, "CR2", master_update << 4);
    auto const unit = oc1 ? source.substr(0, 5) : source;
    if(oc1)
    {
      r.base(unit, 199);
      r.write(unit, "CCR1", 100);
      r.write(unit, "CCMR1_Output", pwm1);
      r.set(unit, "CCER", "CC1E", 1);
    }
    else if(has_source)
    {
      r.base(source, 199);
      r.write(source, "CR2", master_update << 4);
    }
    r.base(slave, 0xffff);
    if(remap != 0)
    {
      r.set(slave, "OR1", "ITR1_RMP", 1);
    }
    r.write(slave, "SMCR", sms_trigger | (select << 4));
    r.enable(bystander);
    if(has_source)
    {
      r.enable(unit);
    }
    r.at_tick(250);
    if(has_source)
    {
      CHECK(r.get(slave, "CR1", "CEN") == 1);
      CHECK(r.read(slave, "CNT") == 50);
    }
    else
    {
      CHECK(r.get(slave, "CR1", "CEN") == 0);
      CHECK(r.warnings() == 1);
    }
  }
  REQUIRE(stm32::l4_triggers.size() == table.size());
  for(std::size_t index = 0; index < table.size(); ++index)
  {
    auto const& [slave, select, remap, source] = table[index];
    auto const& entry                          = stm32::l4_triggers[index];
    CAPTURE(index);
    CHECK(entry.slave == slave);
    CHECK(entry.select == select);
    CHECK(entry.remap.has_value() == (slave == "TIM2" and select == 1));
    CHECK((entry.remap ? entry.remap->value : 0u) == remap);
    auto expected = source == "OTG_FS" ? std::string{"otg_fs_sof"} : std::string{source};
    std::ranges::transform(expected, expected.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    CHECK(entry.source == expected);
  }
}

TEST_CASE("under strict a firmware TIM2 OR1 write that moves TS 001 onto OTG_FS_SOF throws")
{
  rig r{true};
  r.base("TIM2", 0xffff);
  r.write("TIM2", "SMCR", sms_trigger | (1u << 4));
  CHECK_THROWS_AS(r.cpu_write("TIM2", "OR1", 1), popcorn::model_error);
  CHECK(r.read("TIM2", "OR1") == 0);
  r.write("TIM2", "OR1", 1);
  CHECK(r.read("TIM2", "OR1") == 1);
}

TEST_CASE("RM0351 31.4.3: SMS 1000 resets and starts TIM3 at each TIM2 update on ITR1")
{
  rig r;
  r.base("TIM2", 99);
  r.write("TIM2", "CR2", master_update << 4);
  r.base("TIM3", 0xffff);
  r.write("TIM3", "CNT", 1000);
  r.write("TIM3", "SMCR", sms_reset_trg | (1u << 4));
  CHECK(r.get("TIM3", "SMCR", "SMS_1") == 1);
  r.enable("TIM2");
  r.at_tick(150);
  CHECK(r.get("TIM3", "CR1", "CEN") == 1);
  CHECK(r.read("TIM3", "CNT") == 50);
  r.at_tick(230);
  CHECK(r.read("TIM3", "CNT") == 30);
  CHECK(r.get("TIM3", "SR", "TIF") == 1);
  CHECK(r.warnings() == 0);
}

TEST_CASE("RM0351 31.3.16: UIFREMAP gives UIF in CNT bit 31, over the count on TIM2 and TIM5")
{
  rig r;
  for(auto const* unit : {"TIM3", "TIM4", "TIM5"})
  {
    CAPTURE(unit);
    r.base(unit, 9);
    r.enable(unit);
  }
  r.base("TIM2", 0xffff'ffff);
  r.write("TIM2", "CNT", 0x8000'0000);
  r.write("TIM2", "CR1", uifremap | 1u);
  r.at_tick(15);
  for(auto const* unit : {"TIM3", "TIM4", "TIM5"})
  {
    CAPTURE(unit);
    CHECK(r.read(unit, "CNT") == 5);
    r.write(unit, "CR1", uifremap | 1u);
    CHECK(r.read(unit, "CNT") == 0x8000'0005u);
    r.write(unit, "SR", 0);
    CHECK(r.read(unit, "CNT") == 5);
  }
  CHECK(r.get("TIM2", "SR", "UIF") == 0);
  CHECK(r.read("TIM2", "CNT") == 0x0000'000fu);
  r.write("TIM2", "CR1", 1u);
  CHECK(r.read("TIM2", "CNT") == 0x8000'000fu);
  r.write("TIM2", "CR1", uifremap | 1u);
  REQUIRE(r.f->target->mem().write(r.at("TIM2", "CNT"), 2, 0x0020).has_value());
  CHECK(r.read("TIM2", "CNT") == 0x0000'0020u);
  r.write("TIM2", "CR1", 1u);
  CHECK(r.read("TIM2", "CNT") == 0x8000'0020u);
}

TEST_CASE("RM0351 31.3.2: TIM2 to TIM5 count down with DIR 1, the update at the wrap below 0")
{
  rig r;
  for(auto const* unit : {"TIM2", "TIM3", "TIM4", "TIM5"})
  {
    CAPTURE(unit);
    r.base(unit, 99);
    r.write(unit, "CR1", dir_down | 1u);
  }
  r.at_tick(130);
  for(auto const* unit : {"TIM2", "TIM3", "TIM4", "TIM5"})
  {
    CAPTURE(unit);
    CHECK(r.read(unit, "CNT") == 70);
    CHECK(r.get(unit, "SR", "UIF") == 1);
    CHECK(r.get(unit, "CR1", "DIR") == 1);
  }
}

TEST_CASE("RM0351 31.4.22: TIM2 TI4_RMP 00 captures PA3 edges on CH4, 01 to 11 read low and log")
{
  for(std::uint32_t const remap : {0u, 1u, 2u, 3u})
  {
    CAPTURE(remap);
    rig         r;
    auto&       porta = r.port("GPIOA", 3, 1);
    auto const& unit  = "TIM2";
    r.base(unit, 0xffff);
    r.set(unit, "OR1", "TI4_RMP", remap);
    r.set(unit, "CCMR2_Input", "CC4S", 1);
    r.set(unit, "CCER", "CC4E", 1);
    r.enable(unit);
    porta.drive_at(3, true, vtime{r.start + 40 * ps_per_tick});
    r.at_tick(60);
    CHECK(r.get(unit, "SR", "CC4IF") == (remap == 0 ? 1u : 0u));
    CHECK(r.read(unit, "CCR4") == (remap == 0 ? 40u : 0u));
    CHECK(r.warnings() == (remap == 0 ? 0u : 1u));
  }
}

TEST_CASE("an OR1 write of TI4_RMP or TI1_RMP 00 over a held-high pin captures at the write")
{
  struct leg
  {
    std::string_view unit;
    std::string_view field;
    std::uint8_t     pin;
    std::uint32_t    af;
    std::string_view ccmr;
    std::string_view channel;
  };
  for(auto const& [unit, field, pin, af, ccmr, channel] : {
          leg{"TIM2", "TI4_RMP", 3, 1, "CCMR2_Input", "4"},
          leg{"TIM3", "TI1_RMP", 6, 2, "CCMR1_Input", "1"},
  })
  {
    CAPTURE(unit);
    rig   r;
    auto& porta = r.port("GPIOA", pin, af);
    porta.drive(pin, true);
    r.base(unit, 0xffff);
    r.set(unit, "OR1", field, 1);
    r.set(unit, ccmr, std::format("CC{}S", channel), 1);
    r.set(unit, "CCER", std::format("CC{}E", channel), 1);
    r.enable(unit);
    r.at_tick(40);
    CHECK(r.get(unit, "SR", std::format("CC{}IF", channel)) == 0);
    r.set(unit, "OR1", field, 0);
    r.at_tick(60);
    CHECK(r.get(unit, "SR", std::format("CC{}IF", channel)) == 1);
    CHECK(r.read(unit, std::format("CCR{}", channel)) == 40);
  }
}

TEST_CASE("RM0351 31.4.23: TIM3 TI1_RMP 00 captures PA6 edges on CH1, 01 to 11 read low and log")
{
  for(std::uint32_t const remap : {0u, 2u})
  {
    CAPTURE(remap);
    rig         r;
    auto&       porta = r.port("GPIOA", 6, 2);
    auto const& unit  = "TIM3";
    r.base(unit, 0xffff);
    r.set(unit, "OR1", "TI1_RMP", remap);
    r.set(unit, "CCMR1_Input", "CC1S", 1);
    r.set(unit, "CCER", "CC1E", 1);
    r.enable(unit);
    porta.drive_at(6, true, vtime{r.start + 40 * ps_per_tick});
    r.at_tick(60);
    CHECK(r.get(unit, "SR", "CC1IF") == (remap == 0 ? 1u : 0u));
    CHECK(r.warnings() == (remap == 0 ? 0u : 1u));
  }
}

TEST_CASE("RM0351 31.4.22, 31.4.24: TIM2 counts PA0 TIM2_ETR edges with ETR_RMP 0 and ETRSEL 000")
{
  struct leg
  {
    std::string_view reg;
    std::string_view field;
    std::uint32_t    value;
  };
  for(auto const& [reg, field, value] : {
          leg{"OR1", "ETR_RMP", 0},
          leg{"OR1", "ETR_RMP", 1},
          leg{"OR2", "ETRSEL",  1},
          leg{"OR2", "ETRSEL",  2},
          leg{"OR2", "ETRSEL",  5}
  })
  {
    CAPTURE(field);
    CAPTURE(value);
    rig   r;
    auto& porta = r.port("GPIOA", 0, 14);
    r.base("TIM2", 0xffff);
    r.set("TIM2", reg, field, value);
    r.write("TIM2", "SMCR", ece);
    r.enable("TIM2");
    auto const t0 = r.now();
    for(std::uint64_t edge = 0; edge < 5; ++edge)
    {
      porta.drive_at(0, true, vtime{t0 + (20 * edge + 10) * us});
      porta.drive_at(0, false, vtime{t0 + (20 * edge + 20) * us});
    }
    r.run_until(t0 + 95 * us);
    CHECK(r.read("TIM2", "CNT") == (value == 0 ? 5u : 0u));
    CHECK(r.warnings() == (value == 0 ? 0u : 1u));
  }
}

TEST_CASE("RM0351 31.4.25: TIM3 counts PD2 TIM3_ETR edges with ETRSEL 000, COMP1 reads low")
{
  for(std::uint32_t const select : {0u, 1u})
  {
    CAPTURE(select);
    rig   r;
    auto& portd = r.port("GPIOD", 2, 2);
    r.base("TIM3", 0xffff);
    r.set("TIM3", "OR2", "ETRSEL", select);
    r.write("TIM3", "SMCR", ece);
    r.enable("TIM3");
    auto const t0 = r.now();
    for(std::uint64_t edge = 0; edge < 3; ++edge)
    {
      portd.drive_at(2, true, vtime{t0 + (20 * edge + 10) * us});
      portd.drive_at(2, false, vtime{t0 + (20 * edge + 20) * us});
    }
    r.run_until(t0 + 75 * us);
    CHECK(r.read("TIM3", "CNT") == (select == 0 ? 3u : 0u));
    CHECK(r.warnings() == select);
  }
}

TEST_CASE("an OR1 or OR2 write that gives a held-high ETR pin back to TIM2 is a rising ETR edge")
{
  for(auto const* field : {"ETR_RMP", "ETRSEL"})
  {
    CAPTURE(field);
    auto const* reg = std::string_view{field} == "ETR_RMP" ? "OR1" : "OR2";
    rig         r;
    auto&       porta = r.port("GPIOA", 0, 14);
    porta.drive(0, true);
    r.base("TIM2", 0xffff);
    r.write("TIM2", "SMCR", ece);
    r.enable("TIM2");
    r.at_tick(10);
    auto const before = r.read("TIM2", "CNT");
    r.set("TIM2", reg, field, 1);
    r.at_tick(20);
    CHECK(r.read("TIM2", "CNT") == before);
    r.set("TIM2", reg, field, 0);
    r.at_tick(30);
    CHECK(r.read("TIM2", "CNT") == before + 1);
  }
}

TEST_CASE("under strict a firmware OR1 or OR2 write that selects an internal input throws")
{
  struct leg
  {
    std::string_view unit;
    std::string_view reg;
    std::uint32_t    value;
  };
  for(auto const& [unit, reg, value] : {
          leg{"TIM2",  "OR1", 1u << 1 },
          leg{"TIM2",  "OR1", 1u << 2 },
          leg{"TIM2",  "OR2", 1u << 14},
          leg{"TIM3",  "OR1", 1u      },
          leg{"TIM3",  "OR2", 1u << 14},
          leg{"TIM1",  "OR1", 1u << 4 },
          leg{"TIM1",  "OR1", 1u      },
          leg{"TIM1",  "OR1", 1u << 2 },
          leg{"TIM1",  "OR2", 1u << 15},
          leg{"TIM8",  "OR1", 1u << 4 },
          leg{"TIM8",  "OR1", 1u << 1 },
          leg{"TIM8",  "OR1", 3u << 2 },
          leg{"TIM8",  "OR2", 1u << 14},
          leg{"TIM15", "OR1", 1u      },
          leg{"TIM15", "OR1", 1u << 1 },
          leg{"TIM16", "OR1", 3u      },
          leg{"TIM17", "OR1", 2u      }
  })
  {
    CAPTURE(unit);
    CAPTURE(reg);
    CAPTURE(value);
    rig        r{true};
    auto const reset = r.read(unit, reg);
    r.clock(unit);
    CHECK_THROWS_AS(r.cpu_write(unit, reg, reset | value), popcorn::model_error);
    CHECK(r.read(unit, reg) == reset);
    r.write(unit, reg, reset | value);
    CHECK(r.read(unit, reg) == (reset | value));
    CHECK(r.warnings() == 1);
  }
}

TEST_CASE("an L4 timer description against the l4_tim variant throws at construction")
{
  struct leg
  {
    std::string_view unit;
    std::string_view reg;
    std::string_view field;
    std::uint8_t     lsb;
    bool             add;
  };
  for(auto const& [unit, reg, field, lsb, add] : {
          leg{"TIM3",  "SMCR", "TS_1",         20, true },
          leg{"TIM3",  "CR2",  "MMS_1",        25, true },
          leg{"TIM4",  "SMCR", "SMS_1",        16, false},
          leg{"TIM5",  "CR1",  "UIFREMAP",     11, false},
          leg{"TIM2",  "CR1",  "DIR",          4,  false},
          leg{"TIM2",  "OR1",  "ITR1_RMP",     0,  false},
          leg{"TIM2",  "OR1",  "TI4_RMP",      2,  false},
          leg{"TIM2",  "OR2",  "ETRSEL",       14, false},
          leg{"TIM3",  "OR1",  "TI1_RMP",      0,  false},
          leg{"TIM5",  "SMCR", "ETP",          15, false},
          leg{"TIM15", "SMCR", "SMS_1",        16, false},
          leg{"TIM15", "OR1",  "ENCODER_MODE", 1,  false},
          leg{"TIM16", "CR2",  "MMS",          4,  true },
          leg{"TIM17", "OR1",  "TI1_RMP",      0,  false},
  })
  {
    CAPTURE(unit);
    CAPTURE(field);
    auto  desc      = l476rc();
    auto& register_ = register_of(timer_of(desc, unit), reg);
    if(add)
    {
      register_.fields.push_back(
          {.name = std::string{field}, .lsb = lsb, .width = 1, .acc = popcorn::access::read_write});
    }
    else
    {
      REQUIRE(
          std::erase_if(
              register_.fields, [&](popcorn::field_desc const& bits) { return bits.name == field; })
          == 1);
    }
    constructs_throw(desc);
  }
}

TEST_CASE("an ETR input_remaps row on a timer with no ETR pin throws at construction")
{
  static constexpr auto remaps  = std::to_array<stm32::input_remap>({
      {.slave      = "TIM4",
       .channel    = stm32::tim_etr_input,
       .reg        = "SMCR",
       .field      = "ETF",
       .pin_values = 0b1},
  });
  auto                  variant = stm32::l4_tim;
  variant.input_remaps          = remaps;
  variant.etr_pins              = {};
  auto opts                     = options(l476rc(), false);
  opts.models.insert_or_assign("TIM4", stm32::tim_factory(variant));
  CHECK_THROWS_AS(stm32_fixture(l476rc(), firmware, std::move(opts)), popcorn::config_error);
}

TEST_CASE("TIM2 at 1 kHz on HSI16 raises 100 update IRQs exactly 1 ms apart, firmware")
{
  firmware_run run{"tim2_update"};
  auto const   started = run.started();
  CHECK(run.finish() == std::vector<std::uint32_t>{100});
  REQUIRE(run.updates.size() == 100);
  CHECK(run.updates.front() - started > ms);
  CHECK(run.updates.front() - started < ms + 2 * us);
  CHECK(gaps(run.updates) == std::vector<std::uint64_t>(99, ms));
}

TEST_CASE("TIM2 PWM mode 1 on CH1 flags CC1 250 us into each 1 ms period, firmware")
{
  firmware_run run{"tim2_pwm"};
  auto const   started = run.started();
  auto const   logged  = run.finish();
  REQUIRE(run.updates.size() == 10);
  REQUIRE(run.compares.size() >= 10);
  CHECK(
      logged == std::vector<std::uint32_t>{static_cast<std::uint32_t>(run.compares.size()), 0x68});
  CHECK(run.compares.front() - started > 250 * us);
  CHECK(run.compares.front() - started < 252 * us);
  CHECK(std::ranges::all_of(gaps(run.compares), [](std::uint64_t gap) { return gap == ms; }));
  for(std::size_t period = 0; period + 1 < run.updates.size(); ++period)
  {
    CAPTURE(period);
    CHECK(run.compares[period + 1] - run.updates[period] == run.compares[1] - run.updates[0]);
  }
  CHECK(run.compares[1] - run.updates[0] > 249 * us);
  CHECK(run.compares[1] - run.updates[0] < 251 * us);
}

TEST_CASE("RM0351 31.3.6: TIM3 captures PA6 rising edges into CCR1 at 1 MHz, firmware")
{
  constexpr std::array<std::uint64_t, 4> rising{100, 300, 700, 1500};
  firmware_run                           run{"tim3_capture"};
  auto* const                            porta = run.f->model_of<stm32::gpio_port>("GPIOA");
  REQUIRE(porta != nullptr);
  porta->drive(6, false);
  auto const started = run.started();
  for(auto const offset : rising)
  {
    porta->drive_at(6, true, vtime{started + offset * us});
    porta->drive_at(6, false, vtime{started + (offset + 50) * us});
  }
  auto const logged = run.finish(4'000'000);
  REQUIRE(logged.size() == 4);
  for(std::size_t index = 1; index < 4; ++index)
  {
    CAPTURE(index);
    CHECK(logged[index] - logged[0] == rising[index] - rising[0]);
  }
}

TEST_CASE(
    "RM0351 33.4.2: TIM6 tim6_trgo pulses at the UG of MMS 000, then each 100 us update, firmware")
{
  firmware_run run{"tim6_trgo", true};
  auto&        probe = run.probe("tim6_trgo");
  probe.listen(true);
  auto const started = run.started();
  CHECK(run.finish() == std::vector<std::uint32_t>{10, 0x20});
  REQUIRE(run.updates.size() == 10);
  CHECK(gaps(run.updates) == std::vector<std::uint64_t>(9, 100 * us));
  REQUIRE(probe.pulses.size() == 11);
  CHECK(probe.pulses.front() <= started);
  auto const updates = std::vector<std::uint64_t>(probe.pulses.begin() + 1, probe.pulses.end());
  CHECK(gaps(updates) == std::vector<std::uint64_t>(9, 100 * us));
  CHECK(updates.front() - started > 99 * us);
  CHECK(updates.front() - started <= 100 * us);
  CHECK(updates.front() < run.updates.front());
}

TEST_CASE(
    "TIM7 at 1 kHz raises 10 update IRQs 1 ms apart, UIFREMAP puts UIF in CNT bit 31, firmware")
{
  firmware_run run{"tim7_update"};
  auto const   started = run.started();
  CHECK(run.finish() == std::vector<std::uint32_t>{10, 0x8000'0000u});
  REQUIRE(run.updates.size() == 10);
  CHECK(run.updates.front() - started > ms);
  CHECK(run.updates.front() - started < ms + 2 * us);
  CHECK(gaps(run.updates) == std::vector<std::uint64_t>(9, ms));
}

TEST_CASE("RM0351 33.4.2: TIM6 and TIM7 TRGO follow MMS 000 UG, 001 CNT_EN and 010 UG or overflow")
{
  for(auto const& [unit, signal] : {
          std::pair{"TIM6", "tim6_trgo"},
          std::pair{"TIM7", "tim7_trgo"},
  })
  {
    CAPTURE(unit);
    rig   r{false, l476rc(), true};
    auto* found = r.f->model_of<popcorn::testing::trigger_probe>("DAC1");
    REQUIRE(found != nullptr);
    found->watched = signal;
    found->listen(true);
    r.base(unit, 9);
    CHECK(found->pulses.size() == 1);
    r.write(unit, "EGR", 1);
    CHECK(found->pulses.size() == 2);
    CHECK(found->falls.size() == 2);
    r.write(unit, "CR2", 1u << 4);
    r.enable(unit);
    CHECK(found->pulses.size() == 3);
    CHECK(found->falls.size() == 2);
    r.write(unit, "CR1", 0);
    CHECK(found->falls.size() == 3);
    r.write(unit, "CR2", master_update << 4);
    r.write(unit, "EGR", 1);
    CHECK(found->pulses.size() == 4);
    auto const from = r.now();
    r.enable(unit);
    r.run_until(from + 35 * ps_per_tick);
    CHECK(found->pulses.size() == 7);
    CHECK(r.read(unit, "CNT") == 5);
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE("RM0351 33.4.1 and 33.4.6: TIM6 and TIM7 UIFREMAP copies UIF into CNT bit 31")
{
  rig r;
  for(auto const* unit : {"TIM6", "TIM7"})
  {
    CAPTURE(unit);
    r.base(unit, 9);
    r.write(unit, "CR1", uifremap | 1u);
  }
  r.at_tick(15);
  for(auto const* unit : {"TIM6", "TIM7"})
  {
    CAPTURE(unit);
    CHECK(r.read(unit, "CNT") == 0x8000'0005u);
    r.write(unit, "SR", 0);
    CHECK(r.read(unit, "CNT") == 5);
    r.write(unit, "SR", 1);
    r.write(unit, "CR1", 1u);
    CHECK(r.read(unit, "CNT") == 5);
  }
}

TEST_CASE(
    "RM0351 33.4.1, 32.5.1: a TIM6, TIM7, TIM15 to TIM17 CR1.DIR or CMS throws at construction")
{
  struct leg
  {
    std::string_view field;
    std::uint8_t     lsb;
    std::uint8_t     width;
  };
  for(auto const* unit : {"TIM6", "TIM7", "TIM15", "TIM16", "TIM17"})
  {
    for(auto const& [field, lsb, width] : {
            leg{"DIR", 4, 1},
            leg{"CMS", 5, 2}
    })
    {
      CAPTURE(unit);
      CAPTURE(field);
      auto desc = l476rc();
      register_of(timer_of(desc, unit), "CR1")
          .fields.push_back(
              {.name  = std::string{field},
               .lsb   = lsb,
               .width = width,
               .acc   = popcorn::access::read_write});
      constructs_throw(desc);
    }
  }
}

TEST_CASE("RM0351 Table 61: TIM1 raises 24 to 27 by group, TIM8 43 to 46, TIM15 to TIM17 24 to 26")
{
  struct leg
  {
    std::string_view unit;
    std::string_view enable;
    std::string_view generate;
    std::uint32_t    irq;
  };
  for(auto const& [unit, enable, generate, irq] : {
          leg{"TIM1",  "BIE",   "BG",   24},
          leg{"TIM1",  "BIE",   "B2G",  24},
          leg{"TIM1",  "UIE",   "UG",   25},
          leg{"TIM1",  "TIE",   "TG",   26},
          leg{"TIM1",  "COMIE", "COMG", 26},
          leg{"TIM1",  "CC1IE", "CC1G", 27},
          leg{"TIM8",  "BIE",   "BG",   43},
          leg{"TIM8",  "UIE",   "UG",   44},
          leg{"TIM8",  "TIE",   "TG",   45},
          leg{"TIM8",  "CC4IE", "CC4G", 46},
          leg{"TIM15", "UIE",   "UG",   24},
          leg{"TIM15", "CC2IE", "CC2G", 24},
          leg{"TIM15", "TIE",   "TG",   24},
          leg{"TIM16", "UIE",   "UG",   25},
          leg{"TIM16", "BIE",   "BG",   25},
          leg{"TIM17", "CC1IE", "CC1G", 26},
          leg{"TIM17", "COMIE", "COMG", 26},
  })
  {
    CAPTURE(unit);
    CAPTURE(generate);
    rig r;
    r.clock(unit);
    r.set(unit, "DIER", enable, 1);
    r.set(unit, "EGR", generate, 1);
    for(std::uint32_t line = 24; line <= 46; ++line)
    {
      if((line >= 24 and line <= 27) or line >= 43)
      {
        CAPTURE(line);
        CHECK(r.pending(line) == (line == irq));
      }
    }
  }
}

TEST_CASE("RM0351 30.4.15: a TIM1 and TIM8 REP of 256 gives one update per 257 overflows")
{
  rig r;
  for(auto const* unit : {"TIM1", "TIM8"})
  {
    CAPTURE(unit);
    r.base(unit, 1);
    r.write(unit, "RCR", 256);
    r.write(unit, "EGR", 1);
    r.write(unit, "SR", 0);
    r.enable(unit);
  }
  r.at_tick(2 * 257 - 1);
  for(auto const* unit : {"TIM1", "TIM8"})
  {
    CAPTURE(unit);
    CHECK(r.get(unit, "SR", "UIF") == 0);
  }
  r.at_tick(2 * 257);
  for(auto const* unit : {"TIM1", "TIM8"})
  {
    CAPTURE(unit);
    CHECK(r.get(unit, "SR", "UIF") == 1);
    CHECK(r.read(unit, "RCR") == 256);
  }
}

TEST_CASE(
    "RM0351 30.4.23, 30.4.24: TI1_RMP 0 captures the TIM1 PA8 and TIM8 PC6 edges, 1 reads low")
{
  struct leg
  {
    std::string_view unit;
    std::string_view port;
    std::uint8_t     pin;
    std::uint32_t    af;
  };
  for(auto const& [unit, port, pin, af] : {
          leg{"TIM1", "GPIOA", 8, 1},
          leg{"TIM8", "GPIOC", 6, 3},
  })
  {
    for(std::uint32_t const remap : {0u, 1u})
    {
      CAPTURE(unit);
      CAPTURE(remap);
      rig   r;
      auto& gpio = r.port(port, pin, af);
      r.base(unit, 0xffff);
      r.set(unit, "OR1", "TI1_RMP", remap);
      r.set(unit, "CCMR1_Input", "CC1S", 1);
      r.set(unit, "CCER", "CC1E", 1);
      r.enable(unit);
      gpio.drive_at(pin, true, vtime{r.start + 40 * ps_per_tick});
      r.at_tick(60);
      CHECK(r.get(unit, "SR", "CC1IF") == (remap == 0 ? 1u : 0u));
      CHECK(r.read(unit, "CCR1") == (remap == 0 ? 40u : 0u));
      CHECK(r.warnings() == remap);
    }
  }
}

TEST_CASE("RM0351 30.4.23, 30.4.28: TIM1 counts PA12 ETR edges, the ADC watchdog OR keeps the pin")
{
  struct leg
  {
    std::string_view unit;
    std::uint8_t     pin;
    std::uint32_t    af;
    std::string_view reg;
    std::string_view field;
    std::uint32_t    value;
    std::uint32_t    counted;
  };
  for(auto const& [unit, pin, af, reg, field, value, counted] : {
          leg{"TIM1", 12, 1, "OR1", "ETR_ADC1_RMP", 0, 5},
          leg{"TIM1", 12, 1, "OR1", "ETR_ADC1_RMP", 2, 5},
          leg{"TIM1", 12, 1, "OR1", "ETR_ADC3_RMP", 3, 5},
          leg{"TIM1", 12, 1, "OR2", "ETRSEL",       1, 0},
          leg{"TIM8", 0,  3, "OR1", "ETR_ADC2_RMP", 1, 5},
          leg{"TIM8", 0,  3, "OR1", "ETR_ADC3_RMP", 0, 5},
          leg{"TIM8", 0,  3, "OR2", "ETRSEL",       2, 0},
  })
  {
    CAPTURE(unit);
    CAPTURE(field);
    CAPTURE(value);
    rig   r;
    auto& porta = r.port("GPIOA", pin, af);
    r.base(unit, 0xffff);
    r.set(unit, reg, field, value);
    r.write(unit, "SMCR", ece);
    r.enable(unit);
    auto const t0 = r.now();
    for(std::uint64_t edge = 0; edge < 5; ++edge)
    {
      porta.drive_at(pin, true, vtime{t0 + (20 * edge + 10) * us});
      porta.drive_at(pin, false, vtime{t0 + (20 * edge + 20) * us});
    }
    r.run_until(t0 + 95 * us);
    CHECK(r.read(unit, "CNT") == counted);
    CHECK(r.warnings() == (value == 0 ? 0u : 1u));
  }
}

TEST_CASE("BDTR, CCxNE, CCR5, CCR6 and OR2, OR3 break sources are storage, BG clears MOE")
{
  rig r;
  for(auto const* unit : {"TIM1", "TIM8"})
  {
    CAPTURE(unit);
    r.clock(unit);
    CHECK(r.read(unit, "OR2") == 1);
    CHECK(r.read(unit, "OR3") == 1);
    r.write(unit, "BDTR", 0x03ff'ff40u);
    r.write(unit, "CCER", 0x0033'bfffu);
    r.write(unit, "OR2", 0x0000'0f07u);
    r.write(unit, "OR3", 0x0000'0f07u);
    r.write(unit, "CCR5", 0xe000'1234u);
    r.write(unit, "CCR6", 0x5678u);
    CHECK(r.read(unit, "BDTR") == 0x03ff'ff40u);
    CHECK(r.read(unit, "CCER") == 0x0033'bfffu);
    CHECK(r.read(unit, "OR2") == 0x0000'0f07u);
    CHECK(r.read(unit, "OR3") == 0x0000'0f07u);
    CHECK(r.read(unit, "CCR5") == 0xe000'1234u);
    CHECK(r.read(unit, "CCR6") == 0x5678u);
    r.write(unit, "EGR", 1u << 7);
    CHECK(r.get(unit, "SR", "BIF") == 1);
    CHECK(r.get(unit, "BDTR", "MOE") == 0);
  }
  CHECK(r.warnings() == 0);
}

TEST_CASE("RM0351 30.4.6, 30.4.20: BG and B2G clear MOE, AOE 1 sets it at the next update event")
{
  for(auto const* unit : {"TIM1", "TIM8"})
  {
    CAPTURE(unit);
    rig r;
    r.base(unit, 9);
    for(auto const& [generate, flag] : {
            std::pair{1u << 7, "BIF" },
            std::pair{1u << 8, "B2IF"}
    })
    {
      CAPTURE(flag);
      r.write(unit, "SR", 0);
      r.write(unit, "BDTR", 1u << 15);
      r.write(unit, "EGR", generate);
      CHECK(r.get(unit, "BDTR", "MOE") == 0);
      CHECK(r.get(unit, "SR", flag) == 1);
    }
    r.write(unit, "EGR", 1);
    CHECK(r.get(unit, "BDTR", "MOE") == 0);
    r.write(unit, "BDTR", 1u << 14);
    r.write(unit, "SR", 0);
    r.enable(unit);
    r.at_tick(5);
    CHECK(r.get(unit, "BDTR", "MOE") == 0);
    r.at_tick(15);
    CHECK(r.get(unit, "SR", "UIF") == 1);
    CHECK(r.get(unit, "BDTR", "MOE") == 1);
  }
}

TEST_CASE("RM0351 30.4.20: a BDTR read alone sees AOE set MOE at a wrap, a MOE 0 write holds")
{
  for(auto const* unit : {"TIM1", "TIM8"})
  {
    CAPTURE(unit);
    rig r;
    r.base(unit, 9);
    r.write(unit, "BDTR", (1u << 15) | (1u << 14));
    r.write(unit, "EGR", 0x1fu);
    r.write(unit, "EGR", 1u << 7);
    REQUIRE((r.read(unit, "SR") & 0x1fu) == 0x1fu);
    REQUIRE(r.get(unit, "BDTR", "MOE") == 0);
    r.enable(unit);
    r.at_tick(15);
    CHECK(r.get(unit, "BDTR", "MOE") == 1);
    r.at_tick(25);
    r.write(unit, "BDTR", 1u << 14);
    r.at_tick(28);
    CHECK(r.get(unit, "SR", "UIF") == 1);
    CHECK(r.get(unit, "BDTR", "MOE") == 0);
    r.at_tick(35);
    CHECK(r.get(unit, "BDTR", "MOE") == 1);
  }
}

TEST_CASE("RM0351 32.6.6, 32.7.5: TIM15 to TIM17 BG clears MOE, AOE 1 sets it at the update")
{
  rig r;
  for(auto const* unit : {"TIM15", "TIM16", "TIM17"})
  {
    CAPTURE(unit);
    r.clock(unit);
    r.write(unit, "BDTR", 1u << 15);
    r.write(unit, "EGR", 1u << 7);
    CHECK(r.get(unit, "BDTR", "MOE") == 0);
    r.write(unit, "BDTR", 1u << 14);
    r.write(unit, "EGR", 1);
    CHECK(r.get(unit, "BDTR", "MOE") == 1);
  }
}

TEST_CASE("TIM1 complementary PWM on CH1 and CH1N with dead time flags CC1 250 us in, firmware")
{
  firmware_run run{"tim1_pwm"};
  auto const   started = run.started();
  auto const   logged  = run.finish();
  REQUIRE(logged.size() == 3);
  CHECK(logged[0] == run.compares.size());
  CHECK(logged[1] == 0x0005);
  CHECK(logged[2] == 0x8040);
  REQUIRE(run.updates.size() == 10);
  REQUIRE(run.compares.size() >= 10);
  CHECK(run.compares.front() - started > 250 * us);
  CHECK(run.compares.front() - started < 252 * us);
  CHECK(std::ranges::all_of(gaps(run.compares), [](std::uint64_t gap) { return gap == ms; }));
}

TEST_CASE("RM0351 32.6.3: TIM15 SMS 0100 resets at each TIM3 update on ITR1, CCR2 flags CC2IF")
{
  rig r;
  r.base("TIM3", 99);
  r.write("TIM3", "CR2", master_update << 4);
  r.base("TIM15", 0xffff);
  r.write("TIM15", "CCR2", 70);
  r.write("TIM15", "SMCR", sms_reset | (1u << 4));
  r.enable("TIM15");
  r.enable("TIM3");
  r.at_tick(150);
  CHECK(r.read("TIM15", "CNT") == 50);
  CHECK(r.get("TIM15", "SR", "TIF") == 1);
  CHECK(r.get("TIM15", "SR", "CC2IF") == 1);
  r.write("TIM15", "SR", 0);
  r.at_tick(165);
  CHECK(r.read("TIM15", "CNT") == 65);
  CHECK(r.get("TIM15", "SR", "CC2IF") == 0);
  r.at_tick(175);
  CHECK(r.get("TIM15", "SR", "CC2IF") == 1);
  CHECK(r.warnings() == 0);
}

TEST_CASE("RM0351 32.6.19, 32.7.17, 32.7.19: OR1 TI1_RMP and ENCODER_MODE 0 capture the pin")
{
  struct leg
  {
    std::string_view unit;
    std::string_view field;
    std::uint32_t    value;
    std::uint8_t     pin;
    std::string_view channel;
  };
  for(auto const& [unit, field, value, pin, channel] : {
          leg{"TIM15", "TI1_RMP",      0, 2, "1"},
          leg{"TIM15", "TI1_RMP",      1, 2, "1"},
          leg{"TIM15", "ENCODER_MODE", 1, 2, "1"},
          leg{"TIM15", "ENCODER_MODE", 0, 3, "2"},
          leg{"TIM15", "ENCODER_MODE", 3, 3, "2"},
          leg{"TIM16", "TI1_RMP",      0, 6, "1"},
          leg{"TIM16", "TI1_RMP",      1, 6, "1"},
          leg{"TIM16", "TI1_RMP",      3, 6, "1"},
          leg{"TIM17", "TI1_RMP",      0, 7, "1"},
          leg{"TIM17", "TI1_RMP",      2, 7, "1"},
  })
  {
    CAPTURE(unit);
    CAPTURE(field);
    CAPTURE(value);
    rig   r;
    auto& porta = r.port("GPIOA", pin, 14);
    r.base(unit, 0xffff);
    r.set(unit, "OR1", field, value);
    r.set(unit, "CCMR1_Input", std::format("CC{}S", channel), 1);
    r.set(unit, "CCER", std::format("CC{}E", channel), 1);
    r.enable(unit);
    porta.drive_at(pin, true, vtime{r.start + 40 * ps_per_tick});
    r.at_tick(60);
    CHECK(r.get(unit, "SR", std::format("CC{}IF", channel)) == (value == 0 ? 1u : 0u));
    CHECK(r.read(unit, std::format("CCR{}", channel)) == (value == 0 ? 40u : 0u));
    CHECK(r.warnings() == (value == 0 ? 0u : 1u));
  }
}

TEST_CASE("RM0351 32.7.7: TIM16 OC1M_bit3 1 holds tim16_oc1 low and logs, its clear is an edge")
{
  rig r;
  r.base("TIM16", 199);
  r.write("TIM16", "CCR1", 100);
  r.write("TIM16", "CCMR1_Output", pwm1 | (1u << 16));
  r.set("TIM16", "CCER", "CC1E", 1);
  r.base("TIM15", 0xffff);
  r.write("TIM15", "SMCR", sms_trigger | (2u << 4));
  r.enable("TIM16");
  r.at_tick(450);
  CHECK(r.get("TIM15", "CR1", "CEN") == 0);
  CHECK(r.warnings() == 1);
  r.write("TIM16", "CCMR1_Output", pwm1);
  r.at_tick(650);
  CHECK(r.get("TIM15", "CR1", "CEN") == 1);
  CHECK(r.read("TIM15", "CNT") == 200);
}

TEST_CASE("a TIM16 CCMR1_Output with OC1M and a bit 16 field other than OC1M_bit3 throws")
{
  auto  desc = l476rc();
  auto& view = register_of(timer_of(desc, "TIM16"), "CCMR1_Output");
  auto found = std::ranges::find(view.fields, std::string{"OC1M_bit3"}, &popcorn::field_desc::name);
  REQUIRE(found != view.fields.end());
  found->name = "OC1M_3";
  constructs_throw(desc);
}

TEST_CASE("TIM15 to TIM17 BDTR has no BKF, OR2 break sources and CCxNE are storage")
{
  rig r;
  for(auto const* unit : {"TIM15", "TIM16", "TIM17"})
  {
    CAPTURE(unit);
    r.clock(unit);
    CHECK(r.read(unit, "OR2") == 1);
    CHECK_FALSE(
        std::ranges::contains(
            r.reg(unit, "BDTR").fields, std::string{"BKF"}, &popcorn::field_desc::name));
    r.write(unit, "BDTR", 0x0000'ff40u);
    r.write(unit, "OR2", 0x0000'0f07u);
    r.set(unit, "CCER", "CC1NE", 1);
    CHECK(r.read(unit, "BDTR") == 0x0000'ff40u);
    CHECK(r.read(unit, "OR2") == 0x0000'0f07u);
    CHECK(r.get(unit, "CCER", "CC1NE") == 1);
  }
  CHECK(r.warnings() == 0);
}
