#include "../support/inert_model.hpp"
#include "factories.hpp"
#include "stm32_fixture.hpp"
#include "variants.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
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

constexpr std::string_view firmware      = "l476_l4_rcc_ll";
constexpr std::uint64_t    ps_per_tick   = 250'000;
constexpr std::uint64_t    us            = 1'000'000;
constexpr std::uint32_t    sms_trigger   = 6;
constexpr std::uint32_t    sms_external  = 7;
constexpr std::uint32_t    sms_reset_trg = 1u << 16;
constexpr std::uint32_t    master_update = 2;
constexpr std::uint32_t    ece           = 1u << 14;
constexpr std::uint32_t    uifremap      = 1u << 11;
constexpr std::uint32_t    dir_down      = 1u << 4;

class bus_probe final : public popcorn::testing::inert_model
{
public:
  explicit bus_probe(popcorn::periph::model_context& ctx) : context{ctx}
  {}

  popcorn::periph::model_context& context;
};

auto options(part_description const& desc, bool strict) -> core::machine_options
{
  auto out               = popcorn::testing::stm32_options();
  out.models             = stm32::factories({}, desc);
  out.strict_peripherals = strict;
  out.models.insert_or_assign(
      "TSC",
      [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<popcorn::periph::model> { return std::make_shared<bus_probe>(ctx); });
  return out;
}

struct rig
{
  part_description const&        part;
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;
  std::uint64_t                  start{};

  explicit rig(bool strict = false, part_description const& desc = l476rc()) : part{desc}
  {
    f = std::make_unique<stm32_fixture>(desc, firmware, options(desc, strict));
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
    set("RCC", "APB1ENR1", std::string{unit} + "EN", 1);
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

TEST_CASE("RM0351 31.1: TIM2 to TIM5 have a model on STM32L476RCTx, the other timers stay storage")
{
  rig const r;
  for(auto const* unit : {"TIM2", "TIM3", "TIM4", "TIM5"})
  {
    CAPTURE(unit);
    CHECK(r.f->target->model(unit) != nullptr);
  }
  for(auto const* unit : {"TIM1", "TIM6", "TIM7", "TIM8", "TIM15", "TIM16", "TIM17"})
  {
    CAPTURE(unit);
    CHECK(r.f->target->model(unit) == nullptr);
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

TEST_CASE("every entry of RM0351 Table 207 reaches its own source and no other")
{
  struct row
  {
    std::string_view slave;
    std::uint32_t    select;
    std::uint32_t    remap;
    std::string_view source;
  };
  constexpr std::array<row, 17> table{
      {{"TIM2", 0, 0, "TIM1"},
       {"TIM2", 1, 0, "TIM8"},
       {"TIM2", 1, 1, "OTG_FS"},
       {"TIM2", 2, 0, "TIM3"},
       {"TIM2", 3, 0, "TIM4"},
       {"TIM3", 0, 0, "TIM1"},
       {"TIM3", 1, 0, "TIM2"},
       {"TIM3", 2, 0, "TIM15"},
       {"TIM3", 3, 0, "TIM4"},
       {"TIM4", 0, 0, "TIM1"},
       {"TIM4", 1, 0, "TIM2"},
       {"TIM4", 2, 0, "TIM3"},
       {"TIM4", 3, 0, "TIM8"},
       {"TIM5", 0, 0, "TIM2"},
       {"TIM5", 1, 0, "TIM3"},
       {"TIM5", 2, 0, "TIM4"},
       {"TIM5", 3, 0, "TIM8"}}
  };
  constexpr std::array<std::string_view, 4> masters{"TIM2", "TIM3", "TIM4", "TIM5"};
  for(auto const& [slave, select, remap, source] : table)
  {
    CAPTURE(slave);
    CAPTURE(select);
    CAPTURE(remap);
    rig              r;
    auto const       has_source = std::ranges::contains(masters, source);
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
    if(has_source)
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
      r.enable(source);
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
          leg{"TIM2", "OR1", 1u << 1 },
          leg{"TIM2", "OR1", 1u << 2 },
          leg{"TIM2", "OR2", 1u << 14},
          leg{"TIM3", "OR1", 1u      },
          leg{"TIM3", "OR2", 1u << 14}
  })
  {
    CAPTURE(unit);
    CAPTURE(reg);
    rig r{true};
    r.clock(unit);
    CHECK_THROWS_AS(r.cpu_write(unit, reg, value), popcorn::model_error);
    CHECK(r.read(unit, reg) == 0);
    r.write(unit, reg, value);
    CHECK(r.read(unit, reg) == value);
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
          leg{"TIM3", "SMCR", "TS_1",     20, true },
          leg{"TIM3", "CR2",  "MMS_1",    25, true },
          leg{"TIM4", "SMCR", "SMS_1",    16, false},
          leg{"TIM5", "CR1",  "UIFREMAP", 11, false},
          leg{"TIM2", "CR1",  "DIR",      4,  false},
          leg{"TIM2", "OR1",  "ITR1_RMP", 0,  false},
          leg{"TIM2", "OR1",  "TI4_RMP",  2,  false},
          leg{"TIM2", "OR2",  "ETRSEL",   14, false},
          leg{"TIM3", "OR1",  "TI1_RMP",  0,  false},
          leg{"TIM5", "SMCR", "ETP",      15, false},
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
