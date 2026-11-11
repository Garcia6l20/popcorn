#include "../support/inert_model.hpp"
#include "dma.hpp"
#include "stm32_fixture.hpp"
#include "trigger_probe.hpp"

#include "trigger_signal.hpp"
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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
using popcorn::testing::set_debug_freeze;
using popcorn::testing::stm32_fixture;

constexpr std::string_view firmware = "h573_rcc_ll";
constexpr address          isr      = 0x00;
constexpr address          icr      = 0x04;
constexpr address          dier     = 0x08;
constexpr address          cfgr     = 0x0C;
constexpr address          cr       = 0x10;
constexpr address          ccr1     = 0x14;
constexpr address          arr      = 0x18;
constexpr address          cnt      = 0x1C;
constexpr address          ccmr1    = 0x2C;
constexpr std::uint32_t    cc1if    = 1u << 0;
constexpr std::uint32_t    arrm     = 1u << 1;
constexpr std::uint32_t    ue       = 1u << 7;
constexpr std::uint32_t    enable   = 1u << 0;
constexpr std::uint32_t    cntstrt  = 1u << 2;
constexpr std::uint32_t    sngstrt  = 1u << 1;
constexpr std::uint32_t    cmp1ok   = 1u << 3;
constexpr std::uint32_t    arrok    = 1u << 4;
constexpr std::uint32_t    dierok   = 1u << 24;
constexpr std::uint32_t    uede     = 1u << 23;
constexpr std::uint32_t    cc1e     = 1u << 1;
constexpr std::uint32_t    enc      = 1u << 24;
constexpr std::uint32_t    wavpol   = 1u << 21;
constexpr std::uint32_t    rising   = 1u << 17;
constexpr std::uint64_t    tick     = 31'250;

struct unit
{
  std::string_view name;
  std::string_view clock_reg;
  std::string_view reset_reg;
  bool             full;
  bool             encoder;
};

constexpr auto units = std::to_array<unit>({
    {"LPTIM1", "RCC_APB3ENR",  "RCC_APB3RSTR",  true,  true },
    {"LPTIM2", "RCC_APB1HENR", "RCC_APB1HRSTR", true,  true },
    {"LPTIM3", "RCC_APB3ENR",  "RCC_APB3RSTR",  true,  true },
    {"LPTIM4", "RCC_APB3ENR",  "RCC_APB3RSTR",  false, false},
    {"LPTIM5", "RCC_APB3ENR",  "RCC_APB3RSTR",  true,  true },
    {"LPTIM6", "RCC_APB3ENR",  "RCC_APB3RSTR",  true,  true },
});

class recorder final : public popcorn::testing::inert_model, public stm32::dma_controller
{
public:
  std::vector<std::pair<std::uint32_t, bool>> levels;

  void on_request(std::uint32_t request, bool level) override
  {
    levels.emplace_back(request, level);
  }

  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    if(not signal.starts_with("lptim") or not signal.ends_with("_dma"))
    {
      return std::nullopt;
    }
    auto const number = static_cast<std::uint32_t>(signal[5] - '0');
    auto const kind   = signal.substr(7, signal.size() - 11);
    return 10 * number + (kind == "ic1" ? 1u : kind == "ic2" ? 2u : 3u);
  }
};

using requests = std::vector<std::pair<std::uint32_t, bool>>;

struct rig
{
  std::unique_ptr<stm32_fixture>   f;
  unit                             u;
  address                          base{};
  recorder*                        dma{};
  popcorn::testing::trigger_probe* probe{};
  std::vector<core::log_record>    logged;
  core::hook_handle                sink;

  explicit rig(unit which, bool record = false, bool strict = false, std::string_view watch = {})
      : u{which}
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(stm32::board{}, popcorn::testing::h573());
    options.strict_peripherals = strict;
    if(record)
    {
      options.models.insert_or_assign(
          "GPDMA1", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
            return std::make_shared<recorder>();
          });
    }
    if(not watch.empty())
    {
      options.models.insert_or_assign(
          "DAC", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
            return std::make_shared<popcorn::testing::trigger_probe>(ctx);
          });
    }
    f = std::make_unique<stm32_fixture>(popcorn::testing::h573(), firmware, std::move(options));
    if(not watch.empty())
    {
      probe = f->model_of<popcorn::testing::trigger_probe>("DAC");
      REQUIRE(probe != nullptr);
      probe->watched = watch;
      probe->listen(true);
    }
    dma  = record ? f->model_of<recorder>("GPDMA1") : nullptr;
    base = f->target->peripheral(u.name).description().base;
    f->set("pc", popcorn::testing::sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record_) { logged.push_back(record_); });
    auto const& rcc = f->target->peripheral("RCC").description();
    auto const* reg = popcorn::find_register(rcc, u.clock_reg);
    REQUIRE(reg != nullptr);
    auto const bit =
        std::ranges::find(reg->fields, std::string{u.name} + "EN", &popcorn::field_desc::name);
    REQUIRE(bit != reg->fields.end());
    auto const at = rcc.base + reg->offset;
    write_at(at, read_at(at) | (1u << bit->lsb));
  }

  explicit rig(no_reset_t, unit which) : u{which}
  {
    f    = std::make_unique<stm32_fixture>(no_reset, popcorn::testing::h573(), firmware);
    base = f->target->peripheral(u.name).description().base;
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

  [[nodiscard]] auto saved() const -> std::vector<std::byte>
  {
    auto out = std::vector<std::byte>{};
    f->target->model(u.name)->save(out);
    return out;
  }

  [[nodiscard]] auto out_level(std::string_view signal) const -> std::optional<bool>
  {
    auto* const source = dynamic_cast<stm32::trigger_source*>(f->target->model(u.name));
    REQUIRE(source != nullptr);
    return source->level(signal);
  }

  void rcc_set(std::string_view reg, std::string_view name, std::uint32_t value) const
  {
    auto const& rcc   = f->target->peripheral("RCC").description();
    auto const* found = popcorn::find_register(rcc, reg);
    REQUIRE_MESSAGE(found != nullptr, reg);
    auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE_MESSAGE(bits != found->fields.end(), name);
    auto const mask = ((std::uint32_t{1} << bits->width) - 1u) << bits->lsb;
    auto const at   = rcc.base + found->offset;
    write_at(at, (read_at(at) & ~mask) | (value << bits->lsb));
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

  [[nodiscard]] auto warnings(std::string_view text) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record_) {
      return record_.message.find(text) != std::string::npos;
    });
  }

  void settle(std::uint32_t compare, std::uint32_t reload) const
  {
    write(arr, reload);
    idle_to(now_ps() + 20 * tick);
    write(ccr1, compare);
    idle_to(now_ps() + 20 * tick);
    write(icr, cmp1ok | arrok);
  }

  [[nodiscard]] auto started(
      std::uint32_t compare,
      std::uint32_t reload,
      std::uint32_t irqs   = 0,
      std::uint32_t config = 0,
      std::uint32_t mode   = cntstrt) const -> std::uint64_t
  {
    write(cfgr, config);
    write(cr, enable);
    write(dier, irqs);
    idle_to(now_ps() + 20 * tick);
    write(icr, dierok);
    settle(compare, reload);
    auto const at = now_ps();
    write(cr, enable | mode);
    return at;
  }
};
}

TEST_CASE("RM0481 48.3 Table 527: LPTIM1 to LPTIM6 each count and set CC1IF, ARRM and UE")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig r{which};
    static_cast<void>(r.started(10, 20));
    r.idle_to(r.now_ps() + 60 * tick);
    CHECK((r.read(isr) & (cc1if | arrm | ue)) == (cc1if | arrm | ue));
    CHECK(r.logged.empty());
  }
}

TEST_CASE("RM0481 48.7.19 Table 544: LPTIM4 has no CCMR1 and CCR2, the other instances have them")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig         r{which};
    auto const& desc = r.f->target->peripheral(which.name).description();
    CHECK((popcorn::find_register(desc, "CCMR1") != nullptr) == which.full);
    CHECK((popcorn::find_register(desc, "CCR2") != nullptr) == which.full);
  }
}

TEST_CASE("RM0481 48.3 Table 527: the encoder mode is on LPTIM1, 2, 3, 5 and 6, not LPTIM4")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig r{which};
    r.write(cfgr, enc);
    r.write(cr, enable);
    r.idle_to(r.now_ps() + 20 * tick);
    r.write(cr, enable | sngstrt);
    CHECK(r.warnings("SNGSTRT with ENC 1") == (which.encoder ? 1 : 0));
  }
}

TEST_CASE("RM0481 48.4.3 Table 532: TRIGSEL 4 names the instance's trigger, reserved on LPTIM3")
{
  for(auto const& [which, name] : {
          std::pair{units[0], std::string_view{"tamp_trg2"}     },
          std::pair{units[1], std::string_view{"gpdma1_ch0_tcf"}},
          std::pair{units[2], std::string_view{"reserved"}      },
          std::pair{units[3], std::string_view{"reserved"}      },
          std::pair{units[4], std::string_view{"reserved"}      },
          std::pair{units[5], std::string_view{"reserved"}      },
  })
  {
    CAPTURE(which.name);
    rig r{which};
    r.write(cfgr, rising | (4u << 13));
    r.write(cr, enable);
    r.idle_to(r.now_ps() + 20 * tick);
    r.write(cr, enable | cntstrt);
    CHECK(r.warnings(std::string{"TRIGSEL 4, "} + std::string{name}) == 1);
  }
}

TEST_CASE("RM0481 48.4.21: each instance with channels raises its ue request, LPTIM4 none")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig r{which, true};
    if(which.full)
    {
      r.write(ccmr1, cc1e);
    }
    static_cast<void>(r.started(6, 10, uede));
    r.idle_to(r.now_ps() + 60 * tick);
    CHECK((r.read(isr) & ue) == ue);
    auto const request = 10 * static_cast<std::uint32_t>(which.name.back() - '0') + 3;
    auto const expected = which.full ? requests{{request, true}} : requests{};
    CHECK(r.dma->levels == expected);
  }
}

TEST_CASE("RM0481 48.4.22: DBG_LPTIMx_STOP freezes LPTIMx alone, on each of the six instances")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig        r{which};
    auto const t = r.started(50, 100);
    r.idle_to(t + 40 * tick);
    auto const frozen = r.read(cnt);
    set_debug_freeze(*r.f, std::string{"DBG_"} + std::string{which.name} + "_STOP", true);
    r.f->target->hold_core(true);
    r.idle_to(t + 80 * tick);
    CHECK(r.read(cnt) == frozen);
    r.f->target->hold_core(false);
    r.idle_to(r.now_ps() + 5 * tick);
    CHECK(r.read(cnt) > frozen);
  }
}

TEST_CASE("RM0481 48.7.10: LPTIM4 drives lptim4_out from WAVPOL, with no channel polarity bit")
{
  rig r{units[3]};
  r.write(cfgr, wavpol);
  auto* const source = dynamic_cast<stm32::trigger_source*>(r.f->target->model("LPTIM4"));
  REQUIRE(source != nullptr);
  CHECK(source->produces("lptim4_out"));
  CHECK_FALSE(source->produces("lptim4_ch1"));
  for(auto const& which : units)
  {
    if(which.full)
    {
      rig         other{which};
      auto* const each = dynamic_cast<stm32::trigger_source*>(other.f->target->model(which.name));
      REQUIRE(each != nullptr);
      CHECK(each->produces(std::string{"lptim"} + which.name.back() + "_ch1"));
    }
  }
}

TEST_CASE("RM0481 48.7.7: the DIER bits LPTIM4 reserves read 0, the implemented ones are stored")
{
  rig r{units[3]};
  r.write(cr, enable);
  r.write(dier, (1u << 23) | (1u << 16) | (1u << 9) | (1u << 5) | (1u << 7));
  CHECK(r.read(dier) == (1u << 7));
  r.idle_to(r.now_ps() + 20 * tick);
  CHECK(r.read(dier) == (1u << 7));
}

TEST_CASE(
    "RM0481 48.7.10: LPTIM4 drives lptim4_out from CCR1 and ARR, WAVPOL inverts it at the CFGR "
    "write, also with ENABLE 0")
{
  rig        r{units[3], false, false, "lptim4_out"};
  auto const x = r.now_ps();
  r.write(cfgr, wavpol);
  CHECK(r.probe->pulses == std::vector{x});
  CHECK(r.out_level("lptim4_out") == true);
  r.idle_to(x + 2 * tick);
  r.write(cfgr, 0);
  CHECK(r.probe->falls == std::vector{x + 2 * tick});
  CHECK(r.out_level("lptim4_out") == false);
}

TEST_CASE("RM0481 Table 532: TRIGSEL 1 to 7 name the trigger of each instance")
{
  auto const row = [](std::string_view four, std::string_view five) {
    return std::array<std::string_view, 7>{"rtc_alra_trg", "rtc_alrb_trg", "tamp_trg1", four,
                                           five,           "reserved",     "reserved"};
  };
  auto const names = std::array{
      row("tamp_trg2", "gpdma1_ch1_tcf"), row("gpdma1_ch0_tcf", "gpdma1_ch4_tcf"),
      row("reserved", "reserved"),        row("reserved", "reserved"),
      row("reserved", "reserved"),        row("reserved", "reserved"),
  };
  for(std::size_t index = 0; index < units.size(); ++index)
  {
    for(std::uint32_t select = 1; select <= 7; ++select)
    {
      CAPTURE(units[index].name);
      CAPTURE(select);
      rig r{units[index]};
      r.write(cfgr, rising | (select << 13));
      r.write(cr, enable);
      r.idle_to(r.now_ps() + 20 * tick);
      r.write(cr, enable | cntstrt);
      auto const text = std::format("TRIGSEL {}, {}", select, names[index][select - 1]);
      CHECK(r.warnings(text) == 1);
    }
  }
}

namespace
{
void clock_on(rig const& r, std::string_view source)
{
  auto const ready = [&](std::string_view reg, std::string_view flag) {
    for(auto round = 0; round < 4000 and r.f->field("RCC", reg, flag) == 0; ++round)
    {
      r.idle_to(r.now_ps() + 1000 * tick);
    }
    REQUIRE(r.f->field("RCC", reg, flag) == 1);
  };
  if(source == "csi")
  {
    r.rcc_set("RCC_CR", "CSION", 1);
    ready("RCC_CR", "CSIRDY");
    return;
  }
  auto const& desc = r.f->target->peripheral("PWR").description();
  auto const* reg  = popcorn::find_register(desc, "PWR_DBPCR");
  REQUIRE(reg != nullptr);
  auto const bit =
      std::ranges::find(reg->fields, std::string_view{"DBP"}, &popcorn::field_desc::name);
  REQUIRE(bit != reg->fields.end());
  r.write_at(desc.base + reg->offset, r.read_at(desc.base + reg->offset) | (1u << bit->lsb));
  r.rcc_set("RCC_BDCR", source == "lsi" ? "LSION" : "LSEON", 1);
  ready("RCC_BDCR", source == "lsi" ? "LSIRDY" : "LSERDY");
}

auto capture_gaps(rig const& r, std::uint32_t periods) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> values;
  auto const                 end = r.now_ps() + 1'000'000'000;
  while(values.size() < periods and r.now_ps() < end)
  {
    r.idle_to(r.now_ps() + 2 * tick);
    if((r.read(isr) & (1u << 9)) != 0)
    {
      values.push_back(r.read(0x34));
      r.write(icr, 1u << 9);
    }
  }
  std::vector<std::uint32_t> gaps;
  for(std::size_t index = 1; index < values.size(); ++index)
  {
    gaps.push_back((values[index] - values[index - 1]) & 0xFFFF);
  }
  return gaps;
}
}

TEST_CASE(
    "RM0481 Tables 536 to 538: IC2SEL takes LSI and LSE on LPTIM1, HSI/1024, CSI/128 and HSI/8 on "
    "LPTIM2, the captures one source period apart in counts, the phase inferred")
{
  struct source
  {
    unit             which;
    std::uint32_t    select;
    std::string_view clock;
    std::uint32_t    counts;
  };
  for(auto const& [which, select, clock, counts] : {
          source{units[0], 1, "lsi", 1000},
          source{units[0], 2, "lse", 977 },
          source{units[1], 1, "hsi", 1024},
          source{units[1], 2, "csi", 1024},
          source{units[1], 3, "hsi", 8   },
  })
  {
    CAPTURE(which.name);
    CAPTURE(select);
    rig r{which};
    r.rcc_set("RCC_CCIPR1", "TIMICSEL", 1);
    if(clock != "hsi")
    {
      clock_on(r, clock);
    }
    r.idle_to(r.now_ps() + 40'000'000);
    r.write(0x24, select << 20);
    r.write(ccmr1, (1u << 16) | (1u << 17));
    static_cast<void>(r.started(10, 0xFFFF));
    auto const gaps = capture_gaps(r, 5);
    REQUIRE(gaps.size() >= 3);
    for(auto const gap : gaps)
    {
      CAPTURE(gap);
      CHECK((gap == counts or gap + 1 == counts));
    }
  }
}

TEST_CASE("RM0481 Tables 536 to 538: reserved IC2SEL inputs warn on LPTIM1 and on LPTIM3, 5 and 6")
{
  for(auto const& [index, select] : {
          std::pair{std::size_t{0}, 3u},
          std::pair{std::size_t{2}, 1u},
          std::pair{std::size_t{4}, 2u},
          std::pair{std::size_t{5}, 3u},
  })
  {
    CAPTURE(units[index].name);
    rig r{units[index]};
    r.write(0x24, select << 20);
    r.write(ccmr1, (1u << 16) | (1u << 17));
    static_cast<void>(r.started(10, 0xFFFF));
    CHECK(r.warnings("IC1SEL or IC2SEL not 00") == 1);
  }
}

TEST_CASE(
    "RM0481 48.4.16: the RCR write crosses the APB clock of the instance, pclk1 for LPTIM2 and "
    "pclk3 for LPTIM1")
{
  std::array<std::uint64_t, 2> waits{};
  for(std::size_t index = 0; index < 2; ++index)
  {
    rig r{units[index]};
    r.rcc_set("RCC_CFGR2", "PPRE1", 0b111);
    r.write(cr, enable);
    r.idle_to(r.now_ps() + 800 * tick);
    auto const x = r.now_ps();
    r.write(0x28, 5);
    auto const due = r.pending();
    REQUIRE(due.has_value());
    waits[index] = *due - x;
  }
  CHECK(waits[1] == 7 * 16 * tick);
  CHECK(waits[0] == 7 * tick);
}

TEST_CASE(
    "checklist 66 and 72: a snapshot with the ue request raised and a held core restores into a "
    "machine with no reset on each of the six instances, the state equal")
{
  for(auto const& which : units)
  {
    CAPTURE(which.name);
    rig r{which};
    if(which.full)
    {
      r.write(ccmr1, cc1e);
    }
    static_cast<void>(r.started(6, 10, uede));
    for(auto round = 0; round < 40 and (r.read(isr) & ue) == 0; ++round)
    {
      r.idle_to(r.now_ps() + 5 * tick);
    }
    set_debug_freeze(*r.f, std::string{"DBG_"} + std::string{which.name} + "_STOP", true);
    r.f->target->hold_core(true);
    auto const saved = r.f->target->snapshot();
    rig        other{no_reset, which};
    other.f->target->restore(
        core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    CHECK(other.saved() == r.saved());
    r.write(cr, 0);
    other.write(cr, 0);
    CHECK(other.saved() == r.saved());
    r.f->target->hold_core(false);
  }
}

TEST_CASE("checklist 31: an LPTIMxRST pulse drops the raised ue request and zeroes CNT on LPTIM5")
{
  rig r{units[4], true};
  r.write(ccmr1, cc1e);
  static_cast<void>(r.started(6, 10, uede));
  for(auto round = 0; round < 40 and (r.read(isr) & ue) == 0; ++round)
  {
    r.idle_to(r.now_ps() + 5 * tick);
  }
  REQUIRE(
      r.dma->levels
      == requests{
          {53, true}
  });
  rcc_reset_pulse(*r.f, r.u.reset_reg, "LPTIM5RST");
  CHECK(
      r.dma->levels
      == requests{
          {53, true },
          {53, false}
  });
  CHECK(r.read(cnt) == 0);
}

TEST_CASE(
    "RM0481 11.8.40: with TIMICSEL 0 an internal IC2 clock of LPTIM2 gives no capture and logs "
    "nothing, TIMICSEL 1 starts it and TIMICSEL 0 stops it, also under strict_peripherals")
{
  auto const start = [](rig const& r) {
    r.write(0x24, 3u << 20);
    r.write(ccmr1, (1u << 16) | (1u << 17));
    return r.started(10, 0xFFFF);
  };
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig        r{units[1], false, strict};
    auto const t = start(r);
    r.idle_to(t + 600 * tick);
    CHECK((r.read(isr) & (1u << 9)) == 0);
    CHECK(r.logged.empty());
    r.rcc_set("RCC_CCIPR1", "TIMICSEL", 1);
    r.idle_to(r.now_ps() + 40 * tick);
    CHECK((r.read(isr) & (1u << 9)) != 0);
    r.write(icr, 1u << 9);
    r.rcc_set("RCC_CCIPR1", "TIMICSEL", 0);
    r.idle_to(r.now_ps() + 80 * tick);
    CHECK((r.read(isr) & (1u << 9)) == 0);
    CHECK(r.logged.empty());
  }
  rig lsi_free{units[0], false, true};
  lsi_free.write(0x24, 1u << 20);
  lsi_free.write(ccmr1, (1u << 16) | (1u << 17));
  CHECK_NOTHROW(static_cast<void>(lsi_free.started(10, 0xFFFF)));
}

TEST_CASE("a description without RCC_CCIPR1.TIMICSEL is refused at construction on the H5")
{
  auto desc = popcorn::testing::h573();
  auto rcc =
      std::ranges::find(desc.peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  REQUIRE(rcc != desc.peripherals.end());
  auto owner = std::ranges::find_if(rcc->registers, [](auto const& one) {
    return one.name == "RCC_CCIPR1"
        or (one.aliases and std::ranges::contains(*one.aliases, std::string{"RCC_CCIPR1"}));
  });
  REQUIRE(owner != rcc->registers.end());
  REQUIRE(
      std::erase_if(owner->fields, [](auto const& one) { return one.name == "TIMICSEL"; }) == 1);
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(stm32::board{}, desc);
  CHECK_THROWS_AS(stm32_fixture(desc, firmware, std::move(options)), popcorn::config_error);
}
