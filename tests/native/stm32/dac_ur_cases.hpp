#pragma once

#include "analog_peers.hpp"
#include "dac_rows.hpp"
#include "stm32_fixture.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dac_ur
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::uint64_t ps_per_s  = 1'000'000'000'000;
constexpr std::uint32_t en1       = 1u << 0;
constexpr std::uint32_t ten1      = 1u << 1;
constexpr std::uint32_t en2       = 1u << 16;
constexpr std::uint32_t rdy1      = 1u << 11;
constexpr std::uint32_t rdy2      = 1u << 27;
constexpr std::uint32_t dmaen1    = 1u << 12;
constexpr std::uint32_t dmaudrie1 = 1u << 13;
constexpr std::uint32_t dmaudr1   = 1u << 13;
constexpr std::uint32_t dorstat1  = 1u << 12;
constexpr std::uint32_t double1   = 1u << 8;
constexpr std::uint64_t samples   = 0x2000'8000;

struct rig
{
  std::string_view firmware;
  auto (*desc)() -> part_description const&;
  std::string_view                 instance;
  std::string_view                 clock_reg;
  std::string_view                 clock_field;
  std::string_view                 reset_reg;
  std::string_view                 reset_field;
  std::uint64_t                    hclk_hz;
  std::uint64_t                    wake_buffer_ps;
  std::uint64_t                    wake_no_buffer_ps;
  std::array<std::string_view, 15> triggers;
  stm32::dac_variant const*        variant;
  std::uint8_t                     chip_modes;
  std::uint8_t                     pad_modes;
  std::uint8_t                     reserved_modes;
};

auto the_rig() -> rig const&;

constexpr auto tsel1(std::uint32_t select) -> std::uint32_t
{
  return select << 2;
}

constexpr auto cycles(std::uint64_t count, std::uint64_t hz) -> std::uint64_t
{
  return (count * ps_per_s + hz - 1) / hz;
}

struct bench
{
  rig const&                     r;
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  bench(bench const&)                    = delete;
  auto operator=(bench const&) -> bench& = delete;

  explicit bench(rig const& which, bool strict = false, part_description desc = {}) : r{which}
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    f                          = std::make_unique<stm32_fixture>(
        desc.part.empty() ? which.desc() : std::move(desc), which.firmware, std::move(options));
    f->set("pc", sym(r.firmware, "idle"));
    sink = f->target->add_log_sink([this](core::log_record const& record) {
      if(record.kind == core::log_kind::model_warning)
      {
        logged.push_back(record);
      }
    });
    popcorn::testing::enable_clocks(
        *f->target, {
                        {r.clock_reg, r.clock_field}
    });
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> address
  {
    auto const& desc  = f->target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto read(std::string_view reg) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at(r.instance, reg), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at(r.instance, reg), 4, value).has_value());
  }

  [[nodiscard]] auto try_write(std::string_view reg, std::uint32_t value) const -> bool
  {
    try
    {
      return f->target->mem().write(at(r.instance, reg), 4, value).has_value();
    }
    catch(popcorn::model_error const&)
    {
      return false;
    }
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void idle_to(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto events(std::uint32_t kind) const -> std::vector<std::uint64_t>
  {
    auto const& units = f->target->description().peripherals;
    auto const  found = std::ranges::find(units, r.instance, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    auto const                 owner = static_cast<std::uint32_t>(found - units.begin());
    std::vector<std::uint64_t> dues;
    for(auto const& event : f->target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner and event.id.kind == kind)
      {
        dues.push_back(event.due_ps);
      }
    }
    return dues;
  }

  [[nodiscard]] auto sink_of() const -> stm32::trigger_sink&
  {
    auto* const found = f->model_of<stm32::trigger_sink>(r.instance);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto pad() const -> stm32::gpio_port&
  {
    auto* const port = f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(port != nullptr);
    return *port;
  }

  [[nodiscard]] auto read_of(std::string_view peripheral, std::string_view reg) const
      -> std::uint32_t
  {
    auto const value = f->target->mem().read(at(peripheral, reg), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write_of(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at(peripheral, reg), 4, value).has_value());
  }

  [[nodiscard]] auto irq_number() const -> std::uint32_t
  {
    auto const& irqs = f->target->description().irqs;
    auto const  one  = std::ranges::find(irqs, std::string{"DAC1_IRQn"}, &popcorn::irq_desc::name);
    REQUIRE(one != irqs.end());
    return one->number;
  }

  [[nodiscard]] auto irq_pending() const -> bool
  {
    auto const number = irq_number();
    auto const word   = f->target->mem().read(0xE000'E200u + 4 * (number / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (number % 32) & 1u) != 0;
  }

  void clear_irq() const
  {
    auto const number = irq_number();
    REQUIRE(f->target->mem()
                .write(0xE000'E280u + 4 * (number / 32), 4, 1u << (number % 32))
                .has_value());
  }

  void dma_channel(
      std::uint32_t              request,
      std::string_view           holding,
      std::vector<std::uint32_t> data,
      std::string_view           controller = "GPDMA1") const
  {
    auto const prefix = std::format("{}_C0", controller.substr(0, 5));
    if(controller == "LPDMA1")
    {
      popcorn::testing::enable_clocks(
          *f->target, {
                          {"RCC_AHB3ENR", "LPDMA1EN"}
      });
    }
    else
    {
      popcorn::testing::enable_clocks(
          *f->target, {
                          {"RCC_AHB1ENR", "GPDMA1EN"}
      });
    }
    for(std::size_t index = 0; index < data.size(); ++index)
    {
      REQUIRE(f->target->mem().write(samples + 4 * index, 4, data[index]).has_value());
    }
    write_of(controller, prefix + "TR1", 0x2000A);
    write_of(controller, prefix + "TR2", request | 1u << 10);
    write_of(controller, prefix + "BR1", static_cast<std::uint32_t>(4 * data.size()));
    write_of(controller, prefix + "SAR", static_cast<std::uint32_t>(samples));
    write_of(controller, prefix + "DAR", static_cast<std::uint32_t>(at(r.instance, holding)));
    write_of(controller, prefix + "LLR", 0);
    write_of(controller, prefix + "FCR", (1u << 8) | (1u << 9));
    write_of(controller, prefix + "CR", 1u);
  }

  [[nodiscard]] auto dma_left(std::string_view controller = "GPDMA1") const -> std::uint32_t
  {
    return read_of(controller, std::format("{}_C0BR1", controller.substr(0, 5))) & 0xFFFF;
  }

  void trigger(std::string_view name, std::uint64_t settle_cycles = 100) const
  {
    auto const when = now_ps();
    sink_of().triggered(name, stm32::trigger_edge::rising, when);
    idle_to(when + cycles(settle_cycles, r.hclk_hz));
  }

  void reset_pulse() const
  {
    auto const bit = std::uint32_t{1} << shift("RCC", r.reset_reg, r.reset_field);
    write_of("RCC", r.reset_reg, bit);
    write_of("RCC", r.reset_reg, 0);
  }

  void lsi_on() const
  {
    auto const on    = std::uint32_t{1} << shift("RCC", "RCC_BDCR", "LSION");
    auto const ready = std::uint32_t{1} << shift("RCC", "RCC_BDCR", "LSIRDY");
    write_of("RCC", "RCC_BDCR", read_of("RCC", "RCC_BDCR") | on);
    for(int step = 0; step < 1000 and (read_of("RCC", "RCC_BDCR") & ready) == 0; ++step)
    {
      idle_to(now_ps() + 10'000'000);
    }
    REQUIRE((read_of("RCC", "RCC_BDCR") & ready) != 0);
  }

  [[nodiscard]] auto
      shift(std::string_view peripheral, std::string_view reg, std::string_view field) const
      -> std::uint32_t
  {
    auto const& desc  = f->target->peripheral(peripheral).description();
    auto const* found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const at = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(at != found->fields.end());
    return at->lsb;
  }

  void ready(std::uint32_t mask = rdy1) const
  {
    idle_to(now_ps() + 10 * ps_per_s / 1'000'000);
    REQUIRE((read("SR") & mask) == mask);
  }
};

inline auto without(part_description desc, std::string_view instance, std::string_view name)
    -> part_description
{
  auto unit = std::ranges::find(desc.peripherals, instance, &popcorn::peripheral_desc::name);
  REQUIRE(unit != desc.peripherals.end());
  for(auto& reg : unit->registers)
  {
    std::erase_if(reg.fields, [&](popcorn::field_desc const& one) { return one.name == name; });
  }
  return desc;
}
}

namespace
{
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
}

TEST_CASE(
    "RM0456 Table 342 and RM0481 Table 306: TSEL 0000 is SWTRIG and 0001 to 1111 listen to "
    "dac_chx_trg1 to trg15, each on its own source only, three dac_hclk cycles after the edge")
{
  auto const& r = dac_ur::the_rig();
  for(std::uint32_t select = 1; select <= 15; ++select)
  {
    auto const name = r.triggers[select - 1];
    if(name.empty())
    {
      continue;
    }
    CAPTURE(select);
    dac_ur::bench b{r};
    b.write("CR", dac_ur::ten1 | dac_ur::tsel1(select));
    b.write("DHR12R1", 0x2A5);
    auto& sink = b.sink_of();
    for(auto const other : r.triggers)
    {
      CHECK(sink.listens(other) == (other == name and not other.empty()));
    }
    b.idle_to(b.now_ps() + 5 * dac_ur::cycles(1, r.hclk_hz));
    CHECK(b.read("DOR1") == 0);
    auto const at = b.now_ps();
    sink.triggered(name, stm32::trigger_edge::rising, at);
    b.idle_to(at + dac_ur::cycles(3, r.hclk_hz));
    CHECK(b.read("DOR1") == 0x2A5);
    CHECK(b.logged.empty());
  }
}

TEST_CASE(
    "RM0456 35.7.1: TSEL 0000 is the software trigger, SWTRIGx loads one dac_hclk cycle later, and "
    "TSEL 0111 is dac_chx_trg7 and not the software trigger")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(0));
  b.write("DHR12R1", 0x123);
  b.idle_to(b.now_ps() + 5 * dac_ur::cycles(1, r.hclk_hz));
  CHECK(b.read("DOR1") == 0);
  auto const at = b.now_ps();
  b.write("SWTRGR", 1);
  CHECK(b.events(0) == std::vector<std::uint64_t>{at + dac_ur::cycles(1, r.hclk_hz)});
  b.idle_to(at + dac_ur::cycles(1, r.hclk_hz));
  CHECK(b.read("DOR1") == 0x123);
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(7));
  b.write("DHR12R1", 0x456);
  b.write("SWTRGR", 1);
  b.idle_to(b.now_ps() + 5 * dac_ur::cycles(1, r.hclk_hz));
  CHECK(b.read("DOR1") == 0x123);
}

TEST_CASE(
    "RM0456 Table 342 and RM0481 Table 306: a reserved TSEL logs one "
    "warning with TEN set, a model_error when strict that leaves DAC_CR")
{
  auto const& r = dac_ur::the_rig();
  for(std::uint32_t select = 1; select <= 15; ++select)
  {
    auto const name = r.triggers[select - 1];
    if(not name.empty())
    {
      continue;
    }
    CAPTURE(select);
    {
      dac_ur::bench b{r};
      b.write("CR", dac_ur::ten1 | dac_ur::tsel1(select));
      b.write("CR", dac_ur::ten1 | dac_ur::tsel1(select));
      CHECK(b.logged.size() == 1);
      CHECK(b.logged.front().message.contains(name.empty() ? "reserved" : name));
    }
    {
      dac_ur::bench b{r, true};
      CHECK_FALSE(b.try_write("CR", dac_ur::ten1 | dac_ur::tsel1(select)));
      CHECK(b.read("CR") == 0);
    }
  }
}

TEST_CASE(
    "RM0456 35.4.4: DAC1RDY sets tWAKEUP after EN1, the typical datasheet value with the buffer on "
    "or off, inferred, and clears with EN1")
{
  auto const& r = dac_ur::the_rig();
  for(auto const buffer_off : {false, true})
  {
    CAPTURE(buffer_off);
    dac_ur::bench b{r};
    if(buffer_off)
    {
      b.write("MCR", 0b010);
    }
    auto const at = b.now_ps();
    b.write("CR", dac_ur::en1);
    auto const wake = buffer_off ? r.wake_no_buffer_ps : r.wake_buffer_ps;
    CHECK(b.events(6) == std::vector<std::uint64_t>{at + wake});
    CHECK((b.read("SR") & dac_ur::rdy1) == 0);
    b.idle_to(at + wake - 2 * dac_ur::cycles(1, r.hclk_hz));
    CHECK((b.read("SR") & dac_ur::rdy1) == 0);
    b.idle_to(at + wake);
    CHECK((b.read("SR") & dac_ur::rdy1) != 0);
    CHECK((b.read("SR") & dac_ur::rdy2) == 0);
    b.write("CR", 0);
    CHECK((b.read("SR") & dac_ur::rdy1) == 0);
  }
}

TEST_CASE(
    "RM0456 35.4.4: a data write or a software trigger with EN1 set and DAC1RDY clear logs one "
    "warning and is a model_error when strict, which leaves the register")
{
  auto const& r = dac_ur::the_rig();
  {
    dac_ur::bench b{r};
    b.write("CR", dac_ur::en1);
    b.write("DHR12R1", 0x111);
    b.write("DHR12R1", 0x222);
    CHECK(b.logged.size() == 1);
  }
  {
    dac_ur::bench b{r, true};
    b.write("CR", dac_ur::en1);
    CHECK_FALSE(b.try_write("DHR12R1", 0x111));
    CHECK((b.read("DHR12R1") & 0xFFF) == 0);
    b.write("CR", dac_ur::en1 | dac_ur::ten1);
    CHECK_FALSE(b.try_write("SWTRGR", 1));
    b.ready();
    CHECK(b.try_write("DHR12R1", 0x111));
    CHECK((b.read("DHR12R1") & 0xFFF) == 0x111);
  }
  {
    dac_ur::bench b{r};
    b.write("DHR12R1", 0x111);
    CHECK(b.logged.empty());
  }
}

TEST_CASE("a reset clears DAC1RDY and a first enable's wake-up does not set it after the second")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("CR", dac_ur::en1);
  auto const first = b.now_ps();
  b.f->target->reset(popcorn::reset_kind::software);
  b.f->set("pc", dac_ur::sym(r.firmware, "idle"));
  popcorn::testing::enable_clocks(
      *b.f->target, {
                        {r.clock_reg, r.clock_field}
  });
  CHECK((b.read("SR") & dac_ur::rdy1) == 0);
  b.idle_to(b.now_ps() + r.wake_buffer_ps / 2);
  auto const second = b.now_ps();
  b.write("CR", dac_ur::en1);
  b.idle_to(first + r.wake_buffer_ps + 1'000);
  CHECK((b.read("SR") & dac_ur::rdy1) == 0);
  b.idle_to(second + r.wake_buffer_ps);
  CHECK((b.read("SR") & dac_ur::rdy1) != 0);
}

TEST_CASE(
    "a snapshot taken during the wake-up sets DAC1RDY at the same time once restored, keeps DHRB "
    "and still cancels the wake-up when ENx is cleared")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("DHR12R1", 0x0ABC'0123);
  auto const at = b.now_ps();
  b.write("CR", dac_ur::en1);
  b.idle_to(at + r.wake_buffer_ps / 2);
  auto const saved = b.f->target->snapshot();

  auto          options = popcorn::testing::stm32_options();
  stm32_fixture other{r.desc(), r.firmware, std::move(options)};
  other.target->restore(
      dac_ur::core::snapshot{
          dac_ur::core::decode_snapshot(dac_ur::core::encode_snapshot(saved.data()))});
  other.set("pc", dac_ur::sym(r.firmware, "idle"));
  auto const sr = [&] {
    auto const& desc = other.target->peripheral(r.instance).description();
    auto const* reg  = popcorn::find_register(desc, "SR");
    auto const  word = other.target->mem().read(desc.base + reg->offset, 4);
    REQUIRE(word.has_value());
    return static_cast<std::uint32_t>(*word);
  };
  CHECK((sr() & dac_ur::rdy1) == 0);
  REQUIRE(
      other
          .run_to_stop(
              {.max_instructions = icount{100'000'000}, .until_time = vtime{at + r.wake_buffer_ps}})
          .reason
      == stop_reason::time);
  CHECK((sr() & dac_ur::rdy1) != 0);
  auto const& desc = other.target->peripheral(r.instance).description();
  auto const  word = [&](std::string_view name) {
    auto const value =
        other.target->mem().read(desc.base + popcorn::find_register(desc, name)->offset, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  };
  CHECK(word("DHR12R1") == 0x0ABC'0123);
  CHECK(word("DHR12L1") == (0x0ABCu << 20 | 0x1230u));
}

TEST_CASE("a snapshot restored during the wake-up cancels it when ENx is cleared")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    at = b.now_ps();
  b.write("CR", dac_ur::en1);
  b.idle_to(at + r.wake_buffer_ps / 2);
  auto const    saved = b.f->target->snapshot();
  stm32_fixture other{r.desc(), r.firmware, popcorn::testing::stm32_options()};
  other.target->restore(
      dac_ur::core::snapshot{
          dac_ur::core::decode_snapshot(dac_ur::core::encode_snapshot(saved.data()))});
  other.set("pc", dac_ur::sym(r.firmware, "idle"));
  auto const& desc = other.target->peripheral(r.instance).description();
  auto const  cr   = desc.base + popcorn::find_register(desc, "CR")->offset;
  auto const  sr   = desc.base + popcorn::find_register(desc, "SR")->offset;
  REQUIRE(other.target->mem().write(cr, 4, 0).has_value());
  REQUIRE(
      other
          .run_to_stop(
              {.max_instructions = icount{100'000'000},
               .until_time       = vtime{at + 2 * r.wake_buffer_ps}})
          .reason
      == stop_reason::time);
  auto const value = other.target->mem().read(sr, 4);
  REQUIRE(value.has_value());
  CHECK((*value & dac_ur::rdy1) == 0);
}

TEST_CASE("RM0456 35.4.5 Table 343: SINFORMAT1 inverts the MSB of the data loaded into DAC_DOR1")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("MCR", 1u << 9);
  for(auto const& [written, loaded] : {
          std::pair{0x7FFu, 0xFFFu},
          std::pair{0x000u, 0x800u},
          std::pair{0xFFFu, 0x7FFu},
          std::pair{0x800u, 0x000u}
  })
  {
    CAPTURE(written);
    b.write("DHR12R1", written);
    b.idle_to(b.now_ps() + 2 * dac_ur::cycles(1, r.hclk_hz));
    CHECK(b.read("DOR1") == loaded);
  }
  b.write("MCR", 0);
  b.write("DHR12R1", 0x800);
  b.idle_to(b.now_ps() + 2 * dac_ur::cycles(1, r.hclk_hz));
  CHECK(b.read("DOR1") == 0x800);
}

TEST_CASE("RM0456 35.7.3: the DACCxDHRB fields store through DAC_DHR12R, DHR12L and DHR8R")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("DHR12R1", 0x0ABC'0123);
  CHECK(b.read("DHR12R1") == 0x0ABC'0123);
  CHECK(b.read("DHR12L1") == (0x0ABCu << 20 | 0x1230u));
  CHECK(b.read("DHR8R1") == (0xABu << 8 | 0x12u));
  b.idle_to(b.now_ps() + 2 * dac_ur::cycles(1, r.hclk_hz));
  CHECK((b.read("DOR1") & 0xFFF) == 0x123);
  b.write("DHR12L2", 0xDEF0'4560);
  CHECK(b.read("DHR12R2") == 0x0DEF'0456);
  b.write("DHR8R2", 0x7766);
  CHECK(b.read("DHR12R2") == 0x0770'0660);
}

TEST_CASE(
    "RM0456 35.4.9: DMADOUBLE1 written while EN1 or DMAEN1 is 1 keeps the old bit, one warning, a "
    "model_error when strict that leaves DAC_MCR")
{
  auto const& r = dac_ur::the_rig();
  {
    dac_ur::bench b{r};
    b.write("MCR", 1u << 8);
    CHECK(b.read("MCR") == 1u << 8);
    b.write("CR", dac_ur::en1);
    b.write("MCR", 0);
    b.write("MCR", 0);
    CHECK(b.read("MCR") == 1u << 8);
    CHECK(b.logged.size() == 1);
  }
  {
    dac_ur::bench b{r, true};
    b.write("CR", dac_ur::dmaen1);
    CHECK_FALSE(b.try_write("MCR", 1u << 8));
    CHECK(b.read("MCR") == 0);
  }
}

TEST_CASE("RM0456 35.7.16: HFSEL is stored")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("MCR", 2u << 14);
  CHECK(b.read("MCR") == 2u << 14);
}

TEST_CASE("checklist 39: the DAC row needs DAC1RDY, SINFORMAT1, DMADOUBLE1 and the DHRB fields")
{
  auto const& r = dac_ur::the_rig();
  CHECK_NOTHROW(stm32_fixture(r.desc(), r.firmware));
  for(auto const name : {"DAC1RDY", "SINFORMAT1", "DMADOUBLE1"})
  {
    CAPTURE(name);
    CHECK_THROWS_AS(
        stm32_fixture(dac_ur::without(r.desc(), r.instance, name), r.firmware),
        popcorn::config_error);
  }
}

TEST_CASE(
    "RM0456 35.7.16 and RM0481 30.7.16: MODE1 000 to 111 drive PA4 and the on-chip level as the "
    "tables say, and a reserved value keeps the old MODE1 with one warning")
{
  auto const& r = dac_ur::the_rig();
  for(std::uint32_t mode = 0; mode < 8; ++mode)
  {
    CAPTURE(mode);
    auto const    reserved = (r.reserved_modes >> mode & 1u) != 0;
    dac_ur::bench b{r};
    b.lsi_on();
    b.write("MCR", mode);
    CHECK(b.read("MCR") == (reserved ? 0u : mode));
    CHECK(b.logged.size() == (reserved ? 1u : 0u));
    b.write("CR", dac_ur::en1);
    b.ready();
    b.write("DHR12R1", 0x800);
    b.idle_to(b.now_ps() + 2'000'000'000);
    auto const effective = reserved ? 0u : mode;
    auto const pad       = (r.pad_modes >> effective & 1u) != 0;
    auto const chip      = (r.chip_modes >> effective & 1u) != 0;
    CHECK(b.logged.size() == (reserved ? 1u : 0u));
    CHECK((b.pad().analog_level(4) == 1'500'000) == pad);
    auto* const source = b.f->model_of<stm32::internal_analog_source>(r.instance);
    REQUIRE(source != nullptr);
    CHECK(source->internal_level(0).has_value() == chip);
  }
}

TEST_CASE("RM0456 35.4.6: the loads count dac_hclk cycles, the AHB clock, and not the APB1 clock")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const&   rcc = b.f->target->peripheral("RCC").description();
  auto const*   cfg = popcorn::find_register(rcc, "CFGR2");
  REQUIRE(cfg != nullptr);
  REQUIRE(b.f->target->mem().write(rcc.base + cfg->offset, 4, 4u << 4).has_value());
  auto const at = b.now_ps();
  b.write("DHR12R1", 0x111);
  CHECK(b.events(0) == std::vector<std::uint64_t>{at + dac_ur::cycles(1, r.hclk_hz)});
}

TEST_CASE(
    "RM0456 35.4.4: ENx cleared before tWAKEUP cancels the wake-up, DAC1RDY stays 0 past the old "
    "due, and a later enable sets it at its own due")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    first = b.now_ps();
  b.write("CR", dac_ur::en1);
  b.idle_to(first + r.wake_buffer_ps / 10);
  b.write("CR", 0);
  CHECK(b.events(6).empty());
  b.idle_to(first + r.wake_buffer_ps + 1'000'000);
  CHECK((b.read("SR") & dac_ur::rdy1) == 0);
  auto const second = b.now_ps();
  b.write("CR", dac_ur::en1);
  CHECK(b.events(6) == std::vector<std::uint64_t>{second + r.wake_buffer_ps});
  b.idle_to(second + r.wake_buffer_ps - 2 * dac_ur::cycles(1, r.hclk_hz));
  CHECK((b.read("SR") & dac_ur::rdy1) == 0);
  b.idle_to(second + r.wake_buffer_ps);
  CHECK((b.read("SR") & dac_ur::rdy1) != 0);
}

TEST_CASE(
    "RM0456 35.4.4: a re-enable with a longer tWAKEUP sets DAC1RDY at the new due and not at the "
    "due of the cancelled enable")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("MCR", 0b010);
  auto const first = b.now_ps();
  b.write("CR", dac_ur::en1);
  b.idle_to(first + r.wake_no_buffer_ps / 4);
  b.write("CR", 0);
  b.write("MCR", 0);
  auto const second = b.now_ps();
  b.write("CR", dac_ur::en1);
  b.idle_to(first + r.wake_no_buffer_ps + 1'000'000);
  CHECK((b.read("SR") & dac_ur::rdy1) == 0);
  b.idle_to(second + r.wake_buffer_ps - 2 * dac_ur::cycles(1, r.hclk_hz));
  CHECK((b.read("SR") & dac_ur::rdy1) == 0);
  b.idle_to(second + r.wake_buffer_ps);
  CHECK((b.read("SR") & dac_ur::rdy1) != 0);
}

TEST_CASE("checklist 31: the RCC reset pulse during the wake-up drops it and DAC1RDY stays 0")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    at = b.now_ps();
  b.write("CR", dac_ur::en1);
  b.idle_to(at + r.wake_buffer_ps / 2);
  b.reset_pulse();
  CHECK(b.events(6).empty());
  b.idle_to(at + 2 * r.wake_buffer_ps);
  CHECK(b.read("SR") == 0);
}

TEST_CASE(
    "RM0456 35.4.4, inferred: a hardware trigger in the DAC1RDY 0 window "
    "loads DOR1 and logs no warning")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5));
  b.write("DHR12R1", 0x2A5);
  REQUIRE((b.read("DOR1") & 0xFFF) == 0);
  b.write("CR", dac_ur::en1 | dac_ur::ten1 | dac_ur::tsel1(5));
  REQUIRE((b.read("SR") & dac_ur::rdy1) == 0);
  b.trigger(r.triggers[4], 10);
  REQUIRE((b.read("SR") & dac_ur::rdy1) == 0);
  CHECK((b.read("DOR1") & 0xFFF) == 0x2A5);
  CHECK(b.logged.empty());
}

TEST_CASE(
    "RM0456 35.4.5, inferred: SINFORMAT1 inverts the holding value before the triangle term is "
    "added, which saturates at 0xFFF")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("MCR", 1u << 9);
  auto const wave = b.shift(r.instance, "CR", "WAVE1");
  auto const amp  = b.shift(r.instance, "CR", "MAMP1");
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(0) | 2u << wave | 3u << amp);
  b.write("DHR12R1", 0x7FF);
  for(auto const loaded : {0xFFFu, 0xFFFu, 0xFFFu})
  {
    b.write("SWTRGR", 1);
    b.idle_to(b.now_ps() + 4 * dac_ur::cycles(1, r.hclk_hz));
    CHECK((b.read("DOR1") & 0xFFF) == loaded);
  }
  b.write("DHR12R1", 0x000);
  b.write("SWTRGR", 1);
  b.idle_to(b.now_ps() + 4 * dac_ur::cycles(1, r.hclk_hz));
  CHECK((b.read("DOR1") & 0xFFF) == 0x800 + 3);
}

TEST_CASE(
    "RM0456 35.4.9 and RM0481 30.4.9: a hardware load with DMAEN1 raises dac1_ch1_dma, GPDMA1 "
    "writes the next sample into DHR12R1 and a software trigger, a DHR write and DMAEN1 0 raise "
    "none, the instant inferred")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.write("DHR12R1", 0x111);
  b.dma_channel(2, "DHR12R1", {0x222, 0x333});
  b.idle_to(b.now_ps() + 100 * dac_ur::cycles(1, r.hclk_hz));
  CHECK(b.dma_left() == 8);
  for(auto const& [loaded, next, left] : {
          std::tuple{0x111u, 0x222u, 4u},
          std::tuple{0x222u, 0x333u, 0u},
  })
  {
    CAPTURE(loaded);
    b.trigger(tim6);
    CHECK((b.read("DOR1") & 0xFFF) == loaded);
    CHECK((b.read("DHR12R1") & 0xFFF) == next);
    CHECK(b.dma_left() == left);
  }
  b.trigger(tim6);
  CHECK((b.read("DOR1") & 0xFFF) == 0x333);
  CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dmaudr1) != 0);
  CHECK(b.logged.empty());
}

TEST_CASE(
    "RM0456 35.4.9 and RM0481 30.4.9: SWTRIG1, a TEN1 0 write and DMAEN1 0 raise no request, and "
    "clearing DMAEN1 drops a high request, inferred")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(0) | dac_ur::dmaen1);
  b.write("DHR12R1", 0x111);
  b.write("SWTRGR", 1);
  b.write("SWTRGR", 1);
  b.idle_to(b.now_ps() + 100 * dac_ur::cycles(1, r.hclk_hz));
  CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
  b.write("CR", dac_ur::dmaen1);
  b.write("DHR12R1", 0x222);
  b.idle_to(b.now_ps() + 100 * dac_ur::cycles(1, r.hclk_hz));
  CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
  b.dma_channel(2, "DHR12R1", {0x333, 0x444});
  b.idle_to(b.now_ps() + 100 * dac_ur::cycles(1, r.hclk_hz));
  CHECK(b.dma_left() == 8);
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.trigger(r.triggers[4]);
  CHECK(b.dma_left() == 4);
  b.write_of("GPDMA1", "GPDMA_C0CR", 0);
}

TEST_CASE(
    "RM0456 35.4.9 and RM0481 30.4.9: a hardware load while the request is high sets DMAUDR1, "
    "raises no new request and still loads DOR1, and DMAUDR1 with DMAUDRIE1 holds DAC1_IRQn")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1 | dac_ur::dmaudrie1);
  b.write("DHR12R1", 0x111);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
  CHECK_FALSE(b.irq_pending());
  b.write("DHR12R1", 0x222);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dmaudr1) != 0);
  CHECK((b.read("DOR1") & 0xFFF) == 0x222);
  CHECK(b.irq_pending());
  b.clear_irq();
  CHECK(b.irq_pending());
  b.write("SR", dac_ur::dmaudr1);
  b.clear_irq();
  CHECK_FALSE(b.irq_pending());
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dmaudr1) != 0);
  CHECK_FALSE(b.irq_pending());
}

TEST_CASE(
    "RM0456 35.4.9 and RM0481 30.4.9 double data: DMADOUBLE1 gives a request every second trigger, "
    "the second makes DORB1 the output with DORSTAT1 set and the third loads DOR1 again")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("MCR", dac_ur::double1);
  b.write("CR", dac_ur::en1 | dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.ready();
  b.write("DHR12R1", 0x0400'0800);
  b.dma_channel(2, "DHR12R1", {0x0C00'0200, 0x0100'0300});
  struct step
  {
    std::uint32_t dor;
    bool          second;
    std::uint32_t dhr;
    std::uint32_t left;
    std::uint32_t level_uv;
  };
  for(auto const& want : {
          step{0x800, false, 0x0C00'0200, 4, 1'500'000},
          step{0x800, true,  0x0C00'0200, 4, 750'000  },
          step{0x200, false, 0x0100'0300, 0, 375'000  },
          step{0x200, true,  0x0100'0300, 0, 2'250'000},
  })
  {
    CAPTURE(want.dor);
    CAPTURE(want.second);
    b.trigger(tim6);
    CHECK((b.read("DOR1") & 0xFFF) == want.dor);
    CHECK(((b.read("SR") & dac_ur::dorstat1) != 0) == want.second);
    CHECK(b.read("DHR12R1") == want.dhr);
    CHECK(b.dma_left() == want.left);
    CHECK(b.pad().analog_level(4) == want.level_uv);
    CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
  }
  CHECK(b.logged.empty());
}

TEST_CASE(
    "RM0456 35.4.9 and RM0481 30.4.9 double data: the underrun rule applies to the first trigger, "
    "a software trigger and a DMAEN1 0 trigger load DOR1 and clear DORSTAT1, inferred")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("MCR", dac_ur::double1);
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.write("DHR12R1", 0x0400'0800);
  b.trigger(tim6);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dorstat1) != 0);
  CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dmaudr1) != 0);
  CHECK((b.read("SR") & dac_ur::dorstat1) == 0);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dorstat1) != 0);
  b.write("SR", dac_ur::dmaudr1);
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(0) | dac_ur::dmaen1);
  b.write("DHR12R1", 0x0400'0300);
  b.write("SWTRGR", 1);
  b.idle_to(b.now_ps() + 100 * dac_ur::cycles(1, r.hclk_hz));
  CHECK((b.read("DOR1") & 0xFFF) == 0x300);
  CHECK((b.read("SR") & dac_ur::dorstat1) == 0);
  CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.trigger(tim6);
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5));
  b.write("SR", dac_ur::dmaudr1);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dorstat1) == 0);
  CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
}

TEST_CASE(
    "checklist 72: a snapshot restored into a machine never reset keeps the request, the "
    "alternation and DORB, so the second trigger sets DORSTAT1, drives DHRB with or without "
    "SINFORMAT1 and GPDMA1 serves the restored request")
{
  auto const& r = dac_ur::the_rig();
  for(auto const signed_format : {false, true})
  {
    CAPTURE(signed_format);
    dac_ur::bench b{r};
    auto const    tim6 = r.triggers[4];
    b.write("MCR", dac_ur::double1 | (signed_format ? 1u << 9 : 0u));
    b.write("CR", dac_ur::en1 | dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
    b.ready();
    b.write("DHR12R1", 0x0400'0800);
    b.trigger(tim6);
    auto const bytes = dac_ur::core::encode_snapshot(b.f->target->snapshot().data());

    dac_ur::bench fresh{r};
    fresh.f->target->restore(dac_ur::core::snapshot{dac_ur::core::decode_snapshot(bytes)});
    fresh.f->set("pc", dac_ur::sym(r.firmware, "idle"));
    fresh.trigger(tim6);
    CHECK((fresh.read("SR") & dac_ur::dorstat1) != 0);
    CHECK((fresh.read("SR") & dac_ur::dmaudr1) == 0);
    CHECK(fresh.pad().analog_level(4) == (signed_format ? 2'250'000u : 750'000u));
    fresh.dma_channel(2, "DHR12R1", {0x0C00'0200});
    fresh.idle_to(fresh.now_ps() + 100 * dac_ur::cycles(1, r.hclk_hz));
    CHECK(fresh.dma_left() == 0);
    CHECK(fresh.read("DHR12R1") == 0x0C00'0200);
    fresh.trigger(tim6);
    CHECK((fresh.read("DOR1") & 0xFFF) == (signed_format ? 0xA00u : 0x200u));
    CHECK((fresh.read("SR") & dac_ur::dorstat1) == 0);
  }
}

TEST_CASE(
    "checklist 31: the RCC pulse and a machine reset drop the request, the alternation and a "
    "pending load, and a trigger before the old due loads nothing")
{
  auto const& r = dac_ur::the_rig();
  for(auto const machine_reset : {false, true})
  {
    CAPTURE(machine_reset);
    dac_ur::bench b{r};
    auto const    tim6 = r.triggers[4];
    b.write("MCR", dac_ur::double1);
    b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1 | dac_ur::dmaudrie1);
    b.write("DHR12R1", 0x0400'0800);
    b.trigger(tim6);
    b.trigger(tim6);
    REQUIRE((b.read("SR") & dac_ur::dorstat1) != 0);
    auto const at = b.now_ps();
    b.sink_of().triggered(tim6, stm32::trigger_edge::rising, at);
    if(machine_reset)
    {
      b.f->target->reset(popcorn::reset_kind::software);
      b.f->set("pc", dac_ur::sym(r.firmware, "idle"));
      popcorn::testing::enable_clocks(
          *b.f->target, {
                            {r.clock_reg, r.clock_field}
      });
    }
    else
    {
      b.reset_pulse();
    }
    b.idle_to(at + dac_ur::cycles(100, r.hclk_hz));
    CHECK(b.read("SR") == 0);
    CHECK(b.read("MCR") == 0);
    CHECK((b.read("DOR1") & 0xFFF) == 0);
    b.write("MCR", dac_ur::double1);
    b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
    b.write("DHR12R1", 0x0400'0800);
    b.dma_channel(2, "DHR12R1", {0x0C00'0200});
    b.trigger(tim6);
    CHECK((b.read("SR") & dac_ur::dorstat1) == 0);
    CHECK((b.read("SR") & dac_ur::dmaudr1) == 0);
    CHECK(b.dma_left() == 0);
  }
}

TEST_CASE(
    "RM0456 35.4.9 double data: ENx cleared restarts the alternation, so the next trigger "
    "after the wake-up loads DOR1 again and DORSTAT1 stays 0, inferred")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("MCR", dac_ur::double1);
  b.write("CR", dac_ur::en1 | dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.ready();
  b.write("DHR12R1", 0x0400'0800);
  b.trigger(tim6);
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.write("DHR12R1", 0x0400'0300);
  b.write("CR", dac_ur::en1 | dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.ready();
  b.write("SR", dac_ur::dmaudr1);
  b.trigger(tim6);
  CHECK((b.read("DOR1") & 0xFFF) == 0x300);
  CHECK((b.read("SR") & dac_ur::dorstat1) == 0);
}

TEST_CASE("RM0456 35.4.5 and 35.4.9: SINFORMAT1 inverts the bit 11 of DORB1 as it does of DOR1")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("MCR", dac_ur::double1 | 1u << 9);
  b.write("CR", dac_ur::en1 | dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.ready();
  b.write("DHR12R1", 0);
  b.trigger(tim6);
  CHECK((b.read("DOR1") & 0xFFF) == 0x800);
  CHECK(b.pad().analog_level(4) == 1'500'000);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dorstat1) != 0);
  CHECK(b.pad().analog_level(4) == 1'500'000);
}

TEST_CASE(
    "RM0456 35.4.9 double data, inferred: a DHRx write that loads and a trigger with DMAEN1 0 "
    "between the two triggers of a pair restart it, so the next trigger loads DOR1 and "
    "DORSTAT1 stays 0")
{
  auto const& r = dac_ur::the_rig();
  for(auto const dmaen_leg : {false, true})
  {
    CAPTURE(dmaen_leg);
    dac_ur::bench b{r};
    auto const    tim6 = r.triggers[4];
    auto const    both = dac_ur::en1 | dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1;
    b.write("MCR", dac_ur::double1);
    b.write("CR", both);
    b.ready();
    b.write("DHR12R1", 0x0400'0800);
    b.trigger(tim6);
    if(dmaen_leg)
    {
      b.write("CR", both & ~dac_ur::dmaen1);
      b.trigger(tim6);
      b.write("CR", both);
    }
    else
    {
      b.write("CR", both & ~dac_ur::ten1);
      b.write("DHR12R1", 0x0400'0300);
      b.idle_to(b.now_ps() + 10 * dac_ur::cycles(1, r.hclk_hz));
      b.write("CR", both);
    }
    b.write("SR", dac_ur::dmaudr1);
    b.write("DHR12R1", 0x0400'0300);
    b.trigger(tim6);
    CHECK((b.read("DOR1") & 0xFFF) == 0x300);
    CHECK((b.read("SR") & dac_ur::dorstat1) == 0);
  }
}

TEST_CASE(
    "RM0456 35.4.9 and 35.4.10 double data, inferred: the triangle counter steps at the first "
    "trigger of a pair only")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("MCR", dac_ur::double1);
  auto const wave = b.shift(r.instance, "CR", "WAVE1");
  auto const amp  = b.shift(r.instance, "CR", "MAMP1");
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1 | 2u << wave | 3u << amp);
  b.write("DHR12R1", 0x0400'0100);
  b.trigger(tim6);
  CHECK((b.read("DOR1") & 0xFFF) == 0x100);
  b.trigger(tim6);
  CHECK((b.read("SR") & dac_ur::dorstat1) != 0);
  b.trigger(tim6);
  CHECK((b.read("DOR1") & 0xFFF) == 0x101);
}
