#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
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

TEST_CASE(
    "WAVE, DMAEN, DMAUDRIE and a hardware TSEL with TEN each log once as not modelled, a "
    "model_error when strict that leaves DAC_CR as before the write")
{
  struct leg
  {
    std::string_view what;
    std::uint32_t    word;
  };
  for(auto const& [what, word] : {
          leg{"WAVE",     1u << 6        },
          leg{"WAVE",     1u << 22       },
          leg{"DMAEN",    1u << 12       },
          leg{"DMAUDRIE", 1u << 29       },
          leg{"TEN with", ten1           },
          leg{"TEN with", ten2 | 3u << 19},
  })
  {
    CAPTURE(what);
    CAPTURE(word);
    {
      rig const r;
      r.write(dac1 + cr, word);
      r.write(dac1 + cr, word);
      REQUIRE(r.logged.size() == 1);
      CHECK(r.logged[0].message.contains(what));
      CHECK(r.read(dac1 + cr) == word);
    }
    rig const strict{true};
    strict.write(dac1 + cr, 1u);
    CHECK_THROWS_AS(
        static_cast<void>(strict.f->target->mem().write(dac1 + cr, 4, word | 1u)),
        popcorn::model_error);
    CHECK(strict.read(dac1 + cr) == 1u);
  }
  rig const quiet{true};
  quiet.write(dac1 + cr, 1u | ten1 | software1 | 1u << 16 | ten2 | software2);
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
