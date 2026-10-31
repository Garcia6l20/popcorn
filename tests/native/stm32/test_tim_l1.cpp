#include "factories.hpp"
#include "stm32_fixture.hpp"
#include "trigger_probe.hpp"
#include "trigger_signal.hpp"
#include "variants.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
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
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152rc;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware       = "l152xe_l1_tim_ll";
constexpr std::string_view firmware_b     = "l152xb_l1_tim_ll";
constexpr std::uint64_t    ps_per_second  = 1'000'000'000'000;
constexpr std::uint64_t    us             = 1'000'000;
constexpr std::uint64_t    ms             = 1'000 * us;
constexpr std::uint64_t    msi_hz         = 2'097'152;
constexpr std::uint32_t    sms_trigger    = 6;
constexpr std::uint32_t    sms_external   = 7;
constexpr std::uint32_t    master_update  = 2;
constexpr std::uint32_t    master_compare = 3;
constexpr std::uint32_t    pwm1           = 6u << 4;
constexpr std::uint32_t    pwm2           = 7u << 4;
constexpr std::uint32_t    toggle         = 3u << 4;
constexpr std::uint32_t    sms_gated      = 5;
constexpr std::uint32_t    sms_reset      = 4;

auto ticks_ps(std::uint64_t ticks) -> std::uint64_t
{
  return (ticks * ps_per_second + msi_hz - 1) / msi_hz;
}

class bus_probe final : public popcorn::periph::model
{
public:
  explicit bus_probe(popcorn::periph::model_context& ctx) : context{ctx}
  {}

  popcorn::periph::model_context& context;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

struct rig
{
  part_description const&        part;
  std::unique_ptr<stm32_fixture> f;
  std::string                    elf;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;
  std::uint64_t                  start{};

  explicit rig(
      part_description const& desc    = l152re(),
      std::string_view        image   = firmware,
      bool                    strict  = false,
      bool                    trigger = false)
      : part{desc}, elf{image}
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories({}, desc);
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "DAC",
        [trigger](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<popcorn::periph::model> {
          if(trigger)
          {
            return std::make_shared<popcorn::testing::trigger_probe>(ctx);
          }
          return std::make_shared<bus_probe>(ctx);
        });
    f = std::make_unique<stm32_fixture>(desc, image, std::move(options));
    f->set("pc", sym(elf, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    start = now();
  }

  [[nodiscard]] auto probe(std::string_view signal) const -> popcorn::testing::trigger_probe&
  {
    auto* const found = f->model_of<popcorn::testing::trigger_probe>("DAC");
    REQUIRE(found != nullptr);
    found->watched = signal;
    return *found;
  }

  [[nodiscard]] auto has(std::string_view unit) const -> bool
  {
    return std::ranges::contains(part.peripherals, unit, &popcorn::peripheral_desc::name);
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
    auto* const probe = f->model_of<bus_probe>("DAC");
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
    run_until(start + ticks_ps(ticks));
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
  }

  void clock(std::string_view unit) const
  {
    auto const apb2 = unit == "TIM9" or unit == "TIM10" or unit == "TIM11";
    set("RCC", apb2 ? "APB2ENR" : "APB1ENR", std::string{unit} + "EN", 1);
  }

  void base(std::string_view unit, std::uint32_t reload) const
  {
    clock(unit);
    write(unit, "PSC", 0);
    write(unit, "ARR", reload);
    write(unit, "EGR", 1);
    write(unit, "SR", 0);
  }

  void master(std::string_view unit) const
  {
    write(unit, "CR2", master_update << 4);
  }

  void slave(std::string_view unit, std::uint32_t select, std::uint32_t mode = sms_trigger) const
  {
    write(unit, "SMCR", mode | (select << 4));
  }

  void enable(std::string_view unit) const
  {
    write(unit, "CR1", read(unit, "CR1") | 1u);
  }
};

struct firmware_run
{
  std::unique_ptr<stm32_fixture>      f;
  std::string                         elf;
  std::vector<std::uint64_t>          updates;
  std::vector<std::uint64_t>          compares;
  std::vector<core::intercept_handle> hooks;

  firmware_run(std::string_view entry, part_description const& desc, std::string_view image)
      : elf{image}
  {
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories({}, desc);
    f              = std::make_unique<stm32_fixture>(desc, image, std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(elf, "main")}).reason
        == stop_reason::address);
    f->set("pc", sym(elf, entry));
    record("on_update", updates);
    record("on_compare", compares);
  }

  void record(std::string_view symbol, std::vector<std::uint64_t>& into)
  {
    hooks.push_back(
        f->target->add_intercept(sym(elf, symbol), [&into](core::intercept_call const& c) {
          into.push_back(c.target().virtual_clock().time().ps);
          return core::intercept_result::proceed();
        }));
  }

  auto started() const -> std::uint64_t
  {
    auto const stopped =
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(elf, "on_started")});
    REQUIRE(stopped.reason == stop_reason::address);
    return f->target->virtual_clock().time().ps;
  }

  auto finish(std::uint64_t instructions = 2'000'000) const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{instructions}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
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

auto with_field(
    part_description          desc,
    std::string_view          unit,
    std::string_view          reg,
    popcorn::field_desc const added) -> part_description
{
  auto const timer =
      std::ranges::find(desc.peripherals, std::string{unit}, &popcorn::peripheral_desc::name);
  REQUIRE(timer != desc.peripherals.end());
  auto const found =
      std::ranges::find(timer->registers, std::string{reg}, &popcorn::register_desc::name);
  REQUIRE(found != timer->registers.end());
  found->fields.push_back(added);
  return desc;
}

void constructs_throw(part_description const& desc)
{
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories({}, desc);
  CHECK_THROWS_AS(stm32_fixture(desc, firmware, std::move(options)), popcorn::config_error);
}
}

TEST_CASE("RM0038 17.1: TIM2 to TIM4 have a model on every category, TIM5 on Cat.3 and up only")
{
  for(auto const* desc : {&l152rb(), &l152rc(), &l152re()})
  {
    CAPTURE(desc->part);
    rig const r{*desc, desc == &l152rb() ? firmware_b : firmware};
    for(auto const* unit : {"TIM2", "TIM3", "TIM4"})
    {
      CAPTURE(unit);
      CHECK(r.f->target->model(unit) != nullptr);
    }
    CHECK(r.has("TIM5") == (desc != &l152rb()));
    if(r.has("TIM5"))
    {
      CHECK(r.f->target->model("TIM5") != nullptr);
    }
  }
}

TEST_CASE("TIM2 at 1 kHz on HSI 16 MHz raises 100 update IRQs exactly 1 ms apart, firmware")
{
  for(auto const& [desc, image] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(image);
    firmware_run run{"tim2_update", *desc, image};
    auto const   started = run.started();
    CHECK(run.finish() == std::vector<std::uint32_t>{100});
    REQUIRE(run.updates.size() == 100);
    CHECK(run.updates.front() - started > ms);
    CHECK(run.updates.front() - started < ms + 2 * us);
    CHECK(gaps(run.updates) == std::vector<std::uint64_t>(99, ms));
  }
}

TEST_CASE("TIM2 PWM mode 1 on CH1 flags CC1 250 us into each 1 ms period, firmware")
{
  firmware_run run{"tim2_pwm", l152re(), firmware};
  auto const   started = run.started();
  auto const   logged  = run.finish();
  REQUIRE(run.updates.size() == 10);
  REQUIRE(run.compares.size() >= 10);
  CHECK(logged == std::vector<std::uint32_t>{static_cast<std::uint32_t>(run.compares.size())});
  CHECK(run.compares.front() - started > 250 * us);
  CHECK(run.compares.front() - started < 252 * us);
  auto const compare_gaps = gaps(run.compares);
  CHECK(std::ranges::all_of(compare_gaps, [](std::uint64_t gap) { return gap == ms; }));
  for(std::size_t period = 0; period + 1 < run.updates.size(); ++period)
  {
    CAPTURE(period);
    CHECK(run.compares[period + 1] - run.updates[period] == run.compares[1] - run.updates[0]);
  }
  CHECK(run.compares[1] - run.updates[0] > 249 * us);
  CHECK(run.compares[1] - run.updates[0] < 251 * us);
}

TEST_CASE("RM0038 17.4.7: TIM3 captures PA6 edges into CCR1, IC1PSC 01 every second edge, firmware")
{
  constexpr std::array<std::uint64_t, 8> rising{100, 300, 700, 1500, 3100, 6300, 12700, 25500};
  struct leg
  {
    std::string_view           entry;
    std::array<std::size_t, 4> edges;
  };
  for(auto const& [entry, edges] : {
          leg{"tim3_capture",      {0, 1, 2, 3}},
          leg{"tim3_capture_div2", {1, 3, 5, 7}}
  })
  {
    CAPTURE(entry);
    firmware_run run{entry, l152re(), firmware};
    auto* const  porta = run.f->model_of<stm32::gpio_port>("GPIOA");
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
      CHECK(logged[index] - logged[0] == rising[edges[index]] - rising[edges[0]]);
    }
  }
}

TEST_CASE("RM0038 Table 80: TIM3 in external clock mode 1 on ITR1 counts TIM2 updates, firmware")
{
  for(auto const& [desc, image] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(image);
    firmware_run run{"tim2_clocks_tim3", *desc, image};
    auto const   started = run.started();
    CHECK(run.finish() == std::vector<std::uint32_t>{10});
    auto const done = run.f->target->virtual_clock().time().ps;
    CHECK(done - started > ms);
    CHECK(done - started < ms + 2 * us);
  }
}

TEST_CASE("RM0038 17.4.19: LL_TIM_SetRemap gives TIM2 ITR1 the TIM5 TRGO, firmware")
{
  firmware_run run{"tim5_remap_tim2", l152re(), firmware};
  auto const   started = run.started();
  CHECK(run.finish() == std::vector<std::uint32_t>{10, 1});
  auto const done = run.f->target->virtual_clock().time().ps;
  CHECK(done - started > ms);
  CHECK(done - started < ms + 2 * us);
}

TEST_CASE("every entry of RM0038 Tables 80 and 83 reaches its own source and no other")
{
  struct row
  {
    std::string_view slave;
    std::uint32_t    select;
    std::uint32_t    remap;
    std::string_view source;
  };
  constexpr std::array<row, 19> cat3{
      {{"TIM2", 0, 0, "TIM9"},
       {"TIM2", 1, 0, "TIM10"},
       {"TIM2", 1, 1, "TIM5"},
       {"TIM2", 2, 0, "TIM3"},
       {"TIM2", 3, 0, "TIM4"},
       {"TIM3", 0, 0, "TIM9"},
       {"TIM3", 1, 0, "TIM2"},
       {"TIM3", 2, 0, "TIM11"},
       {"TIM3", 2, 1, "TIM5"},
       {"TIM3", 3, 0, "TIM4"},
       {"TIM4", 0, 0, "TIM10"},
       {"TIM4", 1, 0, "TIM2"},
       {"TIM4", 2, 0, "TIM3"},
       {"TIM4", 3, 0, "TIM9"},
       {"TIM9", 0, 0, "TIM2"},
       {"TIM9", 1, 0, "TIM3"},
       {"TIM9", 1, 1, "TS"},
       {"TIM9", 2, 0, "TIM10"},
       {"TIM9", 3, 0, "TIM11"}}
  };
  constexpr std::array<std::string_view, 5> masters{"TIM2", "TIM3", "TIM4", "TIM5", "TIM9"};
  for(auto const* desc : {&l152re(), &l152rc(), &l152rb()})
  {
    CAPTURE(desc->part);
    auto const cat12 = desc == &l152rb();
    for(auto const& [slave, select, remap, source] : cat3)
    {
      if(cat12 and remap != 0)
      {
        continue;
      }
      CAPTURE(slave);
      CAPTURE(select);
      CAPTURE(remap);
      rig              r{*desc, cat12 ? firmware_b : firmware};
      auto const       oc1        = source == "TIM10" or source == "TIM11";
      auto const       has_source = oc1 or std::ranges::contains(masters, source);
      std::string_view bystander{};
      for(auto const unit : masters)
      {
        if(unit != slave and unit != source and r.has(unit))
        {
          bystander = unit;
        }
      }
      REQUIRE(not bystander.empty());
      r.base(bystander, 99);
      r.master(bystander);
      if(oc1)
      {
        r.base(source, 199);
        r.write(source, "CCR1", 100);
        r.write(source, "CCMR1_Output", pwm1);
        r.set(source, "CCER", "CC1E", 1);
      }
      else if(has_source)
      {
        r.base(source, 199);
        r.master(source);
      }
      r.base(slave, 0xffff);
      if(remap != 0)
      {
        r.set(slave, "OR", slave == "TIM3" ? "ITR2_RMP" : "ITR1_RMP", 1);
      }
      r.slave(slave, select);
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
  }
}

TEST_CASE("RM0038 Table 80 has no TIM5 row: every TS of TIM5 holds and logs once")
{
  for(std::uint32_t select = 0; select < 4; ++select)
  {
    CAPTURE(select);
    rig r;
    for(auto const* unit : {"TIM2", "TIM3", "TIM4"})
    {
      r.base(unit, 99);
      r.master(unit);
    }
    r.base("TIM5", 0xffff);
    r.slave("TIM5", select);
    for(auto const* unit : {"TIM2", "TIM3", "TIM4"})
    {
      r.enable(unit);
    }
    r.at_tick(250);
    CHECK(r.get("TIM5", "CR1", "CEN") == 0);
    CHECK(r.warnings() == 1);
  }
}

TEST_CASE("a TIM2_OR write moves TIM2 ITR1 between TIM10 OC1 and TIM5 from the write on")
{
  rig r;
  r.base("TIM5", 99);
  r.master("TIM5");
  r.base("TIM2", 0xffff);
  r.slave("TIM2", 1, sms_external);
  r.enable("TIM2");
  r.enable("TIM5");
  r.at_tick(550);
  CHECK(r.read("TIM2", "CNT") == 0);
  r.write("TIM2", "OR", 1);
  r.at_tick(1050);
  CHECK(r.read("TIM2", "CNT") == 5);
  r.write("TIM2", "OR", 0);
  r.at_tick(1550);
  CHECK(r.read("TIM2", "CNT") == 5);
  r.write("TIM3", "OR", 1);
  r.at_tick(2050);
  CHECK(r.read("TIM2", "CNT") == 5);
}

TEST_CASE("RM0038 Table 82: TIM5 counts on 32 bits from its 0xFFFFFFFF ARR reset, TIM2 on 16")
{
  rig r;
  CHECK(r.read("TIM5", "ARR") == 0xffff'ffffu);
  CHECK(r.read("TIM2", "ARR") == 0xffffu);
  for(auto const* unit : {"TIM2", "TIM5"})
  {
    r.clock(unit);
    r.write(unit, "CCR1", 0x1'0005);
    r.enable(unit);
  }
  r.at_tick(70'000);
  CHECK(r.read("TIM5", "CNT") == 70'000);
  CHECK(r.get("TIM5", "SR", "UIF") == 0);
  CHECK(r.get("TIM5", "SR", "CC1IF") == 1);
  CHECK(r.read("TIM5", "CCR1") == 0x1'0005);
  CHECK(r.read("TIM2", "CNT") == 70'000 - 65'536);
  CHECK(r.get("TIM2", "SR", "UIF") == 1);
  CHECK(r.read("TIM2", "CCR1") == 0x0005);
}

TEST_CASE("RM0038 17.3.3: TIM3 external clock mode 2 counts TIM3_ETR edges on PD2, ETP inverts")
{
  for(std::uint32_t const inverted : {0u, 1u})
  {
    CAPTURE(inverted);
    rig r;
    r.set("RCC", "AHBENR", "GPIODEN", 1);
    auto* const portd = r.f->model_of<stm32::gpio_port>("GPIOD");
    REQUIRE(portd != nullptr);
    portd->drive(2, false);
    r.set("GPIOD", "MODER", "MODER2", 2);
    r.set("GPIOD", "AFRL", "AFRL2", 2);
    r.base("TIM3", 0xffff);
    r.set("TIM3", "SMCR", "ETP", inverted);
    r.set("TIM3", "SMCR", "ECE", 1);
    r.enable("TIM3");
    auto const t0 = r.now();
    for(std::uint64_t edge = 0; edge < 5; ++edge)
    {
      portd->drive_at(2, true, vtime{t0 + (20 * edge + 10) * us});
      portd->drive_at(2, false, vtime{t0 + (20 * edge + 20) * us});
    }
    r.run_until(t0 + 95 * us);
    CHECK(r.read("TIM3", "CNT") == (inverted == 0 ? 5u : 4u));
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE("with DBG_TIM2_STOP a held TIM2 keeps its count, without it TIM2 counts on")
{
  for(bool const frozen : {true, false})
  {
    CAPTURE(frozen);
    rig r;
    r.clock("TIM2");
    r.enable("TIM2");
    r.at_tick(1000);
    popcorn::testing::set_debug_freeze(*r.f, "DBG_TIM2_STOP", frozen);
    r.f->target->hold_core(true);
    r.at_tick(6000);
    CHECK(r.read("TIM2", "CNT") == (frozen ? 1000u : 6000u));
    r.f->target->hold_core(false);
  }
}

TEST_CASE("an L1 timer description with a field the l1_tim variant lacks throws at construction")
{
  struct leg
  {
    std::string_view reg;
    std::string_view field;
    std::uint8_t     lsb;
  };
  for(auto const& [reg, field, lsb] : {
          leg{"SMCR", "SMS_1",    16},
          leg{"SMCR", "TS_1",     20},
          leg{"CR1",  "UIFREMAP", 11},
          leg{"CNT",  "UIFCPY",   31},
          leg{"CR2",  "MMS_1",    25}
  })
  {
    CAPTURE(field);
    for(auto const* desc : {&l152re(), &l152rb()})
    {
      CAPTURE(desc->part);
      auto const changed = with_field(
          *desc, "TIM3", reg,
          {.name = std::string{field}, .lsb = lsb, .width = 1, .acc = popcorn::access::read_write});
      constructs_throw(changed);
    }
  }
}

TEST_CASE("a Cat.3 TIM2 or TIM3 description without OR throws at construction, TIM4 needs none")
{
  for(auto const* unit : {"TIM2", "TIM3"})
  {
    CAPTURE(unit);
    auto       desc = l152re();
    auto const timer =
        std::ranges::find(desc.peripherals, std::string{unit}, &popcorn::peripheral_desc::name);
    REQUIRE(timer != desc.peripherals.end());
    REQUIRE(
        std::erase_if(
            timer->registers, [](popcorn::register_desc const& reg) { return reg.name == "OR"; })
        == 1);
    constructs_throw(desc);
  }
  rig const r;
  CHECK(not r.f->target->peripheral("TIM4").description().registers.empty());
  CHECK(popcorn::find_register(r.f->target->peripheral("TIM4").description(), "OR") == nullptr);
}

TEST_CASE("RM0038 17.3.3: TIM2_ETR is the TIM2_CH1 pad, PA0 at AF1 counts in external clock 2")
{
  rig r;
  r.set("RCC", "AHBENR", "GPIOAEN", 1);
  auto* const porta = r.f->model_of<stm32::gpio_port>("GPIOA");
  REQUIRE(porta != nullptr);
  porta->drive(0, false);
  r.set("GPIOA", "MODER", "MODER0", 2);
  r.set("GPIOA", "AFRL", "AFRL0", 1);
  r.base("TIM2", 0xffff);
  r.set("TIM2", "CCMR1_Input", "CC1S", 1);
  r.set("TIM2", "CCER", "CC1E", 1);
  r.set("TIM2", "SMCR", "ECE", 1);
  r.enable("TIM2");
  auto const t0 = r.now();
  for(std::uint64_t edge = 0; edge < 5; ++edge)
  {
    porta->drive_at(0, true, vtime{t0 + (20 * edge + 10) * us});
    porta->drive_at(0, false, vtime{t0 + (20 * edge + 20) * us});
  }
  r.run_until(t0 + 95 * us);
  CHECK(r.read("TIM2", "CNT") == 5);
  CHECK(r.read("TIM2", "CCR1") == 4);
  CHECK(r.get("TIM2", "SR", "CC1OF") == 1);
  CHECK(r.warnings() == 0);
}

TEST_CASE("an L1 TIM3 description with IC1F and no IC1PSC throws at construction")
{
  auto       desc = l152re();
  auto const timer =
      std::ranges::find(desc.peripherals, std::string{"TIM3"}, &popcorn::peripheral_desc::name);
  REQUIRE(timer != desc.peripherals.end());
  auto const view = std::ranges::find(
      timer->registers, std::string{"CCMR1_Input"}, &popcorn::register_desc::name);
  REQUIRE(view != timer->registers.end());
  REQUIRE(
      std::erase_if(
          view->fields, [](popcorn::field_desc const& bits) { return bits.name == "IC1PSC"; })
      == 1);
  constructs_throw(desc);
}

TEST_CASE("under strict_peripherals an SMCR or OR write that selects an unmodelled ITR throws")
{
  {
    rig r{l152re(), firmware, true};
    r.base("TIM5", 0xffff);
    CHECK_THROWS_AS(r.cpu_write("TIM5", "SMCR", sms_external), popcorn::model_error);
    CHECK(r.read("TIM5", "SMCR") == 0);
    CHECK(r.warnings() == 1);
    r.base("TIM3", 0xffff);
    r.cpu_write("TIM3", "SMCR", sms_external | (1u << 4));
    CHECK(r.read("TIM3", "SMCR") == (sms_external | (1u << 4)));
  }
  {
    rig r{l152re(), firmware, true};
    r.base("TIM9", 0xffff);
    r.slave("TIM9", 1, sms_external);
    CHECK_THROWS_AS(r.cpu_write("TIM9", "OR", 1u << 2), popcorn::model_error);
    CHECK(r.read("TIM9", "OR") == 0);
  }
}

TEST_CASE(
    "under strict a firmware MMS 100 or TDE write throws before the store, a debugger one stores")
{
  rig r{l152re(), firmware, true};
  r.clock("TIM2");
  r.base("TIM3", 0xffff);
  CHECK_THROWS_AS(r.cpu_write("TIM2", "CR2", 4u << 4), popcorn::model_error);
  CHECK(r.read("TIM2", "CR2") == 0);
  CHECK_THROWS_AS(r.cpu_write("TIM2", "DIER", 1u << 14), popcorn::model_error);
  CHECK(r.read("TIM2", "DIER") == 0);
  CHECK(r.warnings() == 2);
  r.cpu_write("TIM2", "CR2", master_update << 4);
  CHECK(r.read("TIM2", "CR2") == (master_update << 4));
  r.write("TIM2", "CR2", 4u << 4);
  CHECK(r.read("TIM2", "CR2") == (4u << 4));
  r.write("TIM2", "DIER", 1u << 14);
  CHECK(r.read("TIM2", "DIER") == (1u << 14));
  r.clock("TIM5");
  r.write("TIM5", "SMCR", sms_external);
  CHECK(r.read("TIM5", "SMCR") == sms_external);
  CHECK(r.warnings() == 3);
}

TEST_CASE(
    "under strict a firmware TRC capture, CCDS or gated TI1F_ED write throws, debugger stores")
{
  struct leg
  {
    std::string_view reg;
    std::uint32_t    value;
    std::size_t      warnings;
  };
  auto const legs = std::array{
      leg{"CCMR1_Input", 3u,                    1},
      leg{"CR2",         1u << 3,               2},
      leg{"SMCR",        sms_gated | (4u << 4), 3},
      leg{"CCMR2_Input", 3u << 8,               3},
  };
  rig r{l152re(), firmware, true};
  r.clock("TIM3");
  for(auto const& [reg, value, warnings] : legs)
  {
    CAPTURE(reg);
    CHECK_THROWS_AS(r.cpu_write("TIM3", reg, value), popcorn::model_error);
    CHECK(r.read("TIM3", reg) == 0);
    CHECK(r.warnings() == warnings);
  }
  for(auto const& [reg, value, warnings] : legs)
  {
    CAPTURE(reg);
    r.write("TIM3", reg, value);
    CHECK(r.read("TIM3", reg) == value);
  }
  CHECK(r.warnings() == 3);
}

TEST_CASE("an input_remaps row on a channel past the timer's channels throws at construction")
{
  static constexpr auto remaps  = std::to_array<stm32::input_remap>({
      {.slave = "TIM9", .channel = 2, .reg = "OR", .field = "TI1_RMP", .pin_values = 0b1101},
  });
  auto                  variant = stm32::l1_cat3_tim;
  variant.input_remaps          = remaps;
  auto options                  = popcorn::testing::stm32_options();
  options.models                = stm32::factories({}, l152re());
  options.models.insert_or_assign("TIM9", stm32::tim_factory(variant));
  CHECK_THROWS_AS(stm32_fixture(l152re(), firmware, std::move(options)), popcorn::config_error);
}

TEST_CASE("a TIM2_OR write opens the TIM2 gate on a high TIM5 enable at the write")
{
  rig r;
  r.base("TIM5", 0xffff);
  r.write("TIM5", "CR2", 1u << 4);
  r.base("TIM2", 0xffff);
  r.slave("TIM2", 1, 5);
  r.enable("TIM2");
  r.enable("TIM5");
  r.at_tick(300);
  CHECK(r.read("TIM2", "CNT") == 0);
  r.write("TIM2", "OR", 1);
  r.at_tick(800);
  auto const opened = r.read("TIM2", "CNT");
  CHECK(opened >= 499);
  CHECK(opened <= 500);
  r.at_tick(1300);
  CHECK(r.read("TIM2", "CNT") - opened == 500);
}

TEST_CASE("a TIM2RST pulse with TIM2_OR 1 clears OR, and TIM2 ITR1 then reaches TIM10 OC1")
{
  rig r;
  r.base("TIM5", 99);
  r.master("TIM5");
  r.base("TIM2", 0xffff);
  r.write("TIM2", "OR", 1);
  r.slave("TIM2", 1, sms_external);
  r.enable("TIM2");
  r.enable("TIM5");
  r.at_tick(550);
  CHECK(r.read("TIM2", "CNT") == 5);
  popcorn::testing::rcc_reset_pulse(*r.f, "APB1RSTR", "TIM2RST");
  CHECK(r.read("TIM2", "OR") == 0);
  CHECK(r.read("TIM2", "SMCR") == 0);
  r.slave("TIM2", 1, sms_external);
  r.enable("TIM2");
  r.at_tick(1550);
  CHECK(r.read("TIM2", "CNT") == 0);
  CHECK(r.read("TIM5", "CNT") == 50);
}

TEST_CASE("an L1 TIM3 description with an AF1.ETRSEL field throws at construction")
{
  auto       desc = l152re();
  auto const timer =
      std::ranges::find(desc.peripherals, std::string{"TIM3"}, &popcorn::peripheral_desc::name);
  REQUIRE(timer != desc.peripherals.end());
  popcorn::register_desc af1;
  af1.name   = "AF1";
  af1.offset = 0x60;
  af1.width  = 32;
  af1.fields = {
      {.name = "ETRSEL", .lsb = 14, .width = 4, .acc = popcorn::access::read_write}
  };
  timer->registers.push_back(af1);
  constructs_throw(desc);
}

TEST_CASE("RM0038 18.1 and 19.1: TIM6, TIM7 and TIM9 to TIM11 have a model on every category")
{
  for(auto const* desc : {&l152rb(), &l152rc(), &l152re()})
  {
    CAPTURE(desc->part);
    rig const r{*desc, desc == &l152rb() ? firmware_b : firmware};
    for(auto const* unit : {"TIM6", "TIM7", "TIM9", "TIM10", "TIM11"})
    {
      CAPTURE(unit);
      CHECK(r.f->target->model(unit) != nullptr);
    }
  }
}

TEST_CASE("TIM6 at 1 kHz on HSI 16 MHz raises 10 update IRQs exactly 1 ms apart, firmware")
{
  for(auto const& [desc, image] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(image);
    firmware_run run{"tim6_update", *desc, image};
    auto const   started = run.started();
    CHECK(run.finish() == std::vector<std::uint32_t>{10});
    REQUIRE(run.updates.size() == 10);
    CHECK(run.updates.front() - started > ms);
    CHECK(run.updates.front() - started < ms + 2 * us);
    CHECK(gaps(run.updates) == std::vector<std::uint64_t>(9, ms));
  }
}

TEST_CASE("RM0038 18.4.8: TIM9 PWM on CH1 and CH2 flags CC1 at 250 us and CC2 at 750 us, firmware")
{
  for(auto const& [desc, image] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(image);
    firmware_run run{"tim9_pwm", *desc, image};
    auto const   started = run.started();
    auto const   logged  = run.finish();
    REQUIRE(logged.size() == 2);
    CHECK(logged[1] == 0x11);
    REQUIRE(run.updates.size() >= 10);
    REQUIRE(run.compares.size() >= 20);
    CHECK(logged[0] == run.compares.size());
    CHECK(run.compares[0] - started > 250 * us);
    CHECK(run.compares[0] - started < 252 * us);
    CHECK(run.compares[1] - run.compares[0] >= 500 * us);
    CHECK(run.compares[1] - run.compares[0] < 501 * us);
    for(std::size_t index = 2; index < 20; ++index)
    {
      CAPTURE(index);
      CHECK(run.compares[index] - run.compares[index - 2] == ms);
    }
  }
}

TEST_CASE("RM0038 18.2.1: TIM9 counts down on Cat.3 and up, up only on Cat.1 and Cat.2")
{
  for(auto const* desc : {&l152re(), &l152rb()})
  {
    CAPTURE(desc->part);
    auto const cat12 = desc == &l152rb();
    rig        r{*desc, cat12 ? firmware_b : firmware};
    r.base("TIM9", 999);
    r.write("TIM9", "CR1", 0x71);
    r.at_tick(300);
    if(cat12)
    {
      CHECK(r.read("TIM9", "CR1") == 0x01);
      CHECK(r.read("TIM9", "CNT") == 300);
    }
    else
    {
      CHECK(r.read("TIM9", "CR1") == 0x61);
      CHECK(r.read("TIM9", "CNT") == 300);
      r.write("TIM9", "CR1", 0);
      r.write("TIM9", "CNT", 500);
      r.write("TIM9", "CR1", 0x11);
      r.at_tick(500);
      CHECK(r.read("TIM9", "CNT") == 300);
      CHECK(r.get("TIM9", "CR1", "DIR") == 1);
    }
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE("RM0038 18.4.3: TIM9 encoder mode 1 counts PA2 edges on Cat.3, reserved on Cat.1")
{
  for(auto const* desc : {&l152re(), &l152rb()})
  {
    CAPTURE(desc->part);
    auto const cat12 = desc == &l152rb();
    rig        r{*desc, cat12 ? firmware_b : firmware};
    r.set("RCC", "AHBENR", "GPIOAEN", 1);
    auto* const porta = r.f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(porta != nullptr);
    porta->drive(2, false);
    porta->drive(3, false);
    for(auto const* moder : {"MODER2", "MODER3"})
    {
      r.set("GPIOA", "MODER", moder, 2);
    }
    r.set("GPIOA", "AFRL", "AFRL2", 3);
    r.set("GPIOA", "AFRL", "AFRL3", 3);
    r.base("TIM9", 0xffff);
    r.write("TIM9", "CCMR1_Input", 0x0101);
    r.slave("TIM9", 0, 1);
    r.enable("TIM9");
    auto const t0 = r.now();
    for(std::uint64_t cycle = 0; cycle < 5; ++cycle)
    {
      porta->drive_at(2, true, vtime{t0 + (40 * cycle + 10) * us});
      porta->drive_at(3, true, vtime{t0 + (40 * cycle + 20) * us});
      porta->drive_at(2, false, vtime{t0 + (40 * cycle + 30) * us});
      porta->drive_at(3, false, vtime{t0 + (40 * cycle + 40) * us});
    }
    r.run_until(t0 + 205 * us);
    CHECK(r.read("TIM9", "CNT") == (cat12 ? 0u : 10u));
    CHECK(r.warnings() == (cat12 ? 1u : 0u));
  }
  rig r{l152rb(), firmware_b, true};
  r.base("TIM9", 0xffff);
  CHECK_THROWS_AS(r.cpu_write("TIM9", "SMCR", 1), popcorn::model_error);
  CHECK(r.read("TIM9", "SMCR") == 0);
}

TEST_CASE("RM0038 18.4.14: TIM9_OR TI1_RMP 01 takes the LSE, tim_ti1 reads low and logs once")
{
  for(std::uint32_t const remap : {0u, 1u, 2u, 3u})
  {
    CAPTURE(remap);
    rig r;
    r.set("RCC", "AHBENR", "GPIOAEN", 1);
    auto* const porta = r.f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(porta != nullptr);
    porta->drive(2, false);
    r.set("GPIOA", "MODER", "MODER2", 2);
    r.set("GPIOA", "AFRL", "AFRL2", 3);
    r.base("TIM9", 0xffff);
    r.set("TIM9", "OR", "TI1_RMP", remap);
    r.set("TIM9", "CCMR1_Input", "CC1S", 1);
    r.set("TIM9", "CCER", "CC1E", 1);
    r.enable("TIM9");
    auto const t0 = r.now();
    porta->drive_at(2, true, vtime{t0 + 10 * us});
    r.run_until(t0 + 20 * us);
    CHECK(r.get("TIM9", "SR", "CC1IF") == (remap == 1 ? 0u : 1u));
    CHECK(r.warnings() == (remap == 1 ? 1u : 0u));
  }
  for(auto const* desc : {&l152re(), &l152rb()})
  {
    CAPTURE(desc->part);
    rig r{*desc, desc == &l152rb() ? firmware_b : firmware};
    r.set("RCC", "AHBENR", "GPIOAEN", 1);
    auto* const porta = r.f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(porta != nullptr);
    porta->drive(2, true);
    r.set("GPIOA", "MODER", "MODER2", 2);
    r.set("GPIOA", "AFRL", "AFRL2", 3);
    r.base("TIM9", 0xffff);
    r.set("TIM9", "CCMR1_Input", "CC1S", 1);
    r.set("TIM9", "CCER", "CC1E", 1);
    r.enable("TIM9");
    r.at_tick(10);
    r.write("TIM9", "SR", 0);
    r.set("TIM9", "OR", "TI1_RMP", 1);
    r.at_tick(20);
    CHECK(r.get("TIM9", "SR", "CC1IF") == 0);
    r.set("TIM9", "OR", "TI1_RMP", 0);
    r.at_tick(30);
    CHECK(r.get("TIM9", "SR", "CC1IF") == 1);
    CHECK(r.read("TIM9", "CCR1") >= 20);
  }
  rig r{l152re(), firmware, true};
  r.clock("TIM9");
  CHECK_THROWS_AS(r.cpu_write("TIM9", "OR", 1), popcorn::model_error);
  CHECK(r.read("TIM9", "OR") == 0);
  r.write("TIM9", "OR", 2);
  CHECK(r.read("TIM9", "OR") == 2);
}

TEST_CASE("RM0038 18.3.3: TIM9 ECE takes the internal ETR, logs once, model_error under strict")
{
  {
    rig r;
    r.base("TIM9", 0xffff);
    r.set("TIM9", "SMCR", "ECE", 1);
    r.enable("TIM9");
    r.at_tick(100);
    CHECK(r.read("TIM9", "CNT") == 0);
    CHECK(r.warnings() == 1);
  }
  rig r{l152re(), firmware, true};
  r.clock("TIM9");
  CHECK_THROWS_AS(r.cpu_write("TIM9", "SMCR", 1u << 14), popcorn::model_error);
  CHECK(r.read("TIM9", "SMCR") == 0);
}

TEST_CASE("with DBG_TIMx_STOP a held TIM9, TIM10 or TIM11 keeps its count, without it counts on")
{
  for(std::string_view const unit : {"TIM9", "TIM10", "TIM11"})
  {
    for(bool const frozen : {true, false})
    {
      CAPTURE(unit);
      CAPTURE(frozen);
      rig r;
      r.clock(unit);
      r.enable(unit);
      r.at_tick(1000);
      popcorn::testing::set_debug_freeze(*r.f, std::format("DBG_{}_STOP", unit), frozen);
      r.f->target->hold_core(true);
      r.at_tick(6000);
      CHECK(r.read(unit, "CNT") == (frozen ? 1000u : 6000u));
      r.f->target->hold_core(false);
    }
  }
}

TEST_CASE("an L1 TIM9 description against the up-only rule of its category throws at construction")
{
  auto const changed = with_field(
      l152rb(), "TIM9", "CR1",
      {.name = "DIR", .lsb = 4, .width = 1, .acc = popcorn::access::read_write});
  constructs_throw(changed);
  for(auto const* unit : {"TIM9", "TIM2"})
  {
    CAPTURE(unit);
    auto       desc = l152re();
    auto const timer =
        std::ranges::find(desc.peripherals, std::string{unit}, &popcorn::peripheral_desc::name);
    REQUIRE(timer != desc.peripherals.end());
    auto const cr1 =
        std::ranges::find(timer->registers, std::string{"CR1"}, &popcorn::register_desc::name);
    REQUIRE(cr1 != timer->registers.end());
    REQUIRE(
        std::erase_if(
            cr1->fields, [](popcorn::field_desc const& bits) { return bits.name == "CMS"; })
        == 1);
    constructs_throw(desc);
  }
  auto       desc = l152rb();
  auto const timer =
      std::ranges::find(desc.peripherals, std::string{"TIM9"}, &popcorn::peripheral_desc::name);
  REQUIRE(timer != desc.peripherals.end());
  auto const options =
      std::ranges::find(timer->registers, std::string{"OR"}, &popcorn::register_desc::name);
  REQUIRE(options != timer->registers.end());
  options->fields.clear();
  constructs_throw(desc);
}

TEST_CASE("RM0038 18.5.6: TIM10 OC1 clocks TIM9 on ITR2 at its wrap in PWM mode 1, its match in 2")
{
  for(std::uint32_t const mode : {pwm1, pwm2})
  {
    CAPTURE(mode);
    rig r;
    r.base("TIM10", 99);
    r.write("TIM10", "CCR1", 25);
    r.write("TIM10", "CCMR1_Output", mode);
    r.set("TIM10", "CCER", "CC1E", 1);
    r.base("TIM9", 0xffff);
    r.slave("TIM9", 2, sms_external);
    r.enable("TIM9");
    r.enable("TIM10");
    r.at_tick(1010);
    CHECK(r.read("TIM9", "CNT") == 10);
    r.at_tick(1050);
    CHECK(r.read("TIM9", "CNT") == (mode == pwm1 ? 10u : 11u));
    CHECK(r.get("TIM9", "SR", "TIF") == 1);
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE("RM0038 18.5.6: TIM11 OC1 in PWM mode 1 gates TIM3 on ITR2, 25 ticks of every 100")
{
  for(auto const* desc : {&l152re(), &l152rb()})
  {
    CAPTURE(desc->part);
    rig r{*desc, desc == &l152rb() ? firmware_b : firmware};
    r.base("TIM11", 99);
    r.write("TIM11", "CCR1", 25);
    r.write("TIM11", "CCMR1_Output", pwm1);
    r.set("TIM11", "CCER", "CC1E", 1);
    r.base("TIM3", 0xffff);
    r.slave("TIM3", 2, sms_gated);
    r.enable("TIM3");
    r.enable("TIM11");
    r.at_tick(1000);
    CHECK(r.read("TIM3", "CNT") == 250);
    r.at_tick(1010);
    CHECK(r.read("TIM3", "CNT") == 260);
    r.at_tick(1050);
    CHECK(r.read("TIM3", "CNT") == 275);
  }
}

TEST_CASE("a CCMR1, CCR1 or CNT write that moves TIM10 OC1 is an edge, CC1S 01 low inferred")
{
  rig r;
  r.clock("TIM10");
  r.set("TIM10", "CCER", "CC1E", 1);
  r.base("TIM9", 0xffff);
  r.slave("TIM9", 2, sms_external);
  r.enable("TIM9");
  r.write("TIM10", "CCMR1_Output", 5u << 4);
  CHECK(r.read("TIM9", "CNT") == 1);
  r.write("TIM10", "CCMR1_Output", 4u << 4);
  r.write("TIM10", "CCMR1_Output", 5u << 4);
  CHECK(r.read("TIM9", "CNT") == 2);
  r.write("TIM10", "CCMR1_Output", (5u << 4) | 1u);
  r.write("TIM10", "CCMR1_Output", 5u << 4);
  CHECK(r.read("TIM9", "CNT") == 3);
  r.write("TIM10", "CCMR1_Output", 0);
  r.write("TIM10", "CCMR1_Output", pwm1);
  CHECK(r.read("TIM9", "CNT") == 3);
  r.write("TIM10", "CCR1", 10);
  CHECK(r.read("TIM9", "CNT") == 4);
  r.write("TIM10", "CNT", 20);
  r.write("TIM10", "CNT", 5);
  CHECK(r.read("TIM9", "CNT") == 5);
  r.write("TIM10", "CCMR1_Output", 0);
  r.write("TIM10", "CCMR1_Output", pwm2);
  CHECK(r.read("TIM9", "CNT") == 5);
  CHECK(r.warnings() == 0);
}

TEST_CASE("a TIM10RST pulse drops a high TIM10 OC1, TIM9 gated on it stops at the pulse")
{
  rig r;
  r.clock("TIM10");
  r.write("TIM10", "CCMR1_Output", 5u << 4);
  r.set("TIM10", "CCER", "CC1E", 1);
  r.base("TIM9", 0xffff);
  r.slave("TIM9", 2, sms_gated);
  r.enable("TIM9");
  r.at_tick(100);
  r.set("RCC", "APB2RSTR", "TIM10RST", 1);
  r.set("RCC", "APB2RSTR", "TIM10RST", 0);
  r.at_tick(200);
  CHECK(r.read("TIM9", "CNT") == 100);
  CHECK(r.read("TIM10", "CCMR1_Output") == 0);
}

TEST_CASE("RM0038 18.5.6: a TIM10 CC1P write inverts OC1 at the write, TIM9 counts each rise")
{
  rig r;
  r.clock("TIM10");
  r.write("TIM10", "CCMR1_Output", 4u << 4);
  r.set("TIM10", "CCER", "CC1E", 1);
  r.base("TIM9", 0xffff);
  r.slave("TIM9", 2, sms_external);
  r.enable("TIM9");
  r.set("TIM10", "CCER", "CC1P", 1);
  CHECK(r.read("TIM9", "CNT") == 1);
  r.set("TIM10", "CCER", "CC1P", 0);
  CHECK(r.read("TIM9", "CNT") == 1);
  r.set("TIM10", "CCER", "CC1P", 1);
  CHECK(r.read("TIM9", "CNT") == 2);
  CHECK(r.warnings() == 0);
}

TEST_CASE("RM0038 18.5.6: TIM11 OC1 with CC1P 1 in PWM mode 1 gates TIM3 75 ticks of every 100")
{
  rig r;
  r.base("TIM11", 99);
  r.write("TIM11", "CCR1", 25);
  r.write("TIM11", "CCMR1_Output", pwm1);
  r.set("TIM11", "CCER", "CC1E", 1);
  r.set("TIM11", "CCER", "CC1P", 1);
  r.base("TIM3", 0xffff);
  r.slave("TIM3", 2, sms_gated);
  r.enable("TIM3");
  r.enable("TIM11");
  r.at_tick(1000);
  CHECK(r.read("TIM3", "CNT") == 750);
  r.at_tick(1010);
  CHECK(r.read("TIM3", "CNT") == 750);
  r.at_tick(1050);
  CHECK(r.read("TIM3", "CNT") == 775);
}

TEST_CASE("RM0038 18.5.6: TIM3 reset by TIM11 OC1 sets UIF each period and reads the TIM11 count")
{
  rig r;
  r.base("TIM11", 99);
  r.write("TIM11", "CCR1", 25);
  r.write("TIM11", "CCMR1_Output", pwm1);
  r.set("TIM11", "CCER", "CC1E", 1);
  r.base("TIM3", 99);
  r.set("TIM3", "CR1", "URS", 1);
  r.slave("TIM3", 2, sms_reset);
  r.enable("TIM3");
  r.enable("TIM11");
  std::size_t flagged = 0;
  std::size_t exact   = 0;
  for(std::uint64_t period = 1; period <= 100; ++period)
  {
    r.at_tick(period * 100 + 50);
    flagged += r.get("TIM3", "SR", "UIF");
    exact += r.read("TIM3", "CNT") == r.read("TIM11", "CNT") ? 1u : 0u;
    r.write("TIM3", "SR", 0);
  }
  CHECK(flagged == 100);
  CHECK(exact == 100);
  CHECK(r.warnings() == 0);
}

TEST_CASE("RM0038 18.5.6: TIM3 triggered by a TIM11 OC1 rise counts every TIM11 tick since it")
{
  rig r;
  r.base("TIM11", 99);
  r.write("TIM11", "CCR1", 50);
  r.write("TIM11", "CCMR1_Output", pwm2);
  r.set("TIM11", "CCER", "CC1E", 1);
  r.base("TIM3", 0xffff);
  r.slave("TIM3", 2, sms_trigger);
  r.enable("TIM11");
  std::size_t exact = 0;
  for(std::uint64_t period = 1; period <= 100; ++period)
  {
    r.at_tick(period * 100);
    auto const master = period * 100 + r.read("TIM11", "CNT");
    exact += r.read("TIM3", "CNT") == master - 50 ? 1u : 0u;
  }
  CHECK(exact == 100);
  CHECK(r.get("TIM3", "SR", "TIF") == 1);
}

TEST_CASE("RM0038 18.5.6: TIM3 gated by TIM11 OC1 with CC1P 1 reads every open tick mid-window")
{
  rig r;
  r.base("TIM11", 99);
  r.write("TIM11", "CCR1", 25);
  r.write("TIM11", "CCMR1_Output", pwm1);
  r.set("TIM11", "CCER", "CC1E", 1);
  r.set("TIM11", "CCER", "CC1P", 1);
  r.base("TIM3", 0xffff);
  r.slave("TIM3", 2, sms_gated);
  r.enable("TIM3");
  r.enable("TIM11");
  std::size_t exact = 0;
  for(std::uint64_t period = 0; period < 100; ++period)
  {
    r.at_tick(period * 100 + 60);
    auto const open = period * 75 + r.read("TIM11", "CNT") - 25;
    exact += r.read("TIM3", "CNT") == open ? 1u : 0u;
  }
  CHECK(exact == 100);
}

TEST_CASE("TIM4 reset by a TIM3 update or CC1 match TRGO, TIM3 clocked by TIM11 OC1, sets UIF")
{
  struct leg
  {
    std::uint32_t mode;
    std::uint32_t offset;
  };
  for(auto const [mode, offset] : {
          leg{master_update,  50},
          leg{master_compare, 0 }
  })
  {
    CAPTURE(mode);
    rig r;
    r.base("TIM11", 49);
    r.write("TIM11", "CCR1", 25);
    r.write("TIM11", "CCMR1_Output", pwm1);
    r.set("TIM11", "CCER", "CC1E", 1);
    r.base("TIM3", 1);
    r.write("TIM3", "CCR1", 1);
    r.write("TIM3", "CR2", mode << 4);
    r.slave("TIM3", 2, sms_external);
    r.base("TIM4", 99);
    r.set("TIM4", "CR1", "URS", 1);
    r.slave("TIM4", 2, sms_reset);
    r.enable("TIM3");
    r.enable("TIM11");
    r.at_tick(30);
    r.enable("TIM4");
    std::size_t flagged   = 0;
    std::size_t triggered = 0;
    std::size_t exact     = 0;
    for(std::uint64_t period = 2; period <= 101; ++period)
    {
      r.at_tick(period * 100 + 75);
      flagged += r.get("TIM4", "SR", "UIF");
      triggered += r.get("TIM4", "SR", "TIF");
      exact += r.read("TIM4", "CNT") == r.read("TIM11", "CNT") + offset ? 1u : 0u;
      r.write("TIM4", "SR", 0);
    }
    CHECK(flagged == 100);
    CHECK(triggered == 100);
    CHECK(exact == 100);
    CHECK(r.warnings() == 0);
  }
}

TEST_CASE(
    "under strict a firmware TIM10 CC1S 10 or 11 write throws as reserved, a debugger one stores")
{
  rig r{l152re(), firmware, true};
  r.clock("TIM10");
  for(std::uint32_t const select : {0b10u, 0b11u})
  {
    CAPTURE(select);
    CHECK_THROWS_AS(r.cpu_write("TIM10", "CCMR1_Input", select), popcorn::model_error);
    CHECK(r.read("TIM10", "CCMR1_Input") == 0);
  }
  REQUIRE(r.warnings() == 1);
  CHECK(r.logged.back().message.contains("reserved on a one-channel timer"));
  r.write("TIM10", "CCMR1_Input", 0b10);
  CHECK(r.read("TIM10", "CCMR1_Input") == 0b10);
  CHECK(r.warnings() == 1);
}

TEST_CASE("TIM3 at half the TIM11 clock, reset by each TIM11 OC1 rise, reads half the TIM11 count")
{
  rig r;
  r.set("RCC", "CFGR", "PPRE1", 0b101);
  r.base("TIM11", 99);
  r.write("TIM11", "CCR1", 25);
  r.write("TIM11", "CCMR1_Output", pwm1);
  r.set("TIM11", "CCER", "CC1E", 1);
  r.base("TIM3", 0xffff);
  r.slave("TIM3", 2, sms_reset);
  r.enable("TIM3");
  r.enable("TIM11");
  std::size_t exact = 0;
  for(std::uint64_t period = 1; period <= 100; ++period)
  {
    r.at_tick(period * 100 + 60);
    exact += r.read("TIM3", "CNT") == r.read("TIM11", "CNT") / 2 ? 1u : 0u;
  }
  CHECK(exact == 100);
  CHECK(r.read("TIM3", "CNT") < 40);
}

TEST_CASE("an L1 TIM10 description with a CCMR1_Output field at bit 16 throws at construction")
{
  constructs_throw(with_field(
      l152re(), "TIM10", "CCMR1_Output",
      {.name = "OC1M_3", .lsb = 16, .width = 1, .acc = popcorn::access::read_write}));
}

TEST_CASE("TIM10 OC1M 011 or OC1CE 1 holds OC1 low and logs once, under strict the write throws")
{
  {
    rig r;
    r.base("TIM9", 0xffff);
    r.slave("TIM9", 2, sms_external);
    r.enable("TIM9");
    r.base("TIM10", 9);
    r.write("TIM10", "CCR1", 5);
    r.write("TIM10", "CCMR1_Output", toggle);
    r.enable("TIM10");
    r.at_tick(100);
    CHECK(r.read("TIM9", "CNT") == 0);
    CHECK(r.warnings() == 1);
  }
  for(std::uint32_t const unmodelled : {toggle, pwm1 | (1u << 7)})
  {
    CAPTURE(unmodelled);
    rig r{l152re(), firmware, true};
    r.base("TIM10", 9);
    r.base("TIM9", 0xffff);
    r.cpu_write("TIM9", "SMCR", sms_external | (2u << 4));
    CHECK_THROWS_AS(r.cpu_write("TIM10", "CCMR1_Output", unmodelled), popcorn::model_error);
    CHECK(r.read("TIM10", "CCMR1_Output") == 0);
    CHECK(r.warnings() == 1);
    r.write("TIM9", "SMCR", 0);
    r.write("TIM10", "CCMR1_Output", unmodelled);
    CHECK_THROWS_AS(r.cpu_write("TIM9", "SMCR", sms_external | (2u << 4)), popcorn::model_error);
    CHECK(r.read("TIM9", "SMCR") == 0);
    CHECK(r.warnings() == 2);
  }
}

TEST_CASE(
    "RM0038 18.5.12, 18.5.13: TI1_RMP keeps TIM10 tim_ti1 on the pin for 00, TIM11 for 00, 11")
{
  struct leg
  {
    std::string_view unit;
    std::uint8_t     pin;
    std::uint32_t    pin_values;
  };
  for(auto const& [unit, pin, pin_values] : {
          leg{"TIM10", 6, 0b0001},
          leg{"TIM11", 7, 0b1001}
  })
  {
    for(std::uint32_t const remap : {0u, 1u, 2u, 3u})
    {
      CAPTURE(unit);
      CAPTURE(remap);
      rig r;
      r.set("RCC", "AHBENR", "GPIOAEN", 1);
      auto* const porta = r.f->model_of<stm32::gpio_port>("GPIOA");
      REQUIRE(porta != nullptr);
      porta->drive(pin, false);
      r.set("GPIOA", "MODER", std::format("MODER{}", pin), 2);
      r.set("GPIOA", "AFRL", std::format("AFRL{}", pin), 3);
      r.base(unit, 0xffff);
      r.set(unit, "OR", "TI1_RMP", remap);
      r.set(unit, "CCMR1_Input", "CC1S", 1);
      r.set(unit, "CCER", "CC1E", 1);
      r.enable(unit);
      auto const t0 = r.now();
      porta->drive_at(pin, true, vtime{t0 + 10 * us});
      r.run_until(t0 + 20 * us);
      auto const on_pin = ((pin_values >> remap) & 1u) != 0;
      CHECK(r.get(unit, "SR", "CC1IF") == (on_pin ? 1u : 0u));
      CHECK(r.warnings() == (on_pin ? 0u : 1u));
    }
  }
  for(std::string_view const unit : {"TIM10", "TIM11"})
  {
    CAPTURE(unit);
    {
      rig r;
      r.set("RCC", "AHBENR", "GPIOAEN", 1);
      auto* const porta = r.f->model_of<stm32::gpio_port>("GPIOA");
      REQUIRE(porta != nullptr);
      auto const pin = unit == "TIM10" ? 6u : 7u;
      porta->drive(pin, false);
      r.set("GPIOA", "MODER", std::format("MODER{}", pin), 2);
      r.set("GPIOA", "AFRL", std::format("AFRL{}", pin), 3);
      r.base(unit, 0xffff);
      r.set(unit, "OR", "TI1_RMP_RI", 1);
      r.set(unit, "CCMR1_Input", "CC1S", 1);
      r.set(unit, "CCER", "CC1E", 1);
      r.enable(unit);
      auto const t0 = r.now();
      porta->drive_at(pin, true, vtime{t0 + 10 * us});
      r.run_until(t0 + 20 * us);
      CHECK(r.get(unit, "SR", "CC1IF") == 0);
      CHECK(r.warnings() == 1);
    }
    rig r{l152re(), firmware, true};
    r.clock(unit);
    CHECK_THROWS_AS(r.cpu_write(unit, "OR", 1), popcorn::model_error);
    CHECK_THROWS_AS(r.cpu_write(unit, "OR", 1u << 3), popcorn::model_error);
    CHECK(r.read(unit, "OR") == 0);
    r.cpu_write(unit, "OR", 1u << 2);
    CHECK(r.read(unit, "OR") == (1u << 2));
  }
}

TEST_CASE("RM0038 18.5.12 and 18.5.13: TIM10 and TIM11 ECE count nothing whatever ETR_RMP")
{
  for(std::string_view const unit : {"TIM10", "TIM11"})
  {
    for(std::uint32_t const etr : {0u, 1u})
    {
      CAPTURE(unit);
      CAPTURE(etr);
      {
        rig r;
        r.base("TIM9", 99);
        r.master("TIM9");
        r.enable("TIM9");
        r.base(unit, 0xffff);
        r.set(unit, "OR", "ETR_RMP", etr);
        r.set(unit, "SMCR", "ECE", 1);
        r.enable(unit);
        r.at_tick(1000);
        CHECK(r.read(unit, "CNT") == 0);
        CHECK(r.warnings() == 1);
      }
      rig r{l152re(), firmware, true};
      r.clock(unit);
      r.set(unit, "OR", "ETR_RMP", etr);
      CHECK_THROWS_AS(r.cpu_write(unit, "SMCR", 1u << 14), popcorn::model_error);
      CHECK(r.read(unit, "SMCR") == 0);
    }
  }
}

TEST_CASE("an L1 TIM10 or TIM11 description with DIR, or a Cat.3 one without TI1_RMP_RI, throws")
{
  for(std::string_view const unit : {"TIM10", "TIM11"})
  {
    CAPTURE(unit);
    for(auto const* desc : {&l152re(), &l152rb()})
    {
      CAPTURE(desc->part);
      constructs_throw(with_field(
          *desc, unit, "CR1",
          {.name = "DIR", .lsb = 4, .width = 1, .acc = popcorn::access::read_write}));
    }
    auto       desc = l152re();
    auto const timer =
        std::ranges::find(desc.peripherals, std::string{unit}, &popcorn::peripheral_desc::name);
    REQUIRE(timer != desc.peripherals.end());
    auto const options =
        std::ranges::find(timer->registers, std::string{"OR"}, &popcorn::register_desc::name);
    REQUIRE(options != timer->registers.end());
    REQUIRE(
        std::erase_if(
            options->fields,
            [](popcorn::field_desc const& bits) { return bits.name == "TI1_RMP_RI"; })
        == 1);
    constructs_throw(desc);
  }
}

namespace
{
using wide = unsigned __int128;

constexpr std::uint64_t msi_clock_ps = (ps_per_second + msi_hz - 1) / msi_hz;

auto exact_at(std::uint64_t from, std::uint64_t ticks) -> wide
{
  return wide{from} * msi_hz + wide{ticks} * ps_per_second;
}

auto exact_ps(std::uint64_t from, std::uint64_t ticks) -> std::uint64_t
{
  return static_cast<std::uint64_t>((exact_at(from, ticks) + msi_hz - 1) / msi_hz);
}
}

TEST_CASE(
    "RM0038 Table 61 inferred: TIM2 CC3 pulses tim2_cc3 one timer clock wide at each match's "
    "exact instant")
{
  rig   r{l152re(), firmware, false, true};
  auto& probe = r.probe("tim2_cc3");
  probe.listen(true);
  r.base("TIM2", 99);
  r.write("TIM2", "CCR3", 30);
  auto const from = r.now();
  r.enable("TIM2");
  r.run_until(from + ticks_ps(450));
  REQUIRE(probe.rises.size() == 5);
  REQUIRE(probe.falls.size() == 5);
  std::size_t lagged = 0;
  for(std::uint64_t k = 0; k < 5; ++k)
  {
    CAPTURE(k);
    auto const ps = exact_ps(from, 30 + 100 * k);
    CHECK(probe.rises[k].ps == ps);
    CHECK(probe.rises[k].hz == msi_hz);
    CHECK(
        probe.rises[k].lag
        == static_cast<std::uint64_t>(wide{ps} * msi_hz - exact_at(from, 30 + 100 * k)));
    CHECK(probe.falls[k] == ps + msi_clock_ps);
    lagged += probe.rises[k].lag != 0 ? 1u : 0u;
  }
  CHECK(lagged > 0);
  CHECK(r.get("TIM2", "SR", "CC3IF") == 1);
}

TEST_CASE(
    "a TIM2 CNT read five periods after a sink listens unscheduled pulses tim2_cc1 at each match")
{
  rig   r{l152re(), firmware, false, true};
  auto& probe = r.probe("tim2_cc1");
  r.base("TIM2", 99);
  r.write("TIM2", "CCR1", 30);
  auto const from = r.now();
  r.enable("TIM2");
  r.run_until(from + ticks_ps(250));
  static_cast<void>(r.read("TIM2", "CNT"));
  probe.listening = true;
  r.run_until(from + ticks_ps(750));
  CHECK(probe.pulses.empty());
  static_cast<void>(r.read("TIM2", "CNT"));
  std::vector<std::uint64_t> expected;
  for(std::uint64_t k = 3; k <= 7; ++k)
  {
    expected.push_back(exact_ps(from, 30 + 100 * k));
  }
  CHECK(probe.pulses == expected);
}

TEST_CASE(
    "TIM2 CC2G pulses tim2_cc2 at the EGR write, on an output and on an input channel, inferred")
{
  rig   r{l152re(), firmware, false, true};
  auto& probe = r.probe("tim2_cc2");
  probe.listen(true);
  r.clock("TIM2");
  auto const first = r.now();
  r.write("TIM2", "EGR", 1u << 2);
  r.run_until(first + 10 * us);
  r.write("TIM2", "CCMR1_Input", 1u << 8);
  auto const second = r.now();
  r.write("TIM2", "EGR", 1u << 2);
  CHECK(probe.pulses == std::vector<std::uint64_t>{first, second});
  CHECK(r.get("TIM2", "SR", "CC2IF") == 1);
  CHECK(r.get("TIM2", "SR", "CC2OF") == 1);
}

TEST_CASE(
    "a timer produces tim<n>_ccx for each of its channels, TIM10 cc1 only, TIM6 none, and a CC "
    "event has no level")
{
  rig         r{l152re(), firmware, false, true};
  auto const& peers = r.probe("tim2_cc1").context.peers;
  for(auto const* const signal :
      {"tim2_cc1", "tim2_cc4", "tim3_cc3", "tim4_cc2", "tim9_cc2", "tim10_cc1"})
  {
    CAPTURE(signal);
    CHECK(stm32::trigger_source_of(peers, signal) != nullptr);
  }
  for(auto const* const signal : {"tim9_cc3", "tim10_cc2", "tim6_cc1", "tim2_cc5"})
  {
    CAPTURE(signal);
    CHECK(stm32::trigger_source_of(peers, signal) == nullptr);
  }
  r.clock("TIM2");
  r.write("TIM2", "CR2", 1u << 4);
  r.enable("TIM2");
  auto* const source = stm32::trigger_source_of(peers, "tim2_cc1");
  REQUIRE(source != nullptr);
  CHECK(source->level("tim2_trgo") == std::optional<bool>{true});
  CHECK_FALSE(source->level("tim2_cc1").has_value());
}

TEST_CASE(
    "a TIM2 match with CC1IF set is scheduled only while a sink listens to tim2_cc1, "
    "else the wrap alone")
{
  for(bool const listening : {false, true})
  {
    CAPTURE(listening);
    rig   r{l152re(), firmware, false, true};
    auto& probe = r.probe("tim2_cc1");
    r.base("TIM2", 99);
    r.write("TIM2", "CCR1", 30);
    auto const from = r.now();
    r.enable("TIM2");
    r.run_until(from + ticks_ps(105));
    REQUIRE(r.get("TIM2", "SR", "CC1IF") == 1);
    probe.listen(listening);
    r.write("TIM2", "SR", ~1u);
    auto const& units = r.f->target->description().peripherals;
    auto const  owner = static_cast<std::uint32_t>(
        std::ranges::find(units, std::string_view{"TIM2"}, &popcorn::peripheral_desc::name)
        - units.begin());
    std::vector<std::uint64_t> due;
    for(auto const& event : r.f->target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner and event.due_ps >= r.now())
      {
        due.push_back(event.due_ps);
      }
    }
    CHECK(due == std::vector<std::uint64_t>{exact_ps(from, listening ? 130 : 200)});
  }
}
