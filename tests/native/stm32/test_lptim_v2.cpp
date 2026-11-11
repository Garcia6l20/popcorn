#include "../support/inert_model.hpp"
#include "dma.hpp"
#include "stm32_fixture.hpp"
#include "trigger_probe.hpp"

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
#include <cstddef>
#include <cstdint>
#include <format>
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
using popcorn::testing::no_reset;
using popcorn::testing::no_reset_t;
using popcorn::testing::rcc_reset_pulse;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::trigger_probe;
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
constexpr std::uint32_t    uede     = 1u << 23;
constexpr std::uint32_t    cc1de    = 1u << 16;
constexpr std::uint32_t    cc2de    = 1u << 25;
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
constexpr std::uint32_t    cc1p     = 1u << 2;
constexpr std::uint32_t    cc2e     = 1u << 17;
constexpr std::uint32_t    cc2sel   = 1u << 16;
constexpr std::uint32_t    cc2p     = 1u << 18;
constexpr std::uint32_t    cc2if    = 1u << 9;
constexpr std::uint32_t    cmp2ok   = 1u << 19;
constexpr std::uint32_t    wavpol   = 1u << 21;
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

class dma_recorder final : public popcorn::testing::inert_model, public stm32::dma_controller
{
public:
  explicit dma_recorder(bool knows) : knows_{knows}
  {}

  std::vector<std::pair<std::uint32_t, bool>> levels;

  void on_request(std::uint32_t request, bool level) override
  {
    levels.emplace_back(request, level);
  }

  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    if(not knows_ or not signal.starts_with("lptim") or not signal.ends_with("_dma"))
    {
      return std::nullopt;
    }
    auto const number = static_cast<std::uint32_t>(signal[5] - '0');
    auto const kind   = signal.substr(7, signal.size() - 11);
    return 10 * number + (kind == "ic1" ? 1u : kind == "ic2" ? 2u : 3u);
  }

private:
  bool knows_;
};

constexpr std::uint32_t ic1_request = 11;
constexpr std::uint32_t ic2_request = 12;
constexpr std::uint32_t ue_request  = 13;

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  unit                           u;
  address                        base{};
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;
  trigger_probe*                 probe{};
  dma_recorder*                  dma{};
  mutable std::uint64_t          enabled_at{};
  std::uint32_t                  cc2{};

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  explicit rig(
      unit                             which  = units[0],
      bool                             strict = false,
      popcorn::part_description const& desc   = u575(),
      std::string_view                 watch  = {},
      bool                             record = false)
      : u{which}, cc2{which.name == "LPTIM4" ? 0u : cc2if}
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(stm32::board{}, desc);
    options.strict_peripherals = strict;
    if(record)
    {
      options.models.insert_or_assign(
          "GPDMA1", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
            return std::make_shared<dma_recorder>(true);
          });
      options.models.insert_or_assign(
          "LPDMA1", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
            return std::make_shared<dma_recorder>(false);
          });
    }
    if(not watch.empty())
    {
      options.models.insert_or_assign(
          "DAC1", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
            return std::make_shared<trigger_probe>(ctx);
          });
    }
    f = std::make_unique<stm32_fixture>(desc, firmware, std::move(options));
    if(not watch.empty())
    {
      probe = f->model_of<trigger_probe>("DAC1");
      REQUIRE(probe != nullptr);
      probe->watched = watch;
      probe->listen(true);
    }
    dma  = record ? f->model_of<dma_recorder>("GPDMA1") : nullptr;
    base = f->target->peripheral(u.name).description().base;
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    rcc(u.clock_reg, u.clock_bit, 1);
  }

  explicit rig(no_reset_t, unit which = units[0])
      : u{which}, cc2{which.name == "LPTIM4" ? 0u : cc2if}
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

  void ack(std::uint32_t request) const
  {
    auto* const requester = f->model_of<stm32::dma_requester>(u.name);
    REQUIRE(requester != nullptr);
    REQUIRE(dma != nullptr);
    requester->acknowledged(*dma, request);
  }

  [[nodiscard]] auto irq_pending() const -> bool
  {
    return f->pending(u.irq);
  }

  void clear_irq() const
  {
    write_at(0xE000'E280u + 4 * (u.irq / 32), 1u << (u.irq % 32));
    REQUIRE_FALSE(irq_pending());
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
    enabled_at = now_ps();
    write(dier, irqs);
    idle_to(now_ps() + 4 * cycle);
    write(icr, dierok);
  }

  [[nodiscard]] auto out_level(std::string_view signal) const -> std::optional<bool>
  {
    auto* const source = dynamic_cast<stm32::trigger_source*>(f->target->model(u.name));
    REQUIRE(source != nullptr);
    return source->level(signal);
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

  void hsi_on() const
  {
    rcc("RCC_CR", "HSION", 1);
    for(auto round = 0; round < 100 and f->field("RCC", "RCC_CR", "HSIRDY") == 0; ++round)
    {
      idle_to(now_ps() + 100 * cycle);
    }
    REQUIRE(f->field("RCC", "RCC_CR", "HSIRDY") == 1);
  }

  [[nodiscard]] auto cpu_read(address offset, popcorn::initiator by = popcorn::initiator::cpu) const
      -> std::uint32_t
  {
    auto& regs  = f->target->peripheral(u.name);
    auto  value = f->target->model(u.name)->on_read(
        regs, static_cast<std::uint32_t>(offset), 4, {.pc = 0, .now = icount{0}, .by = by});
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
    return std::max(at + 3 * cycle, enabled_at + (cycle << ((config >> 9) & 7)) * 2);
  }
};

auto started_with_channel_2(
    rig const&    r,
    std::uint32_t compare,
    std::uint32_t compare2,
    std::uint32_t reload,
    std::uint32_t irqs   = 0,
    std::uint32_t config = 0) -> std::uint64_t
{
  r.enable_with(irqs, config);
  r.settle_values(compare, reload);
  r.write(ccr2, compare2);
  r.idle_to(r.now_ps() + 4 * cycle);
  r.write(icr, cmp2ok);
  auto const at = r.now_ps();
  r.write(cr, enable | cntstrt);
  return std::max(at + 3 * cycle, r.enabled_at + (cycle << ((config >> 9) & 7)) * 2);
}

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
    "RM0456 58.4.7 and 58.4.19: each U5 LPTIM starts 3 kernel clocks after CNTSTRT, sets "
    "CC1IF at CNT CCR1 + 1 and ARRM at CNT 0, one counter cycle after the match, and UE one "
    "cycle after that, inferred, and raises its IRQ through DIER")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig        r{which};
    auto const start = r.started(6, 10, cntstrt, cc1if | arrm);
    CHECK(r.read(cr) == enable);
    if(r.cc2 != 0)
    {
      CHECK(r.pending() == start + cycle);
      r.idle_to(start + cycle);
    }
    CHECK(r.pending() == start + 7 * cycle);
    r.idle_to(start + 6 * cycle);
    CHECK(r.read(cnt) == 6);
    CHECK(r.read(isr) == r.cc2);
    CHECK_FALSE(r.irq_pending());
    r.idle_to(start + 7 * cycle);
    CHECK(r.read(cnt) == 7);
    CHECK(r.read(isr) == (cc1if | r.cc2));
    CHECK(r.irq_pending());
    CHECK(r.pending() == start + 11 * cycle);
    r.idle_to(start + 10 * cycle);
    CHECK(r.read(isr) == (cc1if | r.cc2));
    r.idle_to(start + 11 * cycle);
    CHECK(r.read(cnt) == 0);
    CHECK(r.read(isr) == (cc1if | arrm | r.cc2));
    CHECK(r.pending() == start + 12 * cycle);
    r.idle_to(start + 12 * cycle);
    CHECK(r.read(cnt) == 1);
    CHECK(r.read(isr) == (cc1if | arrm | ue | r.cc2));
    r.write(icr, cc1if | arrm | ue);
    CHECK(r.read(isr) == r.cc2);
    CHECK(r.pending() == start + 18 * cycle);
  }
}

TEST_CASE("RM0456 58.4.8: SNGSTRT stops the counter at 0 at the wrap after ARR")
{
  rig        r;
  auto const start = r.started(6, 10, sngstrt);
  r.idle_to(start + 11 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm | r.cc2));
  r.write(icr, cc1if | arrm);
  CHECK(r.read(cnt) == 0);
  CHECK(r.pending() == start + 12 * cycle);
  r.idle_to(start + 12 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK_FALSE(r.pending().has_value());
  r.idle_to(start + 40 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK(r.read(isr) == (ue | r.cc2));
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
  CHECK(r.pending() == at + 8 * cycle + 4 * cycle);
  r.idle_to(at + 8 * cycle + 4 * cycle);
  CHECK(r.pending() == at + 8 * cycle + 16 * cycle);
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
          shot{2,  false, 21, 42},
          shot{8,  true,  32, 53},
          shot{24, false, 43, 64},
          shot{30, true,  54, 75},
  })
  {
    CAPTURE(written);
    rig        r;
    auto const start = r.started(30, 10);
    r.idle_to(start + written * cycle);
    r.write(icr, arrm);
    r.write(arr, 20);
    CHECK(r.read(arr) == 20);
    CHECK(r.pending() == start + (written + 3) * cycle);
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
    "counter passed matches in the next period, the value it holds then matches one cycle later")
{
  for(auto const offset : {std::uint64_t{0}, std::uint64_t{22}})
  {
    for(auto const& [value, match] : {
            std::pair{8u, std::uint64_t{9} },
            std::pair{3u, std::uint64_t{15}},
            std::pair{4u, std::uint64_t{16}},
            std::pair{5u, std::uint64_t{6} }
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
      CHECK((r.read(isr) & ~ue) == (cmp1ok | r.cc2));
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
  r.idle_to(start + 5 * cycle);
  r.write(dier, cc1if);
  CHECK(r.read(dier) == cc1if);
  CHECK(r.pending() == start + 7 * cycle);
  r.idle_to(start + 7 * cycle);
  CHECK(r.read(isr) == (cc1if | r.cc2));
  CHECK_FALSE(r.irq_pending());
  r.write(icr, cc1if);
  CHECK(r.pending() == start + 8 * cycle);
  r.idle_to(start + 8 * cycle);
  CHECK(r.read(isr) == (dierok | r.cc2));
  r.write(icr, dierok);
  r.idle_to(start + 18 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm | ue | r.cc2));
  CHECK(r.irq_pending());
}

TEST_CASE(
    "ES0499 2.17.3: a DIER write clears the flag of each bit it changes when it takes effect, "
    "the instant inferred, and keeps the others")
{
  rig        r;
  auto const start = r.started(6, 10, cntstrt, cc1if | arrm);
  r.idle_to(start + 11 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm | r.cc2));
  r.write(dier, arrm);
  CHECK(r.pending() == start + 12 * cycle);
  r.idle_to(start + 12 * cycle);
  CHECK(r.pending() == start + 14 * cycle);
  r.idle_to(start + 14 * cycle - cycle);
  CHECK(r.read(isr) == (cc1if | arrm | ue | r.cc2));
  r.idle_to(start + 14 * cycle);
  CHECK(r.read(isr) == (arrm | ue | dierok | r.cc2));
  CHECK(r.irq_pending());
  r.write(icr, arrm | ue | dierok);
  r.write_at(0xE000'E280 + 4 * (r.u.irq / 32), 1u << (r.u.irq % 32));
  CHECK_FALSE(r.irq_pending());
  r.idle_to(start + 18 * cycle);
  CHECK(r.read(isr) == (cc1if | r.cc2));
  CHECK_FALSE(r.irq_pending());
  r.idle_to(start + 22 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm | r.cc2));
  CHECK(r.irq_pending());
}

TEST_CASE(
    "RM0456 58.7.8, 58.7.12, 58.7.13, 58.7.16 and 58.7.10: a write outside its ENABLE rule "
    "keeps the register, inferred, logs one warning and raises under strict_peripherals")
{
  for(auto const& [reg, value, on] : {
          std::tuple{dier, cc1if,    false},
          std::tuple{ccr1, 5u,       false},
          std::tuple{arr,  5u,       false},
          std::tuple{rcr,  3u,       false},
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
    "RM0456 58.4.11 and 58.7.8, inferred: a second CCR1, ARR, RCR or DIER write before its OK flag "
    "replaces the pending value and logs one warning")
{
  for(auto const& [reg, flag, latency] : {
          std::tuple{ccr1, cmp1ok, 3 * cycle},
          std::tuple{arr,  arrok,  3 * cycle},
          std::tuple{dier, dierok, 3 * cycle},
          std::tuple{rcr,  repok,  7 * cycle},
  })
  {
    CAPTURE(reg);
    rig        r;
    auto const a = r.started(30, 40);
    r.idle_to(a + cycle);
    auto const at = r.now_ps();
    r.write(reg, 2);
    r.idle_to(at + cycle);
    r.write(reg, 9);
    CHECK(r.read(reg) == 9);
    CHECK(r.warnings("before") == 1);
    CHECK(r.pending() == at + cycle + latency);
    r.idle_to(at + latency);
    CHECK((r.read(isr) & flag) == 0);
    r.idle_to(at + cycle + latency);
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
          std::pair{std::uint64_t{9},  std::uint64_t{11}},
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
    auto const due = start + at + hsi + (match - count) * 2 * hsi;
    if(count == 10)
    {
      CHECK(r.pending() == start + at + hsi);
      r.idle_to(start + at + hsi + cycle);
      CHECK((r.read(isr) & ue) == ue);
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
  CHECK(r.read(isr) == r.cc2);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  CHECK(r.pending() == start + 53 * cycle);
  r.idle_to(start + 53 * cycle);
  CHECK(r.read(isr) == (cmp1ok | r.cc2));
  CHECK(r.read(cnt) == 6);
}

TEST_CASE(
    "RM0456 11.8: LPTIMxRST resets the registers and the state, a new run started before the "
    "stale due follows its own closed form")
{
  rig        r;
  auto const start = r.started(6, 10, cntstrt, cc1if);
  CHECK(r.pending() == start + cycle);
  r.idle_to(start + cycle);
  CHECK(r.pending() == start + 7 * cycle);
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
  REQUIRE(again < start + 7 * cycle);
  CHECK(r.pending() == again);
  r.idle_to(start + 6 * cycle);
  CHECK(r.read(isr) == (dierok | arrok | cmp1ok | r.cc2));
  CHECK(r.pending() == again + 9 * cycle);
  r.idle_to(again + 9 * cycle);
  CHECK(r.read(isr) == (dierok | arrok | cmp1ok | cc1if | r.cc2));
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
    CHECK(one->read(isr) == (arrm | ue | cmp1ok | one->cc2));
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
          one{"CC1SEL or CC2SEL 1 with", units[1], 1u << 23, cc2sel, true },
          one{"CC1SEL or CC2SEL 1 with", units[1], 0,        cc2sel, false},
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
    r.idle_to(start + 7 * cycle);
    CHECK((r.read(isr) & cc1if) == (config == 0 ? cc1if : 0u));
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
    "RM0456 58.7.7: the DIER bits LPTIM4 reserves, UEDE included, are not stored, and TRIGSEL 1 "
    "logs one warning, raises under strict_peripherals and leaves the counter stopped")
{
  {
    rig four{units[3]};
    four.write(cr, enable);
    four.write(dier, 1u << 23);
    CHECK(four.read(dier) == 0);
  }
  rig r;
  r.enable_with(0, rising | 1u << 13);
  r.settle_values(2, 4);
  r.write(cr, enable | cntstrt);
  CHECK(r.warnings("TRIGSEL 1, rtc_alra_trg") == 1);
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK(r.read(isr) == 0);
}

TEST_CASE(
    "RM0456 58.4.19: the PWM output rises at CNT CCR1 + 1 and falls at CNT 0, one counter "
    "cycle after the matches")
{
  struct shot
  {
    unit             which;
    std::string_view signal;
    std::uint32_t    config;
    std::uint32_t    scale;
  };
  for(auto const& [which, signal, config, scale] : {
          shot{units[0], "lptim1_ch1", 0,        1},
          shot{units[0], "lptim1_ch1", presc(3), 8},
          shot{units[1], "lptim2_ch1", 0,        1},
          shot{units[2], "lptim3_ch1", 0,        1},
  })
  {
    CAPTURE(which.name);
    CAPTURE(config);
    rig r{which, false, u575(), signal};
    r.write(ccmr1, cc1e);
    auto const a = r.started(2, 5, cntstrt, 0, config);
    auto const k = std::uint64_t{scale} * cycle;
    r.idle_to(a + 12 * k + cycle);
    CHECK(r.probe->pulses == std::vector{a + 3 * k, a + 9 * k});
    CHECK(r.probe->falls == std::vector{a + 6 * k, a + 12 * k});
    CHECK(r.pending() == a + 15 * k);
    CHECK(r.out_level(signal) == false);
    r.idle_to(a + 15 * k);
    CHECK(r.out_level(signal) == true);
  }
}

TEST_CASE(
    "RM0456 58.4.19 Figures 747 and 748: CC1IF and OC1REF come at CNT CCR1 + 1 and the fall at "
    "CNT 0, CCR1 0 at CNT 1, UE one counter cycle after, counted in prescaled ticks")
{
  for(auto const config : {0u, presc(3)})
  {
    for(auto const compare : {3u, 6u, 0u})
    {
      CAPTURE(config);
      CAPTURE(compare);
      rig r{units[0], false, u575(), "lptim1_ch1"};
      r.write(ccmr1, cc1e);
      auto const a = r.started(compare, 8, cntstrt, 0, config);
      auto const k = (config == 0 ? 1u : 8u) * cycle;
      r.idle_to(a + compare * k);
      CHECK(r.read(cnt) == compare);
      CHECK((r.read(isr) & cc1if) == 0);
      CHECK(r.out_level("lptim1_ch1") == false);
      CHECK(r.probe->pulses.empty());
      r.idle_to(a + (compare + 1) * k - cycle);
      CHECK((r.read(isr) & cc1if) == 0);
      r.idle_to(a + (compare + 1) * k);
      CHECK(r.read(cnt) == compare + 1);
      CHECK((r.read(isr) & cc1if) == cc1if);
      CHECK(r.out_level("lptim1_ch1") == true);
      CHECK(r.probe->pulses == std::vector{a + (compare + 1) * k});
      r.idle_to(a + 8 * k);
      CHECK(r.out_level("lptim1_ch1") == true);
      CHECK((r.read(isr) & arrm) == 0);
      r.idle_to(a + 9 * k);
      CHECK(r.read(cnt) == 0);
      CHECK((r.read(isr) & arrm) == arrm);
      CHECK((r.read(isr) & ue) == 0);
      CHECK(r.out_level("lptim1_ch1") == false);
      CHECK(r.probe->falls == std::vector{a + 9 * k});
      r.idle_to(a + 10 * k - cycle);
      CHECK((r.read(isr) & ue) == 0);
      r.idle_to(a + 10 * k);
      CHECK((r.read(isr) & ue) == ue);
    }
  }
}

TEST_CASE(
    "inferred: a CC1IF or ARRM in flight when CCR1 or ARR takes effect inside a prescaled tick "
    "sets at that instant, the old value having matched")
{
  for(auto const reload : {false, true})
  {
    CAPTURE(reload);
    rig        r;
    auto const a = r.started(2, 6, cntstrt, 0, presc(3));
    auto const k = 8 * cycle;
    auto const w = a + (reload ? 6 : 2) * k - cycle;
    auto const m = reload ? arrm : cc1if;
    r.idle_to(w);
    CHECK((r.read(isr) & m) == 0);
    r.write(reload ? arr : ccr1, reload ? 9 : 4);
    r.idle_to(w + 3 * cycle - cycle);
    CHECK((r.read(isr) & m) == 0);
    r.idle_to(w + 3 * cycle);
    CHECK((r.read(isr) & m) == m);
    CHECK((r.read(isr) & (reload ? arrok : cmp1ok)) != 0);
  }
}

TEST_CASE(
    "inferred: a CCR1 value that takes effect at the match instant, or after it, keeps the CC1IF "
    "of the old value, one before the match it does not, at PRESC 0 and 3")
{
  for(auto const config : {0u, presc(3)})
  {
    for(auto const delta : {-1, 0, 1})
    {
      CAPTURE(config);
      CAPTURE(delta);
      rig        r;
      auto const a  = r.started(2, 20, cntstrt, 0, config);
      auto const k  = (config == 0 ? 1u : 8u) * cycle;
      auto const ok = delta < 0 ? a + 2 * k - cycle : a + 2 * k + delta * cycle;
      r.idle_to(ok - 3 * cycle);
      r.write(ccr1, 10);
      r.idle_to(ok);
      CHECK((r.read(isr) & cmp1ok) == cmp1ok);
      CHECK((r.read(isr) & cc1if) == (delta < 0 ? 0u : cc1if));
      r.idle_to(a + 11 * k - cycle);
      CHECK((r.read(isr) & cc1if) == (delta < 0 ? 0u : cc1if));
      if(delta < 0)
      {
        r.idle_to(a + 11 * k);
        CHECK((r.read(isr) & cc1if) == cc1if);
      }
    }
  }
}

TEST_CASE(
    "inferred: a CC1P write at the CMP1OK or ARROK instant that flips the level, and at the "
    "update instant that applies a preloaded CCR1, applies after that term")
{
  struct shot
  {
    std::uint32_t config;
    bool          reload;
    bool          update;
  };
  for(auto const& [config, reload, update] : {
          shot{0,       false, false},
          shot{0,       true,  false},
          shot{preload, false, true },
  })
  {
    CAPTURE(config);
    CAPTURE(reload);
    rig r{units[0], false, u575(), "lptim1_ch1"};
    r.write(ccmr1, cc1e);
    auto const a = r.started(2, 10, cntstrt, 0, config);
    r.idle_to(a + 4 * cycle);
    CHECK(r.out_level("lptim1_ch1") == true);
    auto const x = r.now_ps();
    r.write(reload ? arr : ccr1, reload ? 4 : 8);
    auto const at = update ? a + 11 * cycle : x + 3 * cycle;
    r.idle_to(at);
    r.write(ccmr1, cc1e | cc1p);
    CHECK(r.probe->falls == std::vector{at});
    CHECK(r.probe->pulses == std::vector{a + 3 * cycle, at});
    CHECK(r.out_level("lptim1_ch1") == true);
  }
}

TEST_CASE(
    "RM0456 58.4.10: CC1P inverts the output at the CCMR1 write, also with ENABLE 0 and while "
    "the counter runs, CC1E 0 or CC1SEL 1 leave it inactive")
{
  rig        r{units[0], false, u575(), "lptim1_ch1"};
  auto const x = r.now_ps();
  r.write(ccmr1, cc1e | cc1p);
  CHECK(r.probe->pulses == std::vector{x});
  CHECK(r.out_level("lptim1_ch1") == true);
  r.idle_to(x + 2 * cycle);
  r.write(ccmr1, cc1p);
  CHECK(r.probe->falls == std::vector{x + 2 * cycle});
  CHECK(r.out_level("lptim1_ch1") == false);
  r.write(ccmr1, cc1e | cc1p | cc1sel);
  CHECK(r.out_level("lptim1_ch1") == false);
  r.write(ccmr1, cc1e | cc1p);
  r.probe->pulses.clear();
  r.probe->falls.clear();
  auto const a = r.started(2, 5);
  r.idle_to(a + 7 * cycle);
  CHECK(r.probe->falls == std::vector{a + 3 * cycle});
  CHECK(r.probe->pulses == std::vector{a + 6 * cycle});
  auto const y = r.now_ps();
  r.write(ccmr1, cc1e);
  CHECK(r.probe->falls == std::vector{a + 3 * cycle, y});
  CHECK(r.out_level("lptim1_ch1") == false);
}

TEST_CASE(
    "RM0456 58.4.10: a one-pulse run gives one pulse, set-once keeps the level and discards a "
    "restart, ENABLE 0 drops it, the software restart inferred")
{
  for(auto const config : {0u, wave})
  {
    CAPTURE(config);
    rig r{units[0], false, u575(), "lptim1_ch1"};
    r.write(ccmr1, cc1e);
    auto const a = r.started(2, 5, sngstrt, 0, config);
    r.idle_to(a + 20 * cycle);
    CHECK(r.probe->pulses == std::vector{a + 3 * cycle});
    CHECK(r.probe->falls.size() == (config == wave ? 0u : 1u));
    CHECK(r.out_level("lptim1_ch1") == (config == wave));
    CHECK(r.read(cnt) == 0);
    r.write(cr, enable | sngstrt);
    r.idle_to(a + 25 * cycle);
    CHECK(r.read(cnt) == (config == wave ? 0u : 2u));
    r.idle_to(a + 30 * cycle);
    CHECK(r.probe->pulses.size() == (config == wave ? 1u : 2u));
    if(config == wave)
    {
      auto const x = r.now_ps();
      r.write(cr, 0);
      CHECK(r.probe->falls == std::vector{x});
      CHECK(r.out_level("lptim1_ch1") == false);
    }
  }
}

TEST_CASE("inferred: set-once latches the output with no sink listening")
{
  rig r;
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 5, cntstrt, 0, wave);
  CHECK(r.pending() == a + cycle);
  r.idle_to(a + cycle);
  CHECK(r.pending() == a + 3 * cycle);
  r.idle_to(a + 30 * cycle);
  CHECK(r.out_level("lptim1_ch1") == true);
  CHECK(r.read(cnt) == 0);
  CHECK_FALSE(r.pending().has_value());
}

TEST_CASE(
    "inferred: set-once latches a channel with CCxE 0 and stale match flags, a later CCxE 1 "
    "gives a high output")
{
  for(auto const& [signal, enable_bit] : {
          std::pair{"lptim1_ch1", cc1e},
          std::pair{"lptim1_ch2", cc2e},
  })
  {
    CAPTURE(signal);
    rig        r{units[0], false, u575(), signal};
    auto const first = started_with_channel_2(r, 2, 2, 8);
    r.idle_to(first + 12 * cycle);
    REQUIRE((r.read(isr) & (cc1if | cc2if | arrm)) == (cc1if | cc2if | arrm));
    r.write(cr, 0);
    auto const a = started_with_channel_2(r, 2, 2, 8, 0, wave);
    r.idle_to(a + 20 * cycle);
    CHECK(r.probe->pulses.empty());
    auto const x = r.now_ps();
    r.write(ccmr1, enable_bit);
    CHECK(r.out_level(signal) == true);
    CHECK(r.probe->pulses == std::vector{x});
  }
}

TEST_CASE("inferred: CCR1 equal to or above ARR keeps the output inactive")
{
  for(auto const compare : {5u, 9u})
  {
    CAPTURE(compare);
    rig r{units[0], false, u575(), "lptim1_ch1"};
    r.write(ccmr1, cc1e);
    auto const a = r.started(compare, 5);
    r.idle_to(a + 30 * cycle);
    CHECK(r.probe->pulses.empty());
    CHECK(r.out_level("lptim1_ch1") == false);
  }
}

TEST_CASE("inferred: ENABLE 0 and an LPTIMxRST pulse drop the output at the write")
{
  for(auto const reset : {false, true})
  {
    CAPTURE(reset);
    rig r{units[0], false, u575(), "lptim1_ch1"};
    r.write(ccmr1, cc1e);
    auto const a = r.started(2, 5);
    r.idle_to(a + 4 * cycle);
    auto const x = r.now_ps();
    if(reset)
    {
      rcc_reset_pulse(*r.f, r.u.reset_reg, r.u.reset_bit);
    }
    else
    {
      r.write(cr, 0);
    }
    CHECK(r.probe->falls == std::vector{x});
    CHECK(r.out_level("lptim1_ch1") == false);
    r.idle_to(x + 20 * cycle);
    CHECK(r.probe->pulses.size() == 1);
  }
}

TEST_CASE("inferred: COUNTRST drops a high output and the PWM restarts from the reset instant")
{
  rig r{units[0], false, u575(), "lptim1_ch1"};
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 20);
  r.idle_to(a + 5 * cycle);
  r.write(cr, enable | countrst);
  auto const reset = a + 8 * cycle;
  r.idle_to(reset);
  CHECK(r.probe->falls == std::vector{reset});
  CHECK(r.pending() == reset + 3 * cycle);
  r.idle_to(reset + 3 * cycle);
  CHECK(r.probe->pulses == std::vector{a + 3 * cycle, reset + 3 * cycle});
}

TEST_CASE("inferred: a CCR1 value applied at its CMP1OK moves the PWM level within the period")
{
  rig r{units[0], false, u575(), "lptim1_ch1"};
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 20);
  r.idle_to(a + 4 * cycle);
  CHECK(r.out_level("lptim1_ch1") == true);
  auto const x = r.now_ps();
  r.write(ccr1, 9);
  r.idle_to(x + 3 * cycle);
  CHECK(r.probe->falls == std::vector{x + 3 * cycle});
  CHECK(r.out_level("lptim1_ch1") == false);
  r.idle_to(a + 10 * cycle);
  CHECK(r.probe->pulses == std::vector{a + 3 * cycle, a + 10 * cycle});
}

TEST_CASE("inferred: a kernel rate change keeps the PWM phase, the next edge follows the new rate")
{
  rig r{units[0], false, u575(), "lptim1_ch1"};
  r.hsi_on();
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 5);
  r.idle_to(a + 3 * cycle);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 2);
  auto const x = r.now_ps();
  CHECK(r.pending() == x + 3 * hsi);
  r.idle_to(x + 3 * hsi + 2 * hsi);
  CHECK(r.probe->falls == std::vector{x + 3 * hsi});
}

TEST_CASE("inferred: a CC1P write at the instant of an output edge applies after the edge")
{
  rig r{units[0], false, u575(), "lptim1_ch1"};
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 5);
  r.idle_to(a + 3 * cycle);
  r.write(ccmr1, cc1e | cc1p);
  CHECK(r.probe->pulses == std::vector{a + 3 * cycle});
  CHECK(r.probe->falls == std::vector{a + 3 * cycle});
  CHECK(r.out_level("lptim1_ch1") == false);
}

TEST_CASE("inferred: a stopped kernel clock keeps the output level until it runs again")
{
  rig r{units[0], false, u575(), "lptim1_ch1"};
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 5);
  r.idle_to(a + 3 * cycle);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
  r.idle_to(a + 40 * cycle);
  CHECK(r.out_level("lptim1_ch1") == true);
  CHECK(r.probe->falls.empty());
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  auto const resumed = r.now_ps();
  r.idle_to(resumed + 4 * cycle);
  CHECK(r.probe->falls == std::vector{resumed + 3 * cycle});
}

TEST_CASE("inferred: a PWM with no sink listening schedules no output event")
{
  rig r;
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 5);
  r.idle_to(a + 7 * cycle);
  CHECK((r.read(isr) & (cc1if | arrm | ue)) == (cc1if | arrm | ue));
  CHECK_FALSE(r.pending().has_value());
}

TEST_CASE("a snapshot restored under a listening sink gives it the next output edges and the level")
{
  rig r{units[0], false, u575(), "lptim1_ch1"};
  r.write(ccmr1, cc1e);
  auto const a = r.started(2, 5);
  r.idle_to(a + 3 * cycle);
  auto const saved = r.f->target->snapshot();
  rig        other{units[0], false, u575(), "lptim1_ch1"};
  other.probe->listen(false);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.probe->listening = true;
  CHECK(other.out_level("lptim1_ch1") == true);
  other.idle_to(a + 9 * cycle);
  CHECK(other.probe->falls == std::vector{a + 6 * cycle});
  CHECK(other.probe->pulses == std::vector{a + 9 * cycle});
}

TEST_CASE(
    "RM0456 58.4.10: LPTIM4 drives lptim4_out from CCR1 and ARR, WAVPOL inverts it at the CFGR "
    "write, also with ENABLE 0")
{
  rig        r{units[3], false, u575(), "lptim4_out"};
  auto const x = r.now_ps();
  r.write(cfgr, wavpol);
  CHECK(r.probe->pulses == std::vector{x});
  CHECK(r.out_level("lptim4_out") == true);
  r.idle_to(x + 2 * cycle);
  r.write(cfgr, 0);
  CHECK(r.probe->falls == std::vector{x + 2 * cycle});
  r.probe->pulses.clear();
  r.probe->falls.clear();
  auto const a = r.started(2, 5, cntstrt, 0, wavpol);
  r.idle_to(a + 7 * cycle);
  CHECK(r.probe->falls == std::vector{a + 3 * cycle});
  REQUIRE(r.probe->pulses.size() == 2);
  CHECK(r.probe->pulses.back() == a + 6 * cycle);
  auto* const source = dynamic_cast<stm32::trigger_source*>(r.f->target->model("LPTIM4"));
  REQUIRE(source != nullptr);
  CHECK(source->produces("lptim4_out"));
  CHECK_FALSE(source->produces("lptim4_ch1"));
}

TEST_CASE(
    "RM0456 58.4.19 and 58.7.18: channel 2 compares CCR2 on its own, CC2IF and lptim<n>_ch2 come "
    "at CNT CCR2 + 1 and the fall at CNT 0, channel 1 keeps its own instants, PRESC 0 and 3")
{
  for(auto const config : {0u, presc(3)})
  {
    CAPTURE(config);
    rig r{units[0], false, u575(), "lptim1_ch2"};
    r.write(ccmr1, cc1e | cc2e);
    auto const a = started_with_channel_2(r, 2, 5, 8, 0, config);
    auto const k = (config == 0 ? 1u : 8u) * cycle;
    r.idle_to(a + 3 * k);
    CHECK((r.read(isr) & (cc1if | cc2if)) == cc1if);
    CHECK(r.out_level("lptim1_ch2") == false);
    r.idle_to(a + 6 * k - cycle);
    CHECK((r.read(isr) & cc2if) == 0);
    r.idle_to(a + 6 * k);
    CHECK(r.read(cnt) == 6);
    CHECK((r.read(isr) & cc2if) == cc2if);
    CHECK(r.out_level("lptim1_ch2") == true);
    CHECK(r.probe->pulses == std::vector{a + 6 * k});
    r.idle_to(a + 9 * k);
    CHECK(r.probe->falls == std::vector{a + 9 * k});
    CHECK(r.out_level("lptim1_ch2") == false);
    r.write(icr, cc2if);
    r.idle_to(a + 15 * k);
    CHECK(r.probe->pulses == std::vector{a + 6 * k, a + 15 * k});
    CHECK((r.read(isr) & cc2if) == cc2if);
  }
}

TEST_CASE(
    "RM0456 58.4.10 and 58.7.17: CC2P inverts the channel 2 output at the CCMR1 write, also with "
    "ENABLE 0, CC2E 0 or CC2SEL 1 leave it inactive, inferred")
{
  rig        r{units[0], false, u575(), "lptim1_ch2"};
  auto const x = r.now_ps();
  r.write(ccmr1, cc2e | cc2p);
  CHECK(r.probe->pulses == std::vector{x});
  CHECK(r.out_level("lptim1_ch2") == true);
  r.write(ccmr1, cc2p);
  CHECK(r.probe->falls == std::vector{x});
  r.write(ccmr1, cc2e | cc2p | cc2sel);
  CHECK(r.out_level("lptim1_ch2") == false);
  r.write(ccmr1, cc2e);
  CHECK(r.out_level("lptim1_ch2") == false);
  auto* const source = dynamic_cast<stm32::trigger_source*>(r.f->target->model("LPTIM1"));
  REQUIRE(source != nullptr);
  CHECK(source->produces("lptim1_ch1"));
  CHECK(source->produces("lptim1_ch2"));
  CHECK_FALSE(source->produces("lptim1_ch3"));
}

TEST_CASE(
    "RM0456 58.4.11 and 58.7.2: a CCR2 write sets CMP2OK 3 kernel clocks later, takes effect then "
    "or with PRELOAD 1 at the next update, and CC2IF sets with CC2E 0")
{
  for(auto const config : {0u, preload})
  {
    CAPTURE(config);
    rig r;
    r.write(ccmr1, cc2e);
    auto const a = started_with_channel_2(r, 30, 30, 10, cc2if | cmp2ok, config);
    r.idle_to(a + 2 * cycle);
    auto const x = r.now_ps();
    r.write(ccr2, 5);
    CHECK(r.read(ccr2) == 5);
    CHECK(r.logged.empty());
    r.idle_to(x + 3 * cycle - cycle);
    CHECK((r.read(isr) & cmp2ok) == 0);
    r.idle_to(x + 3 * cycle);
    CHECK((r.read(isr) & cmp2ok) == cmp2ok);
    CHECK(r.irq_pending());
    r.write(icr, cmp2ok);
    if(config == 0)
    {
      r.idle_to(a + 6 * cycle);
      CHECK((r.read(isr) & cc2if) == cc2if);
    }
    else
    {
      r.idle_to(a + 11 * cycle);
      CHECK((r.read(isr) & cc2if) == 0);
      r.idle_to(a + 17 * cycle);
      CHECK((r.read(isr) & cc2if) == cc2if);
    }
  }
  rig        r;
  auto const a = r.started(30, 10);
  CHECK(r.read(ccmr1) == 0);
  r.idle_to(a);
  CHECK((r.read(isr) & cc2if) == 0);
  r.idle_to(a + cycle);
  CHECK(r.read(cnt) == 1);
  CHECK((r.read(isr) & cc2if) == cc2if);
}

TEST_CASE(
    "inferred: a CC2P write at the CMP2OK instant that flips the level applies after the "
    "CMP2OK, the channel 2 CCMR1 catch-up")
{
  rig r{units[0], false, u575(), "lptim1_ch2"};
  r.write(ccmr1, cc2e);
  auto const a = started_with_channel_2(r, 1, 2, 10);
  r.idle_to(a + 4 * cycle);
  CHECK(r.out_level("lptim1_ch2") == true);
  auto const x = r.now_ps();
  r.write(ccr2, 8);
  r.idle_to(x + 3 * cycle);
  r.write(ccmr1, cc2e | cc2p);
  CHECK(r.probe->falls == std::vector{x + 3 * cycle});
  CHECK(r.probe->pulses == std::vector{a + 3 * cycle, x + 3 * cycle});
  CHECK(r.out_level("lptim1_ch2") == true);
}

TEST_CASE(
    "RM0456 58.4.19: channel 2 of LPTIM2 and LPTIM3 drives its own lptim<n>_ch2 signal, CC2IF and "
    "the rise at CNT CCR2 + 1, the fall at CNT 0")
{
  for(auto const& [which, signal] : {
          std::pair{units[1], "lptim2_ch2"},
          std::pair{units[2], "lptim3_ch2"},
  })
  {
    CAPTURE(signal);
    rig r{which, false, u575(), signal};
    r.write(ccmr1, cc2e);
    auto const a = started_with_channel_2(r, 2, 5, 8);
    r.idle_to(a + 5 * cycle);
    CHECK((r.read(isr) & cc2if) == 0);
    r.idle_to(a + 6 * cycle);
    CHECK((r.read(isr) & cc2if) == cc2if);
    CHECK(r.probe->pulses == std::vector{a + 6 * cycle});
    r.idle_to(a + 9 * cycle);
    CHECK(r.probe->falls == std::vector{a + 9 * cycle});
  }
}

TEST_CASE("each channel instance drives its own lptim<n>_ch1 signal")
{
  rig r;
  for(auto const& [unit, own, other] : {
          std::tuple{"LPTIM1", "lptim1_ch1", "lptim1_out"},
          std::tuple{"LPTIM2", "lptim2_ch1", "lptim1_ch1"},
          std::tuple{"LPTIM3", "lptim3_ch1", "lptim3_out"},
  })
  {
    CAPTURE(unit);
    auto* const source = dynamic_cast<stm32::trigger_source*>(r.f->target->model(unit));
    REQUIRE(source != nullptr);
    CHECK(source->produces(own));
    CHECK_FALSE(source->produces(other));
  }
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
    r.idle_to(start + 11 * cycle);
    CHECK(r.read(isr) == (arrm | r.cc2));
    if(repetition != 0)
    {
      CHECK(r.pending() == start + span);
    }
    r.idle_to(start + span);
    CHECK(r.read(isr) == (arrm | r.cc2));
    CHECK(r.pending() == start + span + cycle);
    CHECK_FALSE(r.irq_pending());
    r.idle_to(start + span + cycle);
    CHECK(r.read(isr) == (arrm | ue | r.cc2));
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
  r.idle_to(start + 11 * cycle);
  CHECK(r.pending() == start + 44 * cycle);
  r.idle_to(start + 43 * cycle);
  CHECK(r.read(cnt) == 10);
  r.idle_to(start + 44 * cycle);
  CHECK(r.read(cnt) == 0);
  CHECK(r.read(isr) == (arrm | r.cc2));
  CHECK(r.pending() == start + 45 * cycle);
  r.idle_to(start + 45 * cycle);
  CHECK(r.read(isr) == (arrm | ue | r.cc2));
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
    "inferred: a new REP reaches the repetition counter at the first underflow at or after "
    "REPOK, PRELOAD 0 and 1, RM0456 58.4.16 and 58.4.11")
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
      r.idle_to(start + (loaded + 1) * cycle);
      CHECK((r.read(isr) & ue) == ue);
      r.write(icr, ue | repok);
      CHECK(r.pending() == start + (loaded + 33) * cycle);
      r.idle_to(start + (loaded + 34) * cycle);
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
    CHECK(r.pending() == start + (applied + 1) * cycle);
    r.idle_to(start + (applied + 1) * cycle);
    r.write(icr, ue);
    CHECK(r.pending() == start + (applied + 4) * cycle);
    r.idle_to(start + (applied + 4) * cycle);
    CHECK(r.read(isr) == (cc1if | r.cc2));
    r.idle_to(start + (applied + 21) * cycle);
    CHECK(r.read(isr) == (cc1if | arrm | r.cc2));
    r.write(icr, cc1if | arrm);
    CHECK(r.pending() == start + (applied + (repetition == 0 ? 22 : 25)) * cycle);
  }
}

TEST_CASE("RM0456 58.7.2: an ARR write clears UE and lowers its IRQ")
{
  rig        r;
  auto const start = r.started(30, 10, cntstrt, ue);
  r.idle_to(start + 12 * cycle);
  CHECK(r.read(isr) == (arrm | ue | r.cc2));
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
    one->idle_to(start + 45 * cycle);
    CHECK((one->read(isr) & ue) == ue);
    one->write(icr, ue);
    CHECK(one->pending() == start + (44 + 4 * 21) * cycle);
  }
}

TEST_CASE("RM0456 58.4.16: REPOKIE raises the IRQ at REPOK, and only with its DIER bit")
{
  for(auto const irqs : {repok, 0u})
  {
    CAPTURE(irqs);
    rig r;
    r.enable_with(irqs);
    r.write(rcr, 2);
    auto const at = r.now_ps();
    r.idle_to(at + 7 * cycle - cycle);
    CHECK_FALSE(r.irq_pending());
    r.idle_to(at + 7 * cycle);
    CHECK(r.read(isr) == repok);
    CHECK(r.irq_pending() == (irqs != 0));
  }
}

TEST_CASE(
    "inferred: ENABLE written to 0 applies an ARR value preloaded for the next update, the next "
    "run counts to it")
{
  rig        r;
  auto const start = r.started(30, 10, cntstrt, 0, preload);
  r.idle_to(start + 2 * cycle);
  r.write(arr, 20);
  r.idle_to(start + 7 * cycle);
  CHECK((r.read(isr) & arrok) == arrok);
  r.write(cr, 0);
  r.write(icr, arrok);
  r.write(cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const again = r.now_ps() + 3 * cycle;
  r.write(cr, enable | cntstrt);
  CHECK(r.pending() == again + 21 * cycle);
}

TEST_CASE(
    "inferred: ENABLE written to 0 keeps a pending RCR write, its REPOK sets at the due and the "
    "next start loads the new REP")
{
  rig r;
  r.enable_with();
  r.settle_values(5, 10);
  r.write(rcr, 2);
  r.write(cr, 0);
  auto const at = r.now_ps();
  CHECK(r.pending() == at + 7 * cycle);
  r.idle_to(at + 7 * cycle);
  CHECK(r.read(isr) == repok);
  CHECK(r.read(rcr) == 2);
  r.write(icr, repok);
  r.write(cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const again = r.now_ps() + 3 * cycle;
  r.write(cr, enable | cntstrt);
  r.idle_to(again + 33 * cycle);
  CHECK((r.read(isr) & ue) == 0);
  r.idle_to(again + 34 * cycle);
  CHECK((r.read(isr) & ue) == ue);
}

TEST_CASE(
    "inferred: REPOK with the kernel clock stopped waits for the clock and then 3 kernel clocks, "
    "the APB part dropped")
{
  rig r;
  r.write(cr, enable);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
  r.write(rcr, 2);
  CHECK_FALSE(r.pending().has_value());
  r.idle_to(r.now_ps() + 50 * cycle);
  CHECK(r.read(isr) == 0);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  auto const resumed = r.now_ps();
  CHECK(r.pending() == resumed + 3 * cycle);
  r.idle_to(resumed + 3 * cycle);
  CHECK(r.read(isr) == repok);
}

TEST_CASE(
    "inferred: a kernel rate change moves the repetition counter, the update follows the rate")
{
  for(auto const& [at, remaining] : {
          std::pair{std::uint64_t{5},  std::uint64_t{39}},
          std::pair{std::uint64_t{22}, std::uint64_t{22}},
          std::pair{std::uint64_t{25}, std::uint64_t{19}},
          std::pair{std::uint64_t{30}, std::uint64_t{14}},
  })
  {
    CAPTURE(at);
    rig r;
    r.hsi_on();
    auto const start = r.started(30, 10, cntstrt, ue, 0, 3);
    r.idle_to(start + at * cycle);
    r.rcc("RCC_CCIPR3", "LPTIM1SEL", 2);
    auto const due = start + at * cycle + remaining * hsi;
    if(at > 11)
    {
      CHECK(r.pending() == due);
    }
    r.idle_to(due - cycle);
    CHECK((r.read(isr) & ue) == 0);
    r.idle_to(due);
    CHECK((r.read(isr) & ue) == ue);
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
  CHECK(r.read(isr) == (cc1if | r.cc2));
  r.write(ccr1, 4);
  r.write(arr, 20);
  r.write(dier, cc1if);
  r.write(cr, 0);
  CHECK(r.read(isr) == (cc1if | r.cc2));
  CHECK(r.pending() == start + 10 * cycle);
  r.idle_to(start + 10 * cycle);
  CHECK(r.read(isr) == (cmp1ok | arrok | dierok | r.cc2));
  r.write(icr, cmp1ok | arrok | dierok);
  r.write(cr, enable);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const again = r.now_ps() + 3 * cycle;
  r.write(cr, enable | cntstrt);
  CHECK(r.pending() == again + 5 * cycle);
  r.idle_to(again + 5 * cycle);
  CHECK(r.read(isr) == (cc1if | r.cc2));
  CHECK(r.irq_pending());
  r.idle_to(again + 21 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm | r.cc2));
}

TEST_CASE(
    "inferred: CCR1 equal to ARR sets CC1IF with ARRM, and a counter above a lowered ARR runs to "
    "0xFFFF before it wraps")
{
  rig        r;
  auto const start = r.started(10, 10);
  CHECK(r.pending() == start + cycle);
  r.idle_to(start + cycle);
  CHECK(r.pending() == start + 11 * cycle);
  r.idle_to(start + 11 * cycle);
  CHECK(r.read(isr) == (cc1if | arrm | r.cc2));
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
    "checklist 43: CFGR2 holds no storage bits, IN1SEL, IN2SEL, IC1SEL and IC2SEL writes log "
    "nothing at the write and the register keeps them")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{units[0], strict};
    CHECK_FALSE(r.f->target->model("LPTIM1")->storage_only(static_cast<std::uint32_t>(cfgr2)));
    for(auto const value : {1u, 0x10u, 1u << 16, 1u << 20 | 1u << 16 | 0x11})
    {
      CAPTURE(value);
      r.write(cfgr2, value);
      CHECK(r.read(cfgr2) == value);
    }
    CHECK(r.logged.empty());
  }
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
  CHECK(r.pending() == start + 11 * cycle);
  r.idle_to(start + 11 * cycle);
  CHECK(r.read(isr) == (arrm | arrok | r.cc2));
  CHECK(r.read(cnt) == 0);
  CHECK(r.pending() == start + 12 * cycle);
  r.idle_to(start + 12 * cycle);
  CHECK(r.read(isr) == (arrm | arrok | ue | r.cc2));
  CHECK_FALSE(r.pending().has_value());
}

TEST_CASE(
    "RM0456 58.4.14: COUNTRST resets CNT 3 kernel clocks after the write and reads 1 until then, "
    "the count restarting at that instant, inferred")
{
  for(auto const& [written, first] : {
          std::pair{std::uint64_t{4},  std::uint64_t{7} },
          std::pair{std::uint64_t{26}, std::uint64_t{29}},
          std::pair{std::uint64_t{8},  std::uint64_t{11}},
  })
  {
    CAPTURE(written);
    rig        r;
    auto const start = r.started(6, 10);
    r.idle_to(start + written * cycle);
    r.write(icr, cc1if | arrm | ue);
    r.write(cr, enable | countrst);
    CHECK(r.read(cr) == (enable | countrst));
    auto const reset = start + (written + 3) * cycle;
    CHECK(r.pending() == start + first * cycle);
    r.idle_to(reset - cycle);
    CHECK(r.read(cr) == (enable | countrst));
    r.idle_to(reset);
    CHECK(r.read(cr) == enable);
    CHECK(r.read(cnt) == 0);
    CHECK((r.read(isr) & ue) == 0);
    r.write(icr, cc1if | arrm | ue);
    CHECK(r.pending() == reset + 7 * cycle);
    r.idle_to(reset + 7 * cycle);
    CHECK(r.read(isr) == (cc1if | r.cc2));
    CHECK(r.read(cnt) == 7);
  }
  rig r;
  r.write(cr, countrst);
  CHECK(r.read(cr) == 0);
}

TEST_CASE(
    "inferred: a COUNTRST at the one-shot update instant drops the stop, the counter runs on to "
    "the next update")
{
  rig        r;
  auto const start = r.started(30, 10, sngstrt);
  r.idle_to(start + 8 * cycle);
  r.write(cr, enable | countrst);
  r.idle_to(start + 11 * cycle);
  CHECK(r.read(cr) == enable);
  CHECK(r.read(isr) == (arrm | r.cc2));
  CHECK(r.pending() == start + 22 * cycle);
  r.idle_to(start + 22 * cycle);
  CHECK(r.read(isr) == (arrm | r.cc2));
  CHECK(r.pending() == start + 23 * cycle);
  r.idle_to(start + 23 * cycle);
  CHECK(r.read(isr) == (arrm | ue | r.cc2));
  CHECK_FALSE(r.pending().has_value());
}

TEST_CASE("RM0456 58.7.11: a second COUNTRST while one is pending is ignored")
{
  rig        r;
  auto const start = r.started(6, 10);
  r.idle_to(start + 12 * cycle);
  r.write(cr, enable | countrst);
  r.idle_to(start + 13 * cycle);
  r.write(cr, enable | countrst);
  CHECK(r.read(cr) == (enable | countrst));
  CHECK(r.pending() == start + 15 * cycle);
  r.idle_to(start + 15 * cycle);
  CHECK(r.read(cr) == enable);
  CHECK(r.read(cnt) == 0);
}

TEST_CASE("inferred: ENABLE written to 0 drops a pending COUNTRST")
{
  rig        r;
  auto const start = r.started(6, 10);
  r.idle_to(start + 12 * cycle);
  r.write(cr, enable | countrst);
  r.idle_to(start + 13 * cycle);
  r.write(cr, 0);
  CHECK(r.read(cr) == 0);
  r.idle_to(start + 20 * cycle);
  CHECK(r.read(cr) == 0);
  CHECK(r.read(cnt) == 0);
}

TEST_CASE("inferred: a kernel rate change keeps the kernel clocks left before a pending COUNTRST")
{
  rig r;
  r.hsi_on();
  auto const start = r.started(6, 10);
  r.idle_to(start + 12 * cycle);
  r.write(cr, enable | countrst);
  r.idle_to(start + 13 * cycle);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 2);
  auto const reset = start + 13 * cycle + 2 * hsi;
  CHECK(r.pending() == reset);
  CHECK(r.read(cr) == (enable | countrst));
  r.idle_to(reset);
  CHECK(r.read(cr) == enable);
}

TEST_CASE("inferred: a COUNTRST written with CNTSTRT clears its bit at the due, CNT stays 0")
{
  rig r;
  r.enable_with();
  r.settle_values(6, 10);
  r.write(cr, enable | cntstrt);
  auto const at = r.now_ps();
  r.write(cr, enable | countrst);
  CHECK(r.read(cr) == (enable | countrst));
  r.idle_to(at + 3 * cycle);
  CHECK(r.read(cr) == enable);
  CHECK(r.read(cnt) == 0);
  CHECK(r.pending() == at + 4 * cycle);
  r.idle_to(at + 4 * cycle);
  CHECK(r.pending() == at + 10 * cycle);
}

TEST_CASE(
    "inferred: COUNTRST and an RSTARE read before the counter leaves the start latency keep the "
    "start instant")
{
  for(auto const by_read : {false, true})
  {
    CAPTURE(by_read);
    rig r;
    r.write(cfgr, presc(3));
    r.write(cr, enable);
    auto const enabled = r.now_ps();
    r.write(arr, 10);
    r.write(ccr1, 6);
    r.idle_to(enabled + 4 * cycle);
    r.write(icr, cmp1ok | arrok);
    r.write(cr, enable | cntstrt | (by_read ? rstare : 0u));
    r.idle_to(enabled + 8 * cycle);
    if(by_read)
    {
      CHECK(r.cpu_read(cnt) == 0);
    }
    else
    {
      r.write(cr, enable | countrst);
    }
    r.idle_to(enabled + 12 * cycle);
    CHECK((r.read(cr) & countrst) == 0);
    CHECK(r.read(cnt) == 0);
    CHECK(r.pending() == enabled + 16 * cycle + 8 * cycle);
    r.idle_to(enabled + 16 * cycle + 8 * cycle);
    CHECK(r.pending() == enabled + 16 * cycle + 7 * 8 * cycle);
  }
}

TEST_CASE(
    "RM0456 58.4.14 and 58.7.11: with RSTARE a CPU read of CNT returns the count and resets it, "
    "a debugger read leaves it, inferred")
{
  rig        r;
  auto const start = r.started(30, 10);
  r.write(cr, enable | rstare);
  CHECK(r.read(cr) == (enable | rstare));
  r.idle_to(start + 5 * cycle);
  CHECK(r.read(cnt) == 5);
  CHECK(r.read(cnt) == 5);
  CHECK(r.cpu_read(cnt) == 5);
  CHECK(r.read(cnt) == 0);
  r.idle_to(start + 8 * cycle);
  CHECK(r.read(cnt) == 3);
  r.write(cr, enable);
  CHECK(r.cpu_read(cnt) == 3);
  CHECK(r.read(cnt) == 3);
  r.write(cr, enable | rstare);
  r.idle_to(start + 10 * cycle);
  CHECK(r.cpu_read(cnt, popcorn::initiator::dma) == 5);
  CHECK(r.read(cnt) == 0);
  rig off;
  off.write(cr, rstare);
  CHECK(off.read(cr) == 0);
}

namespace
{
constexpr std::uint32_t countmode = 1u << 23;
constexpr std::uint32_t falling   = 1u << 1;
constexpr std::uint32_t both      = 2u << 1;

constexpr auto ckflt(std::uint32_t value) -> std::uint32_t
{
  return value << 3;
}

auto in1_pin(rig const& r, std::string_view port, std::uint8_t pin, std::uint32_t af)
    -> stm32::gpio_port&
{
  r.rcc("RCC_AHB2ENR1", std::string{port} + "EN", 1);
  auto const  base  = r.f->target->peripheral(port).description().base;
  auto* const found = r.f->model_of<stm32::gpio_port>(port);
  REQUIRE(found != nullptr);
  found->drive(pin, false);
  r.write_at(base, (r.read_at(base) & ~(3u << (2 * pin))) | (2u << (2 * pin)));
  auto const afr   = base + (pin < 8 ? 0x20u : 0x24u);
  auto const shift = 4u * (pin % 8u);
  r.write_at(afr, (r.read_at(afr) & ~(15u << shift)) | (af << shift));
  return *found;
}

auto lptim1_in1(rig const& r) -> stm32::gpio_port&
{
  return in1_pin(r, "GPIOB", 5, 1);
}

auto pulses(stm32::gpio_port& port, std::uint8_t pin, std::uint64_t from, std::uint32_t count)
    -> std::vector<std::uint64_t>
{
  auto rises = std::vector<std::uint64_t>{};
  for(std::uint32_t index = 0; index < count; ++index)
  {
    auto const at = from + index * 10 * cycle;
    port.drive_at(pin, true, vtime{at});
    port.drive_at(pin, false, vtime{at + 5 * cycle});
    rises.push_back(at);
  }
  return rises;
}
}

TEST_CASE(
    "RM0456 58.4.12 CKSEL 1: the first five PB5 edges after ENABLE are lost, PRESC divides the "
    "edges after them, PRESC 0 to 7, the ENABLE and start split inferred")
{
  for(std::uint32_t prescaler = 0; prescaler < 8; ++prescaler)
  {
    CAPTURE(prescaler);
    rig        r;
    auto&      pins = lptim1_in1(r);
    auto const k    = 1u << prescaler;
    r.write(cfgr, cksel | presc(prescaler));
    r.write(cr, enable);
    r.write(arr, 1000);
    r.write(cr, enable | cntstrt);
    auto const rises = pulses(pins, 5, r.now_ps() + 20 * cycle, 5 + 3 * k);
    r.idle_to(rises[3] + cycle);
    CHECK((r.read(isr) & arrok) == 0);
    r.idle_to(rises[4] + cycle);
    CHECK((r.read(isr) & arrok) == arrok);
    CHECK(r.read(cnt) == 0);
    r.idle_to(rises[4 + k] - cycle);
    CHECK(r.read(cnt) == 0);
    r.idle_to(rises[4 + k]);
    CHECK(r.read(cnt) == 1);
    r.idle_to(rises.back() + cycle);
    CHECK(r.read(cnt) == 3);
  }
}

TEST_CASE(
    "RM0456 58.4.12: with CKSEL 1 and no PB5 edge, ARROK never sets and the counter never starts")
{
  rig r;
  r.write(cfgr, cksel);
  r.write(cr, enable);
  r.write(arr, 100);
  r.write(cr, enable | cntstrt);
  r.idle_to(r.now_ps() + 500 * cycle);
  CHECK((r.read(isr) & arrok) == 0);
  CHECK(r.read(cnt) == 0);
  CHECK_FALSE(r.pending().has_value());
}

TEST_CASE(
    "RM0456 58.7.10 CKPOL: COUNTMODE 1 counts rising, falling or both PB5 edges, CKSEL 1 the "
    "falling ones after the five lost")
{
  for(auto const& [config, before, after] : {
          std::tuple{countmode,           3u, 3u},
          std::tuple{countmode | falling, 2u, 3u},
          std::tuple{countmode | both,    5u, 6u},
          std::tuple{cksel | falling,     2u, 3u},
  })
  {
    CAPTURE(config);
    rig        r;
    auto&      pins  = lptim1_in1(r);
    auto const t     = r.started(60, 100, cntstrt, 0, config);
    auto const total = (config & cksel) != 0 ? 8u : 3u;
    auto const rises = pulses(pins, 5, t + 20 * cycle, total);
    r.idle_to(rises.back() + 2 * cycle);
    CHECK(r.read(cnt) == before);
    r.idle_to(rises.back() + 6 * cycle);
    CHECK(r.read(cnt) == after);
  }
}

TEST_CASE(
    "RM0456 58.4.12 and 58.4.19 COUNTMODE 1: each PB5 edge counts at its instant, CC1IF and ARRM "
    "at the edge leaving CCR1 and ARR, the update there and UE at the next counted edge, inferred")
{
  rig        r;
  auto&      pins  = lptim1_in1(r);
  auto const t     = r.started(3, 5, cntstrt, cc1if | arrm | ue, countmode);
  auto const rises = pulses(pins, 5, t + 20 * cycle, 7);
  r.idle_to(rises[2]);
  CHECK(r.read(cnt) == 3);
  CHECK((r.read(isr) & cc1if) == 0);
  r.idle_to(rises[3] - cycle);
  CHECK_FALSE(r.irq_pending());
  r.idle_to(rises[3]);
  CHECK(r.irq_pending());
  CHECK(r.read(cnt) == 4);
  CHECK((r.read(isr) & cc1if) == cc1if);
  r.write(icr, cc1if);
  r.idle_to(rises[5] - cycle);
  CHECK((r.read(isr) & arrm) == 0);
  r.idle_to(rises[5]);
  CHECK(r.read(cnt) == 0);
  CHECK((r.read(isr) & (arrm | ue)) == arrm);
  r.idle_to(rises[6] - cycle);
  CHECK((r.read(isr) & ue) == 0);
  r.idle_to(rises[6]);
  CHECK(r.read(cnt) == 1);
  CHECK((r.read(isr) & ue) == ue);
  CHECK_FALSE(r.pending().has_value());
}

TEST_CASE(
    "RM0456 58.4.8: SNGSTRT with COUNTMODE 1 stops at 0 at the update edge, UE set at once there, "
    "inferred, later edges leave CNT at 0")
{
  rig        r;
  auto&      pins  = lptim1_in1(r);
  auto const t     = r.started(60, 3, sngstrt, 0, countmode);
  auto const rises = pulses(pins, 5, t + 20 * cycle, 6);
  r.idle_to(rises[2]);
  CHECK(r.read(cnt) == 3);
  CHECK((r.read(isr) & (arrm | ue)) == 0);
  r.idle_to(rises[3]);
  CHECK(r.read(cnt) == 0);
  CHECK((r.read(isr) & (arrm | ue)) == (arrm | ue));
  r.idle_to(rises[5] + cycle);
  CHECK(r.read(cnt) == 0);
}

TEST_CASE(
    "RM0456 58.4.16 with COUNTMODE 1: REP 1 makes every second overflow edge the update, UE at the "
    "edge after it")
{
  rig        r;
  auto&      pins  = lptim1_in1(r);
  auto const t     = r.started(60, 1, cntstrt, 0, countmode, 1);
  auto const rises = pulses(pins, 5, t + 20 * cycle, 6);
  r.idle_to(rises[1]);
  CHECK(r.read(cnt) == 0);
  CHECK((r.read(isr) & (arrm | ue)) == arrm);
  r.write(icr, arrm);
  r.idle_to(rises[2]);
  CHECK((r.read(isr) & ue) == 0);
  r.idle_to(rises[3]);
  CHECK((r.read(isr) & (arrm | ue)) == arrm);
  r.idle_to(rises[4]);
  CHECK((r.read(isr) & ue) == ue);
}

TEST_CASE(
    "RM0456 58.4.11 with COUNTMODE 1: PRELOAD 1 applies a new ARR at the update edge, and "
    "PRELOAD 0 at its ARROK")
{
  for(auto const config : {countmode | preload, countmode})
  {
    CAPTURE(config);
    rig        r;
    auto&      pins  = lptim1_in1(r);
    auto const t     = r.started(60, 3, cntstrt, 0, config);
    auto const rises = pulses(pins, 5, t + 20 * cycle, 8);
    r.idle_to(rises[1] + cycle);
    r.write(arr, 5);
    r.idle_to(rises[2]);
    CHECK((r.read(isr) & arrok) == arrok);
    CHECK(r.read(cnt) == 3);
    r.idle_to(rises[3]);
    CHECK(r.read(cnt) == (config == countmode ? 4u : 0u));
    r.idle_to(rises[5]);
    CHECK(r.read(cnt) == (config == countmode ? 0u : 2u));
    r.idle_to(rises[7]);
    CHECK(r.read(cnt) == (config == countmode ? 2u : 4u));
  }
}

TEST_CASE(
    "RM0456 58.4.19 with COUNTMODE 1: lptim1_ch1 rises at the edge to CNT CCR1 + 1 and falls at "
    "the edge to CNT 0, inferred")
{
  rig r{units[0], false, u575(), "lptim1_ch1"};
  r.write(ccmr1, cc1e);
  auto&      pins  = lptim1_in1(r);
  auto const t     = r.started(2, 4, cntstrt, 0, countmode);
  auto const rises = pulses(pins, 5, t + 20 * cycle, 6);
  r.idle_to(rises[5] + cycle);
  CHECK(r.probe->pulses == std::vector{rises[2]});
  CHECK(r.probe->falls == std::vector{rises[4]});
  CHECK(r.out_level("lptim1_ch1") == false);
}

TEST_CASE(
    "RM0456 58.7.10 CKFLT: with COUNTMODE 1 a PB5 level change counts once held 2, 4 or 8 kernel "
    "clocks, a shorter pulse is rejected, the sampling phase taken as the edge, inferred")
{
  for(std::uint32_t code = 1; code < 4; ++code)
  {
    CAPTURE(code);
    rig        r;
    auto&      pins     = lptim1_in1(r);
    auto const n        = std::uint64_t{1} << code;
    auto const t        = r.started(60, 100, cntstrt, 0, countmode | ckflt(code));
    auto const short_at = t + 20 * cycle;
    pins.drive_at(5, true, vtime{short_at});
    pins.drive_at(5, false, vtime{short_at + (n - 1) * cycle});
    r.idle_to(short_at + 20 * cycle);
    CHECK(r.read(cnt) == 0);
    auto const e = r.now_ps() + 2 * cycle;
    pins.drive_at(5, true, vtime{e});
    pins.drive_at(5, false, vtime{e + (n + 1) * cycle});
    r.idle_to(e);
    CHECK(r.pending() == e + n * cycle);
    r.idle_to(e + n * cycle - cycle);
    CHECK(r.read(cnt) == 0);
    r.idle_to(e + n * cycle);
    CHECK(r.read(cnt) == 1);
    r.idle_to(e + 40 * cycle);
    CHECK(r.read(cnt) == 1);
  }
}

TEST_CASE(
    "RM0456 58.4.3 Table 605: IN1SEL 01, comp1_out, logs one warning at the counter start and "
    "raises under strict_peripherals, PB5 edges then count nothing, IN1SEL 00 written with PB5 "
    "high counts the edge at the write")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  r.write(cfgr2, 1);
  auto const t = r.started(60, 100, cntstrt, 0, countmode);
  CHECK(r.warnings("IN1SEL") == 1);
  auto const rises = pulses(pins, 5, t + 20 * cycle, 3);
  r.idle_to(rises.back() + 6 * cycle);
  CHECK(r.read(cnt) == 0);
  pins.drive(5, true);
  r.idle_to(r.now_ps() + 4 * cycle);
  CHECK(r.read(cnt) == 0);
  r.write(cfgr2, 0);
  CHECK(r.read(cnt) == 1);
  rig strict{units[0], true};
  strict.write(cfgr2, 1);
  CHECK_THROWS_AS(
      static_cast<void>(strict.started(60, 100, cntstrt, 0, countmode)), popcorn::model_error);
  rig internal;
  internal.write(cfgr2, 1);
  static_cast<void>(internal.started(60, 100));
  CHECK(internal.warnings("IN1SEL") == 0);
}

TEST_CASE(
    "RM0456 58.4.5, 58.4.12 and 58.7.10: PRESC with COUNTMODE 1, CKPOL 11, CKPOL 10 with CKSEL 1 "
    "and CKFLT with the kernel clock stopped log one warning each at a start and raise under "
    "strict_peripherals")
{
  for(auto const& [config, stopped, text] : {
          std::tuple{countmode | presc(1), false, std::string_view{"PRESC not 0 with COUNTMODE"}},
          std::tuple{countmode | 3u << 1,  false, std::string_view{"CKPOL 11"}                  },
          std::tuple{cksel | both,         false, std::string_view{"CKPOL 10 with CKSEL 1"}     },
          std::tuple{countmode | ckflt(1), true,  std::string_view{"CKFLT not 0"}               },
          std::tuple{rising | (1u << 6),   true,  std::string_view{"TRGFLT not 0"}              },
  })
  {
    CAPTURE(text);
    for(auto const strict : {false, true})
    {
      CAPTURE(strict);
      rig r{units[0], strict};
      if(stopped)
      {
        r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
      }
      r.write(cfgr, config);
      r.write(cr, enable);
      if(strict)
      {
        CHECK_THROWS_AS(r.write(cr, enable | cntstrt), popcorn::model_error);
        continue;
      }
      r.write(cr, enable | cntstrt);
      r.write(cr, 0);
      r.write(cr, enable);
      r.write(cr, enable | cntstrt);
      CHECK(r.warnings(text) == 1);
    }
  }
}

TEST_CASE("PRESC 2 with COUNTMODE 1 counts every PB5 edge")
{
  rig        r;
  auto&      pins  = lptim1_in1(r);
  auto const t     = r.started(60, 100, cntstrt, 0, countmode | presc(2));
  auto const rises = pulses(pins, 5, t + 40 * cycle, 3);
  r.idle_to(rises.back() + cycle);
  CHECK(r.read(cnt) == 3);
}

TEST_CASE(
    "checklist 31: an LPTIM1RST pulse with PB5 high takes the level, a falling edge after the "
    "start then counts")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  pins.drive(5, true);
  rcc_reset_pulse(*r.f, r.u.reset_reg, r.u.reset_bit);
  auto const t = r.started(60, 100, cntstrt, 0, countmode | falling);
  pins.drive_at(5, false, vtime{t + 20 * cycle});
  r.idle_to(t + 30 * cycle);
  CHECK(r.read(cnt) == 1);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot of a CKSEL 1 count mid prescaler and mid OK restores into a "
    "machine with no reset, the state and the next counts equal")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  r.write(cfgr, cksel | presc(1));
  r.write(cr, enable);
  r.write(arr, 100);
  r.write(cr, enable | cntstrt);
  auto const rises = pulses(pins, 5, r.now_ps() + 20 * cycle, 8);
  r.idle_to(rises.back() + 6 * cycle);
  REQUIRE(r.read(cnt) == 1);
  r.write(ccr1, 2);
  auto const saved = r.f->target->snapshot();
  rig        other{no_reset};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  for(auto const* one : {&r, &other})
  {
    auto* const port = one->f->model_of<stm32::gpio_port>("GPIOB");
    REQUIRE(port != nullptr);
    auto const more = pulses(*port, 5, one->now_ps() + 2 * cycle, 4);
    one->idle_to(more[0] + cycle);
    CHECK(one->read(cnt) == 2);
    one->idle_to(more[1] + cycle);
    CHECK((one->read(isr) & cmp1ok) == 0);
    one->idle_to(more[2] + cycle);
    CHECK((one->read(isr) & cmp1ok) == cmp1ok);
    CHECK(one->read(cnt) == 3);
    one->idle_to(more[3] + cycle);
    CHECK(one->read(cnt) == 3);
  }
}

TEST_CASE(
    "RM0456 58.4.14 with CKSEL 1: COUNTRST resets CNT at the third PB5 edge after the write, "
    "the edge counting first, inferred")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  r.write(cfgr, cksel);
  r.write(cr, enable);
  r.write(arr, 100);
  r.write(cr, enable | cntstrt);
  auto const rises = pulses(pins, 5, r.now_ps() + 20 * cycle, 9);
  r.idle_to(rises[8] + cycle);
  REQUIRE(r.read(cnt) == 4);
  r.write(cr, enable | countrst);
  auto const more = pulses(pins, 5, r.now_ps() + 10 * cycle, 4);
  r.idle_to(more[1] + cycle);
  CHECK(r.read(cnt) == 6);
  CHECK((r.read(cr) & countrst) == countrst);
  r.idle_to(more[2] + cycle);
  CHECK(r.read(cnt) == 0);
  CHECK((r.read(cr) & countrst) == 0);
  r.idle_to(more[3] + cycle);
  CHECK(r.read(cnt) == 1);
}

TEST_CASE(
    "RM0456 58.4.2 and the U575 datasheet: each LPTIM counts its own IN1 pin, PB5 AF1, PB1 AF14, "
    "PB13 AF2 and PD13 AF13")
{
  struct pin_of
  {
    unit             which;
    std::string_view port;
    std::uint8_t     pin;
    std::uint32_t    af;
  };
  for(auto const& [which, port, pin, af] : {
          pin_of{units[0], "GPIOB", 5,  1 },
          pin_of{units[1], "GPIOB", 1,  14},
          pin_of{units[2], "GPIOB", 13, 2 },
          pin_of{units[3], "GPIOD", 13, 13},
  })
  {
    CAPTURE(which.name);
    rig        r{which};
    auto&      pins  = in1_pin(r, port, pin, af);
    auto const t     = r.started(60, 100, cntstrt, 0, countmode);
    auto const rises = pulses(pins, pin, t + 20 * cycle, 3);
    r.idle_to(rises.back() + cycle);
    CHECK(r.read(cnt) == 3);
  }
}

TEST_CASE(
    "inferred: with COUNTMODE 1 a stopped kernel clock drops the PB5 edges and keeps CNT, the "
    "count goes on when it runs again")
{
  rig        r;
  auto&      pins  = lptim1_in1(r);
  auto const t     = r.started(60, 100, cntstrt, 0, countmode);
  auto const first = pulses(pins, 5, t + 20 * cycle, 2);
  r.idle_to(first.back() + 6 * cycle);
  REQUIRE(r.read(cnt) == 2);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
  auto const held = pulses(pins, 5, r.now_ps() + 2 * cycle, 2);
  r.idle_to(held.back() + 6 * cycle);
  CHECK(r.read(cnt) == 2);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  auto const more = pulses(pins, 5, r.now_ps() + 2 * cycle, 2);
  r.idle_to(more.back() + 6 * cycle);
  CHECK(r.read(cnt) == 4);
}

TEST_CASE(
    "RM0456 58.4.8 with CKSEL 1: a CNTSTRT written in one-shot mode reaches the counter at the "
    "third PB5 edge, after the update edge stopped it, and starts it, inferred")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  r.write(cfgr, cksel);
  r.write(cr, enable);
  r.write(arr, 2);
  r.write(cr, enable | sngstrt);
  auto const rises = pulses(pins, 5, r.now_ps() + 20 * cycle, 7);
  r.idle_to(rises.back() + cycle);
  REQUIRE(r.read(cnt) == 2);
  r.write(cr, enable | cntstrt);
  auto const more = pulses(pins, 5, r.now_ps() + 10 * cycle, 5);
  r.idle_to(more[0] + cycle);
  CHECK(r.read(cnt) == 0);
  CHECK((r.read(isr) & (arrm | ue)) == (arrm | ue));
  r.idle_to(more[2] + cycle);
  CHECK(r.read(cnt) == 0);
  r.idle_to(more[3] + cycle);
  CHECK(r.read(cnt) == 1);
  r.idle_to(more[4] + cycle);
  CHECK(r.read(cnt) == 2);
}

TEST_CASE(
    "checklist 53: IN1SEL written to 01 while the counter counts input 1 edges logs one warning "
    "and raises under strict_peripherals")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{units[0], strict};
    static_cast<void>(lptim1_in1(r));
    static_cast<void>(r.started(60, 100, cntstrt, 0, countmode));
    if(strict)
    {
      CHECK_THROWS_AS(r.write(cfgr2, 1), popcorn::model_error);
      continue;
    }
    r.write(cfgr2, 1);
    r.write(cfgr2, 2);
    CHECK(r.warnings("IN1SEL") == 1);
  }
  rig internal;
  static_cast<void>(internal.started(60, 100));
  internal.write(cfgr2, 1);
  CHECK(internal.warnings("IN1SEL") == 0);
}

TEST_CASE(
    "RM0456 58.4.12 with CKSEL 1: DIEROK and REPOK set at the third PB5 edge after the DIER or "
    "RCR write, REPOK with its APB clocks dropped, inferred")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  r.write(cfgr, cksel);
  r.write(cr, enable);
  auto const rises = pulses(pins, 5, r.now_ps() + 20 * cycle, 10);
  r.idle_to(rises[3] + cycle);
  r.write(dier, cc1if);
  r.idle_to(rises[5] + cycle);
  CHECK((r.read(isr) & dierok) == 0);
  r.idle_to(rises[6] + cycle);
  CHECK((r.read(isr) & dierok) == dierok);
  r.write(rcr, 1);
  r.idle_to(rises[8] + cycle);
  CHECK((r.read(isr) & repok) == 0);
  r.idle_to(rises[9] + cycle);
  CHECK((r.read(isr) & repok) == repok);
}

TEST_CASE(
    "inferred: with CKSEL 1 a REPOK at an update edge applies after that edge's update, which "
    "reloads the old REP, the new one taking effect at the next update")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  r.write(cfgr, cksel);
  r.write(cr, enable);
  r.write(arr, 2);
  r.write(cr, enable | cntstrt);
  auto const rises = pulses(pins, 5, r.now_ps() + 20 * cycle, 18);
  r.idle_to(rises[4] + cycle);
  REQUIRE(r.read(cnt) == 0);
  r.write(rcr, 1);
  r.idle_to(rises[7] + cycle);
  CHECK((r.read(isr) & repok) == repok);
  CHECK(r.read(cnt) == 0);
  r.idle_to(rises[8] + cycle);
  CHECK((r.read(isr) & ue) == ue);
  r.write(icr, ue);
  r.idle_to(rises[11] + cycle);
  CHECK((r.read(isr) & ue) == ue);
  r.write(icr, ue);
  r.idle_to(rises[16] + cycle);
  CHECK((r.read(isr) & ue) == 0);
  r.idle_to(rises[17] + cycle);
  CHECK((r.read(isr) & ue) == ue);
}

TEST_CASE(
    "inferred: with COUNTMODE 1 an ARROK due at the instant of a PB5 edge applies before the "
    "edge counts, so the edge leaving the new ARR overflows")
{
  rig        r;
  auto&      pins  = lptim1_in1(r);
  auto const t     = r.started(60, 100, cntstrt, 0, countmode);
  auto const rises = pulses(pins, 5, t + 20 * cycle, 4);
  r.idle_to(rises[2] - 3 * cycle);
  REQUIRE(r.read(cnt) == 2);
  r.write(arr, 2);
  r.idle_to(rises[2]);
  CHECK(r.read(cnt) == 0);
  CHECK((r.read(isr) & (arrok | arrm)) == (arrok | arrm));
  r.idle_to(rises[3]);
  CHECK(r.read(cnt) == 1);
}

namespace
{
constexpr std::uint32_t trig_falling = 2u << 17;
constexpr std::uint32_t trig_both    = 3u << 17;
constexpr std::uint32_t timout       = 1u << 19;

constexpr auto trigsel(std::uint32_t value) -> std::uint32_t
{
  return value << 13;
}

constexpr auto trgflt(std::uint32_t value) -> std::uint32_t
{
  return value << 6;
}

auto lptim1_etr(rig const& r) -> stm32::gpio_port&
{
  return in1_pin(r, "GPIOB", 6, 1);
}
}

TEST_CASE(
    "RM0456 58.4.7: a rising PB6 LPTIM1_ETR edge with TRIGEN 01 sets EXTTRIG at the edge and "
    "starts the armed counter two counter clocks later, PRESC 0 and 3")
{
  for(auto const config : {rising, rising | presc(3)})
  {
    CAPTURE(config);
    rig        r;
    auto&      pins = lptim1_etr(r);
    auto const t    = r.started(60, 100, cntstrt, exttrig, config);
    auto const k    = (config == rising ? 1u : 8u) * cycle;
    r.idle_to(t + 20 * cycle);
    CHECK(r.read(cnt) == 0);
    CHECK((r.read(isr) & exttrig) == 0);
    auto const e = r.now_ps() + 4 * cycle;
    pins.drive_at(6, true, vtime{e});
    r.idle_to(e - cycle);
    CHECK_FALSE(r.irq_pending());
    r.idle_to(e);
    CHECK(r.irq_pending());
    CHECK((r.read(isr) & exttrig) == exttrig);
    CHECK(r.pending() == e + 3 * k);
    r.idle_to(e + 3 * k - cycle);
    CHECK(r.read(cnt) == 0);
    r.idle_to(e + 3 * k);
    CHECK(r.read(cnt) == 1);
    CHECK(r.logged.empty());
  }
}

TEST_CASE("RM0456 58.7.10 TRIGEN: 10 starts on the falling PB6 edge, 11 on either edge")
{
  for(auto const& [config, rise_starts] : {
          std::pair{trig_falling, false},
          std::pair{trig_both,    true },
  })
  {
    CAPTURE(config);
    rig        r;
    auto&      pins = lptim1_etr(r);
    auto const t    = r.started(60, 100, cntstrt, 0, config);
    auto const e    = t + 20 * cycle;
    pins.drive_at(6, true, vtime{e});
    pins.drive_at(6, false, vtime{e + 10 * cycle});
    r.idle_to(e + 5 * cycle);
    CHECK(r.read(cnt) == (rise_starts ? 3u : 0u));
    r.idle_to(e + 15 * cycle);
    CHECK(r.read(cnt) == (rise_starts ? 13u : 3u));
  }
}

TEST_CASE("inferred: a PB6 trigger before CNTSTRT or SNGSTRT starts nothing and sets no EXTTRIG")
{
  rig   r;
  auto& pins = lptim1_etr(r);
  r.enable_with(0, rising);
  r.settle_values(60, 100);
  auto const e = r.now_ps() + 4 * cycle;
  pins.drive_at(6, true, vtime{e});
  r.idle_to(e + 10 * cycle);
  CHECK((r.read(isr) & exttrig) == 0);
  r.write(cr, enable | cntstrt);
  r.idle_to(e + 30 * cycle);
  CHECK(r.read(cnt) == 0);
}

TEST_CASE(
    "RM0456 58.4.7 and 58.4.9: a trigger while the counter runs is ignored with no EXTTRIG, with "
    "TIMOUT 1 it sets EXTTRIG and restarts the counter and the repetition counter two clocks "
    "later, the latency inferred")
{
  for(auto const config : {rising, rising | timout})
  {
    CAPTURE(config);
    rig        r;
    auto&      pins = lptim1_etr(r);
    auto const t    = r.started(60, 9, cntstrt, 0, config, 2);
    auto const e    = t + 20 * cycle;
    pins.drive_at(6, true, vtime{e});
    pins.drive_at(6, false, vtime{e + 5 * cycle});
    r.idle_to(e + 3 * cycle);
    REQUIRE(r.read(cnt) == 1);
    r.write(icr, exttrig | arrm | ue);
    auto const again = r.now_ps() + 4 * cycle;
    pins.drive_at(6, true, vtime{again});
    r.idle_to(again);
    CHECK((r.read(isr) & exttrig) == (config == rising ? 0u : exttrig));
    r.idle_to(again + 2 * cycle);
    CHECK(r.read(cnt) == (config == rising ? 7u : 0u));
    r.idle_to(again + 3 * cycle);
    CHECK(r.read(cnt) == (config == rising ? 8u : 1u));
    r.idle_to(again + 28 * cycle);
    CHECK((r.read(isr) & ue) == (config == rising ? ue : 0u));
    r.idle_to(again + 33 * cycle);
    CHECK((r.read(isr) & ue) == ue);
  }
}

TEST_CASE(
    "RM0456 58.4.8: with SNGSTRT each trigger after the update event starts a new run, with WAVE 1 "
    "only the first trigger starts one")
{
  for(auto const config : {rising, rising | wave})
  {
    CAPTURE(config);
    rig        r;
    auto&      pins = lptim1_etr(r);
    auto const t    = r.started(60, 3, sngstrt, 0, config);
    auto const e    = t + 20 * cycle;
    pins.drive_at(6, true, vtime{e});
    pins.drive_at(6, false, vtime{e + 5 * cycle});
    r.idle_to(e + 6 * cycle);
    CHECK(r.read(cnt) == 0);
    CHECK((r.read(isr) & arrm) == arrm);
    r.write(icr, exttrig);
    auto const again = e + 20 * cycle;
    pins.drive_at(6, true, vtime{again});
    r.idle_to(again + 4 * cycle);
    CHECK(r.read(cnt) == (config == rising ? 2u : 0u));
    CHECK((r.read(isr) & exttrig) == (config == rising ? exttrig : 0u));
  }
}

TEST_CASE(
    "RM0456 58.7.10 TRGFLT: a PB6 level change starts the counter once held 2, 4 or 8 kernel "
    "clocks, a shorter pulse starts nothing, the sampling phase taken as the edge, inferred")
{
  for(std::uint32_t code = 1; code < 4; ++code)
  {
    CAPTURE(code);
    rig        r;
    auto&      pins     = lptim1_etr(r);
    auto const n        = std::uint64_t{1} << code;
    auto const t        = r.started(60, 100, cntstrt, 0, rising | trgflt(code));
    auto const short_at = t + 20 * cycle;
    pins.drive_at(6, true, vtime{short_at});
    pins.drive_at(6, false, vtime{short_at + (n - 1) * cycle});
    r.idle_to(short_at + 20 * cycle);
    CHECK((r.read(isr) & exttrig) == 0);
    auto const e = r.now_ps() + 2 * cycle;
    pins.drive_at(6, true, vtime{e});
    r.idle_to(e);
    CHECK(r.pending() == e + n * cycle);
    r.idle_to(e + n * cycle - cycle);
    CHECK((r.read(isr) & exttrig) == 0);
    r.idle_to(e + n * cycle);
    CHECK((r.read(isr) & exttrig) == exttrig);
    r.idle_to(e + n * cycle + 3 * cycle);
    CHECK(r.read(cnt) == 1);
  }
}

TEST_CASE(
    "RM0456 58.4.3 Table 604: TRIGSEL 1 to 7 name the instance's trigger input, log one warning, "
    "raise under strict_peripherals and leave the counter stopped, on all four LPTIM")
{
  constexpr auto table = std::to_array<std::array<std::string_view, 7>>({
      {"rtc_alra_trg", "rtc_alrb_trg", "tamp_trg1",    "tamp_trg2",    "lpdma_ch2_tc", "comp1_out",
       "comp2_out"},
      {"rtc_alra_trg", "rtc_alrb_trg", "tamp_trg1",    "gpdma_ch0_tc", "gpdma_ch4_tc", "comp1_out",
       "comp2_out"},
      {"rtc_alra_trg", "rtc_alrb_trg", "tamp_trg1",    "lpdma_ch1_tc", "tamp_trg3",    "comp1_out",
       "comp2_out"},
      {"rtc_alra_trg", "rtc_alrb_trg", "lpdma_ch0_tc", "tamp_trg2",    "tamp_trg3",    "comp1_out",
       "comp2_out"},
  });
  static_assert(table.size() == units.size());
  for(std::size_t index = 0; index < units.size(); ++index)
  {
    for(std::uint32_t source = 1; source < 8; ++source)
    {
      auto const text = std::format("TRIGSEL {}, {},", source, table[index][source - 1]);
      CAPTURE(units[index].name);
      CAPTURE(text);
      rig        r{units[index]};
      auto const t = r.started(2, 10, cntstrt, 0, rising | trigsel(source));
      CHECK(r.warnings(text) == 1);
      CHECK(r.warnings("TRIGSEL") == 1);
      r.idle_to(t + 20 * cycle);
      CHECK(r.read(cnt) == 0);
      rig strict{units[index], true};
      CHECK_THROWS_AS(
          static_cast<void>(strict.started(2, 10, cntstrt, 0, rising | trigsel(source))),
          popcorn::model_error);
    }
  }
}

TEST_CASE(
    "checklist 66 and 72: a snapshot of an armed counter and of a pending TRGFLT change restores "
    "into a machine with no reset, the state and the start on the trigger equal")
{
  for(auto const pending : {false, true})
  {
    CAPTURE(pending);
    rig        r;
    auto&      pins = lptim1_etr(r);
    auto const t    = r.started(60, 100, cntstrt, exttrig, rising | (pending ? trgflt(2) : 0u));
    auto const e    = t + 20 * cycle;
    if(pending)
    {
      pins.drive_at(6, true, vtime{e});
      r.idle_to(e + cycle);
      REQUIRE(r.pending() == e + 4 * cycle);
      REQUIRE((r.read(isr) & exttrig) == 0);
    }
    else
    {
      r.idle_to(e);
    }
    auto const saved = r.f->target->snapshot();
    rig        other{no_reset};
    other.f->target->restore(
        core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    CHECK(other.saved() == r.saved());
    for(auto const* one : {&r, &other})
    {
      if(not pending)
      {
        auto* const port = one->f->model_of<stm32::gpio_port>("GPIOB");
        REQUIRE(port != nullptr);
        port->drive_at(6, true, vtime{e + 4 * cycle});
      }
      auto const edge = e + 4 * cycle;
      one->idle_to(edge - cycle);
      CHECK((one->read(isr) & exttrig) == 0);
      CHECK(one->read(cnt) == 0);
      one->idle_to(edge);
      CHECK((one->read(isr) & exttrig) == exttrig);
      CHECK(one->pending() == edge + 3 * cycle);
      one->idle_to(edge + 3 * cycle);
      CHECK(one->read(cnt) == 1);
      one->idle_to(edge + 10 * cycle);
      CHECK(one->read(cnt) == 8);
    }
  }
}

TEST_CASE(
    "inferred: SNGSTRT written during a trigger-started continuous run re-arms the one-shot "
    "mode, the run stops at its update event and the next trigger starts a one-shot run")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(60, 3, cntstrt, 0, rising);
  auto const e    = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  pins.drive_at(6, false, vtime{e + 3 * cycle});
  r.idle_to(e + 6 * cycle);
  r.write(cr, enable | sngstrt);
  r.idle_to(e + 11 * cycle);
  CHECK(r.read(cnt) == 0);
  r.idle_to(e + 18 * cycle);
  CHECK(r.read(cnt) == 0);
  pins.drive_at(6, true, vtime{e + 20 * cycle});
  r.idle_to(e + 24 * cycle);
  CHECK(r.read(cnt) == 2);
  r.idle_to(e + 29 * cycle);
  CHECK(r.read(cnt) == 0);
}

TEST_CASE(
    "checklist 31: an LPTIM1RST pulse with PB6 high takes the trigger level, and TRIGEN 10 "
    "starts on its fall")
{
  rig   r;
  auto& pins = lptim1_etr(r);
  pins.drive(6, true);
  rcc_reset_pulse(*r.f, r.u.reset_reg, r.u.reset_bit);
  auto const t = r.started(60, 100, cntstrt, 0, trig_falling);
  auto const e = t + 20 * cycle;
  pins.drive_at(6, false, vtime{e});
  r.idle_to(e + 5 * cycle);
  CHECK(r.read(cnt) == 3);
}

TEST_CASE(
    "RM0456 58.4.2 and the U575 datasheet: each LPTIM starts on its own ETR pin, PB6 AF1, PA5 "
    "AF14, PB14 AF2 and PD2 AF13")
{
  struct pin_of
  {
    unit             which;
    std::string_view port;
    std::uint8_t     pin;
    std::uint32_t    af;
  };
  for(auto const& [which, port, pin, af] : {
          pin_of{units[0], "GPIOB", 6,  1 },
          pin_of{units[1], "GPIOA", 5,  14},
          pin_of{units[2], "GPIOB", 14, 2 },
          pin_of{units[3], "GPIOD", 2,  13},
  })
  {
    CAPTURE(which.name);
    rig        r{which};
    auto&      pins = in1_pin(r, port, pin, af);
    auto const t    = r.started(60, 100, cntstrt, 0, rising);
    auto const e    = t + 20 * cycle;
    pins.drive_at(pin, true, vtime{e});
    r.idle_to(e + 5 * cycle);
    CHECK(r.read(cnt) == 3);
  }
}

TEST_CASE(
    "inferred: a CNTSTRT written while SNGSTRT waits for its trigger arms the continuous "
    "mode at the write")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(60, 3, sngstrt, 0, rising);
  r.write(cr, enable | cntstrt);
  auto const e = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  r.idle_to(e + 12 * cycle);
  CHECK(r.read(cnt) == 2);
}

TEST_CASE(
    "RM0456 58.4.7 with CKSEL 1: a PB6 trigger starts the counter after two PB5 edges, the third "
    "counts, inferred")
{
  rig   r;
  auto& clock   = lptim1_in1(r);
  auto& trigger = lptim1_etr(r);
  r.write(cfgr, cksel | rising);
  r.write(cr, enable);
  r.write(arr, 100);
  r.write(cr, enable | cntstrt);
  auto const rises = pulses(clock, 5, r.now_ps() + 20 * cycle, 10);
  auto const e     = rises[5] + 2 * cycle;
  trigger.drive_at(6, true, vtime{e});
  r.idle_to(e);
  CHECK((r.read(isr) & exttrig) == exttrig);
  r.idle_to(rises[7] + cycle);
  CHECK(r.read(cnt) == 0);
  r.idle_to(rises[8] + cycle);
  CHECK(r.read(cnt) == 1);
  r.idle_to(rises[9] + cycle);
  CHECK(r.read(cnt) == 2);
}

TEST_CASE(
    "inferred: a PB6 trigger with the kernel clock stopped starts nothing and sets no EXTTRIG")
{
  rig        r;
  auto&      pins = lptim1_etr(r);
  auto const t    = r.started(60, 100, cntstrt, 0, rising);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
  auto const e = t + 20 * cycle;
  pins.drive_at(6, true, vtime{e});
  r.idle_to(e + 10 * cycle);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  r.idle_to(e + 30 * cycle);
  CHECK((r.read(isr) & exttrig) == 0);
  CHECK(r.read(cnt) == 0);
}

TEST_CASE(
    "inferred: with COUNTMODE 1 an ARROK due at the instant of a PB5 drive or of a MODER write "
    "that routes a high PB5 applies before that edge counts")
{
  for(auto const by_mode : {false, true})
  {
    CAPTURE(by_mode);
    rig        r;
    auto&      pins  = lptim1_in1(r);
    auto const t     = r.started(60, 100, cntstrt, 0, countmode);
    auto const rises = pulses(pins, 5, t + 20 * cycle, 2);
    r.idle_to(rises[1] + 6 * cycle);
    REQUIRE(r.read(cnt) == 2);
    auto const gpiob = r.f->target->peripheral("GPIOB").description().base;
    auto const moder = r.read_at(gpiob);
    if(by_mode)
    {
      r.write_at(gpiob, moder & ~(3u << 10));
      pins.drive(5, true);
    }
    auto const x = r.now_ps();
    r.write(arr, 2);
    r.idle_to(x + 3 * cycle);
    if(by_mode)
    {
      r.write_at(gpiob, moder);
    }
    else
    {
      pins.drive(5, true);
    }
    CHECK(r.read(cnt) == 0);
    CHECK((r.read(isr) & (arrok | arrm)) == (arrok | arrm));
  }
}

namespace
{
constexpr std::uint32_t counting_up   = 1u << 5;
constexpr std::uint32_t counting_down = 1u << 6;
constexpr std::uint8_t  pin_in1       = 5;
constexpr std::uint8_t  pin_in2       = 7;

auto lptim1_in2(rig const& r) -> stm32::gpio_port&
{
  return in1_pin(r, "GPIOB", pin_in2, 1);
}

struct quadrature
{
  std::uint32_t config;
  bool          second;
  bool          rise;
  bool          opposite;
  int           direction;
};
}

TEST_CASE(
    "RM0456 58.4.15 Table 612: each CKPOL sub-mode counts up, down or not at all by the edge, the "
    "input and the level of the other input, DOWN set by the first down edge")
{
  constexpr auto up   = +1;
  constexpr auto down = -1;
  constexpr auto none = 0;
  for(auto const& [config, second, rise, opposite, direction] : {
          quadrature{enc,           false, true,  true,  down},
          quadrature{enc,           false, true,  false, up  },
          quadrature{enc,           false, false, true,  none},
          quadrature{enc,           false, false, false, none},
          quadrature{enc,           true,  true,  true,  up  },
          quadrature{enc,           true,  true,  false, down},
          quadrature{enc,           true,  false, true,  none},
          quadrature{enc,           true,  false, false, none},
          quadrature{enc | falling, false, true,  true,  none},
          quadrature{enc | falling, false, true,  false, none},
          quadrature{enc | falling, false, false, true,  up  },
          quadrature{enc | falling, false, false, false, down},
          quadrature{enc | falling, true,  true,  true,  none},
          quadrature{enc | falling, true,  true,  false, none},
          quadrature{enc | falling, true,  false, true,  down},
          quadrature{enc | falling, true,  false, false, up  },
          quadrature{enc | both,    false, true,  true,  down},
          quadrature{enc | both,    false, true,  false, up  },
          quadrature{enc | both,    false, false, true,  up  },
          quadrature{enc | both,    false, false, false, down},
          quadrature{enc | both,    true,  true,  true,  up  },
          quadrature{enc | both,    true,  true,  false, down},
          quadrature{enc | both,    true,  false, true,  down},
          quadrature{enc | both,    true,  false, false, up  },
  })
  {
    CAPTURE(config);
    CAPTURE(second);
    CAPTURE(rise);
    CAPTURE(opposite);
    rig   r;
    auto& pins = lptim1_in1(r);
    static_cast<void>(lptim1_in2(r));
    auto const moving = second ? pin_in2 : pin_in1;
    auto const other  = second ? pin_in1 : pin_in2;
    pins.drive(moving, not rise);
    pins.drive(other, opposite);
    auto const t = r.started(60, 100, cntstrt, 0, config);
    auto const e = t + 20 * cycle;
    pins.drive_at(moving, rise, vtime{e});
    r.idle_to(e - cycle);
    CHECK(r.read(cnt) == 0);
    r.idle_to(e);
    CHECK(r.read(cnt) == (direction == up ? 1u : direction == down ? 100u : 0u));
    CHECK(
        (r.read(isr) & (counting_up | counting_down)) == (direction == down ? counting_down : 0u));
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "RM0456 58.4.15 and 58.7.2: a quadrature sequence counts between 0 and ARR in both "
    "directions, the first edge of a direction sets DOWN or UP and later edges leave it, raising "
    "the IRQ through UPIE and DOWNIE")
{
  struct step
  {
    std::uint8_t  pin;
    bool          level;
    std::uint32_t count;
    std::uint32_t flags;
  };
  rig   r;
  auto& pins = lptim1_in1(r);
  static_cast<void>(lptim1_in2(r));
  auto const t = r.started(60, 4, cntstrt, counting_up | counting_down, enc | both);
  auto       e = t + 20 * cycle;
  for(auto const& [pin, level, count, flags] : {
          step{pin_in1, true,  1, 0            },
          step{pin_in2, true,  2, 0            },
          step{pin_in1, false, 3, 0            },
          step{pin_in2, false, 4, 0            },
          step{pin_in1, true,  0, 0            },
          step{pin_in1, false, 4, counting_down},
          step{pin_in2, true,  3, 0            },
          step{pin_in1, true,  2, 0            },
          step{pin_in2, false, 1, 0            },
          step{pin_in2, true,  2, counting_up  },
  })
  {
    CAPTURE(e);
    CAPTURE(count);
    pins.drive_at(pin, level, vtime{e});
    r.idle_to(e - cycle);
    r.idle_to(e);
    CHECK(r.read(cnt) == count);
    CHECK((r.read(isr) & (counting_up | counting_down)) == flags);
    CHECK(r.irq_pending() == (flags != 0));
    r.write(icr, counting_up | counting_down);
    r.clear_irq();
    e += 10 * cycle;
  }
  CHECK(r.warnings("DIER enable of") == 0);
}

TEST_CASE(
    "RM0456 58.4.19 and 58.4.15: counting down from 0 gives ARR, CC1IF and ARRM set at the counted "
    "edge that leaves CCR1 and ARR in the down direction, inferred")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  lptim1_in2(r).drive(pin_in2, true);
  auto const t     = r.started(2, 5, cntstrt, 0, enc);
  auto const rises = pulses(pins, pin_in1, t + 20 * cycle, 7);
  auto const flags = [&](std::size_t index) {
    r.idle_to(rises[index]);
    return r.read(isr) & (cc1if | arrm | ue);
  };
  CHECK(flags(0) == 0);
  CHECK(r.read(cnt) == 5);
  CHECK(flags(1) == (arrm | ue));
  CHECK(r.read(cnt) == 4);
  r.write(icr, arrm | ue);
  CHECK(flags(3) == 0);
  CHECK(r.read(cnt) == 2);
  CHECK(flags(4) == cc1if);
  CHECK(r.read(cnt) == 1);
}

TEST_CASE(
    "RM0456 58.4.16 and 58.4.15, inferred: a down wrap from 0 counts as an overflow, REP 1 makes "
    "every second wrap the update event and UE sets at the next counted edge")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  lptim1_in2(r).drive(pin_in2, true);
  auto const t     = r.started(60, 1, cntstrt, 0, enc, 1);
  auto const rises = pulses(pins, pin_in1, t + 20 * cycle, 4);
  for(std::size_t index = 0; index < 3; ++index)
  {
    r.idle_to(rises[index]);
    CHECK((r.read(isr) & ue) == 0);
  }
  r.idle_to(rises[3]);
  CHECK((r.read(isr) & ue) == ue);
}

TEST_CASE(
    "RM0456 58.4.11 and 58.4.15, inferred: with PRELOAD 1 a pending ARR applies at the first down "
    "wrap and that wrap loads the new ARR")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  lptim1_in2(r).drive(pin_in2, true);
  auto const t = r.started(60, 10, cntstrt, 0, enc | preload);
  r.write(arr, 3);
  r.idle_to(r.now_ps() + 4 * cycle);
  auto const e = r.now_ps() + 4 * cycle;
  pins.drive_at(pin_in1, true, vtime{e});
  r.idle_to(e);
  CHECK(r.read(cnt) == 3);
  CHECK(t < e);
}

TEST_CASE(
    "RM0456 58.4.15: ENABLE written to 0 and a new start count upwards again, DOWN sets once more")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  lptim1_in2(r).drive(pin_in2, true);
  for(auto const round : {0, 1})
  {
    CAPTURE(round);
    auto const t = r.started(60, 100, cntstrt, 0, enc);
    auto const e = t + 20 * cycle;
    pins.drive_at(pin_in1, true, vtime{e});
    r.idle_to(e);
    CHECK(r.read(cnt) == 100);
    CHECK((r.read(isr) & counting_down) == counting_down);
    pins.drive(pin_in1, false);
    r.write(icr, counting_down);
    r.write(cr, 0);
  }
}

TEST_CASE(
    "RM0456 58.4.5 and 58.7.10 CKFLT: the filter holds back both encoder inputs, the direction "
    "takes the filtered level of the other input, inferred")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  static_cast<void>(lptim1_in2(r));
  auto const t = r.started(60, 100, cntstrt, 0, enc | ckflt(1));
  auto const e = t + 20 * cycle;
  pins.drive_at(pin_in1, true, vtime{e});
  pins.drive_at(pin_in2, true, vtime{e + cycle});
  r.idle_to(e + cycle);
  CHECK(r.read(cnt) == 0);
  r.idle_to(e + 2 * cycle);
  CHECK(r.read(cnt) == 1);
  CHECK((r.read(isr) & (counting_up | counting_down)) == 0);
  r.idle_to(e + 3 * cycle);
  CHECK(r.read(cnt) == 2);
}

TEST_CASE(
    "inferred: an encoder edge with the kernel clock stopped is dropped, the level it leaves still "
    "sets the direction of the next edge")
{
  rig   r;
  auto& pins = lptim1_in1(r);
  static_cast<void>(lptim1_in2(r));
  auto const t = r.started(60, 100, cntstrt, 0, enc);
  r.idle_to(t + 20 * cycle);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
  pins.drive(pin_in2, true);
  pins.drive(pin_in1, true);
  pins.drive(pin_in1, false);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  r.idle_to(r.now_ps() + 10 * cycle);
  CHECK(r.read(cnt) == 0);
  pins.drive(pin_in1, true);
  r.idle_to(r.now_ps() + cycle);
  CHECK(r.read(cnt) == 100);
}

TEST_CASE(
    "inferred: a CFGR2 write that changes IN2SEL counts the level change of input 2 as an edge, "
    "IN2SEL not 00 logs one warning and raises under strict_peripherals")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig   r{units[0], strict};
    auto& pins = lptim1_in1(r);
    lptim1_in2(r).drive(pin_in2, true);
    static_cast<void>(pins);
    auto const t = r.started(60, 100, cntstrt, 0, enc | both);
    r.idle_to(t + 20 * cycle);
    if(strict)
    {
      CHECK_THROWS_AS(r.write(cfgr2, 1u << 4), popcorn::model_error);
      continue;
    }
    r.write(cfgr2, 1u << 4);
    r.idle_to(r.now_ps() + cycle);
    CHECK(r.read(cnt) == 1);
    CHECK(r.warnings("IN2SEL not 00") == 1);
    r.write(cfgr2, 0);
    r.write(cfgr2, 1u << 4);
    CHECK(r.warnings("IN2SEL not 00") == 1);
  }
}

TEST_CASE(
    "RM0456 58.4.15 and 58.7.10: SNGSTRT with ENC 1, ENC 1 with CKSEL 1 and PRESC not 0 with ENC 1 "
    "log one warning each and raise under strict_peripherals, IN2SEL not 00 only with ENC 1")
{
  using refusal = std::tuple<std::uint32_t, bool, std::string_view>;
  for(auto const& [config, single, text] : {
          refusal{enc,            true,  "SNGSTRT with ENC 1"                   },
          refusal{enc | cksel,    false, "ENC 1 with CKSEL 1"                   },
          refusal{enc | presc(1), false, "PRESC not 0 with COUNTMODE 1 or ENC 1"},
  })
  {
    CAPTURE(text);
    for(auto const strict : {false, true})
    {
      CAPTURE(strict);
      rig r{units[0], strict};
      r.write(cfgr, config);
      r.write(cr, enable);
      auto const mode = single ? sngstrt : cntstrt;
      if(strict)
      {
        CHECK_THROWS_AS(r.write(cr, enable | mode), popcorn::model_error);
        continue;
      }
      r.write(cr, enable | mode);
      r.write(cr, 0);
      r.write(cr, enable);
      r.write(cr, enable | mode);
      CHECK(r.warnings(text) == 1);
    }
  }
  rig plain;
  plain.write(cfgr2, 1u << 4);
  static_cast<void>(plain.started(60, 100, cntstrt, 0, 0));
  CHECK(plain.warnings("IN2SEL") == 0);
  rig input_2{units[0], true};
  input_2.write(cfgr2, 1u << 4);
  CHECK_THROWS_AS(
      static_cast<void>(input_2.started(60, 100, cntstrt, 0, enc)), popcorn::model_error);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot of an encoder counting down and of an input 2 change pending "
    "under CKFLT restores into a machine with no reset, the direction and the count equal")
{
  for(auto const filtered : {false, true})
  {
    CAPTURE(filtered);
    rig   r;
    auto& pins = lptim1_in1(r);
    static_cast<void>(lptim1_in2(r));
    auto const t = r.started(60, 100, cntstrt, 0, enc | (filtered ? ckflt(1) : 0u));
    auto const e = t + 20 * cycle;
    if(filtered)
    {
      pins.drive_at(pin_in2, true, vtime{e});
      r.idle_to(e + cycle);
      REQUIRE(r.pending() == e + 2 * cycle);
      REQUIRE(r.read(cnt) == 0);
    }
    else
    {
      pins.drive(pin_in2, true);
      pins.drive_at(pin_in1, true, vtime{e});
      pins.drive_at(pin_in1, false, vtime{e + 5 * cycle});
      r.idle_to(e + 6 * cycle);
      REQUIRE(r.read(cnt) == 100);
      REQUIRE((r.read(isr) & counting_down) == counting_down);
      r.write(icr, counting_down);
    }
    auto const saved = r.f->target->snapshot();
    rig        other{no_reset};
    other.f->target->restore(
        core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    CHECK(other.saved() == r.saved());
    for(auto const* one : {&r, &other})
    {
      auto* const port = one->f->model_of<stm32::gpio_port>("GPIOB");
      REQUIRE(port != nullptr);
      if(filtered)
      {
        CHECK(one->read(cnt) == 0);
        one->idle_to(e + 2 * cycle);
        CHECK(one->read(cnt) == 100);
        CHECK((one->read(isr) & (counting_up | counting_down)) == counting_down);
        continue;
      }
      port->drive_at(pin_in1, true, vtime{e + 10 * cycle});
      one->idle_to(e + 10 * cycle);
      CHECK(one->read(cnt) == 99);
      CHECK((one->read(isr) & (counting_up | counting_down)) == 0);
    }
  }
}

TEST_CASE(
    "RM0456 58.3 Table 599: LPTIM3 and LPTIM4 have no encoder, ENC is reserved and the counter "
    "counts the kernel clock")
{
  for(auto const& which : {units[2], units[3]})
  {
    CAPTURE(which.name);
    rig        r{which};
    auto const t = r.started(6, 10, cntstrt, 0, enc);
    r.idle_to(t + 7 * cycle);
    CHECK((r.read(isr) & cc1if) == cc1if);
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "checklist 39: a description without ISR UP or DOWN, DIER UPIE or DOWNIE, ICR UPCF or "
    "DOWNCF or CFGR ENC on LPTIM1 is refused at construction")
{
  for(auto const& [reg_name, field_name] : {
          std::pair{std::string_view{"ISR_output"},  std::string_view{"UP"}    },
          std::pair{std::string_view{"ISR_output"},  std::string_view{"DOWN"}  },
          std::pair{std::string_view{"DIER_output"}, std::string_view{"UPIE"}  },
          std::pair{std::string_view{"DIER_output"}, std::string_view{"DOWNIE"}},
          std::pair{std::string_view{"ICR_output"},  std::string_view{"UPCF"}  },
          std::pair{std::string_view{"ICR_output"},  std::string_view{"DOWNCF"}},
          std::pair{std::string_view{"CFGR"},        std::string_view{"ENC"}   },
  })
  {
    CAPTURE(field_name);
    auto desc  = u575();
    auto found = std::ranges::find(
        desc.peripherals, std::string_view{"LPTIM1"}, &popcorn::peripheral_desc::name);
    REQUIRE(found != desc.peripherals.end());
    auto owner = std::ranges::find(found->registers, reg_name, &popcorn::register_desc::name);
    REQUIRE(owner != found->registers.end());
    REQUIRE(
        std::erase_if(owner->fields, [&](auto const& one) { return one.name == field_name; }) == 1);
    CHECK_THROWS_AS(rig(units[0], false, desc), popcorn::config_error);
  }
}

namespace
{
constexpr std::uint32_t falling_edge  = 1u << 2;
constexpr std::uint32_t both_edges    = 3u << 2;
constexpr std::uint32_t reserved_cc1p = 2u << 2;

constexpr auto ic1f(std::uint32_t value) -> std::uint32_t
{
  return value << 12;
}

constexpr auto ic1psc(std::uint32_t value) -> std::uint32_t
{
  return value << 8;
}

constexpr auto table_613 = std::to_array<std::array<std::uint32_t, 4>>({
    {2, 7, 9, 13},
    {3, 5, 6, 8 },
    {2, 3, 4, 5 },
    {2, 2, 3, 3 },
    {2, 2, 2, 2 },
    {2, 2, 2, 2 },
    {2, 2, 2, 2 },
    {2, 2, 2, 2 },
});

struct capture_pin
{
  std::string_view port;
  std::uint8_t     pin;
  std::uint32_t    af;
};

constexpr auto capture_pins = std::to_array<std::array<capture_pin, 2>>({
    {capture_pin{"GPIOB", 2, 1},  capture_pin{"GPIOB", 4, 1} },
    {capture_pin{"GPIOA", 4, 14}, capture_pin{"GPIOA", 7, 13}},
    {capture_pin{"GPIOB", 0, 4},  capture_pin{"GPIOB", 1, 4} },
});

auto capture_input(rig const& r, std::size_t channel) -> stm32::gpio_port&
{
  auto const  index = static_cast<std::size_t>(r.u.name.back() - '1');
  auto const& one   = capture_pins[index][channel];
  return in1_pin(r, one.port, one.pin, one.af);
}

auto capture_pin_of(rig const& r, std::size_t channel) -> std::uint8_t
{
  return capture_pins[static_cast<std::size_t>(r.u.name.back() - '1')][channel].pin;
}

auto captured_with(rig const& r, std::uint32_t channels, std::uint32_t config = 0) -> std::uint64_t
{
  r.write(ccmr1, channels);
  return r.started(60, 1000, cntstrt, 0, config);
}
}

TEST_CASE(
    "RM0456 58.4.18 and Table 613: a PB2 rising edge latches CNT into CCR1 and sets CC1IF at the "
    "counter clock that takes CNT 2 past the count at the edge, the instant inferred, channel 1 "
    "compares nothing")
{
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e);
  auto const e    = t + 20 * cycle + cycle / 2;
  pins.drive_at(2, true, vtime{e});
  r.idle_to(e + cycle / 2);
  CHECK(r.pending() == t + 22 * cycle);
  r.idle_to(t + 21 * cycle);
  CHECK(r.read(isr) == r.cc2);
  CHECK(r.read(ccr1) == 0);
  r.idle_to(t + 22 * cycle);
  CHECK(r.read(isr) == (cc1if | r.cc2));
  CHECK(r.read(ccr1) == 22);
  CHECK(r.logged.empty());
}

TEST_CASE(
    "RM0456 58.4.18 and Table 613: for every PRESC and ICxF, CCR1 minus the offset is the count "
    "at the edge and CC1IF sets at the counter clock CCR1 names")
{
  for(std::uint32_t prescaler = 0; prescaler < 8; ++prescaler)
  {
    for(std::uint32_t filter = 0; filter < 4; ++filter)
    {
      CAPTURE(prescaler);
      CAPTURE(filter);
      rig        r;
      auto&      pins   = capture_input(r, 0);
      auto const unit   = cycle << prescaler;
      auto const t      = captured_with(r, cc1sel | cc1e | ic1f(filter), presc(prescaler));
      auto const offset = table_613[prescaler][filter];
      auto const e      = t + 4 * unit + unit / 2;
      auto const due    = t + (4 + offset) * unit;
      pins.drive_at(2, true, vtime{e});
      r.idle_to(e + (filter == 0 ? 0u : 1u << filter) * cycle + cycle / 2);
      CHECK(r.pending() == due);
      r.idle_to(due - cycle);
      CHECK((r.read(isr) & cc1if) == 0);
      r.idle_to(due);
      CHECK((r.read(isr) & cc1if) == cc1if);
      CHECK(r.read(ccr1) == 4 + offset);
    }
  }
}

TEST_CASE(
    "RM0456 58.7.17 CC1P: 00 captures on the rising edge, 01 on the falling edge and 11 on both")
{
  for(auto const& [polarity, rises, falls] : {
          std::tuple{0u,           true,  false},
          std::tuple{falling_edge, false, true },
          std::tuple{both_edges,   true,  true },
  })
  {
    CAPTURE(polarity);
    rig        r;
    auto&      pins = capture_input(r, 0);
    auto const t    = captured_with(r, cc1sel | cc1e | polarity);
    auto const up   = t + 20 * cycle;
    pins.drive_at(2, true, vtime{up});
    pins.drive_at(2, false, vtime{up + 10 * cycle});
    r.idle_to(up + 2 * cycle);
    CHECK((r.read(isr) & cc1if) == (rises ? cc1if : 0u));
    CHECK(r.read(ccr1) == (rises ? 22u : 0u));
    r.write(icr, cc1if);
    r.idle_to(up + 12 * cycle);
    CHECK((r.read(isr) & cc1if) == (falls ? cc1if : 0u));
    CHECK(r.read(ccr1) == (falls ? 32u : rises ? 22u : 0u));
  }
}

TEST_CASE("RM0456 58.7.17 IC1PSC: CCR1 captures once every 1, 2, 4 and 8 rising edges")
{
  for(std::uint32_t prescaler = 0; prescaler < 4; ++prescaler)
  {
    CAPTURE(prescaler);
    rig        r;
    auto&      pins  = capture_input(r, 0);
    auto const t     = captured_with(r, cc1sel | cc1e | ic1psc(prescaler));
    auto const rises = pulses(pins, 2, t + 20 * cycle, 8);
    auto       seen  = std::vector<std::uint32_t>{};
    for(auto const at : rises)
    {
      r.idle_to(at + 2 * cycle);
      if((r.read(isr) & cc1if) != 0)
      {
        seen.push_back(r.read(ccr1));
        r.write(icr, cc1if);
      }
    }
    auto expected = std::vector<std::uint32_t>{};
    for(std::uint32_t edge = (1u << prescaler) - 1u; edge < 8; edge += 1u << prescaler)
    {
      expected.push_back(22 + 10 * edge);
    }
    CHECK(seen == expected);
  }
}

TEST_CASE(
    "RM0456 58.7.17 IC1F: a level change counts once it has held 2, 4 or 8 kernel clocks, a "
    "shorter pulse captures nothing, the sampling phase inferred")
{
  for(std::uint32_t filter = 1; filter < 4; ++filter)
  {
    CAPTURE(filter);
    rig        r;
    auto&      pins     = capture_input(r, 0);
    auto const t        = captured_with(r, cc1sel | cc1e | ic1f(filter));
    auto const n        = 1u << filter;
    auto const short_at = t + 20 * cycle;
    pins.drive_at(2, true, vtime{short_at});
    pins.drive_at(2, false, vtime{short_at + (n - 1) * cycle});
    r.idle_to(short_at + 20 * cycle);
    CHECK((r.read(isr) & cc1if) == 0);
    auto const e = t + 60 * cycle;
    pins.drive_at(2, true, vtime{e});
    r.idle_to(e + (n - 1) * cycle);
    CHECK(r.pending() == e + n * cycle);
    r.idle_to(e + 20 * cycle);
    CHECK((r.read(isr) & cc1if) == cc1if);
    CHECK(r.read(ccr1) == 60 + table_613[0][filter]);
  }
}

TEST_CASE(
    "RM0456 58.7.3: a CPU or DMA read of CCR1 clears CC1IF, a debugger read leaves it, CC1CF "
    "clears it, and a CCR1 write in capture mode is discarded with no CMP1OK, inferred")
{
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | both_edges);
  pins.drive_at(2, true, vtime{t + 20 * cycle});
  pins.drive_at(2, false, vtime{t + 30 * cycle});
  pins.drive_at(2, true, vtime{t + 40 * cycle});
  r.idle_to(t + 22 * cycle);
  CHECK(r.cpu_read(ccr1, popcorn::initiator::debugger) == 22);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.cpu_read(ccr1) == 22);
  CHECK((r.read(isr) & cc1if) == 0);
  r.idle_to(t + 32 * cycle);
  CHECK(r.cpu_read(ccr1, popcorn::initiator::dma) == 32);
  CHECK((r.read(isr) & cc1if) == 0);
  r.idle_to(t + 42 * cycle);
  r.write(icr, cc1if);
  CHECK((r.read(isr) & cc1if) == 0);
  r.write(ccr1, 7);
  r.idle_to(t + 50 * cycle);
  CHECK(r.read(ccr1) == 42);
  CHECK((r.read(isr) & cmp1ok) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE(
    "RM0456 58.3 Table 599: LPTIM1 to LPTIM3 capture on both channels from their own pins, CC2IF "
    "with CCR2, LPTIM4 takes no channel pin")
{
  for(auto const& which : {units[0], units[1], units[2]})
  {
    for(std::size_t channel = 0; channel < 2; ++channel)
    {
      CAPTURE(which.name);
      CAPTURE(channel);
      rig        r{which};
      auto&      pins = capture_input(r, channel);
      auto const flag = channel == 0 ? cc1if : cc2if;
      auto const mode = channel == 0 ? cc1sel | cc1e : cc2sel | cc2e;
      auto const t    = captured_with(r, mode);
      auto const e    = t + 20 * cycle;
      pins.drive_at(capture_pin_of(r, channel), true, vtime{e});
      r.idle_to(e + 2 * cycle);
      CHECK((r.read(isr) & flag) == flag);
      CHECK(r.read(channel == 0 ? ccr1 : ccr2) == 22);
      CHECK(r.read(channel == 0 ? ccr2 : ccr1) == (channel == 0 ? 0u : 60u));
    }
  }
  rig   last{units[3]};
  auto* sink = dynamic_cast<stm32::trigger_sink*>(last.f->target->model("LPTIM4"));
  REQUIRE(sink != nullptr);
  CHECK_FALSE(sink->takes_pin("lptim4_ch1"));
  CHECK_FALSE(sink->takes_pin("lptim4_ch2"));
  rig   first;
  auto* taker = dynamic_cast<stm32::trigger_sink*>(first.f->target->model("LPTIM1"));
  REQUIRE(taker != nullptr);
  CHECK(taker->takes_pin("lptim1_ch1"));
  CHECK(taker->takes_pin("lptim1_ch2"));
}

TEST_CASE(
    "inferred: with the counter stopped a capture comes at the prescaled kernel clock, counted "
    "from ENABLE, the Table 613 offset past the edge's and latches CNT 0, CC1E 0 captures nothing")
{
  rig   r;
  auto& pins = capture_input(r, 0);
  r.write(ccmr1, cc1sel | cc1e);
  r.enable_with(0, presc(1));
  auto const e    = r.now_ps() + 10 * cycle + cycle / 2;
  auto const unit = 2 * cycle;
  auto const due  = r.enabled_at + ((e - r.enabled_at) / unit + 3) * unit;
  pins.drive_at(2, true, vtime{e});
  r.idle_to(e + cycle / 2);
  CHECK(r.pending() == due);
  r.idle_to(due - cycle);
  CHECK((r.read(isr) & cc1if) == 0);
  r.idle_to(due);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.read(ccr1) == 0);
  r.write(icr, cc1if);
  r.write(ccmr1, cc1sel);
  pins.drive(2, false);
  pins.drive(2, true);
  r.idle_to(r.now_ps() + 20 * cycle);
  CHECK((r.read(isr) & cc1if) == 0);
}

TEST_CASE(
    "inferred: CC1E or ENABLE written to 0 drops a capture in flight, CC1E written to 0 resets "
    "the IC1PSC count")
{
  for(auto const by_enable : {false, true})
  {
    CAPTURE(by_enable);
    rig        r;
    auto&      pins = capture_input(r, 0);
    auto const t    = captured_with(r, cc1sel | cc1e | ic1f(3));
    auto const e    = t + 20 * cycle;
    pins.drive_at(2, true, vtime{e});
    r.idle_to(e + 9 * cycle);
    REQUIRE(r.pending() == e + 13 * cycle);
    if(by_enable)
    {
      r.write(cr, 0);
    }
    else
    {
      r.write(ccmr1, cc1sel | ic1f(3));
      r.write(ccmr1, cc1sel | cc1e | ic1f(3));
    }
    r.idle_to(e + 20 * cycle);
    CHECK((r.read(isr) & cc1if) == 0);
    CHECK(r.read(ccr1) == 0);
  }
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | ic1psc(1));
  auto const e    = t + 20 * cycle;
  pins.drive_at(2, true, vtime{e});
  pins.drive_at(2, false, vtime{e + 5 * cycle});
  r.idle_to(e + 7 * cycle);
  r.write(ccmr1, cc1sel | ic1psc(1));
  r.write(ccmr1, cc1sel | cc1e | ic1psc(1));
  static_cast<void>(pulses(pins, 2, e + 10 * cycle, 2));
  r.idle_to(e + 13 * cycle);
  CHECK((r.read(isr) & cc1if) == 0);
  r.idle_to(e + 23 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.read(ccr1) == 42);
}

TEST_CASE(
    "inferred: a second edge accepted inside the Table 613 offset of the first gives a second "
    "capture after the first, each latching its own count")
{
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | both_edges | ic1f(1));
  auto const e    = t + 20 * cycle;
  pins.drive_at(2, true, vtime{e});
  pins.drive_at(2, false, vtime{e + 2 * cycle});
  r.idle_to(e + 4 * cycle);
  REQUIRE(r.pending() == t + 27 * cycle);
  r.idle_to(t + 27 * cycle);
  CHECK(r.read(ccr1) == 27);
  r.write(icr, cc1if);
  r.idle_to(t + 28 * cycle);
  CHECK((r.read(isr) & cc1if) == 0);
  r.idle_to(t + 29 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.read(ccr1) == 29);
}

TEST_CASE(
    "IC1SEL or IC2SEL not 00 on a capture channel logs one warning at a start and at a CFGR2 "
    "write while the counter runs and raises under strict_peripherals, the input reading 0")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{units[0], strict};
    static_cast<void>(capture_input(r, 1));
    r.write(cfgr2, 3u << 20);
    r.write(ccmr1, cc2sel | cc2e);
    if(strict)
    {
      CHECK_THROWS_AS(static_cast<void>(r.started(60, 1000)), popcorn::model_error);
      rig  running{units[0], true};
      auto t = captured_with(running, cc1sel | cc1e);
      running.idle_to(t + 5 * cycle);
      CHECK_THROWS_AS(running.write(cfgr2, 2u << 16), popcorn::model_error);
      continue;
    }
    auto const t = r.started(60, 1000);
    CHECK(r.warnings("IC1SEL or IC2SEL not 00") == 1);
    r.f->model_of<stm32::gpio_port>("GPIOB")->drive_at(4, true, vtime{t + 20 * cycle});
    r.idle_to(t + 30 * cycle);
    CHECK((r.read(isr) & cc2if) == 0);
    r.write(cfgr2, 0);
    r.write(cfgr2, 3u << 20);
    CHECK(r.warnings("IC1SEL or IC2SEL not 00") == 1);
  }
  rig plain;
  plain.write(cfgr2, 1u << 16);
  static_cast<void>(plain.started(60, 1000));
  CHECK(plain.logged.empty());
}

TEST_CASE(
    "inferred: a CFGR2 write that changes IC1SEL counts the level change it makes on the "
    "capture input as an edge")
{
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | falling_edge);
  r.write(cfgr2, 1u << 16);
  pins.drive(2, true);
  r.idle_to(t + 20 * cycle);
  r.write(cfgr2, 0);
  r.write(cfgr2, 1u << 16);
  r.idle_to(t + 25 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.read(ccr1) == 22);
}

TEST_CASE(
    "RM0456 58.7.17: CC1P 10 on a capture channel logs one warning at a start and raises under "
    "strict_peripherals, both edges then capture, inferred")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig   r{units[0], strict};
    auto& pins = capture_input(r, 0);
    if(strict)
    {
      CHECK_THROWS_AS(
          static_cast<void>(captured_with(r, cc1sel | cc1e | reserved_cc1p)), popcorn::model_error);
      continue;
    }
    auto const t = captured_with(r, cc1sel | cc1e | reserved_cc1p);
    CHECK(r.warnings("CC1P 10 on a capture channel") == 1);
    pins.drive_at(2, true, vtime{t + 20 * cycle});
    pins.drive_at(2, false, vtime{t + 30 * cycle});
    r.idle_to(t + 22 * cycle);
    CHECK(r.read(ccr1) == 22);
    r.idle_to(t + 32 * cycle);
    CHECK(r.read(ccr1) == 32);
  }
}

TEST_CASE(
    "inferred: with COUNTMODE 1 a capture latches CNT at the filtered edge after the not "
    "modelled warning")
{
  rig        r;
  auto&      pins = lptim1_in1(r);
  auto&      caps = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e, countmode);
  CHECK(r.warnings("CC1SEL or CC2SEL 1 with") == 1);
  auto const rises = pulses(pins, 5, t + 20 * cycle, 3);
  caps.drive_at(2, true, vtime{rises[2] + 2 * cycle});
  r.idle_to(rises[2] + cycle);
  CHECK(r.read(cnt) == 3);
  CHECK((r.read(isr) & cc1if) == 0);
  r.idle_to(rises[2] + 2 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.read(ccr1) == 3);
}

TEST_CASE("inferred: LPTIMxRST takes the level of the capture inputs")
{
  rig   r;
  auto& pins = capture_input(r, 0);
  pins.drive(2, true);
  rcc_reset_pulse(*r.f, r.u.reset_reg, r.u.reset_bit);
  auto const t = captured_with(r, cc1sel | cc1e | falling_edge);
  pins.drive_at(2, false, vtime{t + 20 * cycle});
  r.idle_to(t + 22 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.read(ccr1) == 22);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot with two captures in flight, an IC2PSC count and a pending "
    "IC2F change restores into a machine with no reset, the captures equal")
{
  rig   r;
  auto& one = capture_input(r, 0);
  static_cast<void>(capture_input(r, 1));
  auto const t = captured_with(
      r, cc1sel | cc1e | both_edges | ic1f(1) | cc2sel | cc2e | (2u << 28) | (1u << 24));
  auto const e = t + 20 * cycle;
  one.drive_at(4, true, vtime{e - 10 * cycle});
  one.drive_at(4, false, vtime{e - 5 * cycle});
  one.drive_at(4, true, vtime{e + 3 * cycle});
  one.drive_at(2, true, vtime{e});
  one.drive_at(2, false, vtime{e + 2 * cycle});
  r.idle_to(e + 5 * cycle);
  REQUIRE(r.pending() == t + 27 * cycle);
  auto const saved = r.f->target->snapshot();
  rig        other{no_reset};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  for(auto const* each : {&r, &other})
  {
    each->idle_to(t + 27 * cycle);
    CHECK(each->read(ccr1) == 27);
    each->idle_to(t + 29 * cycle);
    CHECK(each->read(ccr1) == 29);
    CHECK((each->read(isr) & cc2if) == 0);
    each->idle_to(t + 32 * cycle);
    CHECK((each->read(isr) & cc2if) == cc2if);
    CHECK(each->read(ccr2) == 32);
    each->write(icr, cc2if);
    auto* const port = each->f->model_of<stm32::gpio_port>("GPIOB");
    REQUIRE(port != nullptr);
    for(auto const at : {t + 40 * cycle, t + 60 * cycle})
    {
      port->drive_at(4, false, vtime{at});
      port->drive_at(4, true, vtime{at + 10 * cycle});
    }
    each->idle_to(t + 90 * cycle);
    CHECK((each->read(isr) & cc2if) == cc2if);
    CHECK(each->read(ccr2) == 79);
  }
}

TEST_CASE("inferred: with the kernel clock stopped and CKSEL 0 a capture edge captures nothing")
{
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | both_edges);
  r.idle_to(t + 20 * cycle);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 1);
  pins.drive(2, true);
  r.idle_to(r.now_ps() + 10 * cycle);
  CHECK((r.read(isr) & cc1if) == 0);
  r.rcc("RCC_CCIPR3", "LPTIM1SEL", 0);
  r.idle_to(r.now_ps() + 10 * cycle);
  CHECK((r.read(isr) & cc1if) == 0);
  pins.drive(2, false);
  r.idle_to(r.now_ps() + 3 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
}

TEST_CASE(
    "checklist 6: 400 capture edges one kernel clock apart, counter started or stopped, or 9 "
    "apart with the offset 13, keep at most one capture in flight per counter clock")
{
  for(auto const& [running, prescaler, filter, apart] : {
          std::tuple{true,  7u, 0u, 1u},
          std::tuple{false, 7u, 0u, 1u},
          std::tuple{true,  0u, 3u, 9u},
  })
  {
    CAPTURE(running);
    CAPTURE(prescaler);
    rig   r;
    auto& pins = capture_input(r, 0);
    r.write(ccmr1, cc1sel | cc1e | both_edges | ic1f(filter));
    if(running)
    {
      static_cast<void>(r.started(60, 1000, cntstrt, 0, presc(prescaler)));
    }
    else
    {
      r.enable_with(0, presc(prescaler));
    }
    auto const before = r.saved().size();
    for(auto index = 0; index < 400; ++index)
    {
      pins.drive(2, index % 2 == 0);
      r.idle_to(r.now_ps() + apart * cycle);
    }
    CHECK(r.saved().size() <= before + 4 * 16);
  }
}

TEST_CASE(
    "inferred: a capture that falls due before one already in flight, after a kernel rate "
    "increase or an IC1F change, comes first and both come")
{
  {
    rig r;
    r.hsi_on();
    auto&      pins = capture_input(r, 0);
    auto const t    = captured_with(r, cc1sel | cc1e | both_edges | ic1f(3));
    pins.drive_at(2, true, vtime{t + 20 * cycle});
    r.idle_to(t + 28 * cycle);
    REQUIRE(r.pending() == t + 33 * cycle);
    r.rcc("RCC_CCIPR3", "LPTIM1SEL", 2);
    pins.drive_at(2, false, vtime{t + 29 * cycle});
    r.idle_to(t + 32 * cycle);
    CHECK((r.read(isr) & cc1if) == 0);
    auto const first = r.pending();
    REQUIRE(first.has_value());
    CHECK(*first > t + 32 * cycle);
    CHECK(*first < t + 33 * cycle);
    r.idle_to(t + 33 * cycle);
    CHECK((r.read(isr) & cc1if) == cc1if);
  }
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | both_edges | ic1f(3));
  auto const e    = t + 20 * cycle;
  pins.drive_at(2, true, vtime{e});
  r.idle_to(e + 9 * cycle);
  REQUIRE(r.pending() == e + 13 * cycle);
  r.write(ccmr1, cc1sel | cc1e | both_edges);
  CHECK(r.warnings("a CCMR1 write that changes CC1SEL, IC1F") == 1);
  pins.drive_at(2, false, vtime{e + 10 * cycle});
  r.idle_to(e + 11 * cycle);
  CHECK(r.pending() == e + 12 * cycle);
  r.idle_to(e + 12 * cycle);
  CHECK(r.read(ccr1) == 32);
  r.write(icr, cc1if);
  r.idle_to(e + 13 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
  CHECK(r.read(ccr1) == 33);
}

TEST_CASE(
    "RM0456 58.7.17: a CCMR1 write that changes IC1F, CC1P, IC1PSC or CC1SEL of a capture channel "
    "with CC1E 1 logs one warning and raises under strict_peripherals, CC1E alone passes")
{
  for(auto const changed : {ic1f(1), falling_edge, ic1psc(1), 0u})
  {
    for(auto const strict : {false, true})
    {
      CAPTURE(changed);
      CAPTURE(strict);
      rig r{units[0], strict};
      static_cast<void>(captured_with(r, cc1sel | cc1e));
      auto const value = changed == 0 ? cc1e : cc1sel | cc1e | changed;
      if(strict)
      {
        CHECK_THROWS_AS(r.write(ccmr1, value), popcorn::model_error);
        continue;
      }
      r.write(ccmr1, value);
      r.write(ccmr1, cc1sel | cc1e);
      CHECK(r.warnings("a CCMR1 write that changes CC1SEL") == 1);
    }
  }
  rig quiet{units[0], true};
  static_cast<void>(captured_with(quiet, cc1sel | cc1e | ic1f(2)));
  quiet.write(ccmr1, cc1sel | ic1f(2));
  quiet.write(ccmr1, cc1sel | ic1f(1));
  quiet.write(ccmr1, cc1sel | cc1e | ic1f(1));
  CHECK(quiet.logged.empty());
}

namespace
{
constexpr std::uint32_t cc1of = 1u << 12;
constexpr std::uint32_t cc2of = 1u << 13;

auto capture_times(rig const& r, std::uint64_t from, std::uint64_t until, std::uint64_t step)
    -> std::vector<std::uint32_t>
{
  auto seen = std::vector<std::uint32_t>{};
  for(auto at = from; at <= until; at += step)
  {
    r.idle_to(at);
    if((r.read(isr) & cc2if) != 0)
    {
      seen.push_back(r.read(ccr2));
      r.write(icr, cc2if | cc2of);
    }
  }
  return seen;
}

auto gaps_of(std::vector<std::uint32_t> const& values) -> std::vector<std::uint32_t>
{
  auto out = std::vector<std::uint32_t>{};
  for(std::size_t index = 1; index < values.size(); ++index)
  {
    out.push_back((values[index] + 1001u - values[index - 1]) % 1001u);
  }
  return out;
}
}

TEST_CASE(
    "RM0456 58.4.18 and 58.7.3: a capture while CC1IF is set sets CC1OF, CC1OIE raises the IRQ, "
    "CC1OCF clears it, and CC2OF follows channel 2")
{
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    CAPTURE(channel);
    rig        r;
    auto&      pins = capture_input(r, channel);
    auto const flag = channel == 0 ? cc1if : cc2if;
    auto const over = channel == 0 ? cc1of : cc2of;
    auto const mode = channel == 0 ? cc1sel | cc1e : cc2sel | cc2e;
    r.write(ccmr1, mode);
    auto const t = r.started(60, 1000, cntstrt, over);
    auto const p = capture_pin_of(r, channel);
    pins.drive_at(p, true, vtime{t + 20 * cycle});
    pins.drive_at(p, false, vtime{t + 25 * cycle});
    pins.drive_at(p, true, vtime{t + 30 * cycle});
    r.idle_to(t + 22 * cycle);
    CHECK((r.read(isr) & (flag | over)) == flag);
    CHECK_FALSE(r.irq_pending());
    r.idle_to(t + 32 * cycle);
    CHECK((r.read(isr) & (flag | over)) == (flag | over));
    CHECK(r.irq_pending());
    r.write(icr, over);
    CHECK((r.read(isr) & (flag | over)) == flag);
    CHECK(r.logged.empty());
  }
}

TEST_CASE(
    "ES0499 2.17.4: CC1OCF written from one counter clock before a capture to the capture "
    "leaves CC1OF unset at every later over-capture, the window inferred")
{
  for(auto const& [at, lost] : {
          std::pair{std::int64_t{-2}, false},
          std::pair{std::int64_t{-1}, true },
          std::pair{std::int64_t{0},  true },
          std::pair{std::int64_t{1},  false},
  })
  {
    CAPTURE(at);
    rig        r;
    auto&      pins = capture_input(r, 0);
    auto const t    = captured_with(r, cc1sel | cc1e | both_edges);
    for(std::uint64_t index = 0; index < 6; ++index)
    {
      pins.drive_at(2, index % 2 == 0, vtime{t + (20 + 10 * index) * cycle});
    }
    r.idle_to(t + 32 * cycle);
    REQUIRE((r.read(isr) & cc1of) == cc1of);
    auto const write = static_cast<std::uint64_t>(static_cast<std::int64_t>(42) + at);
    r.idle_to(t + write * cycle);
    r.write(icr, cc1of);
    r.idle_to(t + 72 * cycle);
    CHECK((r.read(isr) & cc1if) == cc1if);
    CHECK((r.read(isr) & cc1of) == (lost ? 0u : cc1of));
  }
}

TEST_CASE(
    "ES0499 2.17.4 workaround: CC1E cleared after the CC1OCF write and set again after PRESC x 3 "
    "kernel clocks brings CC1OF back, sooner it does not, inferred, and ENABLE 0 clears the fault")
{
  for(auto const& [prescaler, wait, back] : {
          std::tuple{0u, 3u,  true },
          std::tuple{0u, 2u,  false},
          std::tuple{2u, 12u, true },
          std::tuple{2u, 11u, false},
  })
  {
    CAPTURE(prescaler);
    CAPTURE(wait);
    rig        r;
    auto&      pins = capture_input(r, 0);
    auto const unit = cycle << prescaler;
    auto const t    = captured_with(r, cc1sel | cc1e | both_edges, presc(prescaler));
    auto const off  = table_613[prescaler][0];
    pins.drive_at(2, true, vtime{t + 20 * unit});
    pins.drive_at(2, false, vtime{t + 30 * unit});
    r.idle_to(t + (30 + off) * unit);
    REQUIRE((r.read(isr) & cc1of) == cc1of);
    r.write(icr, cc1of);
    r.write(ccmr1, cc1sel | both_edges);
    r.idle_to(r.now_ps() + wait * cycle);
    r.write(ccmr1, cc1sel | cc1e | both_edges);
    auto const e = r.now_ps() + 10 * unit;
    pins.drive_at(2, true, vtime{e});
    pins.drive_at(2, false, vtime{e + 10 * unit});
    r.idle_to(e + (10 + off + 1) * unit);
    CHECK((r.read(isr) & cc1of) == (back ? cc1of : 0u));
  }
  rig        r;
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | both_edges);
  pins.drive_at(2, true, vtime{t + 20 * cycle});
  pins.drive_at(2, false, vtime{t + 30 * cycle});
  r.idle_to(t + 32 * cycle);
  r.write(icr, cc1of);
  r.write(cr, 0);
  auto const again = r.started(60, 1000, cntstrt);
  pins.drive_at(2, true, vtime{again + 20 * cycle});
  pins.drive_at(2, false, vtime{again + 30 * cycle});
  r.idle_to(again + 32 * cycle);
  CHECK((r.read(isr) & cc1of) == cc1of);
}

TEST_CASE(
    "RM0456 58.4.3 Tables 608 and 609: IC2SEL takes LSI and LSE on LPTIM1, HSI16/256, MSIS/1024 "
    "and MSIS/4 on LPTIM2, the captures one source period apart in counts, the phase inferred")
{
  struct source
  {
    unit          which;
    std::uint32_t select;
    std::uint32_t counts;
    std::uint64_t step;
  };
  for(auto const& [which, select, counts, step] : {
          source{units[0], 1, 125,  20 * cycle },
          source{units[0], 2, 122,  20 * cycle },
          source{units[1], 1, 64,   10 * cycle },
          source{units[1], 2, 1024, 200 * cycle},
          source{units[1], 3, 4,    cycle      },
  })
  {
    CAPTURE(which.name);
    CAPTURE(select);
    rig r{which};
    if(which.name == "LPTIM1")
    {
      auto const on    = select == 1 ? "LSION" : "LSEON";
      auto const ready = select == 1 ? "LSIRDY" : "LSERDY";
      r.rcc("RCC_AHB3ENR", "PWREN", 1);
      auto const [dbpr, dbp] = field_of(*r.f->target, "PWR", "PWR_DBPR", "DBP");
      r.write_at(dbpr, r.read_at(dbpr) | (1u << dbp.lsb));
      r.rcc("RCC_BDCR", on, 1);
      for(auto round = 0; round < 1000 and r.f->field("RCC", "RCC_BDCR", ready) == 0; ++round)
      {
        r.idle_to(r.now_ps() + 100 * cycle);
      }
      REQUIRE(r.f->field("RCC", "RCC_BDCR", ready) == 1);
    }
    else if(select == 1)
    {
      r.hsi_on();
    }
    r.write(cfgr2, select << 20);
    auto const t    = captured_with(r, cc2sel | cc2e);
    auto const seen = capture_times(r, t + 2 * cycle, t + 2 * cycle + 6 * counts * cycle, step);
    REQUIRE(seen.size() >= 3);
    for(auto const gap : gaps_of(seen))
    {
      CHECK((gap == counts % 1001u or (select == 2 and which.name == "LPTIM1" and gap == 123)));
    }
    CHECK(r.logged.empty());
  }
}

TEST_CASE("RM0456 58.4.3 Table 610: IC2SEL not 00 on LPTIM3 is reserved, warns and reads 0")
{
  rig r{units[2]};
  r.write(cfgr2, 1u << 20);
  auto const t = captured_with(r, cc2sel | cc2e);
  CHECK(r.warnings("IC1SEL or IC2SEL not 00") == 1);
  r.idle_to(t + 200 * cycle);
  CHECK((r.read(isr) & cc2if) == 0);
}

TEST_CASE(
    "inferred: an edge on the running grid before a one-shot stop and an edge on the ENABLE grid "
    "after it give two captures in edge order")
{
  rig   r;
  auto& pins = capture_input(r, 0);
  r.write(ccmr1, cc1sel | cc1e | both_edges);
  auto const unit = 4 * cycle;
  auto const s    = r.started(60, 9, sngstrt, 0, presc(2));
  auto const a    = s + 39 * cycle;
  auto const b    = s + 41 * cycle;
  pins.drive_at(2, true, vtime{a});
  pins.drive_at(2, false, vtime{b});
  r.idle_to(b);
  auto const count = r.read(cnt);
  auto const flags = r.read(isr);
  auto const next  = r.pending().value_or(0);
  CAPTURE(count);
  CAPTURE(flags);
  CAPTURE(next - s);
  CHECK(count == 0);
  CHECK(next == s + 11 * unit);
  r.idle_to(s + 11 * unit);
  auto const at_a  = r.read(isr);
  auto const after = r.pending().value_or(0);
  CAPTURE(at_a);
  CAPTURE(after - s);
  CHECK((at_a & cc1if) == cc1if);
  r.write(icr, cc1if);
  REQUIRE(after != 0);
  CHECK(after == r.enabled_at + ((b - r.enabled_at) / unit + 2) * unit);
  r.idle_to(after);
  CHECK((r.read(isr) & cc1if) == cc1if);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot with an internal IC2 source mid period, a lost over-capture "
    "and a CC1E pause restores into a machine with no reset, the captures and the fault equal")
{
  rig r{units[1]};
  r.write(cfgr2, 3u << 20);
  auto&      pins = capture_input(r, 0);
  auto const t    = captured_with(r, cc1sel | cc1e | both_edges | cc2sel | cc2e);
  for(std::uint64_t index = 0; index < 4; ++index)
  {
    pins.drive_at(4, index % 2 == 0, vtime{t + (20 + 10 * index) * cycle});
  }
  r.idle_to(t + 32 * cycle);
  REQUIRE((r.read(isr) & cc1of) == cc1of);
  r.idle_to(t + 42 * cycle);
  r.write(icr, cc1of | cc2if | cc2of);
  r.write(ccmr1, cc1sel | both_edges | cc2sel | cc2e);
  r.idle_to(t + 43 * cycle + cycle / 2);
  auto const saved = r.f->target->snapshot();
  rig        other{no_reset, units[1]};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  for(auto const* each : {&r, &other})
  {
    each->idle_to(t + 44 * cycle);
    each->write(ccmr1, cc1sel | cc1e | both_edges | cc2sel | cc2e);
    auto* const port = each->f->model_of<stm32::gpio_port>("GPIOA");
    REQUIRE(port != nullptr);
    port->drive_at(4, true, vtime{t + 60 * cycle});
    port->drive_at(4, false, vtime{t + 70 * cycle});
    each->idle_to(t + 72 * cycle);
    CHECK((each->read(isr) & (cc1if | cc1of)) == cc1if);
    each->write(icr, cc2if | cc2of);
    each->idle_to(t + 73 * cycle);
    auto const values = std::array{each->read(ccr2), each->read(isr) & cc2if};
    CHECK(values[1] == cc2if);
    CHECK(values[0] == 73);
  }
}

TEST_CASE(
    "inferred: an internal IC2 source resumes from the level it had when CC2E was written to 0, "
    "its first toggle half a period after CC2E is written to 1")
{
  rig r{units[1]};
  r.write(cfgr2, 3u << 20);
  r.write(ccmr1, cc2sel | cc2e);
  r.enable_with();
  auto const e = r.enabled_at;
  r.idle_to(e + 43 * cycle);
  r.write(ccmr1, cc2sel);
  r.idle_to(e + 60 * cycle);
  r.write(icr, cc2if | cc2of);
  auto const again = r.now_ps();
  r.write(ccmr1, cc2sel | cc2e);
  r.idle_to(again + 5 * cycle);
  CHECK((r.read(isr) & cc2if) == 0);
  r.idle_to(again + 6 * cycle);
  CHECK((r.read(isr) & cc2if) == cc2if);
}

namespace
{
using requests = std::vector<std::pair<std::uint32_t, bool>>;

void run_to_flag(rig const& r, std::uint32_t flag)
{
  for(auto round = 0; round < 40 and (r.read(isr) & flag) == 0; ++round)
  {
    REQUIRE(r.pending().has_value());
    r.idle_to(*r.pending());
  }
  REQUIRE((r.read(isr) & flag) == flag);
}
}

TEST_CASE(
    "RM0456 58.4.21: UEDE raises lptim1_ue_dma at the instant UE sets, an update event before the "
    "ack is absorbed, the ack drops it and the next update event raises it again, inferred")
{
  rig r{units[0], false, u575(), {}, true};
  r.write(ccmr1, cc1e);
  auto const t = r.started(6, 10, cntstrt, uede);
  for(auto round = 0; round < 40 and (r.read(isr) & ue) == 0; ++round)
  {
    CHECK(r.dma->levels.empty());
    REQUIRE(r.pending().has_value());
    r.idle_to(*r.pending());
  }
  REQUIRE((r.read(isr) & ue) == ue);
  auto const first = r.now_ps();
  CHECK(
      r.dma->levels
      == requests{
          {ue_request, true}
  });
  r.idle_to(first + 25 * cycle);
  CHECK(
      r.dma->levels
      == requests{
          {ue_request, true}
  });
  r.ack(ue_request);
  CHECK(
      r.dma->levels
      == requests{
          {ue_request, true },
          {ue_request, false}
  });
  REQUIRE(r.pending().has_value());
  for(auto round = 0; round < 40 and r.dma->levels.size() < 3; ++round)
  {
    REQUIRE(r.pending().has_value());
    r.idle_to(*r.pending());
  }
  REQUIRE(r.dma->levels.size() == 3);
  CHECK(r.dma->levels.back() == std::pair{ue_request, true});
  CHECK((r.now_ps() - t) % (11 * cycle) == (first - t) % (11 * cycle));
}

TEST_CASE(
    "RM0456 58.7.8: lptim1_ue_dma drops at the DIER write that clears UEDE, with ENABLE 0 and with "
    "every CCxE 0, and a channel left on keeps it")
{
  for(auto leg = 0; leg < 4; ++leg)
  {
    CAPTURE(leg);
    rig r{units[0], false, u575(), {}, true};
    r.write(ccmr1, cc1e | (leg == 3 ? cc2e : 0u));
    static_cast<void>(r.started(6, 10, cntstrt, uede));
    run_to_flag(r, ue);
    REQUIRE(
        r.dma->levels
        == requests{
            {ue_request, true}
    });
    if(leg == 0)
    {
      r.write(dier, 0);
    }
    else if(leg == 1)
    {
      r.write(cr, 0);
    }
    else
    {
      r.write(ccmr1, leg == 3 ? cc2e : 0u);
    }
    if(leg == 3)
    {
      CHECK(r.dma->levels.size() == 1);
    }
    else
    {
      CHECK(
          r.dma->levels
          == requests{
              {ue_request, true },
              {ue_request, false}
      });
    }
  }
}

TEST_CASE(
    "RM0456 58.4.21: CC1DE and CC2DE raise lptim1_ic1_dma and lptim1_ic2_dma when the capture sets "
    "CCxIF, a capture before the ack and a CCR read leave the request, the ack drops it, and "
    "CCxDE 0, CCxE 0 and ENABLE 0 reset it, inferred")
{
  for(auto leg = 0; leg < 4; ++leg)
  {
    CAPTURE(leg);
    rig   r{units[0], false, u575(), {}, true};
    auto& pins = capture_input(r, 0);
    r.write(ccmr1, cc1sel | cc1e);
    auto const t = r.started(60, 1000, cntstrt, cc1de);
    pins.drive_at(2, true, vtime{t + 20 * cycle + cycle / 2});
    pins.drive_at(2, false, vtime{t + 30 * cycle + cycle / 2});
    pins.drive_at(2, true, vtime{t + 40 * cycle + cycle / 2});
    r.idle_to(t + 21 * cycle);
    CHECK(r.dma->levels.empty());
    r.idle_to(t + 22 * cycle);
    CHECK(
        r.dma->levels
        == requests{
            {ic1_request, true}
    });
    r.idle_to(t + 43 * cycle);
    CHECK((r.read(isr) & cc1if) == cc1if);
    CHECK(r.read(ccr1) == 42);
    CHECK(
        r.dma->levels
        == requests{
            {ic1_request, true}
    });
    if(leg == 0)
    {
      r.ack(ic1_request);
      CHECK(
          r.dma->levels
          == requests{
              {ic1_request, true },
              {ic1_request, false}
      });
      continue;
    }
    if(leg == 1)
    {
      r.write(dier, 0);
    }
    else if(leg == 2)
    {
      r.write(ccmr1, cc1sel);
    }
    else
    {
      r.write(cr, 0);
    }
    CHECK(
        r.dma->levels
        == requests{
            {ic1_request, true },
            {ic1_request, false}
    });
  }
}

TEST_CASE("RM0456 58.4.21: channel 2 raises lptim1_ic2_dma alone with CC2DE")
{
  rig   r{units[0], false, u575(), {}, true};
  auto& pins = capture_input(r, 1);
  r.write(ccmr1, cc2sel | cc2e);
  auto const t = r.started(60, 1000, cntstrt, cc2de);
  pins.drive_at(capture_pin_of(r, 1), true, vtime{t + 20 * cycle + cycle / 2});
  r.idle_to(t + 22 * cycle);
  CHECK(
      r.dma->levels
      == requests{
          {ic2_request, true}
  });
}

TEST_CASE("RM0456 58.3 Table 599: LPTIM4 has no channel and raises no DMA request")
{
  rig r{units[3], false, u575(), {}, true};
  static_cast<void>(r.started(6, 10, cntstrt, 0));
  r.idle_to(r.now_ps() + 40 * cycle);
  CHECK((r.read(isr) & ue) == ue);
  CHECK(r.dma->levels.empty());
}

TEST_CASE("checklist 31: an LPTIMxRST pulse drops a raised DMA request and the next run raises it")
{
  rig r{units[0], false, u575(), {}, true};
  r.write(ccmr1, cc1e);
  static_cast<void>(r.started(6, 10, cntstrt, uede));
  run_to_flag(r, ue);
  REQUIRE(
      r.dma->levels
      == requests{
          {ue_request, true}
  });
  rcc_reset_pulse(*r.f, r.u.reset_reg, r.u.reset_bit);
  CHECK(
      r.dma->levels
      == requests{
          {ue_request, true },
          {ue_request, false}
  });
  r.idle_to(r.now_ps() + 40 * cycle);
  CHECK(r.dma->levels.size() == 2);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot with a raised DMA request restores into a machine with no "
    "reset, the request state equal")
{
  rig r{units[0]};
  r.write(ccmr1, cc1e);
  static_cast<void>(r.started(6, 10, cntstrt, uede));
  run_to_flag(r, ue);
  auto const saved = r.f->target->snapshot();
  rig        other{no_reset, units[0]};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  r.write(cr, 0);
  other.write(cr, 0);
  CHECK(other.saved() == r.saved());
}

TEST_CASE("RM0456 58.4.21: lptim1_ue_dma with no controller that knows it is ignored")
{
  rig r{units[0]};
  r.write(ccmr1, cc1e);
  static_cast<void>(r.started(6, 10, cntstrt, uede));
  run_to_flag(r, ue);
  CHECK(r.logged.empty());
}

namespace
{
using popcorn::testing::set_debug_freeze;

void hold(rig const& r, std::string_view field = "DBG_LPTIM1_STOP", bool on = true)
{
  set_debug_freeze(*r.f, field, on);
  r.f->target->hold_core(true);
}
}

TEST_CASE(
    "RM0456 58.4.22: DBG_LPTIM1_STOP freezes CNT and the pending CC1IF due while the core is "
    "held, both resume at the release, the pending latencies inferred")
{
  rig        r;
  auto const t = r.started(50, 100);
  auto const x = t + 10 * cycle;
  r.idle_to(x);
  REQUIRE(r.read(cnt) == 10);
  auto const due = r.pending();
  REQUIRE(due.has_value());
  hold(r);
  CHECK(r.pending() == std::nullopt);
  r.idle_to(x + 40 * cycle);
  CHECK(r.read(cnt) == 10);
  CHECK((r.read(isr) & cc1if) == 0);
  r.f->target->hold_core(false);
  auto const span = r.now_ps() - x;
  CHECK(r.pending() == *due + span);
  r.idle_to(x + span + 3 * cycle);
  CHECK(r.read(cnt) == 13);
  r.idle_to(*due + span);
  CHECK((r.read(isr) & cc1if) == cc1if);
}

TEST_CASE(
    "RM0456 58.4.22: DBG_LPTIM2_STOP leaves LPTIM1 counting while the core is held, and "
    "DBG_LPTIM1_STOP 0 does too")
{
  for(auto const& [field, on] : {
          std::pair{std::string_view{"DBG_LPTIM2_STOP"}, true },
          std::pair{std::string_view{"DBG_LPTIM1_STOP"}, false},
  })
  {
    CAPTURE(field);
    rig        r;
    auto const t = r.started(50, 100);
    r.idle_to(t + 10 * cycle);
    hold(r, field, on);
    r.idle_to(t + 30 * cycle);
    CHECK(r.read(cnt) == 30);
    r.f->target->hold_core(false);
    r.idle_to(t + 35 * cycle);
    CHECK(r.read(cnt) == 35);
  }
}

TEST_CASE(
    "RM0456 58.4.22: DBG_LPTIMx_STOP freezes LPTIMx alone, on each of the four instances")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig        r{which};
    auto const t = r.started(50, 100);
    r.idle_to(t + 10 * cycle);
    hold(r, std::format("DBG_{}_STOP", which.name));
    r.idle_to(t + 30 * cycle);
    CHECK(r.read(cnt) == 10);
    r.f->target->hold_core(false);
  }
}

TEST_CASE(
    "RM0456 58.4.22: an edge during the hold does not count or capture, the counter counts and "
    "captures edges after the release, inferred")
{
  {
    rig   r;
    auto& pins = lptim1_in1(r);
    pins.drive(5, true);
    auto const t = r.started(60, 100, cntstrt, 0, countmode | falling);
    r.idle_to(t + 10 * cycle);
    hold(r);
    pins.drive_at(5, false, vtime{t + 20 * cycle});
    pins.drive_at(5, true, vtime{t + 25 * cycle});
    r.idle_to(t + 30 * cycle);
    r.f->target->hold_core(false);
    r.idle_to(t + 35 * cycle);
    CHECK(r.read(cnt) == 0);
    pins.drive_at(5, false, vtime{t + 50 * cycle});
    r.idle_to(t + 55 * cycle);
    CHECK(r.read(cnt) == 1);
  }
  rig   r;
  auto& pins = capture_input(r, 0);
  r.write(ccmr1, cc1sel | cc1e);
  auto const t = r.started(60, 1000);
  r.idle_to(t + 10 * cycle);
  hold(r);
  pins.drive_at(2, true, vtime{t + 20 * cycle + cycle / 2});
  pins.drive_at(2, false, vtime{t + 25 * cycle + cycle / 2});
  r.idle_to(t + 30 * cycle);
  r.f->target->hold_core(false);
  r.idle_to(t + 40 * cycle);
  CHECK((r.read(isr) & cc1if) == 0);
  auto const again = r.now_ps() + 10 * cycle + cycle / 2;
  pins.drive_at(2, true, vtime{again});
  r.idle_to(again + 4 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
}

TEST_CASE(
    "RM0456 58.4.22: a held LPTIM keeps its hold across an LPTIM1RST pulse, a system reset keeps "
    "DBG_LPTIM1_STOP and a power-on reset clears it")
{
  for(auto const kind : {0, 1, 2})
  {
    CAPTURE(kind);
    rig r;
    hold(r);
    if(kind == 0)
    {
      rcc_reset_pulse(*r.f, r.u.reset_reg, r.u.reset_bit);
    }
    else
    {
      r.f->target->reset(kind == 1 ? popcorn::reset_kind::software : popcorn::reset_kind::power_on);
      r.rcc(r.u.clock_reg, r.u.clock_bit, 1);
    }
    r.write(cr, enable);
    r.write(arr, 10);
    r.idle_to(r.now_ps() + 50 * cycle);
    CHECK(((r.read(isr) & arrok) == arrok) == (kind == 2));
    if(kind != 2)
    {
      CHECK(r.pending() == std::nullopt);
      r.f->target->hold_core(false);
      auto const x = r.now_ps();
      CHECK(r.pending() == x + 3 * cycle);
    }
    else
    {
      r.f->target->hold_core(false);
    }
  }
}

TEST_CASE(
    "checklist 66 and 72: a snapshot taken while the core is held restores into a machine with no "
    "reset, the model state equal")
{
  rig        r;
  auto const t = r.started(50, 100);
  r.idle_to(t + 10 * cycle);
  hold(r);
  r.idle_to(t + 20 * cycle);
  auto const saved = r.f->target->snapshot();
  rig        other{no_reset};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.saved() == r.saved());
  r.f->target->hold_core(false);
}

TEST_CASE(
    "inferred: a TIMOUT restart while the counter runs moves the capture grid, a capture in "
    "flight on the old grid and a later edge on the ENABLE grid apply in instant order")
{
  rig   r;
  auto& etr  = lptim1_etr(r);
  auto& pins = capture_input(r, 0);
  r.write(ccmr1, cc1sel | cc1e | both_edges);
  static_cast<void>(r.started(60, 1000, cntstrt, 0, rising | timout));
  auto const base = r.enabled_at;
  etr.drive_at(6, true, vtime{base + 80 * cycle + cycle / 2});
  etr.drive_at(6, false, vtime{base + 85 * cycle});
  pins.drive_at(2, true, vtime{base + 100 * cycle + 7 * cycle / 10});
  etr.drive_at(6, true, vtime{base + 100 * cycle + 8 * cycle / 10});
  pins.drive_at(2, false, vtime{base + 100 * cycle + 9 * cycle / 10});
  r.idle_to(base + 101 * cycle);
  CHECK((r.read(isr) & cc1if) == 0);
  CHECK(r.pending() == base + 102 * cycle);
  r.idle_to(base + 102 * cycle);
  CHECK((r.read(isr) & cc1if) == cc1if);
  r.idle_to(base + 103 * cycle);
  CHECK((r.read(isr) & cc1of) == cc1of);
}

TEST_CASE(
    "RM0456 58.4.22: with CKSEL 1 an input 1 edge, a trigger edge and a COUNTMODE capture edge "
    "during the hold are dropped, and count, trigger and capture after the release, inferred")
{
  {
    rig   r;
    auto& pins = lptim1_in1(r);
    pins.drive(5, true);
    auto const t = r.started(60, 100, cntstrt, 0, cksel | falling);
    pulses(pins, 5, t + 10 * cycle, 8);
    r.idle_to(t + 100 * cycle);
    auto const before = r.read(cnt);
    REQUIRE(before >= 1);
    hold(r);
    pulses(pins, 5, t + 110 * cycle, 2);
    r.idle_to(t + 140 * cycle);
    r.f->target->hold_core(false);
    r.idle_to(t + 145 * cycle);
    CHECK(r.read(cnt) == before);
    pulses(pins, 5, t + 150 * cycle, 1);
    r.idle_to(t + 165 * cycle);
    CHECK(r.read(cnt) == before + 1);
  }
  {
    rig   r;
    auto& etr = lptim1_etr(r);
    r.enable_with(0, cksel | rising);
    r.settle_values(60, 100);
    r.write(cr, enable | cntstrt);
    r.idle_to(r.now_ps() + 20 * cycle);
    hold(r);
    etr.drive_at(6, true, vtime{r.now_ps() + 5 * cycle});
    etr.drive_at(6, false, vtime{r.now_ps() + 8 * cycle});
    r.idle_to(r.now_ps() + 20 * cycle);
    CHECK((r.read(isr) & exttrig) == 0);
    r.f->target->hold_core(false);
    etr.drive_at(6, true, vtime{r.now_ps() + 5 * cycle});
    r.idle_to(r.now_ps() + 10 * cycle);
    CHECK((r.read(isr) & exttrig) == exttrig);
  }
  {
    rig   r;
    auto& pins = capture_input(r, 0);
    r.write(ccmr1, cc1sel | cc1e);
    auto const t = r.started(60, 1000, cntstrt, 0, countmode);
    r.idle_to(t + 10 * cycle);
    hold(r);
    pins.drive_at(2, true, vtime{t + 20 * cycle + cycle / 2});
    pins.drive_at(2, false, vtime{t + 25 * cycle + cycle / 2});
    r.idle_to(t + 30 * cycle);
    r.f->target->hold_core(false);
    r.idle_to(t + 40 * cycle);
    CHECK((r.read(isr) & cc1if) == 0);
    auto const again = r.now_ps() + 10 * cycle + cycle / 2;
    pins.drive_at(2, true, vtime{again});
    r.idle_to(again + 4 * cycle);
    CHECK((r.read(isr) & cc1if) == cc1if);
  }
}
