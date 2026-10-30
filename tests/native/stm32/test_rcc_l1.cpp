#include "rcc_resets.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;

constexpr std::uint64_t mhz      = 1'000'000;
constexpr std::uint64_t hse_hz   = 8 * mhz;
constexpr std::uint64_t hsi_hz   = 16 * mhz;
constexpr auto          firmware = std::string_view{"l152xe_l1_rcc_ll"};

class listener final : public periph::model
{
public:
  explicit listener(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;

  [[nodiscard]] auto hz(std::string_view name) const -> std::uint64_t
  {
    return context.clock.hz(context.clock.domain(name));
  }
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, reset_kind) override
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

auto with_listener() -> core::machine_options
{
  auto               options = popcorn::testing::stm32_options();
  stm32::board const board{.rcc = {.hse_hz = hse_hz}};
  options.models = stm32::factories(board, l152re());
  options.models.insert_or_assign(
      "RI",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<listener>(ctx); });
  return options;
}

auto probe(stm32_fixture const& f) -> listener&
{
  auto* const found = f.model_of<listener>("RI");
  REQUIRE(found != nullptr);
  return *found;
}

auto field_of(std::string_view reg, std::string_view name) -> popcorn::field_desc const&
{
  auto const& rcc = *std::ranges::find(
      l152re().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  auto const* const found = popcorn::find_register(rcc, reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return *bits;
}

auto offset_of(std::string_view reg) -> std::uint32_t
{
  auto const& rcc = *std::ranges::find(
      l152re().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  return popcorn::find_register(rcc, reg)->offset;
}

auto peek(stm32_fixture const& f, std::string_view reg) -> std::uint32_t
{
  return f.target->peripheral("RCC").peek(offset_of(reg));
}

void write(stm32_fixture const& f, std::string_view reg, std::uint32_t value)
{
  auto const& rcc = *std::ranges::find(
      l152re().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  REQUIRE(f.target->mem().write(rcc.base + offset_of(reg), 4, value).has_value());
}

auto field_mask(std::string_view reg, std::string_view name) -> std::uint32_t
{
  auto const& bits = field_of(reg, name);
  return ((std::uint32_t{1} << bits.width) - 1u) << bits.lsb;
}

void set(stm32_fixture const& f, std::string_view reg, std::string_view name, std::uint32_t value)
{
  auto const& bits = field_of(reg, name);
  auto const  mask = ((std::uint32_t{1} << bits.width) - 1u) << bits.lsb;
  write(f, reg, (peek(f, reg) & ~mask) | (value << bits.lsb));
}

void idle_for(stm32_fixture const& f, std::chrono::microseconds span)
{
  auto const now     = f.target->virtual_clock().time();
  auto const stopped = f.run_to_stop(
      {.max_instructions = icount{1'000'000},
       .until_time       = vtime{now.ps + static_cast<std::uint64_t>(span.count()) * 1'000'000}});
  REQUIRE(stopped.reason == stop_reason::time);
}

void backup_access(stm32_fixture const& f)
{
  auto const& pwr = *std::ranges::find(
      l152re().peripherals, std::string_view{"PWR"}, &popcorn::peripheral_desc::name);
  auto const* const cr = popcorn::find_register(pwr, "CR");
  auto const        dbp =
      std::ranges::find(cr->fields, std::string_view{"DBP"}, &popcorn::field_desc::name);
  auto const at  = pwr.base + cr->offset;
  auto const old = f.target->mem().read(at, 4);
  REQUIRE(old.has_value());
  REQUIRE(f.target->mem().write(at, 4, *old | (std::uint32_t{1} << dbp->lsb)).has_value());
  REQUIRE(f.field("PWR", "CR", "DBP") == 1);
}

auto idle_fixture() -> stm32_fixture
{
  stm32_fixture f{l152re(), firmware, with_listener()};
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  return f;
}

constexpr auto delay = std::chrono::microseconds{150};

void pll_on(stm32_fixture const& f, std::uint32_t src, std::uint32_t mul, std::uint32_t div)
{
  set(f, "CR", "PLLON", 0);
  idle_for(f, delay);
  set(f, "CFGR", "PLLSRC", src);
  set(f, "CFGR", "PLLMUL", mul);
  set(f, "CFGR", "PLLDIV", div);
  set(f, "CR", "PLLON", 1);
  idle_for(f, delay);
  REQUIRE(f.field("RCC", "CR", "PLLRDY") == 1);
}
}

TEST_CASE("RM0038 6.3: the LL sequence runs MSI range 6, then HSI x 6 / 3 at 32 MHz on SYSCLK")
{
  stm32_fixture f{l152re(), firmware, with_listener()};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 11);
  CHECK(log[0] == 0x0C00'0000u);
  CHECK(log[1] == 0);
  CHECK(log[2] == 4'194'304);
  CHECK(log[3] > 0);
  CHECK(log[4] > 0);
  CHECK(log[5] > 0);
  CHECK(log[7] == 32 * mhz);
  CHECK(log[8] == 32 * mhz);
  CHECK(log[9] == 16 * mhz);
  CHECK(log[10] == 32 * mhz);
  auto const& clocks = probe(f);
  for(auto const name : {"sysclk", "hclk", "pclk2", "pllclk"})
  {
    CAPTURE(name);
    CHECK(clocks.hz(name) == 32 * mhz);
  }
  CHECK(clocks.hz("pclk1") == 16 * mhz);
  CHECK(clocks.hz("tim2") == 0);
  CHECK(clocks.hz("msi") == 4'194'304);
  CHECK(f.target->virtual_clock().core_hz() == 32 * mhz);
  CHECK(f.field("RCC", "CFGR", "SWS") == 3);
}

TEST_CASE("the xb LL build of the sequence reaches 32 MHz on STM32L152RBTx")
{
  stm32_fixture f{l152rb(), "l152xb_l1_rcc_ll"};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() == 11);
  CHECK(log[7] == 32 * mhz);
  CHECK(f.target->virtual_clock().core_hz() == 32 * mhz);
}

TEST_CASE("RM0038 6.3.2: MSIRANGE n runs the MSI and the core at 32768 x 2^(n+1) Hz, 7 at 0 Hz")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  CHECK(clocks.hz("msi") == 2'097'152);
  CHECK(f.target->virtual_clock().core_hz() == 2'097'152);
  std::vector<core::log_record> logged;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  for(std::uint32_t range = 0; range < 7; ++range)
  {
    CAPTURE(range);
    set(f, "ICSCR", "MSIRANGE", range);
    CHECK(clocks.hz("msi") == std::uint64_t{32'768} << (range + 1));
    CHECK(f.target->virtual_clock().core_hz() == std::uint64_t{32'768} << (range + 1));
  }
  set(f, "ICSCR", "MSIRANGE", 7);
  CHECK(clocks.hz("msi") == 0);
  CHECK(f.target->virtual_clock().core_hz() == 4'194'304);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].reg == "CFGR");
}

TEST_CASE("RM0038 6.3.3: PLLMUL codes 0 to 8 multiply by 3 to 48, PLLDIV 1 to 3 divide by 2 to 4")
{
  auto                                   f      = idle_fixture();
  auto const&                            clocks = probe(f);
  constexpr std::array<std::uint64_t, 9> factors{3, 4, 6, 8, 12, 16, 24, 32, 48};
  set(f, "CR", "HSION", 1);
  set(f, "CR", "HSEON", 1);
  for(std::uint32_t code = 0; code < 16; ++code)
  {
    CAPTURE(code);
    pll_on(f, 0, code, 3);
    CHECK(clocks.hz("pllclk") == (code < 9 ? hsi_hz * factors[code] / 4 : 0));
  }
  for(std::uint32_t code = 0; code < 4; ++code)
  {
    CAPTURE(code);
    pll_on(f, 1, 2, code);
    CHECK(clocks.hz("pllclk") == (code == 0 ? 0 : hse_hz * 6 / (code + 1)));
  }
}

TEST_CASE("RM0038 6.3.3: SW 0 to 3 select MSI, HSI, HSE and the PLL, SWS follows once ready")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  set(f, "CR", "HSION", 1);
  set(f, "CR", "HSEON", 1);
  pll_on(f, 0, 2, 1);
  constexpr std::array<std::uint64_t, 4> rates{2'097'152, hsi_hz, hse_hz, hsi_hz * 6 / 2};
  for(std::uint32_t code : {1u, 2u, 3u, 0u})
  {
    CAPTURE(code);
    set(f, "CFGR", "SW", code);
    CHECK(f.field("RCC", "CFGR", "SWS") == code);
    CHECK(clocks.hz("sysclk") == rates[code]);
    CHECK(f.target->virtual_clock().core_hz() == rates[code]);
  }
}

TEST_CASE("RM0038 6.3.1: the ON bit of the SYSCLK source and of the PLL input holds")
{
  auto f = idle_fixture();
  set(f, "CR", "MSION", 0);
  CHECK(f.field("RCC", "CR", "MSION") == 1);
  set(f, "CR", "HSION", 1);
  pll_on(f, 0, 2, 1);
  set(f, "CFGR", "SW", 3);
  REQUIRE(f.field("RCC", "CFGR", "SWS") == 3);
  set(f, "CR", "HSION", 0);
  set(f, "CR", "PLLON", 0);
  CHECK(f.field("RCC", "CR", "HSION") == 1);
  CHECK(f.field("RCC", "CR", "PLLON") == 1);
  set(f, "CR", "MSION", 0);
  CHECK(f.field("RCC", "CR", "MSION") == 0);
}

TEST_CASE("RM0038 6.3.3: PLLSRC, PLLMUL and PLLDIV hold while the PLL is on, take once it is off")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  set(f, "CR", "HSION", 1);
  pll_on(f, 0, 2, 1);
  set(f, "CFGR", "PLLMUL", 3);
  set(f, "CFGR", "PLLDIV", 3);
  set(f, "CFGR", "PLLSRC", 1);
  CHECK(f.field("RCC", "CFGR", "PLLMUL") == 2);
  CHECK(f.field("RCC", "CFGR", "PLLDIV") == 1);
  CHECK(f.field("RCC", "CFGR", "PLLSRC") == 0);
  set(f, "CFGR", "HPRE", 8);
  CHECK(f.field("RCC", "CFGR", "HPRE") == 8);
  CHECK(clocks.hz("pllclk") == 48 * mhz);
  set(f, "CR", "PLLON", 0);
  idle_for(f, delay);
  set(f, "CFGR", "PLLMUL", 3);
  CHECK(f.field("RCC", "CFGR", "PLLMUL") == 3);
}

TEST_CASE("RM0038 6.3.1: CSSON is set and cleared by software")
{
  auto f = idle_fixture();
  set(f, "CR", "CSSON", 1);
  CHECK(f.field("RCC", "CR", "CSSON") == 1);
  set(f, "CR", "CSSON", 0);
  CHECK(f.field("RCC", "CR", "CSSON") == 0);
}

TEST_CASE("RM0038 6.3.4: a ready flag sets under its enable and xxxRDYC clears it")
{
  auto f = idle_fixture();
  set(f, "CIR", "HSIRDYIE", 1);
  set(f, "CR", "HSION", 1);
  set(f, "CR", "HSEON", 1);
  idle_for(f, delay);
  CHECK(f.field("RCC", "CIR", "HSIRDYF") == 1);
  CHECK(f.field("RCC", "CIR", "HSERDYF") == 0);
  set(f, "CIR", "HSIRDYC", 1);
  CHECK(f.field("RCC", "CIR", "HSIRDYF") == 0);
}

TEST_CASE("RM0038 6.3.14: RTCSEL 3 runs the RTC at HSE over RTCPRE, LSE and LSI directly")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  backup_access(f);
  set(f, "CR", "RTCPRE", 2);
  set(f, "CR", "HSEON", 1);
  set(f, "CSR", "LSION", 1);
  idle_for(f, delay);
  set(f, "CSR", "RTCSEL", 3);
  CHECK(clocks.hz("rtc") == 0);
  set(f, "CSR", "RTCEN", 1);
  CHECK(clocks.hz("rtc") == hse_hz / 8);
  set(f, "CR", "RTCPRE", 3);
  CHECK(f.field("RCC", "CR", "RTCPRE") == 2);
  CHECK(clocks.hz("rtc") == hse_hz / 8);
  set(f, "CSR", "RTCSEL", 2);
  CHECK(f.field("RCC", "CSR", "RTCSEL") == 3);
  set(f, "CSR", "RTCRST", 1);
  set(f, "CSR", "RTCRST", 0);
  set(f, "CSR", "RTCSEL", 2);
  set(f, "CSR", "RTCEN", 1);
  CHECK(clocks.hz("rtc") == 37'000);
}

TEST_CASE("RM0038 6.3.1: RTCPRE and HSEBYP keep their value while HSEON is 1, before the write")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  backup_access(f);
  set(f, "CR", "RTCPRE", 2);
  set(f, "CR", "HSEON", 1);
  idle_for(f, delay);
  set(f, "CSR", "RTCSEL", 3);
  set(f, "CSR", "RTCEN", 1);
  set(f, "CR", "RTCPRE", 3);
  set(f, "CR", "HSEBYP", 1);
  CHECK(f.field("RCC", "CR", "RTCPRE") == 2);
  CHECK(f.field("RCC", "CR", "HSEBYP") == 0);
  auto const off = peek(f, "CR") & ~(field_mask("CR", "HSEON") | field_mask("CR", "RTCPRE"));
  write(f, "CR", off | (std::uint32_t{3} << field_of("CR", "RTCPRE").lsb));
  CHECK(f.field("RCC", "CR", "HSEON") == 0);
  CHECK(f.field("RCC", "CR", "RTCPRE") == 2);
  set(f, "CR", "RTCPRE", 3);
  set(f, "CR", "HSEBYP", 1);
  CHECK(f.field("RCC", "CR", "RTCPRE") == 3);
  CHECK(f.field("RCC", "CR", "HSEBYP") == 1);
  set(f, "CR", "HSEON", 1);
  idle_for(f, delay);
  CHECK(clocks.hz("rtc") == hse_hz / 16);
}

TEST_CASE("RM0038 6.3.14: LSEBYP keeps its value while LSEON is 1")
{
  auto f = idle_fixture();
  backup_access(f);
  set(f, "CSR", "LSEON", 1);
  set(f, "CSR", "LSEBYP", 1);
  CHECK(f.field("RCC", "CSR", "LSEBYP") == 0);
  set(f, "CSR", "LSEON", 0);
  set(f, "CSR", "LSEBYP", 1);
  CHECK(f.field("RCC", "CSR", "LSEBYP") == 1);
}

TEST_CASE("an LSE still starting at a system reset starts again and is ready after the delay")
{
  auto f = idle_fixture();
  backup_access(f);
  set(f, "CSR", "LSEON", 1);
  idle_for(f, std::chrono::microseconds{20});
  REQUIRE(f.field("RCC", "CSR", "LSERDY") == 0);
  f.target->reset(reset_kind::software);
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  CHECK(f.field("RCC", "CSR", "LSEON") == 1);
  CHECK(f.field("RCC", "CSR", "LSERDY") == 0);
  idle_for(f, delay);
  CHECK(f.field("RCC", "CSR", "LSERDY") == 1);
  CHECK(probe(f).hz("lse") == 32'768);
}

TEST_CASE("RM0038 6.3.14: a CSR write of 0 keeps the reset flags, RMVF clears them")
{
  auto f = idle_fixture();
  REQUIRE(f.field("RCC", "CSR", "PORRSTF") == 1);
  write(f, "CSR", 0);
  CHECK(f.field("RCC", "CSR", "PORRSTF") == 1);
  CHECK(f.field("RCC", "CSR", "PINRSTF") == 1);
  set(f, "CSR", "RMVF", 1);
  CHECK((peek(f, "CSR") & 0xFE00'0000u) == 0);
}

TEST_CASE("RM0038 6.3.4: LSECSSC clears LSECSSF")
{
  auto        f   = idle_fixture();
  auto const& rcc = *std::ranges::find(
      l152re().peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
  CHECK(
      std::ranges::contains(
          rcc.cross_rules, std::string_view{"LSECSSC"}, &popcorn::cross_rule::field));
  f.target->peripheral("RCC").poke(offset_of("CIR"), peek(f, "CIR") | field_mask("CIR", "LSECSSF"));
  REQUIRE(f.field("RCC", "CIR", "LSECSSF") == 1);
  set(f, "CIR", "LSECSSC", 1);
  CHECK(f.field("RCC", "CIR", "LSECSSF") == 0);
}

TEST_CASE("RM0038 6.2: a timer runs at twice its APB clock when the APB divides")
{
  auto        f      = idle_fixture();
  auto const& clocks = probe(f);
  set(f, "APB1ENR", "TIM2EN", 1);
  set(f, "APB2ENR", "TIM10EN", 1);
  CHECK(clocks.hz("tim2") == 2'097'152);
  set(f, "CFGR", "PPRE1", 5);
  CHECK(clocks.hz("pclk1") == 2'097'152 / 4);
  CHECK(clocks.hz("tim2") == 2'097'152 / 2);
  CHECK(clocks.hz("tim10") == 2'097'152);
}

TEST_CASE("RM0038 6.1: a system reset keeps the RTC domain of RCC_CSR, RTCPRE and HSEBYP")
{
  auto f = idle_fixture();
  backup_access(f);
  set(f, "CSR", "LSEON", 1);
  set(f, "CSR", "LSION", 1);
  idle_for(f, delay);
  set(f, "CSR", "RTCSEL", 1);
  set(f, "CSR", "RTCEN", 1);
  set(f, "CR", "RTCPRE", 3);
  set(f, "CR", "HSEBYP", 1);
  f.target->reset(reset_kind::software);
  CHECK(f.field("RCC", "CSR", "LSEON") == 1);
  CHECK(f.field("RCC", "CSR", "LSERDY") == 1);
  CHECK(f.field("RCC", "CSR", "RTCSEL") == 1);
  CHECK(f.field("RCC", "CSR", "RTCEN") == 1);
  CHECK(f.field("RCC", "CR", "RTCPRE") == 3);
  CHECK(f.field("RCC", "CR", "HSEBYP") == 1);
  CHECK(f.field("RCC", "CSR", "LSION") == 0);
  CHECK(f.field("RCC", "CSR", "LSIRDY") == 0);
  CHECK(f.field("RCC", "CSR", "SFTRSTF") == 1);
  CHECK(f.field("RCC", "CSR", "PORRSTF") == 1);
  CHECK(probe(f).hz("rtc") == 32'768);
  for(auto const kind : {reset_kind::power_on, reset_kind::brown_out})
  {
    CAPTURE(static_cast<int>(kind));
    backup_access(f);
    set(f, "CSR", "LSEON", 1);
    set(f, "CR", "RTCPRE", 3);
    f.target->reset(reset_kind::software);
    f.target->reset(kind);
    CHECK(f.field("RCC", "CSR", "LSEON") == 0);
    CHECK(f.field("RCC", "CR", "RTCPRE") == 0);
    CHECK((peek(f, "CSR") & 0xFE00'0000u) == 0x0C00'0000u);
  }
}

TEST_CASE("RM0038 6.3.14: RTCRST resets the RTC domain of RCC_CSR and keeps LSI and the flags")
{
  auto f = idle_fixture();
  backup_access(f);
  set(f, "CSR", "LSION", 1);
  set(f, "CSR", "LSEBYP", 1);
  set(f, "CSR", "LSEON", 1);
  idle_for(f, delay);
  set(f, "CSR", "RTCSEL", 1);
  set(f, "CSR", "RTCRST", 1);
  CHECK(f.field("RCC", "CSR", "LSEBYP") == 0);
  CHECK(f.field("RCC", "CSR", "LSEON") == 0);
  CHECK(f.field("RCC", "CSR", "LSERDY") == 0);
  CHECK(f.field("RCC", "CSR", "RTCSEL") == 0);
  CHECK(f.field("RCC", "CSR", "LSION") == 1);
  CHECK(f.field("RCC", "CSR", "LSIRDY") == 1);
  CHECK(f.field("RCC", "CSR", "PORRSTF") == 1);
  set(f, "CSR", "LSEON", 1);
  CHECK(f.field("RCC", "CSR", "LSEON") == 0);
  set(f, "CSR", "RTCRST", 0);
  CHECK(f.field("RCC", "CSR", "LSEBYP") == 0);
  set(f, "CSR", "RMVF", 1);
  CHECK((peek(f, "CSR") & 0xFE00'0000u) == 0);
  CHECK(f.field("RCC", "CSR", "RMVF") == 0);
  CHECK(f.field("RCC", "CSR", "LSION") == 1);
}

TEST_CASE("every RCC reset bit of the L1 parts names an instance and holds it in reset")
{
  for(auto const* part : {&l152re(), &l152rb()})
  {
    CAPTURE(part->part);
    auto const& rcc = *std::ranges::find(
        part->peripherals, std::string_view{"RCC"}, &popcorn::peripheral_desc::name);
    for(auto const& reg : rcc.registers)
    {
      if(not reg.name.contains("RSTR"))
      {
        continue;
      }
      for(auto const& bits : reg.fields)
      {
        CAPTURE(bits.name);
        auto const owner = std::ranges::find_if(part->peripherals, [&](auto const& peripheral) {
          auto const found = stm32::reset_bit_of(rcc, peripheral.name);
          return found and found->field == bits.name;
        });
        CHECK(owner != part->peripherals.end());
      }
    }
  }
  auto f = idle_fixture();
  set(f, "APB2RSTR", "TIM10RST", 1);
  set(f, "APB1RSTR", "WWDGRST", 1);
  set(f, "APB2RSTR", "ADC1RST", 1);
  CHECK(f.target->held_in_reset("TIM10"));
  CHECK(f.target->held_in_reset("WWDG"));
  CHECK(f.target->held_in_reset("ADC"));
}

TEST_CASE("no L1 instance is served by a U5 or H5 row: only the L1 rows attach")
{
  stm32::register_builtin_models();
  for(auto const* part : {&l152re(), &l152rb()})
  {
    CAPTURE(part->part);
    std::set<std::string> attached;
    for(auto const& [name, factory] : stm32::factories(stm32::board{}, *part))
    {
      if(factory)
      {
        attached.insert(name);
      }
    }
    std::set<std::string> expected{"PWR", "RCC", "EXTI", "SYSCFG", "DBGMCU", "FLASH"};
    for(auto const& peripheral : part->peripherals)
    {
      if(peripheral.name.starts_with("GPIO")
         or peripheral.name.starts_with("DMA")
         or peripheral.name.starts_with("USART")
         or peripheral.name.starts_with("UART"))
      {
        expected.insert(peripheral.name);
      }
    }
    CHECK(attached == expected);
    core::machine target{
        *part, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
    for(auto const& peripheral : part->peripherals)
    {
      CAPTURE(peripheral.name);
      CHECK((target.model(peripheral.name) != nullptr) == attached.contains(peripheral.name));
    }
  }
}
