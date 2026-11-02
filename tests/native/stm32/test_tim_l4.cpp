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

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
constexpr std::uint32_t    sms_trigger   = 6;
constexpr std::uint32_t    sms_reset_trg = 1u << 16;
constexpr std::uint32_t    master_update = 2;
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
