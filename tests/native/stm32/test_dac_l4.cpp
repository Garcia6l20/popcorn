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
#include <tuple>
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
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware  = "l476_l4_rcc_ll";
constexpr address          dac1      = 0x4000'7400;
constexpr address          cr        = 0x00;
constexpr address          swtrgr    = 0x04;
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
constexpr address          ccr       = 0x38;
constexpr address          mcr       = 0x3C;
constexpr address          apb1enr1  = 0x4002'1058;
constexpr address          apb1rstr1 = 0x4002'1038;
constexpr address          rcc_cfgr  = 0x4002'1008;
constexpr address          ahb2enr   = 0x4002'104C;
constexpr address          apb2enr   = 0x4002'1060;
constexpr address          gpioa     = 0x4800'0000;
constexpr address          tim_cr1   = 0x00;
constexpr address          tim_cr2   = 0x04;
constexpr address          tim_egr   = 0x14;
constexpr address          tim_arr   = 0x2C;
constexpr address          tim_dier  = 0x0C;
constexpr address          tim_sr    = 0x10;
constexpr address          ahb1enr   = 0x4002'1048;
constexpr address          dma1      = 0x4002'0000;
constexpr address          ccr3      = 0x30;
constexpr address          cndtr3    = 0x34;
constexpr address          cpar3     = 0x38;
constexpr address          cmar3     = 0x3C;
constexpr address          cselr     = 0xA8;
constexpr address          samples   = 0x2000'2000;
constexpr address          rcc_cr    = 0x4002'1000;
constexpr address          pllcfgr   = 0x4002'100C;
constexpr address          flash_acr = 0x4002'2000;
constexpr std::uint32_t    dac1en    = 1u << 29;
constexpr std::uint32_t    ten1      = 1u << 2;
constexpr std::uint32_t    software1 = 7u << 3;
constexpr std::uint32_t    ten2      = 1u << 18;
constexpr std::uint32_t    software2 = 7u << 19;
constexpr std::uint64_t    cycle     = 250'000;
constexpr std::uint32_t    en1       = 1u << 0;
constexpr std::uint32_t    cen1      = 1u << 14;
constexpr std::uint32_t    en2       = 1u << 16;
constexpr std::uint32_t    cen2      = 1u << 30;
constexpr std::uint32_t    cal_flag1 = 1u << 14;
constexpr std::uint32_t    cal_flag2 = 1u << 30;
constexpr std::uint32_t    dmaudr1   = 1u << 13;
constexpr std::uint32_t    dmaudr2   = 1u << 29;
constexpr std::uint32_t    trim      = 16;
constexpr std::uint32_t    dmaen1    = 1u << 12;
constexpr std::uint32_t    dmaudrie1 = 1u << 13;
constexpr std::uint32_t    dmaen2    = 1u << 28;
constexpr std::uint32_t    dmaudrie2 = 1u << 29;
constexpr std::uint32_t    tim6_dac  = 54;

constexpr std::array<address, 6> trigger_timers{0x4000'1000, 0x4001'3400, 0x4000'1400,
                                                0x4000'0C00, 0x4000'0000, 0x4000'0800};

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
    options.models             = stm32::factories(stm32::board{}, l476rc());
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(l476rc(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink([this](core::log_record const& record) {
      if(record.kind == core::log_kind::model_warning)
      {
        logged.push_back(record);
      }
    });
    write(apb1enr1, dac1en);
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

  void apb1_divider(std::uint32_t ppre1) const
  {
    write(rcc_cfgr, (read(rcc_cfgr) & ~(7u << 8)) | ppre1 << 8);
  }

  [[nodiscard]] auto pending(std::uint32_t channel) const -> std::vector<std::uint64_t>
  {
    auto const& units = f->target->description().peripherals;
    auto const  found =
        std::ranges::find(units, std::string_view{"DAC1"}, &popcorn::peripheral_desc::name);
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

  [[nodiscard]] auto events_of(std::string_view unit) const -> std::vector<std::uint64_t>
  {
    auto const& units = f->target->description().peripherals;
    auto const  found = std::ranges::find(units, unit, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    auto const                 owner = static_cast<std::uint32_t>(found - units.begin());
    std::vector<std::uint64_t> dues;
    for(auto const& event : f->target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner)
      {
        dues.push_back(event.due_ps);
      }
    }
    return dues;
  }

  void dma_channel3(std::vector<std::uint32_t> const& data) const
  {
    write(ahb1enr, read(ahb1enr) | 1u);
    for(std::size_t index = 0; index < data.size(); ++index)
    {
      write(samples + 4 * index, data[index]);
    }
    write(dma1 + cselr, 6u << 8);
    write(dma1 + cpar3, dac1 + dhr12r1);
    write(dma1 + cmar3, samples);
    write(dma1 + cndtr3, static_cast<std::uint32_t>(data.size()));
    write(dma1 + ccr3, 1u | 1u << 4 | 1u << 7 | 2u << 8 | 2u << 10);
  }

  [[nodiscard]] auto irq_pending(std::uint32_t irq) const -> bool
  {
    return (read(0xE000'E200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
  }

  void clear_pending(std::uint32_t irq) const
  {
    write(0xE000'E280u + 4 * (irq / 32), 1u << (irq % 32));
  }

  void pll_72mhz() const
  {
    write(flash_acr, (read(flash_acr) & ~7u) | 4u);
    write(pllcfgr, 1u | 36u << 8 | 1u << 24);
    write(rcc_cr, read(rcc_cr) | 1u << 24);
    for(int step = 0; step < 1000 and (read(rcc_cr) & 1u << 25) == 0; ++step)
    {
      idle_to(now_ps() + cycle);
    }
    REQUIRE((read(rcc_cr) & 1u << 25) != 0);
    write(rcc_cfgr, (read(rcc_cfgr) & ~3u) | 3u);
  }

  void sources() const
  {
    write(apb1enr1, read(apb1enr1) | 0x3Du);
    write(apb2enr, read(apb2enr) | 1u << 13);
    write(ahb2enr, read(ahb2enr) | 1u);
    write(gpioa, read(gpioa) & ~(3u << 18));
    exti9_pin().drive(9, false);
  }

  [[nodiscard]] auto exti9_pin() const -> stm32::gpio_port&
  {
    auto* const port = f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(port != nullptr);
    return *port;
  }

  void fire(std::uint32_t select) const
  {
    if(select < trigger_timers.size())
    {
      write(trigger_timers[select] + tim_egr, 1);
      return;
    }
    exti9_pin().drive(9, true);
  }

  [[nodiscard]] auto encoded() const -> std::vector<std::byte>
  {
    return core::encode_snapshot(f->target->snapshot().data());
  }
};

constexpr std::uint32_t div1  = 0;
constexpr std::uint32_t div2  = 4;
constexpr std::uint32_t div16 = 7;
}

TEST_CASE(
    "RM0351 19.4.4: with TEN1 0 a DAC_DHR12R1 write loads DAC_DOR1 one APB1 cycle later at the "
    "prescalers 1, 2 and 16, with the load pending then")
{
  for(auto const& [ppre1, cycles] : {
          std::pair{div1,  1u },
          std::pair{div2,  2u },
          std::pair{div16, 16u},
  })
  {
    CAPTURE(cycles);
    rig const r;
    r.apb1_divider(ppre1);
    auto const at = r.now_ps();
    r.write(dac1 + dhr12r1, 0xABC);
    CHECK(r.pending(0) == std::vector<std::uint64_t>{at + cycles * cycle});
    CHECK(r.read(dac1 + dor1) == 0);
    if(cycles > 1)
    {
      r.idle_to(at + (cycles - 1) * cycle);
      CHECK(r.read(dac1 + dor1) == 0);
    }
    r.idle_to(at + cycles * cycle);
    CHECK(r.read(dac1 + dor1) == 0xABC);
    CHECK(r.pending(0).empty());
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "RM0351 19.4.3: every holding register stores DHR1 and DHR2 by its shift, and each reads "
    "its view of them, inferred")
{
  struct leg
  {
    address       at;
    std::uint32_t value;
    std::uint32_t one;
    std::uint32_t two;
  };
  for(auto const& [at, value, one, two] : {
          leg{dhr12r1, 0xFABC,      0xABC, 0    },
          leg{dhr12l1, 0xABC0,      0xABC, 0    },
          leg{dhr8r1,  0xAB,        0xAB0, 0    },
          leg{dhr12r2, 0x123,       0,     0x123},
          leg{dhr12l2, 0x1230,      0,     0x123},
          leg{dhr8r2,  0x12,        0,     0x120},
          leg{dhr12rd, 0x0123'0ABC, 0xABC, 0x123},
          leg{dhr12ld, 0x1230'ABC0, 0xABC, 0x123},
          leg{dhr8rd,  0x12AB,      0xAB0, 0x120},
  })
  {
    CAPTURE(at);
    rig const r;
    r.write(dac1 + at, value);
    r.idle_to(r.now_ps() + cycle);
    CHECK(r.read(dac1 + dor1) == one);
    CHECK(r.read(dac1 + dor2) == two);
    CHECK(r.read(dac1 + dhr12r1) == one);
    CHECK(r.read(dac1 + dhr12l1) == one << 4);
    CHECK(r.read(dac1 + dhr8r1) == one >> 4);
    CHECK(r.read(dac1 + dhr12r2) == two);
    CHECK(r.read(dac1 + dhr12rd) == (one | two << 16));
    CHECK(r.read(dac1 + dhr12ld) == (one << 4 | two << 20));
    CHECK(r.read(dac1 + dhr8rd) == (one >> 4 | (two >> 4) << 8));
    r.write(dac1 + dor1, 0x555);
    CHECK(r.read(dac1 + dor1) == one);
  }
}

TEST_CASE(
    "RM0351 19.4.6: with TEN 1 and TSEL 111 a DHR write loads nothing and each SWTRIG write "
    "loads DOR one APB1 cycle later, SWTRIG reading 0, RM0351 19.7.2")
{
  rig const r;
  r.apb1_divider(div16);
  r.write(dac1 + cr, ten1 | software1 | ten2 | software2);
  r.write(dac1 + dhr12rd, 0x0123'0ABC);
  CHECK(r.pending(0).empty());
  CHECK(r.pending(1).empty());
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK(r.read(dac1 + dor1) == 0);
  auto const at = r.now_ps();
  r.write(dac1 + swtrgr, 1);
  CHECK(r.read(dac1 + swtrgr) == 0);
  CHECK(r.pending(0) == std::vector<std::uint64_t>{at + 16 * cycle});
  CHECK(r.pending(1).empty());
  r.idle_to(at + 15 * cycle);
  CHECK(r.read(dac1 + dor1) == 0);
  r.idle_to(at + 16 * cycle);
  CHECK(r.read(dac1 + dor1) == 0xABC);
  CHECK(r.read(dac1 + dor2) == 0);
  r.write(dac1 + swtrgr, 3);
  r.idle_to(r.now_ps() + 16 * cycle);
  CHECK(r.read(dac1 + dor2) == 0x123);
  r.write(dac1 + cr, ten1);
  r.write(dac1 + dhr12r1, 0x111);
  r.write(dac1 + swtrgr, 1);
  r.idle_to(r.now_ps() + 32 * cycle);
  CHECK(r.read(dac1 + dor1) == 0xABC);
}

TEST_CASE(
    "popcorn rule for DHR writes closer than one APB1 cycle, with no APB wait states: a load "
    "takes DHR as it is at the load, and a write while a load is pending keeps its own load")
{
  rig const r;
  r.apb1_divider(div16);
  auto const at = r.now_ps();
  r.write(dac1 + dhr12r1, 0x111);
  r.idle_to(at + 4 * cycle);
  r.write(dac1 + dhr12r1, 0x222);
  CHECK(r.pending(0) == std::vector<std::uint64_t>{at + 16 * cycle});
  r.idle_to(at + 16 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x222);
  CHECK(r.pending(0) == std::vector<std::uint64_t>{at + 20 * cycle});
  r.idle_to(at + 17 * cycle);
  r.write(dac1 + dhr12r1, 0x333);
  r.idle_to(at + 19 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x222);
  r.idle_to(at + 20 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x333);
}

TEST_CASE("DMAEN, DMAUDRIE, a hardware TSEL and WAVE with TEN log nothing, also under strict")
{
  rig const quiet{true};
  quiet.write(dac1 + cr, ten1 | ten2 | 6u << 19);
  quiet.write(dac1 + cr, 1u | ten1 | software1 | 1u << 16 | ten2 | software2);
  quiet.write(dac1 + cr, quiet.read(dac1 + cr) | dmaen1 | dmaudrie1 | dmaen2 | dmaudrie2);
  quiet.write(dac1 + cr, 1u | ten1 | software1 | 1u << 6 | 1u << 16 | ten2 | software2 | 2u << 22);
  quiet.write(dac1 + swtrgr, 3);
  CHECK(quiet.logged.empty());
}

TEST_CASE(
    "checklist 31: DAC1RST with a load pending drops it, a DHR write after the pulse loads one "
    "APB1 cycle after its own write")
{
  rig const r;
  r.apb1_divider(div16);
  auto const at = r.now_ps();
  r.write(dac1 + dhr12r1, 0x111);
  r.write(dac1 + cr, 1u);
  r.idle_to(at + 4 * cycle);
  r.write(apb1rstr1, dac1en);
  r.write(apb1rstr1, 0);
  CHECK(r.read(dac1 + cr) == 0);
  CHECK(r.read(dac1 + dhr12r1) == 0);
  r.idle_to(at + 8 * cycle);
  r.write(dac1 + dhr12r1, 0x222);
  r.idle_to(at + 16 * cycle);
  CHECK(r.read(dac1 + dor1) == 0);
  r.idle_to(at + 23 * cycle);
  CHECK(r.read(dac1 + dor1) == 0);
  r.idle_to(at + 24 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x222);
}

TEST_CASE(
    "a snapshot with a load pending loads DOR1 at the same time once restored and keeps DHR1 for "
    "a later software trigger")
{
  rig const r;
  r.apb1_divider(div16);
  auto const at = r.now_ps();
  r.write(dac1 + dhr12r1, 0xABC);
  auto const bytes = r.encoded();
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(fresh.pending(0) == std::vector<std::uint64_t>{at + 16 * cycle});
  fresh.idle_to(at + 15 * cycle);
  CHECK(fresh.read(dac1 + dor1) == 0);
  fresh.idle_to(at + 16 * cycle);
  CHECK(fresh.read(dac1 + dor1) == 0xABC);
  fresh.write(dac1 + cr, ten1 | software1);
  fresh.write(dac1 + dhr12r2, 0x123);
  fresh.write(dac1 + swtrgr, 1);
  fresh.idle_to(fresh.now_ps() + 16 * cycle);
  CHECK(fresh.read(dac1 + dor1) == 0xABC);
  CHECK(fresh.read(dac1 + dhr12rd) == (0xABCu | 0x123u << 16));
}

TEST_CASE("a machine reset restores DAC_CR and the holding registers and drops a pending load")
{
  rig const r;
  r.apb1_divider(div16);
  r.write(dac1 + cr, 1u);
  r.write(dac1 + dhr12r1, 0xABC);
  r.f->target->reset();
  r.f->set("pc", sym(firmware, "idle"));
  r.write(apb1enr1, dac1en);
  CHECK(r.read(dac1 + cr) == 0);
  CHECK(r.read(dac1 + dhr12r1) == 0);
  CHECK(r.pending(0).empty());
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK(r.read(dac1 + dor1) == 0);
  CHECK(r.read(dac1 + mcr) == 0);
}

TEST_CASE(
    "while EN is 1 a TSEL, MAMP or CEN write keeps the old value, RM0351 19.4.6 note, 19.4.9 and "
    "19.7.1, logged once per rule, a model_error when strict that leaves DAC_CR")
{
  struct leg
  {
    std::string_view what;
    std::uint32_t    before;
    std::uint32_t    written;
  };
  for(auto const& [what, before, written] : {
          leg{"TSEL1", en1, en1 | 3u << 3 },
          leg{"TSEL2", en2, en2 | 3u << 19},
          leg{"MAMP1", en1, en1 | 5u << 8 },
          leg{"MAMP2", en2, en2 | 5u << 24},
          leg{"CEN1",  en1, en1 | cen1    },
          leg{"CEN2",  en2, en2 | cen2    },
  })
  {
    CAPTURE(what);
    {
      rig const r;
      r.write(dac1 + cr, before);
      r.write(dac1 + cr, written);
      r.write(dac1 + cr, written);
      CHECK(r.read(dac1 + cr) == before);
      REQUIRE(r.logged.size() == 1);
      CHECK(r.logged[0].message.contains(what));
      r.write(dac1 + cr, 0);
      r.write(dac1 + cr, written & ~(en1 | en2));
      CHECK(r.read(dac1 + cr) == (written & ~(en1 | en2)));
      CHECK(r.logged.size() == 1);
    }
    rig const strict{true};
    strict.write(dac1 + cr, before);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(dac1 + cr, 4, written)),
        popcorn::model_error);
    CHECK(strict.read(dac1 + cr) == before);
  }
}

TEST_CASE(
    "EN set while CEN is 1, RM0351 19.4.11, and WAVE 11, reserved by RM0351 19.7.1, log once, a "
    "model_error when strict that leaves DAC_CR")
{
  struct leg
  {
    std::string_view what;
    std::uint32_t    before;
    std::uint32_t    written;
  };
  for(auto const& [what, before, written] : {
          leg{"EN1 set while CEN1", cen1, cen1 | en1},
          leg{"EN1 set while CEN1", 0,    cen1 | en1},
          leg{"EN2 set while CEN2", cen2, cen2 | en2},
          leg{"WAVE1 11",           0,    3u << 6   },
          leg{"WAVE2 11",           0,    3u << 22  },
  })
  {
    CAPTURE(what);
    CAPTURE(before);
    {
      rig const r;
      r.write(dac1 + cr, before);
      r.write(dac1 + cr, written);
      r.write(dac1 + cr, before);
      r.write(dac1 + cr, written);
      CHECK(r.read(dac1 + cr) == written);
      CHECK(
          std::ranges::count_if(
              r.logged, [&](auto const& record) { return record.message.contains(what); })
          == 1);
    }
    rig const strict{true};
    strict.write(dac1 + cr, before);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(dac1 + cr, 4, written)),
        popcorn::model_error);
    CHECK(strict.read(dac1 + cr) == before);
  }
}

TEST_CASE(
    "RM0351 19.7.16: a DAC_MCR write while EN or CEN is 1 keeps the old MODE, logged once, a "
    "model_error when strict that leaves DAC_MCR, MODE 1xx logs once as not modelled")
{
  for(auto const control : {en1, cen1})
  {
    CAPTURE(control);
    {
      rig const r;
      r.write(dac1 + mcr, 2);
      r.write(dac1 + cr, control);
      r.write(dac1 + mcr, 3);
      r.write(dac1 + mcr, 1);
      CHECK(r.read(dac1 + mcr) == 2);
      REQUIRE(r.logged.size() == 1);
      CHECK(r.logged[0].message.contains("MODE1 written while EN1 or CEN1"));
      r.write(dac1 + mcr, 2u | 3u << 16);
      CHECK(r.read(dac1 + mcr) == (2u | 3u << 16));
    }
    rig const strict{true};
    strict.write(dac1 + mcr, 2);
    strict.write(dac1 + cr, control);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(dac1 + mcr, 4, 3)), popcorn::model_error);
    CHECK(strict.read(dac1 + mcr) == 2);
  }
  {
    rig const r;
    r.write(dac1 + mcr, 4);
    r.write(dac1 + mcr, 5u << 16);
    CHECK(r.read(dac1 + mcr) == 5u << 16);
    REQUIRE(r.logged.size() == 1);
    CHECK(r.logged[0].message.contains("sample and hold"));
  }
  rig const strict{true};
  CHECK_THROWS_AS(
      static_cast<void>(strict.f->target->mem().write(dac1 + mcr, 4, 4)), popcorn::model_error);
  CHECK(strict.read(dac1 + mcr) == 0);
}

TEST_CASE(
    "RM0351 19.4.11: with CEN 1 CAL_FLAG reads 1 from OTRIM at the factory trim with the buffer "
    "on, 0 with MODE 010, and keeps its value with CEN 0, the trim and the MODE rule inferred")
{
  rig const r;
  CHECK(r.read(dac1 + ccr) == (trim | trim << 16));
  r.write(dac1 + cr, cen1 | cen2);
  for(std::uint32_t code = 0; code < 32; ++code)
  {
    CAPTURE(code);
    r.write(dac1 + ccr, code | (31 - code) << 16);
    auto const status = r.read(dac1 + sr);
    CHECK(((status & cal_flag1) != 0) == (code >= trim));
    CHECK(((status & cal_flag2) != 0) == (31 - code >= trim));
  }
  r.write(dac1 + ccr, 31);
  CHECK((r.read(dac1 + sr) & cal_flag1) != 0);
  r.write(dac1 + cr, 0);
  r.write(dac1 + ccr, 0);
  CHECK((r.read(dac1 + sr) & cal_flag1) != 0);
  r.write(dac1 + mcr, 2);
  r.write(dac1 + cr, cen1);
  r.write(dac1 + ccr, 31);
  CHECK((r.read(dac1 + sr) & cal_flag1) == 0);
  r.write(dac1 + cr, 0);
  r.write(dac1 + mcr, 1);
  r.write(dac1 + cr, cen1);
  CHECK((r.read(dac1 + sr) & cal_flag1) != 0);
  CHECK(r.logged.empty());
}

TEST_CASE(
    "DAC_CCR resets to the factory trim at DAC1RST and at a machine reset, DMAUDR clears on a "
    "write of 1 and CAL_FLAG is read only")
{
  rig const r;
  r.write(dac1 + ccr, 3u | 5u << 16);
  r.write(apb1rstr1, dac1en);
  r.write(apb1rstr1, 0);
  CHECK(r.read(dac1 + ccr) == (trim | trim << 16));
  r.write(dac1 + ccr, 3u);
  r.f->target->reset();
  r.f->set("pc", sym(firmware, "idle"));
  r.write(apb1enr1, dac1en);
  CHECK(r.read(dac1 + ccr) == (trim | trim << 16));
  r.f->target->peripheral("DAC1").poke(sr, dmaudr1 | dmaudr2 | cal_flag1);
  r.write(dac1 + sr, dmaudr1 | cal_flag2);
  CHECK(r.read(dac1 + sr) == (dmaudr2 | cal_flag1));
  r.write(dac1 + sr, dmaudr2);
  CHECK(r.read(dac1 + sr) == cal_flag1);
  r.write(dac1 + sr, 0);
  CHECK(r.read(dac1 + sr) == cal_flag1);
}

TEST_CASE(
    "RM0351 19.4.4 and 19.4.6: a TIM6 TRGO rising edge loads DOR1 three APB1 cycles later at the "
    "prescalers 1, 2 and 16, with the load pending then, none for the falling edge or TEN1 0")
{
  for(auto const& [ppre1, cycles] : {
          std::pair{div1,  1u },
          std::pair{div2,  2u },
          std::pair{div16, 16u},
  })
  {
    CAPTURE(cycles);
    rig const r;
    r.sources();
    r.apb1_divider(ppre1);
    r.fire(0);
    CHECK(r.pending(0).empty());
    r.write(dac1 + cr, ten1);
    r.write(dac1 + dhr12r1, 0xABC);
    CHECK(r.pending(0).empty());
    auto const at  = r.now_ps();
    auto const due = at + 3 * cycles * cycle;
    r.fire(0);
    CHECK(r.pending(0) == std::vector<std::uint64_t>{due});
    r.idle_to(due - cycle);
    CHECK(r.read(dac1 + dor1) == 0);
    r.idle_to(due);
    CHECK(r.read(dac1 + dor1) == 0xABC);
    CHECK(r.pending(0).empty());
    CHECK(r.pending(1).empty());
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "RM0351 Table 128: each TSEL below 111 loads on its own source only, TIM6, TIM8, TIM7, TIM5, "
    "TIM2, TIM4 TRGO and EXTI9, on both channels")
{
  for(std::uint32_t select = 0; select < 7; ++select)
  {
    CAPTURE(select);
    auto const other = (select + 1) % 7;
    rig const  r;
    r.sources();
    r.write(dac1 + cr, ten1 | select << 3 | ten2 | other << 19);
    r.write(dac1 + dhr12rd, 0x0123'0ABC);
    for(std::uint32_t source = 0; source < 7; ++source)
    {
      CAPTURE(source);
      r.fire(source);
      CHECK(r.pending(0).size() == (source == select ? 1u : 0u));
      CHECK(r.pending(1).size() == (source == other ? 1u : 0u));
      r.idle_to(r.now_ps() + 3 * cycle);
      CHECK(r.pending(0).empty());
      CHECK(r.pending(1).empty());
    }
    CHECK(r.read(dac1 + dor1) == 0xABC);
    CHECK(r.read(dac1 + dor2) == 0x123);
    r.exti9_pin().drive(9, false);
    CHECK(r.pending(0).empty());
    CHECK(r.pending(1).empty());
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "three trigger edges before a load give three loads in order, each taking DHR1 at its own "
    "load, and a fourth moves the third load to its own due")
{
  constexpr auto apb = 16 * cycle;
  struct leg
  {
    std::uint64_t                edges;
    std::array<std::uint64_t, 3> loads;
  };
  for(auto const& [edges, loads] : {
          leg{3, {6 * apb / 2, 8 * apb / 2, 10 * apb / 2}},
          leg{4, {6 * apb / 2, 7 * apb / 2, 9 * apb / 2} },
  })
  {
    CAPTURE(edges);
    auto const spacing = edges == 3 ? apb : apb / 2;
    rig const  r;
    r.sources();
    r.apb1_divider(div16);
    r.write(dac1 + cr, ten1);
    auto const at = r.now_ps();
    for(std::uint64_t edge = 0; edge < edges; ++edge)
    {
      r.idle_to(at + edge * spacing);
      r.fire(0);
    }
    CHECK(r.pending(0) == std::vector<std::uint64_t>{at + loads[0]});
    for(std::size_t index = 0; index < loads.size(); ++index)
    {
      CAPTURE(index);
      auto const value = static_cast<std::uint32_t>(0x111 * (index + 1));
      r.write(dac1 + dhr12r1, value);
      r.idle_to(at + loads[index] - cycle);
      CHECK(r.read(dac1 + dor1) == (index == 0 ? 0u : value - 0x111));
      r.idle_to(at + loads[index]);
      CHECK(r.read(dac1 + dor1) == value);
      auto const next = index + 1 < loads.size() ? std::vector<std::uint64_t>{at + loads[index + 1]}
                                                 : std::vector<std::uint64_t>{};
      CHECK(r.pending(0) == next);
    }
  }
}

TEST_CASE(
    "a TIM6 with ARR 1 at HCLK 72 MHz and PPRE1 2, three APB1 cycles 83333.3 ps, loads DOR1 at "
    "each of 16 updates, the due counted from the exact instant of the edge")
{
  rig const r;
  r.sources();
  r.apb1_divider(div2);
  r.pll_72mhz();
  auto const tim6 = trigger_timers[0];
  r.write(dac1 + cr, ten1);
  r.write(tim6 + tim_arr, 1);
  r.write(tim6 + tim_cr2, 2u << 4);
  r.write(tim6 + tim_cr1, 1);
  auto dues = std::vector<std::uint64_t>{};
  for(int step = 0; step < 16 and dues.empty(); ++step)
  {
    r.idle_to(r.now_ps() + 14'000);
    dues = r.pending(0);
  }
  REQUIRE(dues.size() == 1);
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    CAPTURE(index);
    auto const due = dues[0];
    r.write(dac1 + dhr12r1, 0x100 + index);
    r.idle_to(due);
    CHECK(r.read(dac1 + dor1) == 0x100 + index);
    dues = r.pending(0);
    REQUIRE(dues.size() == 1);
    CHECK(dues[0] - due >= 27'777);
    CHECK(dues[0] - due <= 27'778);
  }
  CHECK(r.logged.empty());
}

TEST_CASE(
    "EXTI9 edges one APB1 cycle apart from board drives scheduled ahead: the fourth edge, at the "
    "first load, comes after that load, so every edge loads DOR1")
{
  constexpr auto apb = 16 * cycle;
  rig const      r;
  r.sources();
  r.apb1_divider(div16);
  r.write(dac1 + cr, ten1 | 6u << 3);
  auto const at = r.now_ps() + apb;
  for(std::uint64_t edge = 0; edge < 4; ++edge)
  {
    r.exti9_pin().drive_at(9, true, vtime{at + edge * apb});
    r.exti9_pin().drive_at(9, false, vtime{at + edge * apb + apb / 2});
  }
  for(std::uint64_t index = 0; index < 4; ++index)
  {
    CAPTURE(index);
    auto const due   = at + (3 + index) * apb;
    auto const value = static_cast<std::uint32_t>(0x111 * (index + 1));
    r.idle_to(due - cycle);
    r.write(dac1 + dhr12r1, value);
    r.idle_to(due);
    CHECK(r.read(dac1 + dor1) == value);
  }
  CHECK(r.pending(0).empty());
}

TEST_CASE(
    "a TIM6 with ARR 1 at PPRE1 2 updates once per APB1 cycle, three edges inside the three-cycle "
    "window, and every update loads DOR1 three APB1 cycles later")
{
  rig const r;
  r.sources();
  r.apb1_divider(div2);
  constexpr auto apb  = 2 * cycle;
  auto const     tim6 = trigger_timers[0];
  r.write(dac1 + cr, ten1);
  r.write(tim6 + tim_arr, 1);
  r.write(tim6 + tim_cr2, 2u << 4);
  r.write(tim6 + tim_cr1, 1);
  auto first = std::vector<std::uint64_t>{};
  for(std::uint64_t step = 0; step < 16 and first.empty(); ++step)
  {
    r.idle_to(r.now_ps() + cycle);
    first = r.pending(0);
  }
  REQUIRE(first.size() == 1);
  auto const start = first[0];
  for(std::uint64_t index = 0; index < 8; ++index)
  {
    CAPTURE(index);
    auto const due   = start + index * apb;
    auto const value = static_cast<std::uint32_t>(0x100 + index);
    r.idle_to(due - cycle);
    r.write(dac1 + dhr12r1, value);
    r.idle_to(due);
    CHECK(r.read(dac1 + dor1) == value);
    CHECK(r.pending(0) == std::vector<std::uint64_t>{due + apb});
  }
}

TEST_CASE(
    "a counting TIM6 with MMS 010 loads DOR1 at each update after TEN1 selects it, none from "
    "before, and stops once TSEL1 selects TIM7")
{
  rig const r;
  r.sources();
  auto const tim6 = trigger_timers[0];
  r.write(tim6 + tim_arr, 99);
  r.write(tim6 + tim_cr2, 2u << 4);
  r.write(tim6 + tim_cr1, 1);
  r.idle_to(r.now_ps() + 250 * cycle);
  r.write(dac1 + cr, ten1);
  CHECK(r.pending(0).empty());
  r.write(dac1 + dhr12r1, 0x111);
  r.idle_to(r.now_ps() + 3 * cycle);
  CHECK(r.read(dac1 + dor1) == 0);
  r.idle_to(r.now_ps() + 100 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x111);
  r.write(dac1 + dhr12r1, 0x222);
  r.idle_to(r.now_ps() + 100 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x222);
  r.write(dac1 + cr, ten1 | 2u << 3);
  r.idle_to(r.now_ps() + 3 * cycle);
  r.write(dac1 + dhr12r1, 0x333);
  r.idle_to(r.now_ps() + 300 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x222);
}

TEST_CASE(
    "a snapshot with two trigger loads pending restores both at their own times, each taking "
    "DHR1 at its load")
{
  constexpr auto apb = 16 * cycle;
  rig const      r;
  r.sources();
  r.apb1_divider(div16);
  r.write(dac1 + cr, ten1);
  r.write(dac1 + dhr12r1, 0x111);
  auto const at = r.now_ps();
  r.fire(0);
  r.idle_to(at + apb);
  r.fire(0);
  auto const bytes = r.encoded();
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(fresh.pending(0) == std::vector<std::uint64_t>{at + 3 * apb});
  fresh.idle_to(at + 3 * apb);
  CHECK(fresh.read(dac1 + dor1) == 0x111);
  CHECK(fresh.pending(0) == std::vector<std::uint64_t>{at + 4 * apb});
  fresh.write(dac1 + dhr12r1, 0x222);
  fresh.idle_to(at + 4 * apb);
  CHECK(fresh.read(dac1 + dor1) == 0x222);
}

TEST_CASE(
    "RM0351 19.4.7: with DMAEN1 each TIM6 edge raises dac1_ch1_dma at its load, DMA1 channel 3 "
    "writes the next sample into DHR12R1 and its transfer drops the request, the instant inferred")
{
  rig const r;
  r.sources();
  r.dma_channel3({0x222, 0x333});
  r.write(dac1 + cr, ten1 | dmaen1);
  r.write(dac1 + dhr12r1, 0x111);
  for(auto const& [loaded, next, left] : {
          std::tuple{0x111u, 0x222u, 1u},
          std::tuple{0x222u, 0x333u, 0u},
  })
  {
    CAPTURE(loaded);
    auto const due = r.now_ps() + 3 * cycle;
    r.fire(0);
    CHECK(r.events_of("DMA1").empty());
    r.idle_to(due);
    CHECK(r.read(dac1 + dor1) == loaded);
    CHECK(r.events_of("DMA1").size() == 1);
    r.idle_to(due + 10 * cycle);
    CHECK(r.read(dac1 + dhr12r1) == next);
    CHECK(r.read(dma1 + cndtr3) == left);
    CHECK(r.events_of("DMA1").empty());
  }
  r.fire(0);
  r.idle_to(r.now_ps() + 10 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x333);
  CHECK((r.read(dac1 + sr) & dmaudr1) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE(
    "RM0351 19.4.7: a hardware load while dac1_ch1_dma is high sets DMAUDR1, raises no new request "
    "and still loads DOR1 from DHR1, decided at the load, inferred")
{
  rig const r;
  r.sources();
  r.write(dac1 + cr, ten1 | dmaen1);
  r.write(dac1 + dhr12r1, 0x111);
  r.fire(0);
  r.idle_to(r.now_ps() + 3 * cycle);
  CHECK((r.read(dac1 + sr) & dmaudr1) == 0);
  r.write(dac1 + dhr12r1, 0x222);
  auto const due = r.now_ps() + 3 * cycle;
  r.fire(0);
  r.idle_to(due - cycle);
  CHECK((r.read(dac1 + sr) & dmaudr1) == 0);
  r.idle_to(due);
  CHECK((r.read(dac1 + sr) & dmaudr1) != 0);
  CHECK(r.read(dac1 + dor1) == 0x222);
  r.dma_channel3({0x333, 0x444});
  r.idle_to(r.now_ps() + 10 * cycle);
  CHECK(r.read(dma1 + cndtr3) == 1);
  CHECK(r.read(dac1 + dhr12r1) == 0x333);
  CHECK(r.events_of("DMA1").empty());
}

TEST_CASE(
    "RM0351 19.4.7: SWTRIG and a TEN1 0 write raise no DMA request with DMAEN1, and clearing "
    "DMAEN1 drops a high request, inferred")
{
  rig const r;
  r.sources();
  r.dma_channel3({0x222, 0x333});
  r.write(dac1 + cr, dmaen1);
  r.write(dac1 + dhr12r1, 0x111);
  r.write(dac1 + cr, ten1 | software1 | dmaen1);
  r.write(dac1 + swtrgr, 1);
  r.write(dac1 + swtrgr, 1);
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK(r.read(dac1 + dor1) == 0x111);
  CHECK(r.read(dma1 + cndtr3) == 2);
  CHECK((r.read(dac1 + sr) & dmaudr1) == 0);
  r.write(dma1 + ccr3, 0);
  r.write(dac1 + cr, ten1 | dmaen1);
  r.fire(0);
  r.idle_to(r.now_ps() + 4 * cycle);
  r.write(dac1 + cr, ten1);
  r.dma_channel3({0x222, 0x333});
  r.idle_to(r.now_ps() + 10 * cycle);
  CHECK(r.read(dma1 + cndtr3) == 2);
  r.write(dac1 + cr, ten1 | dmaen1);
  r.fire(0);
  r.idle_to(r.now_ps() + 14 * cycle);
  CHECK(r.read(dma1 + cndtr3) == 1);
}

TEST_CASE(
    "DMAUDR1 with DMAUDRIE1 holds TIM6_DAC_IRQn 54 until DMAUDR1 is cleared, and the DAC and TIM6 "
    "each hold the shared line alone")
{
  rig const r;
  r.sources();
  auto const tim6     = trigger_timers[0];
  auto const underrun = [&r] {
    r.fire(0);
    r.idle_to(r.now_ps() + 4 * cycle);
  };
  r.write(dac1 + cr, ten1 | dmaen1 | dmaudrie1);
  underrun();
  CHECK_FALSE(r.irq_pending(tim6_dac));
  underrun();
  CHECK((r.read(dac1 + sr) & dmaudr1) != 0);
  CHECK(r.irq_pending(tim6_dac));
  r.clear_pending(tim6_dac);
  CHECK(r.irq_pending(tim6_dac));
  r.write(dac1 + sr, dmaudr1);
  r.clear_pending(tim6_dac);
  CHECK_FALSE(r.irq_pending(tim6_dac));
  r.write(dac1 + cr, ten1 | dmaen1);
  underrun();
  CHECK((r.read(dac1 + sr) & dmaudr1) != 0);
  CHECK_FALSE(r.irq_pending(tim6_dac));
  r.write(tim6 + tim_dier, 1);
  r.write(tim6 + tim_egr, 1);
  CHECK(r.irq_pending(tim6_dac));
  r.write(dac1 + cr, ten1 | dmaen1 | dmaudrie1);
  r.write(tim6 + tim_sr, 0);
  r.clear_pending(tim6_dac);
  CHECK(r.irq_pending(tim6_dac));
  r.write(tim6 + tim_egr, 1);
  r.write(dac1 + sr, dmaudr1);
  r.clear_pending(tim6_dac);
  CHECK(r.irq_pending(tim6_dac));
  r.write(tim6 + tim_sr, 0);
  r.clear_pending(tim6_dac);
  CHECK_FALSE(r.irq_pending(tim6_dac));
}

TEST_CASE(
    "a snapshot with dac1_ch1_dma high and a trigger load pending keeps both, so the load sets "
    "DMAUDR1 and DMA1 channel 3 serves the restored request, and DAC1RST drops a new one")
{
  rig const r;
  r.sources();
  r.write(dac1 + cr, ten1 | dmaen1 | dmaudrie1);
  r.fire(0);
  r.idle_to(r.now_ps() + 4 * cycle);
  r.fire(0);
  auto const due   = r.now_ps() + 3 * cycle;
  auto const bytes = r.encoded();
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(fresh.pending(0) == std::vector<std::uint64_t>{due});
  fresh.idle_to(due - cycle);
  CHECK((fresh.read(dac1 + sr) & dmaudr1) == 0);
  fresh.idle_to(due);
  CHECK((fresh.read(dac1 + sr) & dmaudr1) != 0);
  CHECK(fresh.irq_pending(tim6_dac));
  fresh.dma_channel3({0x111});
  fresh.idle_to(fresh.now_ps() + 10 * cycle);
  CHECK(fresh.read(dma1 + cndtr3) == 0);
  CHECK(fresh.read(dac1 + dhr12r1) == 0x111);
  fresh.write(dma1 + ccr3, 0);
  fresh.fire(0);
  fresh.idle_to(fresh.now_ps() + 4 * cycle);
  fresh.write(apb1rstr1, dac1en);
  fresh.write(apb1rstr1, 0);
  fresh.clear_pending(tim6_dac);
  CHECK_FALSE(fresh.irq_pending(tim6_dac));
  CHECK(fresh.read(dac1 + sr) == 0);
  fresh.dma_channel3({0x222});
  fresh.idle_to(fresh.now_ps() + 10 * cycle);
  CHECK(fresh.read(dma1 + cndtr3) == 1);
}

TEST_CASE(
    "LL firmware, RM0351 19.4.7: TIM6 TRGO and DMA1 channel 3 play a 4-sample buffer into DOR1, "
    "the fifth update holds the request and the sixth sets DMAUDR1")
{
  constexpr std::string_view    dac_ll = "l476_l4_dac_ll";
  stm32_fixture                 f{l476rc(), dac_ll};
  std::vector<core::log_record> warnings;
  auto const sink    = f.target->add_log_sink([&warnings](core::log_record const& record) {
    if(record.kind == core::log_kind::model_warning)
    {
      warnings.push_back(record);
    }
  });
  auto const stopped = f.run_to_stop({.max_instructions = icount{100'000}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0x100, 0x400, 0x800, 0xC00, 0xFFF, 0xFFF, 1, 0, 1});
  CHECK(warnings.empty());
}

namespace
{
constexpr std::uint32_t noise1    = 1u << 6;
constexpr std::uint32_t triangle1 = 2u << 6;
constexpr std::uint32_t noise2    = 1u << 22;
constexpr std::uint32_t triangle2 = 2u << 22;

auto software_loads(rig const& r, std::uint32_t count, std::uint32_t select = 1)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < count; ++index)
  {
    r.write(dac1 + swtrgr, select);
    r.idle_to(r.now_ps() + cycle);
    out.push_back(r.read(dac1 + (select == 2 ? dor2 : dor1)));
  }
  return out;
}
}

TEST_CASE(
    "RM0351 19.4.8, 19.4.12 and Figures 141 and 142: noise adds the LFSR from 0xAAA, then steps "
    "it, masked by MAMP, MAMP above 1011 as 1011")
{
  struct leg
  {
    std::uint32_t              amplitude;
    std::uint32_t              base;
    std::vector<std::uint32_t> loads;
  };
  for(auto const& [amplitude, base, loads] : {
          leg{11, 0,     {0xAAA, 0xD55, 0xEAA, 0xF55, 0xFAA, 0xFD5}},
          leg{15, 0,     {0xAAA, 0xD55, 0xEAA, 0xF55, 0xFAA, 0xFD5}},
          leg{7,  0x100, {0x1AA, 0x155, 0x1AA, 0x155, 0x1AA, 0x1D5}},
  })
  {
    CAPTURE(amplitude);
    rig const r;
    r.write(dac1 + cr, ten1 | software1 | noise1 | amplitude << 8);
    r.write(dac1 + dhr12r1, base);
    CHECK(software_loads(r, 6) == loads);
    CHECK(r.logged.empty());
  }
}

TEST_CASE("the LFSR returns to 0xAAA after 4095 loads and never reads 0 on the way")
{
  rig const r;
  r.write(dac1 + cr, ten1 | software1 | noise1 | 11u << 8);
  auto const loads = software_loads(r, 4096);
  CHECK(loads.front() == 0xAAA);
  CHECK(loads.back() == 0xAAA);
  CHECK(std::ranges::count(loads, 0xAAAu) == 2);
  CHECK(std::ranges::count(loads, 0u) == 0);
}

TEST_CASE(
    "RM0351 19.4.9, 19.4.12 and Figure 144: the triangle adds its counter, then steps it up to "
    "2^(MAMP+1)-1 and down to 0")
{
  struct leg
  {
    std::uint32_t              amplitude;
    std::vector<std::uint32_t> loads;
  };
  for(auto const& [amplitude, loads] : {
          leg{1, {0xABE, 0xABF, 0xAC0, 0xAC1, 0xAC0, 0xABF, 0xABE, 0xABF}},
          leg{0, {0xABE, 0xABF, 0xABE, 0xABF}                            },
  })
  {
    CAPTURE(amplitude);
    rig const r;
    r.write(dac1 + cr, ten1 | software1 | triangle1 | amplitude << 8);
    r.write(dac1 + dhr12r1, 0xABE);
    CHECK(software_loads(r, static_cast<std::uint32_t>(loads.size())) == loads);
  }
}

TEST_CASE("a triangle at MAMP above 1011 peaks at 0xFFF as at 1011, then turns down")
{
  rig const r;
  r.write(dac1 + cr, ten1 | software1 | triangle1 | 15u << 8);
  auto const loads = software_loads(r, 4097);
  CHECK(loads[4094] == 0xFFE);
  CHECK(loads[4095] == 0xFFF);
  CHECK(loads[4096] == 0xFFE);
}

TEST_CASE("DHR plus a wave saturates at 0xFFF, inferred")
{
  rig const triangle;
  triangle.write(dac1 + cr, ten1 | software1 | triangle1 | 1u << 8);
  triangle.write(dac1 + dhr12r1, 0xFFE);
  CHECK(
      software_loads(triangle, 8)
      == std::vector<std::uint32_t>{0xFFE, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFE, 0xFFF});
  rig const noise;
  noise.write(dac1 + cr, ten1 | software1 | noise1 | 11u << 8);
  noise.write(dac1 + dhr12r1, 0xFF0);
  CHECK(software_loads(noise, 1) == std::vector<std::uint32_t>{0xFFF});
}

TEST_CASE(
    "RM0351 19.4.8 and 19.4.9: WAVE written 00 resets the LFSR to 0xAAA and the triangle to 0, a "
    "DAC_CR write keeping WAVE resets nothing")
{
  rig const  r;
  auto const noise = ten1 | software1 | noise1 | 11u << 8;
  r.write(dac1 + cr, noise);
  CHECK(software_loads(r, 3) == std::vector<std::uint32_t>{0xAAA, 0xD55, 0xEAA});
  r.write(dac1 + cr, noise);
  CHECK(software_loads(r, 1) == std::vector<std::uint32_t>{0xF55});
  r.write(dac1 + cr, ten1 | software1);
  r.write(dac1 + cr, noise);
  CHECK(software_loads(r, 1) == std::vector<std::uint32_t>{0xAAA});
  auto const triangle = ten1 | software1 | triangle1 | 2u << 8;
  r.write(dac1 + cr, ten1 | software1);
  r.write(dac1 + cr, triangle);
  CHECK(software_loads(r, 3) == std::vector<std::uint32_t>{0, 1, 2});
  r.write(dac1 + cr, triangle);
  CHECK(software_loads(r, 1) == std::vector<std::uint32_t>{3});
  r.write(dac1 + cr, ten1 | software1);
  r.write(dac1 + cr, triangle);
  CHECK(software_loads(r, 1) == std::vector<std::uint32_t>{0});
}

TEST_CASE("each channel steps its own wave, and a TIM6 edge adds the wave as SWTRIG does")
{
  rig const r;
  r.write(dac1 + cr, ten1 | software1 | noise1 | 11u << 8 | ten2 | software2 | noise2 | 11u << 24);
  CHECK(software_loads(r, 2, 1) == std::vector<std::uint32_t>{0xAAA, 0xD55});
  CHECK(software_loads(r, 1, 2) == std::vector<std::uint32_t>{0xAAA});
  r.write(dac1 + swtrgr, 3);
  r.idle_to(r.now_ps() + cycle);
  CHECK(r.read(dac1 + dor1) == 0xEAA);
  CHECK(r.read(dac1 + dor2) == 0xD55);
  rig const timed;
  timed.sources();
  timed.write(dac1 + cr, ten1 | triangle1 | 1u << 8);
  timed.write(dac1 + dhr12r1, 0x100);
  for(auto const value : {0x100u, 0x101u, 0x102u})
  {
    timed.fire(0);
    timed.idle_to(timed.now_ps() + 4 * cycle);
    CHECK(timed.read(dac1 + dor1) == value);
  }
}

TEST_CASE(
    "RM0351 19.4.8 and 19.4.9 notes: a holding register write with TEN 0 and WAVE set loads DHR "
    "with no wave and logs once, a model_error when strict that leaves the holding registers")
{
  rig const r;
  r.write(dac1 + cr, noise1 | 11u << 8);
  r.write(dac1 + dhr12r1, 0x123);
  r.idle_to(r.now_ps() + cycle);
  CHECK(r.read(dac1 + dor1) == 0x123);
  r.write(dac1 + dhr12r1, 0x124);
  r.idle_to(r.now_ps() + cycle);
  CHECK(r.read(dac1 + dor1) == 0x124);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].message.contains("TEN1 0"));

  rig const strict{true};
  strict.write(dac1 + dhr12rd, 0x0222'0111);
  strict.idle_to(strict.now_ps() + cycle);
  strict.write(dac1 + cr, noise1 | 11u << 8);
  CHECK_THROWS_AS(
      static_cast<void>(strict.f->target->mem().write(dac1 + dhr12rd, 4, 0x0444'0333)),
      popcorn::model_error);
  CHECK(strict.read(dac1 + dhr12rd) == 0x0222'0111u);
  CHECK(strict.read(dac1 + dhr12r1) == 0x111);
  CHECK(strict.pending(0).empty());
  strict.idle_to(strict.now_ps() + cycle);
  CHECK(strict.read(dac1 + dor1) == 0x111);
  strict.write(dac1 + dhr12r2, 0x555);
  strict.idle_to(strict.now_ps() + cycle);
  CHECK(strict.read(dac1 + dor2) == 0x555);
}

TEST_CASE("a snapshot mid-wave restores the LFSR and the triangle counter and direction")
{
  rig const r;
  r.write(
      dac1 + cr, ten1 | software1 | noise1 | 11u << 8 | ten2 | software2 | triangle2 | 1u << 24);
  CHECK(software_loads(r, 3, 1) == std::vector<std::uint32_t>{0xAAA, 0xD55, 0xEAA});
  CHECK(software_loads(r, 4, 2) == std::vector<std::uint32_t>{0, 1, 2, 3});
  auto const bytes = r.encoded();
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(software_loads(fresh, 1, 1) == std::vector<std::uint32_t>{0xF55});
  CHECK(software_loads(fresh, 2, 2) == std::vector<std::uint32_t>{2, 1});
}
