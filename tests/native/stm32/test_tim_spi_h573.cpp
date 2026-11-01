#include "stm32_fixture.hpp"
#include "trigger_probe.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/devices/at25sf.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core    = popcorn::core;
namespace devices = popcorn::periph::devices;
namespace periph  = popcorn::periph;
namespace stm32   = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware  = "h573_rcc_ll";
constexpr std::uint64_t    tick_ps   = 31'250;
constexpr std::uint32_t    tim12_irq = 120;

constexpr std::uint32_t sms_trigger   = 6;
constexpr std::uint32_t master_update = 2;
constexpr std::uint32_t sms_external  = 7;
constexpr std::uint32_t sms_gated     = 5;
constexpr std::uint32_t sms_reset     = 4;
constexpr std::uint32_t pwm1          = 6u << 4;
constexpr std::uint32_t pwm2          = 7u << 4;
constexpr std::uint32_t ts_itr4       = 8;
constexpr std::uint32_t ts_itr6       = 10;
constexpr std::uint32_t ts_itr7       = 11;
constexpr std::uint32_t ts_itr9       = 13;

class cpu_port final : public periph::model
{
public:
  explicit cpu_port(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
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

class script_target final : public periph::spi_target
{
public:
  std::vector<std::uint32_t> frames;
  std::vector<vtime>         times;

  void select(bool, vtime) override
  {}

  auto exchange(std::uint32_t out, std::uint8_t, vtime at) -> std::uint32_t override
  {
    frames.push_back(out);
    times.push_back(at);
    return 0xA5;
  }

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}
};

class recorded_flash final : public periph::spi_target
{
public:
  explicit recorded_flash(std::shared_ptr<devices::at25sf> device) : flash{std::move(device)}
  {}

  std::shared_ptr<devices::at25sf> flash;
  std::vector<std::uint32_t>       replies;

  void select(bool selected, vtime at) override
  {
    flash->select(selected, at);
  }

  auto exchange(std::uint32_t out, std::uint8_t bits, vtime at) -> std::uint32_t override
  {
    auto const reply = flash->exchange(out, bits, at);
    replies.push_back(reply);
    return reply;
  }

  void save(std::vector<std::byte>& out) const override
  {
    flash->save(out);
  }

  void load(std::span<std::byte const> in) override
  {
    flash->load(in);
  }

  void reset(popcorn::reset_kind kind) override
  {
    flash->reset(kind);
  }
};

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  explicit rig(stm32::board const& board = {}, bool strict = false, bool trigger = false)
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(board, h573());
    options.strict_peripherals = strict;
    if(trigger)
    {
      options.models.insert_or_assign(
          "RNG", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
            return std::make_shared<popcorn::testing::trigger_probe>(ctx);
          });
    }
    if(strict)
    {
      options.models.insert_or_assign(
          "CRC", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
            return std::make_shared<cpu_port>(ctx);
          });
    }
    f = std::make_unique<stm32_fixture>(h573(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
  }

  [[nodiscard]] auto desc(std::string_view unit) const -> popcorn::peripheral_desc const&
  {
    return f->target->peripheral(unit).description();
  }

  [[nodiscard]] auto reg(std::string_view unit, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const* found = popcorn::find_register(desc(unit), name);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto at(std::string_view unit, std::string_view name) const -> popcorn::address
  {
    return desc(unit).base + reg(unit, name).offset;
  }

  [[nodiscard]] auto read(std::string_view unit, std::string_view name, std::uint8_t size = 4) const
      -> std::uint32_t
  {
    auto const value = f->target->mem().read(at(unit, name), size);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(
      std::string_view unit,
      std::string_view name,
      std::uint32_t    value,
      std::uint8_t     size = 4) const
  {
    REQUIRE(f->target->mem().write(at(unit, name), size, value).has_value());
  }

  void cpu_write(std::string_view unit, std::string_view name, std::uint32_t value) const
  {
    auto* const port = f->model_of<cpu_port>("CRC");
    REQUIRE(port != nullptr);
    static_cast<void>(
        port->context.services.write(at(unit, name), 4, value, popcorn::initiator::cpu));
  }

  [[nodiscard]] auto lsb(std::string_view unit, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    auto const& fields = reg(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    return found->lsb;
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
          {.max_instructions = icount{1'000'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{ps}});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == stop_reason::time);
        return;
      }
    }
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    auto const word = f->target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }
};

struct timers : rig
{
  std::uint64_t start{};

  explicit timers(stm32::board const& board = {}, bool strict = false, bool trigger = false)
      : rig{board, strict, trigger}
  {
    for(auto const* const bit :
        {"TIM2EN", "TIM3EN", "TIM4EN", "TIM5EN", "TIM12EN", "TIM13EN", "TIM14EN"})
    {
      set("RCC", "RCC_APB1LENR", bit, 1);
    }
    for(auto const* const bit : {"TIM1EN", "TIM8EN", "TIM15EN", "TIM16EN", "TIM17EN"})
    {
      set("RCC", "RCC_APB2ENR", bit, 1);
    }
    start = now();
  }

  void base(std::string_view tim, std::uint32_t reload) const
  {
    write(tim, "PSC", 0);
    write(tim, "ARR", reload);
    write(tim, "EGR", 1);
    write(tim, "SR", 0);
  }

  void master(std::string_view tim) const
  {
    write(tim, "CR2", master_update << 4);
  }

  void slave(std::string_view tim, std::uint32_t select, std::uint32_t mode = sms_trigger) const
  {
    write(
        tim, "SMCR",
        (mode & 7u) | ((mode >> 3) << 16) | ((select & 7u) << 4) | ((select >> 3) << 20));
  }

  void enable(std::string_view tim) const
  {
    write(tim, "CR1", read(tim, "CR1") | 1u);
  }

  void at_tick(std::uint64_t ticks) const
  {
    run_until(start + ticks * tick_ps);
  }
};

struct scripted
{
  std::shared_ptr<script_target> device = std::make_shared<script_target>();
};

struct spis : scripted, rig
{
  static auto board_of(std::shared_ptr<script_target> const& device, std::string_view instance)
      -> stm32::board
  {
    stm32::board board{};
    board.spi[std::string{instance}].push_back({.device = device});
    return board;
  }

  explicit spis(std::string_view instance) : rig{board_of(device, instance)}
  {}

  void clock(std::string_view unit) const
  {
    set("RCC", "RCC_APB2ENR", std::string{unit} + "EN", 1);
    if(unit == "SPI1")
    {
      set("RCC", "RCC_CCIPR3", "SPI1SEL", 4);
    }
  }

  void controller(
      std::string_view unit,
      std::uint32_t    dsize,
      std::uint32_t    tsize,
      std::uint32_t    fthlv = 0) const
  {
    clock(unit);
    set(unit, "SPI_CFG1", "MBR", 0);
    set(unit, "SPI_CFG1", "DSIZE", dsize);
    set(unit, "SPI_CFG1", "FTHLV", fthlv);
    set(unit, "SPI_CFG2", "SSM", 1);
    set(unit, "SPI_CFG2", "SSOE", 1);
    set(unit, "SPI_CFG2", "MASTER", 1);
    set(unit, "SPI_CR2", "TSIZE", tsize);
    set(unit, "SPI_CR1", "SSI", 1);
    set(unit, "SPI_CR1", "SPE", 1);
  }

  void send(std::string_view unit, std::uint32_t value) const
  {
    write(unit, "SPI_TXDR", value, 1);
  }
};
}

TEST_CASE(
    "TIM3 starts in trigger mode on ITR4 when TIM5 updates, TIM5 with the MMS field the patch "
    "renames from MMS1")
{
  timers t;
  t.base("TIM5", 99);
  t.master("TIM5");
  t.base("TIM3", 0xffff);
  t.slave("TIM3", ts_itr4);
  t.enable("TIM5");
  t.at_tick(150);
  CHECK(t.get("TIM3", "CR1", "CEN") == 1);
  CHECK(t.get("TIM3", "SR", "TIF") == 1);
  CHECK(t.read("TIM3", "CNT") == 50);
}

TEST_CASE(
    "reset + trigger mode, SMS 1000 through the renamed SMS_1, starts TIM3 on ITR4 at TIM5's "
    "update")
{
  timers t;
  t.base("TIM5", 99);
  t.master("TIM5");
  t.base("TIM3", 0xffff);
  t.slave("TIM3", ts_itr4, 8);
  t.enable("TIM5");
  t.at_tick(150);
  CHECK(t.get("TIM3", "CR1", "CEN") == 1);
  CHECK(t.read("TIM3", "CNT") == 50);
}

TEST_CASE("MMS bit 3, renamed from MMS2, is no TRGO2 select on TIM2 to TIM5")
{
  for(auto const* const unit : {"TIM2", "TIM3", "TIM4", "TIM5"})
  {
    CAPTURE(unit);
    timers t;
    t.write(unit, "CR2", 1u << 25);
    REQUIRE(t.warnings() == 1);
    auto const& message = t.logged.back().message;
    CHECK(message.find("0b1000") != std::string::npos);
    CHECK(message.find("with MMS2") == std::string::npos);
  }
}

TEST_CASE("TIM15 SMCR reads 0x033100F7 after 0xFFFFFFFF as on silicon, SMSPE and SMSPS included")
{
  timers t;
  t.write("TIM15", "SMCR", 0xFFFF'FFFFu);
  CHECK(t.read("TIM15", "SMCR") == 0x0331'00F7u);
}

TEST_CASE("TIM15 SMSPE is storage that a counter start logs once, SMSPS alone logs nothing")
{
  SUBCASE("SMSPE")
  {
    timers t;
    t.write("TIM15", "SMCR", 1u << 24);
    CHECK(t.get("TIM15", "SMCR", "SMSPE") == 1);
    t.enable("TIM15");
    REQUIRE(t.warnings() == 1);
    CHECK(t.logged.back().message.find("SMSPE") != std::string::npos);
  }
  SUBCASE("SMSPS")
  {
    timers t;
    t.write("TIM15", "SMCR", 1u << 25);
    CHECK(t.get("TIM15", "SMCR", "SMSPS") == 1);
    t.enable("TIM15");
    CHECK(t.warnings() == 0);
  }
}

TEST_CASE("TIM2, TIM3, TIM4, TIM12 and TIM15 take TS bit 3 of ITR4 from TIM5's update")
{
  for(auto const* const unit : {"TIM2", "TIM3", "TIM4", "TIM12", "TIM15"})
  {
    CAPTURE(unit);
    timers t;
    t.base("TIM5", 99);
    t.master("TIM5");
    t.base(unit, 0xffff);
    t.slave(unit, ts_itr4);
    t.enable("TIM5");
    t.at_tick(150);
    CHECK(t.get(unit, "CR1", "CEN") == 1);
    CHECK(t.read(unit, "CNT") == 50);
  }
}

TEST_CASE("TIM2, TIM3 and TIM4 masters reach TIM5 through MMS 010 on ITR1 to ITR3")
{
  constexpr std::array<std::pair<std::string_view, std::uint32_t>, 3> masters{
      {{"TIM2", 1}, {"TIM3", 2}, {"TIM4", 3}}
  };
  for(auto const& [unit, select] : masters)
  {
    CAPTURE(unit);
    timers t;
    t.base(unit, 99);
    t.master(unit);
    t.base("TIM5", 0xffff);
    t.slave("TIM5", select);
    t.enable(unit);
    t.at_tick(150);
    CHECK(t.get("TIM5", "CR1", "CEN") == 1);
    CHECK(t.read("TIM5", "CNT") == 50);
  }
}

TEST_CASE("TIM5 takes TS bit 3 of ITR6 from TIM12's update")
{
  timers t;
  t.base("TIM12", 99);
  t.master("TIM12");
  t.base("TIM5", 0xffff);
  t.slave("TIM5", ts_itr6);
  t.enable("TIM12");
  t.at_tick(150);
  CHECK(t.get("TIM5", "CR1", "CEN") == 1);
  CHECK(t.read("TIM5", "CNT") == 50);
}

TEST_CASE("TIM3 on ITR6 follows TIM12 and not TIM15, the STM32U5 source of that input")
{
  timers t;
  t.base("TIM12", 99);
  t.master("TIM12");
  t.base("TIM15", 49);
  t.master("TIM15");
  t.base("TIM3", 0xffff);
  t.slave("TIM3", ts_itr6);
  t.enable("TIM15");
  t.enable("TIM12");
  t.at_tick(150);
  CHECK(t.get("TIM3", "CR1", "CEN") == 1);
  CHECK(t.read("TIM3", "CNT") == 50);
}

TEST_CASE("TIM3 on ITR9 follows TIM15")
{
  timers t;
  t.base("TIM15", 99);
  t.master("TIM15");
  t.base("TIM3", 0xffff);
  t.slave("TIM3", ts_itr9);
  t.enable("TIM15");
  t.at_tick(150);
  CHECK(t.get("TIM3", "CR1", "CEN") == 1);
  CHECK(t.read("TIM3", "CNT") == 50);
}

TEST_CASE("every entry of the H5 trigger table reaches its own source and no other")
{
  enum class kind : std::uint8_t
  {
    trgo,
    oc1,
    none,
  };
  struct row
  {
    std::uint32_t    select;
    std::string_view source;
    kind             by;
  };
  constexpr std::array<row, 13> rows{
      {{0, "TIM1", kind::trgo},
       {1, "TIM2", kind::trgo},
       {2, "TIM3", kind::trgo},
       {3, "TIM4", kind::trgo},
       {8, "TIM5", kind::trgo},
       {9, "TIM8", kind::trgo},
       {10, "TIM12", kind::trgo},
       {11, "TIM13", kind::oc1},
       {12, "TIM14", kind::oc1},
       {13, "TIM15", kind::trgo},
       {14, "TIM16", kind::oc1},
       {15, "TIM17", kind::oc1},
       {16, "USBSOF", kind::none}}
  };
  for(auto const& [select, source, by] : rows)
  {
    auto const modelled = by != kind::none;
    CAPTURE(select);
    CAPTURE(source);
    std::string_view const slave     = source == "TIM3" ? "TIM2" : "TIM3";
    std::string_view const bystander = source == "TIM5" ? "TIM4" : "TIM5";
    timers                 t;
    t.base(bystander, 99);
    t.master(bystander);
    if(by == kind::oc1)
    {
      t.base(source, 199);
      t.write(source, "CCR1", 100);
      t.write(source, "CCMR1_Output", pwm1);
      t.set(source, "CCER", "CC1E", 1);
      if(source == "TIM16" or source == "TIM17")
      {
        t.set(source, "BDTR", "MOE", 1);
      }
    }
    else if(modelled)
    {
      t.base(source, 199);
      t.master(source);
    }
    t.base(slave, 0xffff);
    t.slave(slave, select);
    t.enable(bystander);
    if(modelled)
    {
      t.enable(source);
    }
    t.at_tick(250);
    if(modelled)
    {
      CHECK(t.get(slave, "CR1", "CEN") == 1);
      CHECK(t.read(slave, "CNT") == 50);
    }
    else
    {
      CHECK(t.get(slave, "CR1", "CEN") == 0);
      CHECK(t.warnings() == 1);
    }
  }
}

TEST_CASE("a TIM2 SMCR without the renamed SMS_1 or TS_1 throws at construction")
{
  for(auto const* const field : {"SMS_1", "TS_1"})
  {
    CAPTURE(field);
    auto       desc = h573();
    auto const unit =
        std::ranges::find(desc.peripherals, std::string{"TIM2"}, &popcorn::peripheral_desc::name);
    REQUIRE(unit != desc.peripherals.end());
    auto const smcr =
        std::ranges::find(unit->registers, std::string{"SMCR"}, &popcorn::register_desc::name);
    REQUIRE(smcr != unit->registers.end());
    REQUIRE(
        std::erase_if(
            smcr->fields, [&](popcorn::field_desc const& bits) { return bits.name == field; })
        == 1);
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories({}, desc);
    CHECK_THROWS_AS(stm32_fixture(desc, firmware, std::move(options)), popcorn::config_error);
  }
}

TEST_CASE("an H573 timer whose CR2.MMS width disagrees with the variant throws at construction")
{
  struct leg
  {
    std::string_view unit;
    bool             wide;
  };
  for(auto const [unit, wide] : {
          leg{"TIM5",  true },
          leg{"TIM12", false},
          leg{"TIM15", false}
  })
  {
    CAPTURE(unit);
    auto       desc = h573();
    auto const timer =
        std::ranges::find(desc.peripherals, std::string{unit}, &popcorn::peripheral_desc::name);
    REQUIRE(timer != desc.peripherals.end());
    auto const cr2 =
        std::ranges::find(timer->registers, std::string{"CR2"}, &popcorn::register_desc::name);
    REQUIRE(cr2 != timer->registers.end());
    if(wide)
    {
      REQUIRE(
          std::erase_if(
              cr2->fields, [](popcorn::field_desc const& bits) { return bits.name == "MMS_1"; })
          == 1);
    }
    else
    {
      cr2->fields.push_back({.name = "MMS_1", .lsb = 25, .width = 1});
    }
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories({}, desc);
    CHECK_THROWS_AS(stm32_fixture(desc, firmware, std::move(options)), popcorn::config_error);
  }
}

TEST_CASE("ITR7, the OC1 output of TIM13, clocks TIM3 at each OC1 rising edge, none in OC1M 1000")
{
  {
    timers t;
    t.base("TIM5", 99);
    t.master("TIM5");
    t.base("TIM13", 99);
    t.write("TIM13", "CCR1", 25);
    t.write("TIM13", "CCMR1_Output", pwm1);
    t.set("TIM13", "CCER", "CC1E", 1);
    t.base("TIM3", 0xffff);
    t.slave("TIM3", ts_itr7, sms_external);
    t.enable("TIM3");
    t.enable("TIM5");
    t.enable("TIM13");
    t.at_tick(1050);
    CHECK(t.read("TIM3", "CNT") == 10);
    CHECK(t.warnings() == 0);
  }
  timers t;
  t.base("TIM13", 99);
  t.write("TIM13", "CCMR1_Output", 1u << 16);
  t.base("TIM3", 0xffff);
  t.slave("TIM3", ts_itr7, sms_external);
  t.enable("TIM3");
  t.enable("TIM13");
  t.at_tick(1050);
  CHECK(t.read("TIM3", "CNT") == 0);
  CHECK(t.warnings() == 1);
}

namespace
{
constexpr std::uint64_t hse_hz = 48'000'000;

auto hse_board() -> stm32::board
{
  stm32::board board;
  board.rcc.hse_hz = hse_hz;
  return board;
}

struct hse_timers : timers
{
  explicit hse_timers(bool trigger = false) : timers{hse_board(), false, trigger}
  {
    set("RCC", "RCC_CR", "HSEON", 1);
    run_until(now() + 200'000'000);
    REQUIRE(get("RCC", "RCC_CR", "HSERDY") == 1);
    set("RCC", "RCC_CFGR1", "SW", 2);
    run_until(now() + 200'000'000);
    REQUIRE(get("RCC", "RCC_CFGR1", "SWS") == 2);
  }

  void at_hse_tick(std::uint64_t from, std::uint64_t ticks) const
  {
    run_until(from + (ticks * 1'000'000'000'000 + hse_hz - 1) / hse_hz);
  }
};
}

TEST_CASE(
    "RM0481 Table 282: TIM1 CC2 and TIM13 CC1 pulse tim1_cc2 and tim13_cc1 at each match, 48 MHz")
{
  for(auto const& [unit, signal, ccr] : {
          std::tuple{
                     std::string_view{"TIM1"},  std::string_view{"tim1_cc2"},  std::string_view{"CCR2"}},
          std::tuple{
                     std::string_view{"TIM13"}, std::string_view{"tim13_cc1"}, std::string_view{"CCR1"}},
  })
  {
    CAPTURE(signal);
    hse_timers  t{true};
    auto* const probe = t.f->model_of<popcorn::testing::trigger_probe>("RNG");
    REQUIRE(probe != nullptr);
    probe->watched = signal;
    probe->listen(true);
    t.base(unit, 99);
    t.write(unit, ccr, 41);
    t.enable(unit);
    auto const from = t.now();
    t.at_hse_tick(from, 350);
    REQUIRE(probe->rises.size() == 4);
    for(std::uint64_t k = 0; k < 4; ++k)
    {
      CAPTURE(k);
      auto const exact = static_cast<unsigned __int128>(from) * hse_hz
                       + static_cast<unsigned __int128>(41 + 100 * k) * 1'000'000'000'000;
      auto const ps    = static_cast<std::uint64_t>((exact + hse_hz - 1) / hse_hz);
      CHECK(probe->rises[k].ps == ps);
      CHECK(probe->rises[k].hz == hse_hz);
      CHECK(
          probe->rises[k].lag
          == static_cast<std::uint64_t>(static_cast<unsigned __int128>(ps) * hse_hz - exact));
    }
  }
}

TEST_CASE("TIM13 OC1 with CC1P 1 in PWM mode 1 gates TIM3 on ITR7, 75 ticks of every 100 at 48 MHz")
{
  hse_timers t;
  t.base("TIM13", 99);
  t.write("TIM13", "CCR1", 25);
  t.write("TIM13", "CCMR1_Output", pwm1);
  t.set("TIM13", "CCER", "CC1E", 1);
  t.set("TIM13", "CCER", "CC1P", 1);
  t.base("TIM3", 0xffff);
  t.slave("TIM3", ts_itr7, sms_gated);
  t.enable("TIM3");
  t.enable("TIM13");
  auto const from = t.now();
  t.at_hse_tick(from, 1000);
  CHECK(t.read("TIM3", "CNT") == 750);
  t.at_hse_tick(from, 1010);
  CHECK(t.read("TIM3", "CNT") == 750);
  t.at_hse_tick(from, 1050);
  CHECK(t.read("TIM3", "CNT") == 775);
  std::size_t exact = 0;
  for(std::uint64_t period = 11; period < 111; ++period)
  {
    t.at_hse_tick(from, period * 100 + 60);
    auto const open = period * 75 + t.read("TIM13", "CNT") - 25;
    exact += t.read("TIM3", "CNT") == open ? 1u : 0u;
  }
  CHECK(exact == 100);
  CHECK(t.warnings() == 0);
}

TEST_CASE(
    "TIM3 at 48 MHz reset by an edge on a 48 MHz or 7 MHz clock counts from its exact instant")
{
  struct leg
  {
    std::uint64_t hz;
    std::uint64_t lag;
    std::uint32_t count;
  };
  for(auto const [hz, lag, count] : {
          leg{hse_hz,    hse_hz / 3 - 1, 9 },
          leg{7'000'000, 3'000'000,      10},
          leg{7'000'000, 1'000'000,      9 }
  })
  {
    CAPTURE(hz);
    hse_timers t;
    t.base("TIM3", 0xffff);
    t.slave("TIM3", ts_itr4, sms_reset);
    t.enable("TIM3");
    auto const  read = t.now();
    auto* const sink = dynamic_cast<stm32::trigger_sink*>(t.f->target->model("TIM3"));
    REQUIRE(sink != nullptr);
    sink->triggered(
        "tim5_trgo", stm32::trigger_edge::rising, stm32::trigger_time{read - 208'333, hz, lag});
    CHECK(t.read("TIM3", "CNT") == count);
  }
}

TEST_CASE("TIM3 triggered by a TIM13 OC1 rise on ITR7 at 48 MHz reads every TIM13 tick since it")
{
  hse_timers t;
  t.base("TIM13", 99);
  t.write("TIM13", "CCR1", 50);
  t.write("TIM13", "CCMR1_Output", pwm2);
  t.set("TIM13", "CCER", "CC1E", 1);
  t.base("TIM3", 0xffff);
  t.slave("TIM3", ts_itr7, sms_trigger);
  t.enable("TIM13");
  auto const  from  = t.now();
  std::size_t exact = 0;
  for(std::uint64_t period = 1; period <= 100; ++period)
  {
    t.at_hse_tick(from, period * 100);
    auto const master = period * 100 + t.read("TIM13", "CNT");
    exact += t.read("TIM3", "CNT") == master - 50 ? 1u : 0u;
  }
  CHECK(exact == 100);
  CHECK(t.get("TIM3", "SR", "TIF") == 1);
}

TEST_CASE("under strict a CPU MMS2, MMS_1, TRC, CCDS, TI1F_ED or reserved CC1S write throws")
{
  struct leg
  {
    std::string_view unit;
    std::string_view reg;
    std::string_view field;
    std::uint32_t    value;
    std::size_t      warnings;
  };
  auto const legs = std::array{
      leg{"TIM1",  "CR2",         "MMS2",  0b0010, 1},
      leg{"TIM3",  "CR2",         "MMS_1", 1,      2},
      leg{"TIM3",  "CCMR1_Input", "CC1S",  0b11,   3},
      leg{"TIM3",  "CR2",         "CCDS",  1,      4},
      leg{"TIM3",  "SMCR",        "",      0,      5},
      leg{"TIM3",  "CCMR2_Input", "CC4S",  0b11,   5},
      leg{"TIM13", "CCMR1_Input", "CC1S",  0b10,   6},
  };
  timers     t{{}, true};
  auto const value = [&](leg const& l) {
    return l.field.empty() ? sms_gated | (4u << 4) : l.value << t.lsb(l.unit, l.reg, l.field);
  };
  for(auto const& l : legs)
  {
    CAPTURE(l.unit);
    CAPTURE(l.reg);
    CAPTURE(l.field);
    CHECK_THROWS_AS(t.cpu_write(l.unit, l.reg, value(l)), popcorn::model_error);
    CHECK(t.read(l.unit, l.reg) == 0);
    CHECK(t.warnings() == l.warnings);
  }
  for(auto const& l : legs)
  {
    CAPTURE(l.unit);
    CAPTURE(l.reg);
    CAPTURE(l.field);
    t.write(l.unit, l.reg, value(l));
    CHECK(t.read(l.unit, l.reg) == value(l));
  }
  CHECK(t.warnings() == 6);
}

TEST_CASE("an H573 TIM13 description with CR1.DIR or CMS, an up-counting OC1 source, throws")
{
  for(auto const& added : {
          popcorn::field_desc{.name = "DIR", .lsb = 4, .width = 1},
          popcorn::field_desc{.name = "CMS", .lsb = 5, .width = 2}
  })
  {
    CAPTURE(added.name);
    auto       desc = h573();
    auto const timer =
        std::ranges::find(desc.peripherals, std::string{"TIM13"}, &popcorn::peripheral_desc::name);
    REQUIRE(timer != desc.peripherals.end());
    auto const cr1 =
        std::ranges::find(timer->registers, std::string{"CR1"}, &popcorn::register_desc::name);
    REQUIRE(cr1 != timer->registers.end());
    cr1->fields.push_back(added);
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories({}, desc);
    CHECK_THROWS_AS(stm32_fixture(desc, firmware, std::move(options)), popcorn::config_error);
  }
}

TEST_CASE("TIM12, TIM13 and TIM14 keep the IC1F and IC2F bit 3 of CCMR1 the output view lacks")
{
  struct leg
  {
    std::string_view unit;
    std::uint32_t    value;
  };
  timers t;
  for(auto const [unit, value] : {
          leg{"TIM12", 0xF1F1},
          leg{"TIM13", 0xF1  },
          leg{"TIM14", 0xF1  }
  })
  {
    CAPTURE(unit);
    t.write(unit, "CCMR1_Input", value);
    CHECK(t.read(unit, "CCMR1_Input") == value);
    CHECK(t.read(unit, "CCMR1_Output") == value);
    CHECK(t.get(unit, "CCMR1_Input", "IC1F") == 0b1111);
  }
  CHECK(t.get("TIM12", "CCMR1_Input", "IC2F") == 0b1111);
}

TEST_CASE("TIM12 has two channels, TIM13 one, and an absent channel has no register and no flag")
{
  timers t;
  CHECK(popcorn::find_register(t.desc("TIM12"), "CCR2") != nullptr);
  CHECK(popcorn::find_register(t.desc("TIM12"), "CCR3") == nullptr);
  CHECK(popcorn::find_register(t.desc("TIM13"), "CCR1") != nullptr);
  CHECK(popcorn::find_register(t.desc("TIM13"), "CCR2") == nullptr);
  t.base("TIM12", 99);
  t.write("TIM12", "CCR1", 20);
  t.write("TIM12", "CCR2", 40);
  t.base("TIM13", 99);
  t.write("TIM13", "CCR1", 30);
  t.enable("TIM12");
  t.enable("TIM13");
  t.at_tick(25);
  CHECK(t.get("TIM12", "SR", "CC1IF") == 1);
  CHECK(t.get("TIM12", "SR", "CC2IF") == 0);
  CHECK(t.get("TIM13", "SR", "CC1IF") == 0);
  t.at_tick(45);
  CHECK(t.get("TIM12", "SR", "CC2IF") == 1);
  CHECK(t.get("TIM13", "SR", "CC1IF") == 1);
  CHECK(t.get("TIM12", "SR", "UIF") == 0);
  t.set("TIM12", "DIER", "UIE", 1);
  t.at_tick(105);
  CHECK(t.get("TIM12", "SR", "UIF") == 1);
  CHECK(t.pending(tim12_irq));
}

namespace
{
auto frozen_count(bool freeze) -> std::uint32_t
{
  timers t;
  t.base("TIM12", 0xffff);
  t.enable("TIM12");
  t.at_tick(1000);
  if(freeze)
  {
    popcorn::testing::set_debug_freeze(*t.f, "DBG_TIM12_STOP", true);
  }
  t.f->target->hold_core(true);
  t.at_tick(6000);
  auto const held = t.read("TIM12", "CNT");
  t.f->target->hold_core(false);
  t.at_tick(7000);
  return held;
}
}

TEST_CASE("DBG_TIM12_STOP freezes TIM12 while the core is held, a clear one lets it count on")
{
  CHECK(frozen_count(true) == 1000);
  CHECK(frozen_count(false) == 6000);
}

namespace
{
struct stale_run
{
  std::uint32_t update_flag;
  std::uint32_t count;
};

auto tim12_after_reset(bool wait) -> stale_run
{
  timers t;
  t.base("TIM12", 999);
  t.enable("TIM12");
  t.at_tick(100);
  REQUIRE(t.get("TIM12", "SR", "UIF") == 0);
  popcorn::testing::rcc_reset_pulse(*t.f, "RCC_APB1LRSTR", "TIM12RST");
  if(wait)
  {
    t.at_tick(1100);
  }
  auto const begin = wait ? 1100u : 100u;
  t.base("TIM12", 0xffff);
  t.enable("TIM12");
  t.at_tick(begin + 950);
  return {t.get("TIM12", "SR", "UIF"), t.read("TIM12", "CNT")};
}
}

TEST_CASE("TIM12RST drops the update event queued before it, a restart before its time is alone")
{
  auto const early = tim12_after_reset(false);
  auto const late  = tim12_after_reset(true);
  CHECK(early.update_flag == 0);
  CHECK(early.update_flag == late.update_flag);
  CHECK(early.count == late.count);
  CHECK(early.count >= 949);
}

TEST_CASE("SPI4 has a FIFO of 8 bytes and SPI1 one of 16")
{
  for(auto const& [unit, sent] : {
          std::pair<std::string_view, std::size_t>{"SPI4", 8 },
          std::pair<std::string_view, std::size_t>{"SPI1", 10}
  })
  {
    CAPTURE(unit);
    spis s{unit};
    s.controller(unit, 7, 10, 8);
    for(std::uint32_t index = 0; index < 10; ++index)
    {
      s.send(unit, index);
    }
    s.set(unit, "SPI_CR1", "CSTART", 1);
    s.run_until(s.now() + 20'000'000);
    REQUIRE(s.device->frames.size() == sent);
    for(std::uint32_t index = 0; index < sent; ++index)
    {
      CHECK(s.device->frames[index] == index);
    }
  }
}

TEST_CASE("SPI4 counts the frames left of its transfer in CTSIZE, 5 then 4 then 3 as on silicon")
{
  spis s{"SPI4"};
  s.controller("SPI4", 7, 5);
  for(std::uint32_t index = 0; index < 5; ++index)
  {
    s.send("SPI4", index);
  }
  CHECK(s.get("SPI4", "SPI_SR", "CTSIZE") == 5);
  s.set("SPI4", "SPI_CR1", "CSTART", 1);
  for(std::size_t frames = 1; frames <= 2; ++frames)
  {
    for(int step = 0; step < 1000 and s.device->frames.size() < frames; ++step)
    {
      s.run_until(s.now() + 10'000);
    }
    REQUIRE(s.device->frames.size() == frames);
    CHECK(s.get("SPI4", "SPI_SR", "CTSIZE") == 5 - frames);
  }
}

TEST_CASE("the AT25SF with the JEDEC ID 1F 87 01 of the bootloader answers on SPI4")
{
  auto flash =
      std::make_shared<devices::at25sf>(devices::at25sf_options{.part = devices::at25sf321});
  auto         device = std::make_shared<recorded_flash>(flash);
  stm32::board board{};
  board.spi["SPI4"].push_back({.device = device});
  rig r{board};
  r.set("RCC", "RCC_APB2ENR", "SPI4EN", 1);
  r.set("SPI4", "SPI_CFG1", "MBR", 0);
  r.set("SPI4", "SPI_CFG1", "DSIZE", 7);
  r.set("SPI4", "SPI_CFG2", "SSM", 1);
  r.set("SPI4", "SPI_CFG2", "SSOE", 1);
  r.set("SPI4", "SPI_CFG2", "MASTER", 1);
  r.set("SPI4", "SPI_CR2", "TSIZE", 4);
  r.set("SPI4", "SPI_CR1", "SSI", 1);
  r.set("SPI4", "SPI_CR1", "SPE", 1);
  for(std::uint32_t const byte : {0x9Fu, 0u, 0u, 0u})
  {
    r.write("SPI4", "SPI_TXDR", byte, 1);
  }
  r.set("SPI4", "SPI_CR1", "CSTART", 1);
  for(int step = 0; step < 1000 and device->replies.size() < 4; ++step)
  {
    r.run_until(r.now() + 100'000);
  }
  REQUIRE(device->replies.size() == 4);
  CHECK(device->replies == std::vector<std::uint32_t>{0xFF, 0x1F, 0x87, 0x01});
}

namespace
{
struct frames_after_reset
{
  std::vector<std::uint64_t> offsets;
  std::vector<std::uint32_t> data;
};

auto spi4_after_reset(bool wait) -> frames_after_reset
{
  spis s{"SPI4"};
  s.controller("SPI4", 7, 4);
  for(std::uint32_t index = 0; index < 4; ++index)
  {
    s.send("SPI4", 0x10 + index);
  }
  s.set("SPI4", "SPI_CR1", "CSTART", 1);
  for(int step = 0; step < 1000 and s.device->frames.empty(); ++step)
  {
    s.run_until(s.now() + 50'000);
  }
  auto const stale = s.device->frames.size();
  popcorn::testing::rcc_reset_pulse(*s.f, "RCC_APB2RSTR", "SPI4RST");
  if(wait)
  {
    s.run_until(s.now() + 5'000'000);
  }
  s.device->frames.clear();
  s.device->times.clear();
  s.controller("SPI4", 7, 2);
  s.send("SPI4", 0x20);
  s.send("SPI4", 0x21);
  auto const begin = s.now();
  s.set("SPI4", "SPI_CR1", "CSTART", 1);
  s.run_until(begin + 20'000'000);
  REQUIRE(stale >= 1);
  frames_after_reset out;
  for(auto const& at : s.device->times)
  {
    out.offsets.push_back(at.ps - begin);
  }
  out.data = s.device->frames;
  return out;
}
}

TEST_CASE("SPI4RST drops the frame queued before it, a new transfer before its time is alone")
{
  auto const early = spi4_after_reset(false);
  auto const late  = spi4_after_reset(true);
  CHECK(early.data == std::vector<std::uint32_t>{0x20, 0x21});
  CHECK(early.data == late.data);
  CHECK(early.offsets == late.offsets);
}

TEST_CASE("TIM12RST moves the generation past the tick event queued before it, twice")
{
  timers                    t;
  std::vector<std::int64_t> seen;
  for(int pulse = 0; pulse < 2; ++pulse)
  {
    t.base("TIM12", 999);
    t.enable("TIM12");
    t.at_tick(100 * (pulse + 1));
    auto const before = popcorn::testing::saved_integer(*t.f, "TIM12", "generation");
    REQUIRE(before.has_value());
    REQUIRE(popcorn::testing::queued_event(*t.f, "TIM12", 0, static_cast<std::uint64_t>(*before)));
    seen.push_back(*before);
    popcorn::testing::rcc_reset_pulse(*t.f, "RCC_APB1LRSTR", "TIM12RST");
    auto const after = popcorn::testing::saved_integer(*t.f, "TIM12", "generation");
    REQUIRE(after.has_value());
    CHECK(*after > *before);
  }
  CHECK(seen[1] > seen[0]);
}

TEST_CASE("SPI4RST moves the frame generation past the event queued before it, twice")
{
  spis                      s{"SPI4"};
  std::vector<std::int64_t> seen;
  for(int pulse = 0; pulse < 2; ++pulse)
  {
    s.controller("SPI4", 7, 1);
    s.send("SPI4", 0x5A);
    s.set("SPI4", "SPI_CR1", "CSTART", 1);
    s.run_until(s.now() + 100'000);
    auto const before = popcorn::testing::saved_integer(*s.f, "SPI4", "generation");
    REQUIRE(before.has_value());
    REQUIRE(popcorn::testing::queued_event(*s.f, "SPI4", 0, static_cast<std::uint64_t>(*before)));
    seen.push_back(*before);
    popcorn::testing::rcc_reset_pulse(*s.f, "RCC_APB2RSTR", "SPI4RST");
    auto const after = popcorn::testing::saved_integer(*s.f, "SPI4", "generation");
    REQUIRE(after.has_value());
    CHECK(*after > *before);
  }
  CHECK(seen[1] > seen[0]);
}

TEST_CASE("coverage classifies SPI_CFG2 as served and SPI_I2SCFGR as unimplemented")
{
  rig r;
  r.f->target->enable_coverage({}, true);
  constexpr popcorn::address stub = 0x2000'0200;
  REQUIRE(r.f->target->mem().write(stub, 4, 0xE7FE'6802).has_value());
  for(auto const name : {"SPI_CFG2", "SPI_I2SCFGR"})
  {
    r.f->set("r0", r.at("SPI1", name));
    r.f->set("pc", stub | 1u);
    r.run_until(r.now() + 1'000'000);
  }
  auto const found = r.f->target->coverage().unmodelled;
  REQUIRE(found.size() == 1);
  CHECK(found.front().peripheral == "SPI1");
  CHECK(found.front().reg == "I2SCFGR");
  CHECK(found.front().reason == core::unmodelled_reason::unimplemented);
}

TEST_CASE("SPI1 on the H573 keeps the whole SPI_CFG1 while SPE is set, TXDMAEN included")
{
  spis s{"SPI1"};
  s.controller("SPI1", 7, 1);
  auto&      regs   = s.f->target->peripheral("SPI1");
  auto const offset = s.reg("SPI1", "SPI_CFG1").offset;
  auto const cfg1   = s.read("SPI1", "SPI_CFG1");
  auto const fields = s.reg("SPI1", "SPI_CFG1").fields;
  auto const tx =
      std::ranges::find(fields, std::string_view{"TXDMAEN"}, &popcorn::field_desc::name);
  REQUIRE(tx != fields.end());
  auto const result = s.f->target->model("SPI1")->on_write(
      regs, offset, 4, cfg1, cfg1 | (1u << tx->lsb), {.pc = 0, .now = icount{0}});
  REQUIRE(result.has_value());
  CHECK(s.get("SPI1", "SPI_CFG1", "TXDMAEN") == 0);
  s.run_until(s.now() + 1'000'000);
  CHECK(s.warnings() == 1);
}
