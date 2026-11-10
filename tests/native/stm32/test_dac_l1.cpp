#include "analog_peers.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware  = "l152xe_l1_rcc_ll";
constexpr address          dac       = 0x4000'7400;
constexpr address          cr        = 0x00;
constexpr address          swtrigr   = 0x04;
constexpr address          dhr12r1   = 0x08;
constexpr address          dhr12l1   = 0x0C;
constexpr address          dhr8r1    = 0x10;
constexpr address          dhr12r2   = 0x14;
constexpr address          dhr12l2   = 0x18;
constexpr address          dhr8r2    = 0x1C;
constexpr address          dhr12rd   = 0x20;
constexpr address          dhr12ld   = 0x24;
constexpr address          dhr8rd    = 0x28;
constexpr address          dor1      = 0x2C;
constexpr address          dor2      = 0x30;
constexpr address          sr        = 0x34;
constexpr address          rcc_ahb   = 0x4002'381C;
constexpr address          rcc_apb2  = 0x4002'3820;
constexpr address          rcc_apb1  = 0x4002'3824;
constexpr address          gpioa     = 0x4002'0000;
constexpr address          dma1      = 0x4002'6000;
constexpr address          samples   = 0x2000'2000;
constexpr std::uint64_t    hz        = 2'097'152;
constexpr std::uint32_t    dacen     = 1u << 29;
constexpr std::uint32_t    en1       = 1u << 0;
constexpr std::uint32_t    ten1      = 1u << 2;
constexpr std::uint32_t    ten2      = 1u << 18;
constexpr std::uint32_t    dmaen1    = 1u << 12;
constexpr std::uint32_t    dmaudrie1 = 1u << 13;
constexpr std::uint32_t    dmaudr1   = 1u << 13;
constexpr std::uint32_t    dac_irq   = 21;

constexpr auto cycles(std::uint64_t count) -> std::uint64_t
{
  return (count * 1'000'000'000'000 + hz - 1) / hz;
}

constexpr auto tsel1(std::uint32_t select) -> std::uint32_t
{
  return select << 3;
}

constexpr auto tsel2(std::uint32_t select) -> std::uint32_t
{
  return select << 19;
}

constexpr std::array<address, 6> trigger_timers{0x4000'1000, 0x4000'1400, 0x4001'0800,
                                                0x4000'0000, 0x4000'0800, 0};

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  explicit rig(bool strict = false)
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(stm32::board{}, l152re());
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(l152re(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink([this](core::log_record const& record) {
      if(record.kind == core::log_kind::model_warning)
      {
        logged.push_back(record);
      }
    });
    write(rcc_apb1, dacen);
  }

  [[nodiscard]] auto read(address at) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(address at, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at, 4, value).has_value());
  }

  void idle_to(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto pending(std::uint32_t channel) const -> std::vector<std::uint64_t>
  {
    auto const& units = f->target->description().peripherals;
    auto const  found =
        std::ranges::find(units, std::string_view{"DAC"}, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    auto const                 owner = static_cast<std::uint32_t>(found - units.begin());
    std::vector<std::uint64_t> dues;
    for(auto const& event : f->target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner and event.id.kind == channel)
      {
        dues.push_back(event.due_ps);
      }
    }
    return dues;
  }

  [[nodiscard]] auto irq_pending(std::uint32_t irq) const -> bool
  {
    return (read(0xE000'E200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
  }

  [[nodiscard]] auto pad() const -> stm32::gpio_port&
  {
    auto* const port = f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(port != nullptr);
    return *port;
  }

  void sources() const
  {
    write(rcc_apb1, read(rcc_apb1) | 0x35u);
    write(rcc_apb2, read(rcc_apb2) | 1u << 2);
    write(rcc_ahb, read(rcc_ahb) | 1u);
    for(auto const timer : trigger_timers)
    {
      if(timer != 0)
      {
        write(timer + 0x04, 2u << 4);
      }
    }
    write(gpioa, read(gpioa) & ~(3u << 18));
    pad().drive(9, false);
  }

  void fire(std::uint32_t select) const
  {
    if(select < 5)
    {
      write(trigger_timers[select] + 0x14, 1);
      return;
    }
    pad().drive(9, true);
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return logged.size();
  }
};
}

TEST_CASE("RM0038 13.5: the L1 DAC has SWTRIGR and BOFFx, and no MCR, CCR, SHSRx, SHHR or SHRR")
{
  auto const& desc = *std::ranges::find(
      l152re().peripherals, std::string_view{"DAC"}, &popcorn::peripheral_desc::name);
  for(auto const name : {"CR", "SWTRIGR", "DOR1", "DOR2", "SR"})
  {
    CAPTURE(name);
    CHECK(popcorn::find_register(desc, name) != nullptr);
  }
  for(auto const name : {"MCR", "CCR", "SHSR1", "SHSR2", "SHHR", "SHRR", "SWTRGR"})
  {
    CAPTURE(name);
    CHECK(popcorn::find_register(desc, name) == nullptr);
  }
  rig const r;
  r.write(dac + cr, 0b10);
  CHECK(r.read(dac + cr) == 0b10);
}

TEST_CASE(
    "RM0038 13.3.4: with TEN1 0 a DAC_DHR12R1 write loads DAC_DOR1 one APB1 cycle later, with the "
    "load pending then")
{
  rig const  r;
  auto const at = r.now_ps();
  r.write(dac + dhr12r1, 0xABC);
  CHECK(r.pending(0) == std::vector<std::uint64_t>{at + cycles(1)});
  CHECK(r.read(dac + dor1) == 0);
  r.idle_to(at + cycles(1));
  CHECK(r.read(dac + dor1) == 0xABC);
  CHECK(r.pending(0).empty());
  CHECK(r.logged.empty());
}

TEST_CASE("RM0038 13.3.3: every holding register stores DHR1 and DHR2 by its shift, inferred")
{
  rig const r;
  r.write(dac + dhr12r1, 0x123);
  CHECK(r.read(dac + dhr12l1) == 0x1230);
  CHECK(r.read(dac + dhr8r1) == 0x12);
  r.write(dac + dhr12l1, 0xABC0);
  CHECK(r.read(dac + dhr12r1) == 0xABC);
  r.write(dac + dhr8r1, 0xCD);
  CHECK(r.read(dac + dhr12r1) == 0xCD0);
  r.write(dac + dhr12rd, 0x0456'0789);
  CHECK(r.read(dac + dhr12r1) == 0x789);
  CHECK(r.read(dac + dhr12r2) == 0x456);
  CHECK(r.read(dac + dhr12l2) == 0x4560);
  r.write(dac + dhr8rd, 0x1234);
  CHECK(r.read(dac + dhr8r2) == 0x12);
  r.idle_to(r.now_ps() + cycles(2));
  CHECK(r.read(dac + dor1) == 0x340);
  CHECK(r.read(dac + dor2) == 0x120);
  r.write(dac + dhr12ld, 0x9AB0'5670);
  CHECK(r.read(dac + dhr12r2) == 0x9AB);
  CHECK(r.read(dac + dhr12r1) == 0x567);
  static_cast<void>(dhr12l2);
  static_cast<void>(dhr8r2);
}

TEST_CASE(
    "RM0038 13.3.6: with TEN 1 and TSEL 111 a DHR write loads nothing and each SWTRIG write loads "
    "DOR one APB1 cycle later, SWTRIG reading 0")
{
  rig const r;
  r.write(dac + cr, ten1 | tsel1(7) | ten2 | tsel2(7));
  r.write(dac + dhr12rd, 0x0123'0ABC);
  CHECK(r.pending(0).empty());
  r.idle_to(r.now_ps() + cycles(5));
  CHECK(r.read(dac + dor1) == 0);
  auto const at = r.now_ps();
  r.write(dac + swtrigr, 1);
  CHECK(r.read(dac + swtrigr) == 0);
  CHECK(r.pending(0) == std::vector<std::uint64_t>{at + cycles(1)});
  CHECK(r.pending(1).empty());
  r.idle_to(at + cycles(1));
  CHECK(r.read(dac + dor1) == 0xABC);
  CHECK(r.read(dac + dor2) == 0);
  r.write(dac + swtrigr, 2);
  r.idle_to(r.now_ps() + cycles(1));
  CHECK(r.read(dac + dor2) == 0x123);
  r.write(dac + cr, ten1 | tsel1(0));
  r.write(dac + swtrigr, 1);
  r.idle_to(r.now_ps() + cycles(3));
  CHECK(r.read(dac + dor1) == 0xABC);
}

TEST_CASE(
    "RM0038 Table 68: each TSEL loads on its own source only, TIM6, TIM7, TIM9, TIM2, TIM4 TRGO "
    "and "
    "EXTI9, on both channels, three APB1 cycles after the edge")
{
  constexpr std::array<std::uint32_t, 6> selects{0, 2, 3, 4, 5, 6};
  constexpr std::array<std::uint32_t, 6> sources{0, 1, 2, 3, 4, 5};
  for(std::size_t index = 0; index < selects.size(); ++index)
  {
    for(std::uint32_t channel = 0; channel < 2; ++channel)
    {
      CAPTURE(selects[index]);
      CAPTURE(channel);
      rig const r;
      r.sources();
      r.write(
          dac + cr, channel == 0 ? (ten1 | tsel1(selects[index]) | ten2 | tsel2(7))
                                 : (ten1 | tsel1(7) | ten2 | tsel2(selects[index])));
      r.write(dac + dhr12rd, 0x0222'0111);
      r.idle_to(r.now_ps() + cycles(3));
      CHECK(r.read(dac + (channel == 0 ? dor1 : dor2)) == 0);
      for(std::uint32_t other = 0; other < 6; ++other)
      {
        if(other != sources[index])
        {
          r.fire(other);
        }
      }
      r.idle_to(r.now_ps() + cycles(5));
      CHECK(r.read(dac + (channel == 0 ? dor1 : dor2)) == 0);
      auto const at = r.now_ps();
      r.fire(sources[index]);
      r.idle_to(at + cycles(3));
      CHECK(r.read(dac + (channel == 0 ? dor1 : dor2)) == (channel == 0 ? 0x111u : 0x222u));
      CHECK(r.read(dac + (channel == 0 ? dor2 : dor1)) == 0);
    }
  }
}

TEST_CASE(
    "RM0038 Table 68: TSEL 001 is reserved, a CR write with TEN selecting it logs one warning and "
    "loads on no source, a model_error when strict that leaves DAC_CR")
{
  {
    rig const r;
    r.sources();
    r.write(dac + cr, ten1 | tsel1(1));
    r.write(dac + dhr12r1, 0x111);
    r.write(dac + cr, ten1 | tsel1(1));
    CHECK(r.warnings() == 1);
    for(std::uint32_t source = 0; source < 6; ++source)
    {
      r.fire(source);
    }
    r.idle_to(r.now_ps() + cycles(10));
    CHECK(r.read(dac + dor1) == 0);
  }
  {
    rig const r{true};
    r.write(dac + cr, en1);
    CHECK_THROWS_AS(
        static_cast<void>(r.f->target->mem().write(dac + cr, 4, ten1 | tsel1(1))),
        popcorn::model_error);
    CHECK(r.read(dac + cr) == en1);
  }
}

TEST_CASE("RM0038 13.3.8 and 13.3.9: a triangle wave adds its counter at each SWTRIG load")
{
  rig const r;
  r.write(dac + dhr12r1, 0x100);
  r.write(dac + cr, ten1 | tsel1(7) | 2u << 6 | 2u << 8);
  constexpr std::array<std::uint32_t, 10> expected{0x100, 0x101, 0x102, 0x103, 0x104,
                                                   0x105, 0x106, 0x107, 0x106, 0x105};
  for(std::uint32_t step = 0; step < expected.size(); ++step)
  {
    r.write(dac + swtrigr, 1);
    r.idle_to(r.now_ps() + cycles(1));
    CHECK(r.read(dac + dor1) == expected[step]);
  }
}

TEST_CASE(
    "RM0038 13.3.7: with DMAEN1 each TIM6 edge raises dac_ch1_dma, DMA1 channel 2 writes the next "
    "sample into DHR12R1, and the load of the next edge shows it")
{
  rig const r;
  r.sources();
  r.write(rcc_ahb, r.read(rcc_ahb) | 1u << 24);
  for(std::uint32_t index = 0; index < 3; ++index)
  {
    r.write(samples + 4 * index, 0x100 * (index + 1));
  }
  r.write(dma1 + 0x24, dac + dhr12r1);
  r.write(dma1 + 0x28, samples);
  r.write(dma1 + 0x20, 3);
  r.write(dma1 + 0x1C, 1u | 1u << 4 | 1u << 7 | 2u << 8 | 2u << 10);
  r.write(dac + dhr12r1, 0x0F0);
  r.write(dac + cr, ten1 | tsel1(0) | dmaen1);
  r.fire(0);
  r.idle_to(r.now_ps() + cycles(10));
  CHECK(r.read(dac + dor1) == 0x0F0);
  CHECK(r.read(dac + dhr12r1) == 0x100);
  r.fire(0);
  r.idle_to(r.now_ps() + cycles(10));
  CHECK(r.read(dac + dor1) == 0x100);
  CHECK(r.read(dac + dhr12r1) == 0x200);
}

TEST_CASE(
    "RM0038 13.3.7: a hardware load while dac_ch1_dma is high sets DMAUDR1 and the DAC IRQ with "
    "DMAUDRIE1, and DMAUDR1 clears on a write of 1")
{
  rig const r;
  r.sources();
  r.write(dac + cr, ten1 | tsel1(0) | dmaen1 | dmaudrie1);
  r.fire(0);
  r.idle_to(r.now_ps() + cycles(5));
  CHECK((r.read(dac + sr) & dmaudr1) == 0);
  r.fire(0);
  r.idle_to(r.now_ps() + cycles(5));
  CHECK((r.read(dac + sr) & dmaudr1) != 0);
  CHECK(r.irq_pending(dac_irq));
  r.write(dac + sr, dmaudr1);
  CHECK((r.read(dac + sr) & dmaudr1) == 0);
}

TEST_CASE(
    "ES0125 2.6.1: clearing DMAEN1 or the DAC clock leaves a high dac_ch1_dma pending, the "
    "workaround of the sheet restarts the DAC")
{
  rig const r;
  r.sources();
  r.write(rcc_ahb, r.read(rcc_ahb) | 1u << 24);
  r.write(samples, 0x321);
  r.write(dac + cr, ten1 | tsel1(0) | dmaen1);
  r.fire(0);
  r.idle_to(r.now_ps() + cycles(5));
  r.write(dac + cr, ten1 | tsel1(0));
  r.write(rcc_apb1, r.read(rcc_apb1) & ~dacen);
  r.write(dma1 + 0x24, dac + dhr12r1);
  r.write(dma1 + 0x28, samples);
  r.write(dma1 + 0x20, 1);
  r.write(rcc_apb1, r.read(rcc_apb1) | dacen);
  r.write(dma1 + 0x1C, 1u | 1u << 4 | 1u << 7 | 2u << 8 | 2u << 10);
  r.idle_to(r.now_ps() + cycles(10));
  CHECK(r.read(dac + dhr12r1) == 0x321);
}

TEST_CASE("RM0038 13.3.5: with EN1 1 PA4 reads DAC_DOR1 x VDDA / 4096 from each load on")
{
  rig const r;
  r.write(dac + dhr12r1, 0x800);
  r.idle_to(r.now_ps() + cycles(2));
  CHECK(r.pad().analog_level(4) == 0);
  r.write(dac + cr, en1);
  CHECK(r.pad().analog_level(4) == 1'500'000);
  auto* const source = r.f->model_of<stm32::internal_analog_source>("DAC");
  REQUIRE(source != nullptr);
  CHECK_FALSE(source->internal_level(0).has_value());
  r.write(dac + cr, 0);
  CHECK(r.pad().analog_level(4) == 0);
}

TEST_CASE(
    "checklist 39: the L1 DAC row needs SWTRIGR, TEN1, TSEL1, DMAUDR1 and the interrupt line "
    "DAC_IRQn")
{
  CHECK_NOTHROW(stm32_fixture(l152re(), firmware));
  auto const without = [](std::function<void(popcorn::peripheral_desc&)> const& change) {
    auto desc = l152re();
    auto unit = std::ranges::find(
        desc.peripherals, std::string_view{"DAC"}, &popcorn::peripheral_desc::name);
    REQUIRE(unit != desc.peripherals.end());
    change(*unit);
    return desc;
  };
  CHECK_THROWS_AS(
      stm32_fixture(
          without([](popcorn::peripheral_desc& unit) {
            std::erase_if(unit.registers, [](popcorn::register_desc const& reg) {
              return reg.name == "SWTRIGR";
            });
          }),
          firmware),
      popcorn::config_error);
  for(auto const name : {"TEN1", "TSEL1", "DMAUDR1"})
  {
    CAPTURE(name);
    CHECK_THROWS_AS(
        stm32_fixture(
            without([&](popcorn::peripheral_desc& unit) {
              for(auto& reg : unit.registers)
              {
                std::erase_if(
                    reg.fields, [&](popcorn::field_desc const& one) { return one.name == name; });
              }
            }),
            firmware),
        popcorn::config_error);
  }
}

TEST_CASE("checklist 72: a snapshot restored into a machine never reset keeps the TIM6 trigger")
{
  rig const r;
  r.sources();
  r.write(dac + dhr12r1, 0x2A5);
  r.write(dac + cr, ten1 | tsel1(0));
  r.fire(0);
  r.idle_to(r.now_ps() + cycles(1));
  auto const saved = r.f->target->snapshot();

  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(stm32::board{}, l152re());
  stm32_fixture other{popcorn::testing::no_reset, l152re(), firmware, std::move(options)};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.set("pc", sym(firmware, "idle"));
  auto const at    = other.target->virtual_clock().time().ps;
  auto const value = [&](address reg) {
    auto const word = other.target->mem().read(reg, 4);
    REQUIRE(word.has_value());
    return static_cast<std::uint32_t>(*word);
  };
  REQUIRE(
      other
          .run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{at + cycles(4)}})
          .reason
      == stop_reason::time);
  CHECK(value(dac + dor1) == 0x2A5);
}
