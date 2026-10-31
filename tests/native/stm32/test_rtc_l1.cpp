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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
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
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware     = "l152xe_l1_rtc_ll";
constexpr std::string_view firmware_b   = "l152xb_l1_rtc_ll";
constexpr std::string_view idle_image   = "l152xe_l1_exti_ll";
constexpr std::string_view idle_image_b = "l152xb_l1_exti_ll";
constexpr std::uint64_t    us           = 1'000'000;
constexpr std::uint64_t    ms           = 1'000 * us;
constexpr std::uint64_t    second       = 1'000 * ms;
constexpr std::uint64_t    rtc_cycle    = 1'000'000'000'000 / 32'768;
constexpr std::uint32_t    init_bit     = 1u << 7;
constexpr std::uint32_t    initf_bit    = 1u << 6;
constexpr std::uint32_t    rsf_bit      = 1u << 5;
constexpr std::uint32_t    feb_28       = 0x26'C228;
constexpr std::uint32_t    march_1      = 0x26'E301;

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

  explicit rig(
      part_description const& desc   = l152re(),
      std::string_view        image  = idle_image,
      bool                    strict = false)
      : part{desc}, elf{image}
  {
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories({}, desc);
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "DAC", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f = std::make_unique<stm32_fixture>(desc, image, std::move(options));
    f->set("pc", sym(elf, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
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

  [[nodiscard]] auto read(std::string_view name, std::string_view unit = "RTC") const
      -> std::uint32_t
  {
    auto const value = f->target->mem().read(at(unit, name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view name, std::uint32_t value, std::string_view unit = "RTC") const
  {
    REQUIRE(f->target->mem().write(at(unit, name), 4, value).has_value());
  }

  [[nodiscard]] auto probe() const -> popcorn::periph::model_context&
  {
    auto* const found = f->model_of<bus_probe>("DAC");
    REQUIRE(found != nullptr);
    return found->context;
  }

  [[nodiscard]] auto cpu_read_at(popcorn::address address) const -> std::uint32_t
  {
    auto const value = probe().services.read(address, 4, popcorn::initiator::cpu);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto cpu_read(std::string_view name) const -> std::uint32_t
  {
    return cpu_read_at(at("RTC", name));
  }

  void cpu_write(std::string_view name, std::uint32_t value) const
  {
    CHECK(probe().services.write(at("RTC", name), 4, value, popcorn::initiator::cpu).has_value());
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

  void later(std::uint64_t ps) const
  {
    run_until(now() + ps);
  }

  void start_clock() const
  {
    write("CSR", read("CSR", "RCC") | (1u << 8), "RCC");
    for(int step = 0; step < 5'000 and f->field("RCC", "CSR", "LSERDY") == 0; ++step)
    {
      later(ms);
    }
    REQUIRE(f->field("RCC", "CSR", "LSERDY") == 1);
    write("CSR", read("CSR", "RCC") | (1u << 16) | (1u << 22), "RCC");
  }

  void unlock() const
  {
    write("WPR", 0xCA);
    write("WPR", 0x53);
  }

  void enter_init() const
  {
    unlock();
    write("ISR", ~0u);
    later(3 * rtc_cycle);
    REQUIRE((read("ISR") & initf_bit) != 0);
  }

  [[nodiscard]] auto isr(std::uint32_t bit) const -> std::uint32_t
  {
    return (read("ISR") & bit) != 0 ? 1 : 0;
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
  }
};

auto without_register(part_description desc, std::string_view name) -> part_description
{
  auto& rtc = *std::ranges::find(
      desc.peripherals, std::string_view{"RTC"}, &popcorn::peripheral_desc::name);
  std::erase_if(rtc.registers, [&](popcorn::register_desc const& r) { return r.name == name; });
  return desc;
}

auto make_machine(part_description desc) -> std::unique_ptr<core::machine>
{
  stm32::register_builtin_models();
  return std::make_unique<core::machine>(
      std::move(desc), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options());
}
}

TEST_CASE("RM0038 20.3.5: TR, DR, PRER, CALIBR and CR.FMT take writes under the key with INITF set")
{
  rig r;
  r.start_clock();
  r.write("TR", 0x12'3456);
  r.write("PRER", 0x7F'0010);
  r.write("CR", 1u << 6);
  CHECK(r.read("TR") == 0);
  CHECK(r.read("PRER") == 0x7F'00FF);
  CHECK(r.read("CR") == 0);
  r.unlock();
  r.write("CR", 1u << 6);
  r.write("CALIBR", 0x85);
  CHECK(r.read("CR") == 0);
  CHECK(r.read("CALIBR") == 0);
  r.write("ISR", init_bit);
  r.write("TR", 0x12'3456);
  CHECK(r.read("TR") == 0);
  r.later(3 * rtc_cycle);
  r.write("TR", 0x12'3456);
  r.write("DR", feb_28);
  r.write("PRER", 0x7F'00FF);
  r.write("CALIBR", 0x85);
  r.write("CR", 1u << 6);
  CHECK(r.read("TR") == 0x52'3456);
  CHECK(r.read("DR") == feb_28);
  CHECK(r.read("CALIBR") == 0x85);
  CHECK(r.read("CR") == 1u << 6);
  r.write("ISR", 0);
  r.later(2 * second + 10 * ms);
  CHECK(r.read("TR") == 0x52'3458);
}

TEST_CASE(
    "RM0038 20.3.5: INITF sets at the second RTCCLK edge after INIT, counting at the fourth after")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.write("ISR", init_bit);
  auto const entered_at = r.now();
  r.run_until(entered_at + rtc_cycle - us);
  CHECK(r.isr(initf_bit) == 0);
  r.run_until(entered_at + 2 * rtc_cycle + us);
  CHECK(r.isr(initf_bit) == 1);
  r.write("PRER", 0x00'0000);
  r.write("PRER", 0x00'0000);
  r.write("ISR", 0);
  CHECK(r.isr(initf_bit) == 0);
  auto const exited = r.now();
  r.run_until(exited + 4 * rtc_cycle + rtc_cycle / 2);
  CHECK(r.read("SSR") == 0);
  CHECK(r.read("TR") == 0x00'0000);
  r.run_until(exited + 5 * rtc_cycle + rtc_cycle / 2);
  CHECK(r.read("TR") == 0x00'0001);
}

TEST_CASE("RM0038 20.3.5: 0xCA then 0x53 unlocks, the reverse order or a wrong key locks")
{
  rig r;
  r.start_clock();
  r.write("WPR", 0x53);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
  r.write("WPR", 0x53);
  r.write("WPR", 0xCA);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
  r.unlock();
  r.write("WPR", 0xFF);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
  r.unlock();
  CHECK(r.read("WPR") == 0);
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 1);
}

TEST_CASE("RM0038 20.3.5: a system reset keeps the key state, a power-on locks again")
{
  rig r;
  r.start_clock();
  r.unlock();
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 1);
  r.f->target->reset(reset_kind::power_on);
  r.f->set("pc", sym(r.elf, "idle"));
  r.start_clock();
  r.write("ISR", init_bit);
  CHECK(r.isr(init_bit) == 0);
}

TEST_CASE("RM0038 20.6.4: RSF sets at the second RTCCLK edge after a clear under the key")
{
  rig r;
  r.start_clock();
  r.later(ms);
  REQUIRE(r.isr(rsf_bit) == 1);
  r.write("ISR", ~rsf_bit);
  CHECK(r.isr(rsf_bit) == 1);
  r.unlock();
  r.write("ISR", ~rsf_bit & ~init_bit);
  auto const cleared = r.now();
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(cleared + rtc_cycle - us);
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(cleared + 2 * rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE("RM0038 20.6.4: init mode clears RSF, set at the second edge after counting restarts")
{
  rig r;
  r.start_clock();
  r.later(ms);
  REQUIRE(r.isr(rsf_bit) == 1);
  r.enter_init();
  CHECK(r.isr(rsf_bit) == 0);
  r.later(ms);
  CHECK(r.isr(rsf_bit) == 0);
  r.write("ISR", 0);
  auto const exited = r.now();
  r.run_until(exited + 5 * rtc_cycle - us);
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(exited + 6 * rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE("RM0038 20.6.4: a 0 clears ALRAF to TAMP1F without the key, TAMP2F and TAMP3F with it")
{
  rig        r;
  auto&      regs   = r.f->target->peripheral("RTC");
  auto const offset = r.reg("RTC", "ISR").offset;
  auto const flags  = 0x3Fu << 8;
  auto const keyed  = 0x3u << 14;
  regs.poke(offset, regs.peek(offset) | flags | keyed);
  r.write("ISR", 0);
  CHECK((r.read("ISR") & flags) == 0);
  CHECK((r.read("ISR") & keyed) == keyed);
  CHECK((r.read("ISR") & 0x7u) == 0x7u);
  r.write("ISR", ~0u);
  CHECK((r.read("ISR") & flags) == 0);
  r.unlock();
  r.write("ISR", 0);
  CHECK((r.read("ISR") & keyed) == 0);
  CHECK((r.read("ISR") & 0x7u) == 0x7u);
}

TEST_CASE("RM0038 20.3.6: an SSR or TR read holds TR and DR until DR is read")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("TR", 0x23'5959);
  r.write("DR", feb_28);
  r.write("ISR", 0);
  r.later(ms);
  REQUIRE(r.isr(rsf_bit) == 1);
  r.later(second - 2 * ms - 4 * rtc_cycle);
  CHECK(r.cpu_read("TR") == 0x23'5959);
  r.later(10 * ms);
  CHECK(r.cpu_read("TR") == 0x23'5959);
  CHECK(r.cpu_read("DR") == feb_28);
  CHECK(r.cpu_read("SSR") <= 255);
  CHECK(r.cpu_read("TR") == 0);
  CHECK(r.cpu_read("DR") == march_1);
  CHECK(r.read("TR") == 0);
}

TEST_CASE(
    "RM0038 20.6.3: BYPSHAD reads the counters with RSF clear, RSF sets at the second edge after")
{
  rig r;
  r.start_clock();
  r.later(ms);
  r.unlock();
  r.write("CR", 1u << 5);
  CHECK(r.isr(rsf_bit) == 0);
  r.later(ms);
  CHECK(r.isr(rsf_bit) == 0);
  r.f->target->reset(reset_kind::software);
  r.f->set("pc", sym(r.elf, "idle"));
  CHECK(r.cpu_read("DR") == 0x00'2101);
  CHECK(r.cpu_read("TR") == r.read("TR"));
  r.later(second);
  CHECK(r.cpu_read("TR") == 0x00'0001);
  r.write("CR", 0);
  auto const cleared = r.now();
  r.run_until(cleared + rtc_cycle - us);
  CHECK(r.isr(rsf_bit) == 0);
  r.run_until(cleared + 2 * rtc_cycle + us);
  CHECK(r.isr(rsf_bit) == 1);
}

TEST_CASE("RM0038 20.3.1: SSR counts PREDIV_S down at RTCCLK / (PREDIV_A + 1)")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("PRER", 0x07'0FFF);
  r.write("ISR", 0);
  auto const tick = r.now() + (4 + 8) * rtc_cycle;
  r.run_until(tick - us);
  CHECK(r.read("SSR") == 0xFFF);
  r.run_until(tick + us);
  CHECK(r.read("SSR") == 0xFFE);
  r.run_until(tick + 4095 * 8 * rtc_cycle + us);
  CHECK(r.read("SSR") == 0xFFF);
  CHECK(r.read("TR") == 0x00'0001);
}

TEST_CASE("RM0038 20.6.5: PREDIV_S has 15 bits from Cat.2")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("PRER", 0x7F'FFFF);
  CHECK((r.read("PRER") & 0xFFFFu) == 0x7FFFu);
}

TEST_CASE("checklist 39: the RTC variant refuses a description that disagrees with it")
{
  CHECK_NOTHROW(static_cast<void>(make_machine(l152re())));
  CHECK_THROWS_AS(
      static_cast<void>(make_machine(without_register(l152re(), "SSR"))), popcorn::config_error);
  CHECK_THROWS_AS(
      static_cast<void>(make_machine(without_register(l152re(), "CALR"))), popcorn::config_error);
}

TEST_CASE("RM0038 6.1.2: a power-on and a brown-out reset the calendar and the backup registers")
{
  for(auto const kind : {reset_kind::power_on, reset_kind::brown_out})
  {
    CAPTURE(kind);
    rig r;
    r.start_clock();
    r.enter_init();
    r.write("DR", feb_28);
    r.write("ISR", 0);
    r.write("BKP3R", 0x1234);
    r.f->target->reset(kind);
    r.f->set("pc", sym(r.elf, "idle"));
    CHECK(r.read("BKP3R") == 0);
    CHECK(r.read("DR") == 0x00'2101);
    CHECK(r.read("ISR") == 0x7);
  }
}

TEST_CASE("RM0038 20.3.5: the backup registers take CPU and debugger writes with DBP 0, key locked")
{
  rig r;
  REQUIRE(r.f->field("PWR", "CR", "DBP") == 0);
  r.cpu_write("BKP1R", 0xA5A5'0001);
  r.write("BKP2R", 0xA5A5'0002);
  CHECK(r.cpu_read("BKP1R") == 0xA5A5'0001);
  CHECK(r.read("BKP2R") == 0xA5A5'0002);
}

TEST_CASE("RM0038 20.6.3: ADD1H adds an hour, SUB1H subtracts one except at hour 0, both read 0")
{
  rig r;
  r.start_clock();
  r.enter_init();
  r.write("TR", 0x00'3000);
  r.write("ISR", 0);
  r.later(ms);
  r.write("CR", 1u << 17);
  CHECK(r.read("TR") == 0x00'3000);
  r.write("CR", 1u << 16);
  CHECK(r.read("TR") == 0x01'3000);
  CHECK(r.read("CR") == 0);
  r.write("CR", 1u << 17);
  CHECK(r.read("TR") == 0x00'3000);
}

TEST_CASE("with DBG_RTC_STOP a held core stops the calendar until the release")
{
  rig r;
  r.start_clock();
  r.later(ms);
  popcorn::testing::set_debug_freeze(*r.f, "DBG_RTC_STOP", true);
  auto const at_hold = r.read("SSR");
  r.f->target->hold_core(true);
  r.later(2 * second);
  CHECK(r.read("SSR") == at_hold);
  CHECK(r.read("TR") == 0);
  r.f->target->hold_core(false);
  r.later(second);
  CHECK(r.read("TR") == 0x00'0001);
}
