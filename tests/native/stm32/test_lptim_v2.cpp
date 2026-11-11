#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

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
using popcorn::testing::no_reset;
using popcorn::testing::no_reset_t;
using popcorn::testing::rcc_reset_pulse;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::string_view firmware = "rcc_ll";
constexpr std::string_view lptim_ll = "lptim_ll";
constexpr address          isr      = 0x00;
constexpr address          icr      = 0x04;
constexpr address          dier     = 0x08;
constexpr address          cfgr     = 0x0C;
constexpr address          cr       = 0x10;
constexpr address          ccr1     = 0x14;
constexpr address          arr      = 0x18;
constexpr address          cnt      = 0x1C;
constexpr address          cfgr2    = 0x24;
constexpr address          rcr      = 0x28;
constexpr address          ccmr1    = 0x2C;
constexpr address          ccr2     = 0x34;
constexpr std::uint32_t    cc1if    = 1u << 0;
constexpr std::uint32_t    arrm     = 1u << 1;
constexpr std::uint32_t    cmp1ok   = 1u << 3;
constexpr std::uint32_t    arrok    = 1u << 4;
constexpr std::uint32_t    ue       = 1u << 7;
constexpr std::uint32_t    repok    = 1u << 8;
constexpr std::uint32_t    exttrig  = 1u << 2;
constexpr std::uint32_t    dierok   = 1u << 24;
constexpr std::uint32_t    enable   = 1u << 0;
constexpr std::uint32_t    sngstrt  = 1u << 1;
constexpr std::uint32_t    cntstrt  = 1u << 2;
constexpr std::uint32_t    countrst = 1u << 3;
constexpr std::uint32_t    rstare   = 1u << 4;
constexpr std::uint32_t    preload  = 1u << 22;
constexpr std::uint32_t    wave     = 1u << 20;
constexpr std::uint32_t    rising   = 1u << 17;
constexpr std::uint32_t    enc      = 1u << 24;
constexpr std::uint32_t    cksel    = 1u << 0;
constexpr std::uint32_t    cc1e     = 1u << 1;
constexpr std::uint32_t    cc1sel   = 1u << 0;
constexpr std::uint64_t    cycle    = 250'000;
constexpr std::uint64_t    hsi      = 62'500;

constexpr auto presc(std::uint32_t value) -> std::uint32_t
{
  return value << 9;
}

struct unit
{
  std::string_view name;
  std::string_view clock_reg;
  std::string_view clock_bit;
  std::string_view reset_reg;
  std::string_view reset_bit;
  std::string_view selector;
  std::uint32_t    irq;
};

constexpr auto units = std::to_array<unit>({
    {"LPTIM1", "RCC_APB3ENR",  "LPTIM1EN", "RCC_APB3RSTR",  "LPTIM1RST", "LPTIM1SEL",  67 },
    {"LPTIM2", "RCC_APB1ENR2", "LPTIM2EN", "RCC_APB1RSTR2", "LPTIM2RST", "LPTIM2SEL",  68 },
    {"LPTIM3", "RCC_APB3ENR",  "LPTIM3EN", "RCC_APB3RSTR",  "LPTIM3RST", "LPTIM34SEL", 98 },
    {"LPTIM4", "RCC_APB3ENR",  "LPTIM4EN", "RCC_APB3RSTR",  "LPTIM4RST", "LPTIM34SEL", 110},
});

auto field_of(
    core::machine&   target,
    std::string_view peripheral,
    std::string_view reg,
    std::string_view name) -> std::pair<address, popcorn::field_desc>
{
  auto const& desc  = target.peripheral(peripheral).description();
  auto const* found = popcorn::find_register(desc, reg);
  REQUIRE_MESSAGE(found != nullptr, reg);
  auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
  REQUIRE_MESSAGE(bits != found->fields.end(), name);
  return {desc.base + found->offset, *bits};
}

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  unit                           u;
  address                        base{};
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  explicit rig(
      unit                             which  = units[0],
      bool                             strict = false,
      popcorn::part_description const& desc   = u575())
      : u{which}
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(stm32::board{}, desc);
    options.strict_peripherals = strict;
    f    = std::make_unique<stm32_fixture>(desc, firmware, std::move(options));
    base = f->target->peripheral(u.name).description().base;
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    rcc(u.clock_reg, u.clock_bit, 1);
  }

  explicit rig(no_reset_t, unit which = units[0]) : u{which}
  {
    f    = std::make_unique<stm32_fixture>(no_reset, u575(), firmware);
    base = f->target->peripheral(u.name).description().base;
  }

  void rcc(std::string_view reg, std::string_view name, std::uint32_t value) const
  {
    auto const [at, bits] = field_of(*f->target, "RCC", reg, name);
    auto const mask       = ((std::uint32_t{1} << bits.width) - 1u) << bits.lsb;
    write_at(at, (read_at(at) & ~mask) | (value << bits.lsb));
  }

  [[nodiscard]] auto read_at(address at) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write_at(address at, std::uint32_t value) const
  {
    auto const done = f->target->mem().write(at, 4, value);
    REQUIRE(done.has_value());
  }

  [[nodiscard]] auto read(address offset) const -> std::uint32_t
  {
    return read_at(base + offset);
  }

  void write(address offset, std::uint32_t value) const
  {
    write_at(base + offset, value);
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

  [[nodiscard]] auto pending() const -> std::optional<std::uint64_t>
  {
    auto const& peripherals = f->target->description().peripherals;
    auto const  found = std::ranges::find(peripherals, u.name, &popcorn::peripheral_desc::name);
    REQUIRE(found != peripherals.end());
    auto const owner  = static_cast<std::uint32_t>(found - peripherals.begin());
    auto const events = f->target->snapshot().data().events.pending;
    auto const event  = std::ranges::find_if(
        events, [&](auto const& one) { return one.id.owner == owner and one.id.kind == 0; });
    return event == events.end() ? std::nullopt : std::optional{event->due_ps};
  }

  [[nodiscard]] auto warnings(std::string_view text) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.message.find(text) != std::string::npos;
    });
  }

  [[nodiscard]] auto irq_pending() const -> bool
  {
    return f->pending(u.irq);
  }

  [[nodiscard]] auto saved() const -> std::vector<std::byte>
  {
    auto out = std::vector<std::byte>{};
    f->target->model(u.name)->save(out);
    return out;
  }

  void enable_with(std::uint32_t irqs = 0, std::uint32_t config = 0) const
  {
    write(cfgr, config);
    write(cr, enable);
    write(dier, irqs);
    idle_to(now_ps() + 4 * cycle);
    write(icr, dierok);
  }

  void settle_values(std::uint32_t compare, std::uint32_t reload) const
  {
    write(arr, reload);
    idle_to(now_ps() + 4 * cycle);
    write(ccr1, compare);
    idle_to(now_ps() + 4 * cycle);
    write(icr, cmp1ok | arrok);
  }

  void settle_repetition(std::uint32_t value) const
  {
    write(rcr, value);
    idle_to(now_ps() + 8 * cycle);
    write(icr, repok);
  }

  [[nodiscard]] auto cpu_read(address offset) const -> std::uint32_t
  {
    auto& regs  = f->target->peripheral(u.name);
    auto  value = f->target->model(u.name)->on_read(
        regs, static_cast<std::uint32_t>(offset), 4,
        {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto started(
      std::uint32_t compare,
      std::uint32_t reload,
      std::uint32_t mode       = cntstrt,
      std::uint32_t irqs       = 0,
      std::uint32_t config     = 0,
      std::uint32_t repetition = 0) const -> std::uint64_t
  {
    enable_with(irqs, config);
    if(repetition != 0)
    {
      settle_repetition(repetition);
    }
    settle_values(compare, reload);
    auto const at = now_ps();
    write(cr, enable | mode);
    return at + 3 * cycle;
  }
};

struct firmware_run
{
  std::unique_ptr<stm32_fixture>      f;
  std::vector<std::uint64_t>          compares;
  std::vector<std::uint64_t>          reloads;
  std::vector<core::intercept_handle> hooks;

  explicit firmware_run(std::string_view entry)
  {
    f = std::make_unique<stm32_fixture>(u575(), lptim_ll, popcorn::testing::stm32_options());
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(lptim_ll, "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym(lptim_ll, entry));
    record("on_compare", compares);
    record("on_reload", reloads);
  }

  void record(std::string_view symbol, std::vector<std::uint64_t>& into)
  {
    hooks.push_back(
        f->target->add_intercept(sym(lptim_ll, symbol), [&into](core::intercept_call const& c) {
          into.push_back(c.target().virtual_clock().time().ps);
          return core::intercept_result::proceed();
        }));
  }

  [[nodiscard]] auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{4'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }
};

auto gaps(std::vector<std::uint64_t> const& at) -> std::vector<std::uint64_t>
{
  auto out = std::vector<std::uint64_t>{};
  for(std::size_t index = 1; index < at.size(); ++index)
  {
    out.push_back(at[index] - at[index - 1]);
  }
  return out;
}
}

TEST_CASE(
    "RM0456 58.4.7 and 58.7.2: each U5 LPTIM starts 3 kernel clocks after CNTSTRT, sets "
    "CC1IF at CCR1 and ARRM at ARR at that tick, the instant inferred, and raises its "
    "IRQ through DIER")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig        r{which};
    auto const start = r.started(6, 10, cntstrt, cc1if | arrm);
    CHECK(r.read(cr) == enable);
    CHECK(r.pending() == start + 6 * cycle);
    r.idle_to(start + 5 * cycle);
    CHECK(r.read(cnt) == 5);
    CHECK(r.read(isr) == 0);
    CHECK_FALSE(r.irq_pending());
    r.idle_to(start + 6 * cycle);
    CHECK(r.read(isr) == cc1if);
    CHECK(r.irq_pending());
    CHECK(r.pending() == start + 10 * cycle);
    r.idle_to(start + 10 * cycle);
    CHECK(r.read(isr) == (cc1if | arrm));
    CHECK(r.pending() == start + 11 * cycle);
    r.idle_to(start + 11 * cycle);
    CHECK(r.read(cnt) == 0);
    CHECK(r.read(isr) == (cc1if | arrm | ue));
    r.write(icr, cc1if | arrm | ue);
    CHECK(r.read(isr) == 0);
    CHECK(r.pending() == start + 17 * cycle);
  }
}

TEST_CASE("RM0456 58.4.8: SNGSTRT stops the counter at 0 at the wrap after ARR")
{
  rig        r;
  auto const start = r.started(6, 10, sngstrt);
  r.idle_to(start + 10 * cycle);
  r.write(icr, cc1if | arrm);
  CHECK(r.pending() == start + 11 * cycle);
  r.idle_to(start + 11 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK_FALSE(r.pending().has_value());
  r.idle_to(start + 40 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK(r.read(isr) == ue);
}

TEST_CASE(
    "RM0456 58.4.13: the counter starts 2 prescaled clocks after ENABLE when "
    "CNTSTRT follows it at once")
{
  rig r;
  r.write(cfgr, presc(2));
  auto const at = r.now_ps();
  r.write(cr, enable);
  r.write(cr, enable | cntstrt);
  r.write(arr, 10);
  r.write(ccr1, 3);
  CHECK(r.pending() == at + 3 * cycle);
  r.idle_to(at + 3 * cycle);
  CHECK(r.read(isr) == (cmp1ok | arrok));
  CHECK(r.pending() == at + 8 * cycle + 12 * cycle);
}

TEST_CASE(
    "RM0456 58.4.11, inferred: an ARR write sets ARROK and takes effect 3 kernel clocks later, "
    "the ticks before it compare with the old ARR")
{
  struct shot
  {
    std::uint64_t written;
    bool          old_reload;
    std::uint64_t first_reload;
    std::uint64_t next_reload;
  };
  for(auto const& [written, old_reload, first, next] : {
          shot{2,  false, 20, 41},
          shot{8,  true,  31, 52},
          shot{24, false, 42, 63},
          shot{30, true,  53, 74},
  })
  {
    CAPTURE(written);
    rig        r;
    auto const start = r.started(30, 10);
    r.idle_to(start + written * cycle);
    r.write(icr, arrm);
    r.write(arr, 20);
    CHECK(r.read(arr) == 20);
    CHECK(r.pending() == start + (old_reload ? written + 2 : written + 3) * cycle);
    r.idle_to(start + (written + 2) * cycle);
    CHECK((r.read(isr) & arrok) == 0);
    r.idle_to(start + (written + 3) * cycle);
    CHECK((r.read(isr) & (arrok | arrm)) == (arrok | (old_reload ? arrm : 0u)));
    r.write(icr, arrok | arrm);
    r.idle_to(start + first * cycle - cycle);
    CHECK((r.read(isr) & arrm) == 0);
    r.idle_to(start + first * cycle);
    CHECK((r.read(isr) & arrm) == arrm);
    r.write(icr, arrm);
    r.idle_to(start + next * cycle - cycle);
    CHECK((r.read(isr) & arrm) == 0);
    r.idle_to(start + next * cycle);
    CHECK((r.read(isr) & arrm) == arrm);
  }
}

TEST_CASE(
    "RM0456 58.4.11, inferred: a CCR1 write sets CMP1OK 3 kernel clocks later, a value the "
    "counter passed before then matches in the next period")
{
  for(auto const offset : {std::uint64_t{0}, std::uint64_t{22}})
  {
    for(auto const& [value, match] : {
            std::pair{8u, std::uint64_t{8} },
            std::pair{3u, std::uint64_t{14}},
            std::pair{5u, std::uint64_t{16}}
    })
    {
      CAPTURE(offset);
      CAPTURE(value);
      rig        r;
      auto const start = r.started(30, 10) + offset * cycle;
      r.idle_to(start + 2 * cycle);
      r.write(icr, arrm);
      r.write(ccr1, value);
      CHECK(r.pending() == start + 5 * cycle);
      r.idle_to(start + 5 * cycle);
      CHECK((r.read(isr) & ~ue) == cmp1ok);
      r.write(icr, cmp1ok);
      r.idle_to(start + match * cycle - cycle);
      CHECK((r.read(isr) & cc1if) == 0);
      r.idle_to(start + match * cycle);
      CHECK((r.read(isr) & cc1if) == cc1if);
    }
  }
}

TEST_CASE(
    "RM0456 58.7.8, inferred: a DIER write sets DIEROK 3 kernel clocks later and its enables "
    "take effect then")
{
  rig        r;
  auto const start = r.started(6, 10);
  r.idle_to(start + 4 * cycle);
  r.write(dier, cc1if);
  CHECK(r.read(dier) == cc1if);
  CHECK(r.pending() == start + 6 * cycle);
  r.idle_to(start + 6 * cycle);
  CHECK(r.read(isr) == cc1if);
  CHECK_FALSE(r.irq_pending());
  r.write(icr, cc1if);
  CHECK(r.pending() == start + 7 * cycle);
  r.idle_to(start + 7 * cycle);
  CHECK(r.read(isr) == dierok);
  r.write(icr, dierok);
  r.idle_to(start + 17 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm | ue));
  CHECK(r.irq_pending());
}

TEST_CASE(
    "ES0499 2.17.3: a DIER write clears the flag of each bit it changes when it takes effect, "
    "the instant inferred, and keeps the others")
{
  rig        r;
  auto const start = r.started(6, 10, cntstrt, cc1if | arrm);
  r.idle_to(start + 10 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm));
  r.write(dier, arrm);
  CHECK(r.pending() == start + 11 * cycle);
  r.idle_to(start + 11 * cycle);
  CHECK(r.pending() == start + 13 * cycle);
  r.idle_to(start + 13 * cycle - cycle);
  CHECK(r.read(isr) == (cc1if | arrm | ue));
  r.idle_to(start + 13 * cycle);
  CHECK(r.read(isr) == (arrm | ue | dierok));
  CHECK(r.irq_pending());
  r.write(icr, arrm | ue | dierok);
  r.write_at(0xE000'E280 + 4 * (r.u.irq / 32), 1u << (r.u.irq % 32));
  CHECK_FALSE(r.irq_pending());
  r.idle_to(start + 17 * cycle);
  CHECK(r.read(isr) == cc1if);
  CHECK_FALSE(r.irq_pending());
  r.idle_to(start + 21 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm));
  CHECK(r.irq_pending());
}

TEST_CASE(
    "RM0456 58.7.8, 58.7.12, 58.7.13 and 58.7.10: a write outside its ENABLE rule keeps the "
    "register, inferred, logs one warning and raises under strict_peripherals")
{
  for(auto const& [reg, value, on] : {
          std::tuple{dier, cc1if,    false},
          std::tuple{ccr1, 5u,       false},
          std::tuple{arr,  5u,       false},
          std::tuple{cfgr, presc(1), true },
  })
  {
    CAPTURE(reg);
    rig r;
    if(on)
    {
      r.write(cr, enable);
    }
    auto const before = r.read(reg);
    r.write(reg, value);
    r.write(reg, value);
    CHECK(r.read(reg) == before);
    CHECK(r.logged.size() == 1);
    rig strict{units[0], true};
    if(on)
    {
      strict.write(cr, enable);
    }
    CHECK_THROWS_AS(strict.write(reg, value), popcorn::model_error);
    CHECK(strict.read(reg) == before);
  }
}

TEST_CASE(
    "RM0456 58.4.11 and 58.7.8, inferred: a second CCR1, ARR or DIER write before its OK flag "
    "replaces the pending value and logs one warning")
{
  for(auto const& [reg, flag] : {
          std::pair{ccr1, cmp1ok},
          std::pair{arr,  arrok },
          std::pair{dier, dierok}
  })
  {
    CAPTURE(reg);
    rig r;
    static_cast<void>(r.started(30, 40));
    auto const at = r.now_ps();
    r.write(reg, 2);
    r.idle_to(at + cycle);
    r.write(reg, 9);
    CHECK(r.read(reg) == 9);
    CHECK(r.warnings("before") == 1);
    CHECK(r.pending() == at + 4 * cycle);
    r.idle_to(at + 4 * cycle - cycle);
    CHECK((r.read(isr) & flag) == 0);
    r.idle_to(at + 4 * cycle);
    CHECK((r.read(isr) & flag) == flag);
    rig strict{units[0], true};
    static_cast<void>(strict.started(30, 40));
    strict.write(reg, 2);
    CHECK_THROWS_AS(strict.write(reg, 9), popcorn::model_error);
    CHECK(strict.read(reg) == 2);
  }
}

TEST_CASE(
    "RM0456 58.4.8, inferred: SNGSTRT in continuous mode stops the counter at the first wrap "
    "3 kernel clocks after the write, CNTSTRT in one-shot mode keeps it counting")
{
  for(auto const& [written, next] : {
          std::pair{std::uint64_t{4},  std::uint64_t{7} },
          std::pair{std::uint64_t{9},  std::uint64_t{10}},
          std::pair{std::uint64_t{26}, std::uint64_t{29}},
  })
  {
    CAPTURE(written);
    rig        r;
    auto const start = r.started(30, 10);
    r.idle_to(start + written * cycle);
    r.write(cr, enable | sngstrt);
    CHECK(r.pending() == start + next * cycle);
    auto const stop = ((written + 3) / 11 + 1) * 11;
    r.idle_to(start + stop * cycle - cycle);
    CHECK(r.read(cnt) == 10);
    r.idle_to(start + (stop + 5) * cycle);
    CHECK(r.read(cnt) == 0);
  }
  rig        r;
  auto const start = r.started(30, 10, sngstrt);
  r.idle_to(start + 5 * cycle);
  r.write(cr, enable | cntstrt);
  CHECK(r.pending() == start + 8 * cycle);
  r.idle_to(start + 16 * cycle);
  CHECK(r.read(cnt) == 5);
}

TEST_CASE(
    "inferred: a kernel rate change keeps CNT and the kernel clocks spent in the current tick, "
    "the next match follows the new rate")
{
  for(auto const& [at, count, match] : {
          std::tuple{11 * cycle, 5u,  8u },
          std::tuple{55 * cycle, 5u,  8u },
          std::tuple{21 * cycle, 10u, 19u},
  })
  {
    CAPTURE(at);
    rig r;
    r.rcc("RCC_CR", "HSION", 1);
    for(auto round = 0; round < 100 and r.f->field("RCC", "RCC_CR", "HSIRDY") == 0; ++round)
    {
      r.idle_to(r.now_ps() + 100 * cycle);
    }
    REQUIRE(r.f->field("RCC", "RCC_CR", "HSIRDY") == 1);
    auto const start = r.started(8, 10, cntstrt, 0, presc(1));
    r.idle_to(start + at);
    r.write(icr, cc1if | arrm);
    r.rcc("RCC_CCIPR3", "LPTIM1SEL", 2);
    CHECK(r.read(cnt) == count);
    auto const due = start + at + hsi + (match - count - 1) * 2 * hsi;
    if(count == 10)
    {
      CHECK(r.pending() == start + at + hsi);
      r.idle_to(start + at + hsi);
    }
    CHECK(r.pending() == due);
    r.idle_to(due);
    CHECK((r.read(isr) & cc1if) == cc1if);
  }
}

TEST_CASE("inferred: a stopped kernel clock freezes CNT and the pending OK until it runs again")
{
  rig        r;
  auto const start = r.started(8, 10);
  r.idle_to(start + 3 * cycle);
  r.write(ccr1, 9);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
  CHECK_FALSE(r.pending().has_value());
  r.idle_to(start + 50 * cycle);
  CHECK(r.read(cnt) == 3);
  CHECK(r.read(isr) == 0);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  CHECK(r.pending() == start + 53 * cycle);
  r.idle_to(start + 53 * cycle);
  CHECK(r.read(isr) == cmp1ok);
  CHECK(r.read(cnt) == 6);
}

TEST_CASE(
    "RM0456 11.8: LPTIMxRST resets the registers and the state, a new run started before the "
    "stale due follows its own closed form")
{
  rig        r;
  auto const start = r.started(6, 10, cntstrt, cc1if);
  CHECK(r.pending() == start + 6 * cycle);
  r.idle_to(start + 2 * cycle);
  rcc_reset_pulse(*r.f, r.u.reset_reg, r.u.reset_bit);
  CHECK(r.read(cr) == 0);
  CHECK(r.read(arr) == 1);
  CHECK(r.read(dier) == 0);
  CHECK_FALSE(r.pending().has_value());
  auto const again = r.now_ps() + 3 * cycle;
  r.write(cr, enable);
  r.write(dier, cc1if);
  r.write(arr, 10);
  r.write(ccr1, 8);
  r.write(cr, enable | cntstrt);
  REQUIRE(again < start + 6 * cycle);
  CHECK(r.pending() == again);
  r.idle_to(start + 6 * cycle);
  CHECK(r.read(isr) == (dierok | arrok | cmp1ok));
  CHECK(r.pending() == again + 8 * cycle);
  r.idle_to(again + 8 * cycle);
  CHECK(r.read(isr) == (dierok | arrok | cmp1ok | cc1if));
  CHECK(r.irq_pending());
}

TEST_CASE(
    "a snapshot mid period restores into a machine with no reset, the state and "
    "the pending event equal")
{
  rig        r;
  auto const start = r.started(6, 10, cntstrt, cc1if | arrm);
  r.idle_to(start + 12 * cycle);
  r.write(icr, cc1if);
  r.write(ccr1, 4);
  auto const saved = r.f->target->snapshot();
  rig        other{no_reset};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  CHECK(other.pending() == r.pending());
  for(auto const* one : {&r, &other})
  {
    CHECK(one->read(cnt) == 1);
    one->idle_to(start + 15 * cycle);
    CHECK(one->read(isr) == (arrm | ue | cmp1ok));
    CHECK(one->irq_pending());
  }
}

TEST_CASE(
    "each function the model lacks logs one warning at its first use and raises under "
    "strict_peripherals")
{
  struct one
  {
    std::string_view text;
    unit             which;
    std::uint32_t    config;
    std::uint32_t    channels;
    bool             logs;
  };
  for(auto const& [text, which, config, channels, logs] : {
          one{"WAVE",         units[3], wave,  0,      true },
          one{"CKSEL",        units[1], cksel, 0,      true },
          one{"ENC 1",        units[0], enc,   0,      true },
          one{"ENC 1",        units[2], enc,   0,      false},
          one{"CC1E or CC2E", units[2], 0,     cc1e,   true },
          one{"CC1SEL",       units[1], 0,     cc1sel, true },
          one{"CC1SEL",       units[1], 0,     0,      false},
  })
  {
    CAPTURE(text);
    CAPTURE(which.name);
    rig r{which};
    if(channels != 0)
    {
      r.write(ccmr1, channels);
    }
    auto const start = r.started(6, 10, cntstrt, 0, config);
    CHECK(r.warnings(text) == (logs ? 1 : 0));
    r.idle_to(start + 6 * cycle);
    CHECK((r.read(isr) & cc1if) == cc1if);
    r.write(cr, 0);
    r.write(cr, enable);
    r.write(cr, enable | cntstrt);
    CHECK(r.warnings(text) == (logs ? 1 : 0));
    if(logs)
    {
      rig strict{which, true};
      if(channels != 0)
      {
        strict.write(ccmr1, channels);
      }
      CHECK_THROWS_AS(
          static_cast<void>(strict.started(6, 10, cntstrt, 0, config)), popcorn::model_error);
    }
  }
}

TEST_CASE(
    "the register writes the model lacks log one warning each and raise under "
    "strict_peripherals, COUNTRST reading 0 and TRIGEN leaving the counter stopped")
{
  for(auto const& [reg, value, text] : {
          std::tuple{ccr2, 3u,                std::string_view{"CCR2"}                  },
          std::tuple{dier, exttrig,           std::string_view{"DIER enable of EXTTRIG"}},
          std::tuple{cr,   enable | countrst, std::string_view{"COUNTRST"}              },
          std::tuple{cr,   enable | rstare,   std::string_view{"RSTARE"}                },
  })
  {
    CAPTURE(text);
    rig r;
    r.write(cr, enable);
    r.write(reg, value);
    r.write(reg, value);
    CHECK(r.warnings(text) == 1);
    CHECK((r.read(cr) & countrst) == 0);
    rig strict{units[0], true};
    strict.write(cr, enable);
    CHECK_THROWS_AS(strict.write(reg, value), popcorn::model_error);
    CHECK((strict.read(cr) & countrst) == 0);
  }
  rig r;
  r.enable_with(0, rising);
  r.settle_values(2, 4);
  r.write(cr, enable | cntstrt);
  CHECK(r.warnings("TRIGEN") == 1);
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK(r.read(isr) == 0);
}

TEST_CASE(
    "LPTIM4 serves the ISR, ICR and DIER of RM0456 58.7.1 and leaves CCMR1 and CCR2 unclaimed")
{
  rig         r{units[3]};
  auto const* model  = r.f->target->model("LPTIM4");
  auto const  claims = model->claims();
  CHECK(std::ranges::contains(claims, std::string{"ISR"}));
  CHECK_FALSE(std::ranges::contains(claims, std::string{"CCMR1"}));
  CHECK_FALSE(std::ranges::contains(claims, std::string{"CCR2"}));
  CHECK(std::ranges::contains(r.f->target->model("LPTIM3")->claims(), std::string{"CCMR1"}));
}

TEST_CASE("a description without CCMR1.CC1E on LPTIM1 is refused at construction")
{
  auto desc  = u575();
  auto found = std::ranges::find(
      desc.peripherals, std::string_view{"LPTIM1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  auto owner =
      std::ranges::find(found->registers, std::string_view{"CCMR1"}, &popcorn::register_desc::name);
  REQUIRE(owner != found->registers.end());
  REQUIRE(std::erase_if(owner->fields, [](auto const& one) { return one.name == "CC1E"; }) == 1);
  CHECK_THROWS_AS(rig(units[0], false, desc), popcorn::config_error);
}

TEST_CASE("the LL firmware counts LPTIM1 continuously on MSIK / 16: CC1IF and ARRM every period")
{
  firmware_run run{"lptim1_continuous"};
  auto const   log = run.finish();
  REQUIRE(log.size() == 2);
  CHECK(log[0] >= 10);
  CHECK(log[1] == 10);
  REQUIRE(run.reloads.size() == 10);
  constexpr auto period = 100 * 16 * cycle;
  CHECK(std::ranges::all_of(gaps(run.reloads), [](std::uint64_t gap) { return gap == period; }));
  REQUIRE(run.compares.size() >= 10);
  for(std::size_t index = 0; index < 10; ++index)
  {
    CHECK(run.reloads[index] - run.compares[index] < period / 2 + 100 * cycle);
    CHECK(run.reloads[index] - run.compares[index] > period / 2 - 100 * cycle);
  }
}

TEST_CASE("the LL firmware runs LPTIM4 once: one ARRM, the counter stopped at 0")
{
  firmware_run run{"lptim4_one_shot"};
  auto const   log = run.finish();
  CHECK(log == std::vector<std::uint32_t>{1, 1, 0});
}

TEST_CASE(
    "RM0456 58.4.16: with REP r the update event comes at every (r+1)-th overflow, UE raises "
    "the IRQ through UEIE, and ICR.UECF clears it")
{
  for(auto const repetition : {0u, 1u, 3u})
  {
    CAPTURE(repetition);
    rig        r;
    auto const start = r.started(30, 10, cntstrt, ue, 0, repetition);
    auto const span  = (repetition + 1) * 11 * cycle;
    r.idle_to(start + 10 * cycle);
    CHECK(r.read(isr) == arrm);
    CHECK(r.pending() == start + span);
    r.idle_to(start + span - cycle);
    CHECK(r.read(isr) == arrm);
    CHECK_FALSE(r.irq_pending());
    r.idle_to(start + span);
    CHECK(r.read(isr) == (arrm | ue));
    CHECK(r.irq_pending());
    r.idle_to(start + span + 3 * cycle);
    r.write(icr, ue);
    r.write_at(0xE000'E280 + 4 * (r.u.irq / 32), 1u << (r.u.irq % 32));
    CHECK_FALSE(r.irq_pending());
    CHECK(r.pending() == start + 2 * span);
    r.idle_to(start + 5 * span + cycle);
    r.write(icr, ue);
    CHECK(r.pending() == start + 6 * span);
  }
}

TEST_CASE("RM0456 58.4.8 and 58.4.16: SNGSTRT with REP 3 stops the counter at the update event")
{
  rig        r;
  auto const start = r.started(30, 10, sngstrt, 0, 0, 3);
  r.idle_to(start + 10 * cycle);
  CHECK(r.pending() == start + 44 * cycle);
  r.idle_to(start + 43 * cycle);
  CHECK(r.read(cnt) == 10);
  r.idle_to(start + 44 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK(r.read(isr) == (arrm | ue));
  CHECK_FALSE(r.pending().has_value());
  r.idle_to(start + 60 * cycle);
  CHECK(r.read(cnt) == 0);
}

TEST_CASE(
    "RM0456 58.4.16: REPOK sets 4 APB clocks plus 3 kernel clocks after an RCR write, the APB "
    "clock of the instance's bus")
{
  struct shot
  {
    unit          which;
    std::uint32_t ppre3;
    std::uint64_t latency;
  };
  for(auto const& [which, ppre3, latency] : {
          shot{units[0], 0, 7 * cycle             },
          shot{units[0], 7, 64 * cycle + 3 * cycle},
          shot{units[1], 7, 7 * cycle             },
  })
  {
    CAPTURE(which.name);
    CAPTURE(ppre3);
    rig r{which};
    r.rcc("RCC_CFGR3", "PPRE3", ppre3);
    r.write(cr, enable);
    auto const at = r.now_ps();
    r.write(rcr, 2);
    CHECK(r.pending() == at + latency);
    r.idle_to(at + latency - cycle);
    CHECK(r.read(isr) == 0);
    r.idle_to(at + latency);
    CHECK(r.read(isr) == repok);
  }
}

TEST_CASE(
    "RM0456 58.4.16: a new REP reaches the repetition counter at the first underflow at or "
    "after REPOK, PRELOAD 0 and 1, inferred at the underflow instant")
{
  struct shot
  {
    std::uint64_t written;
    std::uint64_t loaded;
  };
  for(auto const config : {0u, preload})
  {
    for(auto const& [written, loaded] : {
            shot{2,  11},
            shot{26, 33},
            shot{30, 44},
    })
    {
      CAPTURE(config);
      CAPTURE(written);
      rig        r;
      auto const start = r.started(30, 10, cntstrt, 0, config);
      r.idle_to(start + written * cycle);
      r.write(icr, ue);
      r.write(rcr, 2);
      auto const ok = start + (written + 7) * cycle;
      CHECK(r.pending() == std::min(ok, start + ((written + 10) / 11) * 11 * cycle));
      r.idle_to(ok);
      CHECK((r.read(isr) & repok) == repok);
      r.idle_to(start + loaded * cycle);
      CHECK((r.read(isr) & ue) == ue);
      r.write(icr, ue | repok);
      CHECK(r.pending() == start + (loaded + 33) * cycle);
      r.idle_to(start + (loaded + 33) * cycle);
      r.write(icr, ue);
      CHECK(r.pending() == start + (loaded + 66) * cycle);
    }
  }
}

TEST_CASE(
    "RM0456 58.4.11: with PRELOAD 1 and the counter started, ARR and CCR1 take effect at the "
    "first update event at or after their OK flag, inferred at the update instant")
{
  struct shot
  {
    std::uint32_t repetition;
    std::uint64_t written;
    std::uint64_t applied;
  };
  for(auto const& [repetition, written, applied] : {
          shot{0, 2,  11},
          shot{0, 24, 33},
          shot{0, 30, 33},
          shot{1, 2,  22},
          shot{1, 41, 44},
  })
  {
    CAPTURE(repetition);
    CAPTURE(written);
    rig        r;
    auto const start = r.started(30, 10, cntstrt, 0, preload, repetition);
    r.idle_to(start + written * cycle);
    r.write(arr, 20);
    r.write(ccr1, 3);
    r.idle_to(start + (written + 3) * cycle);
    CHECK((r.read(isr) & (arrok | cmp1ok)) == (arrok | cmp1ok));
    r.write(icr, arrok | cmp1ok | arrm | ue);
    if(applied > written + 3)
    {
      r.idle_to(start + applied * cycle - cycle);
      CHECK(r.read(cnt) == 10);
      CHECK((r.read(isr) & cc1if) == 0);
    }
    r.idle_to(start + applied * cycle);
    CHECK(r.read(cnt) == 0);
    r.write(icr, ue | cc1if | arrm);
    CHECK(r.pending() == start + (applied + 3) * cycle);
    r.idle_to(start + (applied + 3) * cycle);
    CHECK(r.read(isr) == cc1if);
    r.idle_to(start + (applied + 20) * cycle);
    CHECK(r.read(isr) == (cc1if | arrm));
    r.write(icr, cc1if | arrm);
    CHECK(r.pending() == start + (applied + (repetition == 0 ? 21 : 24)) * cycle);
  }
}

TEST_CASE("RM0456 58.7.2: an ARR write clears UE and lowers its IRQ")
{
  rig        r;
  auto const start = r.started(30, 10, cntstrt, ue);
  r.idle_to(start + 12 * cycle);
  CHECK(r.read(isr) == (arrm | ue));
  CHECK(r.irq_pending());
  r.write(arr, 10);
  CHECK((r.read(isr) & ue) == 0);
  r.write_at(0xE000'E280 + 4 * (r.u.irq / 32), 1u << (r.u.irq % 32));
  CHECK_FALSE(r.irq_pending());
}

TEST_CASE(
    "a snapshot mid repetition with a preloaded ARR restores into a machine with no reset, the "
    "updates equal")
{
  rig        r;
  auto const start = r.started(30, 10, cntstrt, ue, preload, 3);
  r.idle_to(start + 15 * cycle);
  r.write(arr, 20);
  r.idle_to(start + 19 * cycle);
  auto const saved = r.f->target->snapshot();
  rig        other{no_reset};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  CHECK(other.pending() == r.pending());
  for(auto const* one : {&r, &other})
  {
    CHECK(one->pending() == start + 44 * cycle);
    one->idle_to(start + 44 * cycle);
    CHECK((one->read(isr) & ue) == ue);
    one->write(icr, ue);
    CHECK(one->pending() == start + (44 + 4 * 21) * cycle);
  }
}

TEST_CASE(
    "inferred: ENABLE written to 0 drops the repetition count, the next start loads REP again")
{
  rig        r;
  auto const start = r.started(30, 10, cntstrt, 0, 0, 3);
  r.idle_to(start + 20 * cycle);
  r.write(ccr1, 29);
  r.idle_to(start + 25 * cycle);
  r.write(cr, 0);
  r.write(cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const again = r.now_ps() + 3 * cycle;
  r.write(cr, enable | cntstrt);
  r.idle_to(again + 10 * cycle);
  r.write(icr, ue);
  CHECK((r.read(isr) & arrm) == arrm);
  CHECK(r.pending() == again + 44 * cycle);
}

TEST_CASE(
    "RM0456 58.4.7, inferred for the same write: CNTSTRT with ENABLE 0, or in the write that sets "
    "ENABLE, is discarded and reads 0")
{
  rig r;
  r.write(cr, cntstrt);
  CHECK(r.read(cr) == 0);
  r.write(cr, enable | cntstrt);
  CHECK(r.read(cr) == enable);
  CHECK_FALSE(r.pending().has_value());
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK_FALSE(r.pending().has_value());
}

TEST_CASE(
    "inferred: ENABLE written to 0 keeps the flags and the pending CCR1, ARR and DIER writes, "
    "which set their OK flags and apply at their due")
{
  rig        r;
  auto const start = r.started(6, 10);
  r.idle_to(start + 7 * cycle);
  CHECK(r.read(isr) == cc1if);
  r.write(ccr1, 4);
  r.write(arr, 20);
  r.write(dier, cc1if);
  r.write(cr, 0);
  CHECK(r.read(isr) == cc1if);
  CHECK(r.pending() == start + 10 * cycle);
  r.idle_to(start + 10 * cycle);
  CHECK(r.read(isr) == (cmp1ok | arrok | dierok));
  r.write(icr, cmp1ok | arrok | dierok);
  r.write(cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const again = r.now_ps() + 3 * cycle;
  r.write(cr, enable | cntstrt);
  CHECK(r.pending() == again + 4 * cycle);
  r.idle_to(again + 4 * cycle);
  CHECK(r.read(isr) == cc1if);
  CHECK(r.irq_pending());
  r.idle_to(again + 20 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm));
}

TEST_CASE(
    "inferred: CCR1 equal to ARR sets CC1IF with ARRM, and a counter above a lowered ARR runs to "
    "0xFFFF before it wraps")
{
  rig        r;
  auto const start = r.started(10, 10);
  CHECK(r.pending() == start + 10 * cycle);
  r.idle_to(start + 10 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm));
  rig        lowered;
  auto const from = lowered.started(30, 20);
  lowered.idle_to(from + 15 * cycle);
  lowered.write(arr, 5);
  lowered.idle_to(from + 18 * cycle);
  lowered.write(icr, arrok);
  CHECK(lowered.pending() == from + 0x1'0000 * cycle);
  lowered.idle_to(from + 118 * cycle);
  CHECK(lowered.read(cnt) == 118);
}

TEST_CASE(
    "checklist 43: CFGR2 is storage, a firmware write logs one warning and raises under "
    "strict_peripherals before the store, a host write is refused")
{
  auto const store = [](rig const& r, std::uint32_t value,
                        popcorn::initiator by = popcorn::initiator::cpu) {
    auto& regs = r.f->target->peripheral(r.u.name);
    return r.f->target->model(r.u.name)->on_write(
        regs, static_cast<std::uint32_t>(cfgr2), 4, regs.peek(static_cast<std::uint32_t>(cfgr2)),
        value, {.pc = 0, .now = icount{0}, .by = by});
  };
  rig r;
  CHECK(r.f->target->model("LPTIM1")->storage_only(static_cast<std::uint32_t>(cfgr2)));
  CHECK_FALSE(r.f->target->model("LPTIM1")->storage_only(static_cast<std::uint32_t>(cfgr)));
  CHECK_THROWS_AS(r.write(cfgr2, 2), popcorn::model_error);
  CHECK(r.logged.empty());
  REQUIRE(store(r, 1).has_value());
  REQUIRE(store(r, 0x10).has_value());
  CHECK(r.f->target->peripheral("LPTIM1").peek(static_cast<std::uint32_t>(cfgr2)) == 0x10);
  CHECK(r.warnings("is storage") == 1);
  rig strict{units[0], true};
  REQUIRE(store(strict, 0).has_value());
  CHECK(strict.logged.empty());
  CHECK_THROWS_AS(static_cast<void>(store(strict, 1)), popcorn::model_error);
  CHECK(strict.f->target->peripheral("LPTIM1").peek(static_cast<std::uint32_t>(cfgr2)) == 0);
  REQUIRE(store(strict, 2, popcorn::initiator::debugger).has_value());
  CHECK(strict.f->target->peripheral("LPTIM1").peek(static_cast<std::uint32_t>(cfgr2)) == 2);
  rig   narrow{units[0], true};
  auto& regs = narrow.f->target->peripheral("LPTIM1");
  regs.poke(static_cast<std::uint32_t>(cfgr2), 1);
  CHECK_THROWS_AS(
      static_cast<void>(narrow.f->target->model("LPTIM1")->on_write(
          regs, static_cast<std::uint32_t>(cfgr2) + 2, 1, 0, 0,
          {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu})),
      popcorn::model_error);
}

TEST_CASE(
    "the HAL_LPTIM_Init register order runs under strict_peripherals: CFGR, then CFGR2 written "
    "0, as stm32u5xx_hal_lptim.c does for every instance")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig        r{which, true};
    auto const store = [&](address offset, std::uint32_t value) {
      auto& regs = r.f->target->peripheral(r.u.name);
      return r.f->target->model(r.u.name)->on_write(
          regs, static_cast<std::uint32_t>(offset), 4,
          regs.peek(static_cast<std::uint32_t>(offset)), value,
          {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    };
    REQUIRE(store(cfgr, presc(4)).has_value());
    REQUIRE(store(cfgr2, 0).has_value());
    CHECK(r.read(cfgr) == presc(4));
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "inferred: an ARR OK at the instant of a one-shot update event lets the update stop the "
    "counter with the old ARR")
{
  rig        r;
  auto const start = r.started(30, 10, sngstrt);
  r.idle_to(start + 8 * cycle);
  r.write(arr, 20);
  CHECK(r.pending() == start + 10 * cycle);
  r.idle_to(start + 11 * cycle);
  CHECK(r.read(isr) == (arrm | arrok | ue));
  CHECK(r.read(cnt) == 0);
  CHECK_FALSE(r.pending().has_value());
}
