#include "../../../native/src/core/machine_impl.hpp"
#include "gtzc.hpp"
#include "h573_secure_rig.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
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
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware   = "h573_flash_ll";
constexpr address          bank2      = 0x0810'0000;
constexpr address          sector2    = bank2 + 0x4000;
constexpr address          loop_at    = 0x2000'0100;
constexpr std::uint32_t    lock       = 1u << 0;
constexpr std::uint32_t    pg         = 1u << 1;
constexpr std::uint32_t    ser        = 1u << 2;
constexpr std::uint32_t    ber        = 1u << 3;
constexpr std::uint32_t    fw         = 1u << 4;
constexpr std::uint32_t    strt       = 1u << 5;
constexpr std::uint32_t    mer        = 1u << 15;
constexpr std::uint32_t    wrperrie   = 1u << 17;
constexpr std::uint32_t    pgserrie   = 1u << 18;
constexpr std::uint32_t    strberrie  = 1u << 19;
constexpr std::uint32_t    incerrie   = 1u << 20;
constexpr std::uint32_t    bksel      = 1u << 31;
constexpr std::uint32_t    eop        = 1u << 16;
constexpr std::uint32_t    all_errors = 0x00fe'0000;
constexpr std::uint32_t    optstrt    = 1u << 1;
constexpr std::uint32_t    eccd       = 1u << 31;
constexpr std::uint32_t    bk_ecc     = 1u << 22;
constexpr std::uint32_t    flash_irq  = 6;
constexpr std::uint32_t    nmi_mark   = 0x4e4d;
constexpr std::uint64_t    us         = 1'000'000;

auto snb(std::uint32_t sector) -> std::uint32_t
{
  return sector << 6;
}

auto options_for(stm32::board const& board, bool strict = false) -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = strict;
  for(auto& [name, factory] : stm32::factories(board, h573()))
  {
    options.models.emplace(name, std::move(factory));
  }
  return options;
}

struct rig
{
  stm32_fixture                 f;
  std::vector<core::log_record> logged{};
  core::hook_handle             sink{};

  explicit rig(
      stm32::board const& board  = {},
      std::string_view    image  = firmware,
      bool                strict = false)
      : f{h573(), image, options_for(board, strict)}
  {
    sink = f.target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
  }

  [[nodiscard]] auto at(std::string_view name) const -> address
  {
    auto const& desc  = f.target->peripheral("FLASH").description();
    auto const  found = popcorn::find_register(desc, name);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto word(address from) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(from, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto read(std::string_view name) const -> std::uint32_t
  {
    return word(at(name));
  }

  [[nodiscard]] auto try_write(std::string_view name, std::uint32_t value) const -> bool
  {
    return f.target->mem().write(at(name), 4, value).has_value();
  }

  void write(std::string_view name, std::uint32_t value) const
  {
    REQUIRE(try_write(name, value));
  }

  [[nodiscard]] auto field(std::string_view reg, std::string_view name) const -> std::uint32_t
  {
    return f.field("FLASH", reg, name);
  }

  [[nodiscard]] auto status(std::string_view name) const -> std::uint32_t
  {
    return field("FLASH_NSSR", name);
  }

  void unlock() const
  {
    write("FLASH_NSKEYR", 0x4567'0123u);
    write("FLASH_NSKEYR", 0xcdef'89abu);
    REQUIRE(field("FLASH_NSCR", "LOCK") == 0);
  }

  void unlock_options() const
  {
    write("FLASH_OPTKEYR", 0x0819'2a3bu);
    write("FLASH_OPTKEYR", 0x4c5d'6e7fu);
  }

  void control(std::uint32_t value) const
  {
    write("FLASH_NSCR", value);
  }

  void clear() const
  {
    write("FLASH_NSCCR", all_errors | eop);
  }

  void call(
      std::string_view                     entry,
      std::initializer_list<std::uint32_t> args,
      icount                               bound = icount{2'000'000}) const
  {
    auto index = 0;
    for(auto const arg : args)
    {
      f.set(std::format("r{}", index++), arg);
    }
    f.set("pc", sym(firmware, entry));
    REQUIRE(f.run_to_stop({.max_instructions = bound}).reason == stop_reason::user);
  }

  void store_words(address to, std::vector<std::uint32_t> const& values) const
  {
    auto const buffer = sym(firmware, "words");
    for(std::size_t i = 0; i < values.size(); ++i)
    {
      REQUIRE(f.target->mem().write(buffer + 4 * i, 4, values[i]).has_value());
    }
    call(
        "copy_words", {static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(buffer),
                       static_cast<std::uint32_t>(values.size())});
  }

  void wait_idle() const
  {
    call("wait_idle", {});
  }

  void program(address to, std::vector<std::uint32_t> const& values) const
  {
    control(pg);
    store_words(to, values);
    wait_idle();
    control(0);
  }

  void run_for(std::uint64_t ps) const
  {
    f.set("pc", loop_at | 1u);
    auto const until = vtime{f.target->virtual_clock().time().ps + ps};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{50'000'000}, .until_time = until}).reason
        == stop_reason::time);
  }

  void guest_store(address to, std::uint32_t value) const
  {
    constexpr address stub = 0x2000'0200;
    REQUIRE(f.target->mem().write(stub, 4, 0xE7FE'6001).has_value());
    f.set("r0", to);
    f.set("r1", value);
    f.set("pc", stub | 1u);
    auto const until = vtime{f.target->virtual_clock().time().ps + us};
    auto const result =
        f.target->run_slice({.max_instructions = icount{1'000'000}, .until_time = until});
    REQUIRE(result.stopped.has_value());
    REQUIRE(result.stopped->reason == stop_reason::time);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto pending() const -> bool
  {
    return (word(0xe000'e200u) >> flash_irq & 1u) != 0;
  }

  [[nodiscard]] auto words(address from, std::size_t count) const -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> out;
    for(std::size_t i = 0; i < count; ++i)
    {
      out.push_back(word(from + 4 * i));
    }
    return out;
  }

  [[nodiscard]] auto warnings(std::string_view reg) const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count_if(logged, [reg](core::log_record const& record) {
          return record.peripheral == "FLASH" and record.reg == reg;
        }));
  }
};

auto pattern(std::uint32_t count) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t i = 0; i < count; ++i)
  {
    out.push_back(0x1000'0000u + i * 0x0101'0101u);
  }
  return out;
}

auto erased(std::size_t count) -> std::vector<std::uint32_t>
{
  return std::vector<std::uint32_t>(count, ~0u);
}

auto option_fixture(stm32::option_bytes const& wanted) -> stm32_fixture
{
  auto options         = popcorn::testing::stm32_options();
  options.vector_table = 0x0800'0000;
  REQUIRE(stm32::configure_option_bytes(h573(), wanted, options).has_value());
  return stm32_fixture{h573(), firmware, std::move(options)};
}

auto option_word(stm32_fixture const& f, std::string_view name) -> std::uint32_t
{
  auto const& regs  = f.target->peripheral("FLASH");
  auto const  found = popcorn::find_register(regs.description(), name);
  REQUIRE(found != nullptr);
  return regs.peek(found->offset);
}

auto refused_key(stm32::option_bytes const& wanted) -> std::string
{
  auto const result = stm32::check_option_bytes(h573(), wanted);
  REQUIRE_FALSE(result.has_value());
  return result.error().key;
}
}

TEST_CASE("RM0481 7.3.5: the bootloader's 32 stores program 8 flash words with EOP and EOPIE clear")
{
  rig r;
  r.unlock();
  auto const data = pattern(32);
  r.control(pg);
  auto const before = r.now();
  r.store_words(sector2, data);
  r.wait_idle();
  auto const elapsed = r.now() - before;
  CHECK(r.status("EOP") == 1);
  CHECK(r.field("FLASH_NSCR", "EOPIE") == 0);
  CHECK(r.status("BSY") == 0);
  CHECK(r.status("WBNE") == 0);
  CHECK(r.words(sector2, 32) == data);
  CHECK(elapsed >= 8 * 31 * us);
  CHECK(elapsed < 8 * 31 * us + 10 * us);
}

TEST_CASE(
    "RM0481 7.11.13: CLR_EOP in FLASH_NSCCR clears EOP, a 1 written to FLASH_NSSR.EOP does not")
{
  rig r;
  r.unlock();
  r.program(sector2, pattern(4));
  REQUIRE(r.status("EOP") == 1);
  r.write("FLASH_NSSR", eop | all_errors);
  CHECK(r.status("EOP") == 1);
  r.write("FLASH_NSCCR", eop);
  CHECK(r.status("EOP") == 0);
  CHECK(r.read("FLASH_NSCCR") == 0);
}

TEST_CASE("the bootloader's FLASH_NSSR flag clear leaves EOP set, so its BSY | EOP poll never ends")
{
  constexpr std::uint32_t polls = 60'000;
  rig                     r;
  r.unlock();
  auto const data   = pattern(32);
  auto const buffer = sym(firmware, "words");
  for(std::size_t i = 0; i < data.size(); ++i)
  {
    REQUIRE(r.f.target->mem().write(buffer + 4 * i, 4, data[i]).has_value());
  }
  auto const before = r.now();
  r.call(
      "boot_write",
      {static_cast<std::uint32_t>(sector2), static_cast<std::uint32_t>(buffer), polls},
      icount{1'000'000});
  CHECK(r.now() - before > 10 * 8 * 31 * us);
  REQUIRE_FALSE(r.f.log().empty());
  CHECK(r.f.log().back() == polls);
  CHECK(r.status("EOP") == 1);
  CHECK(r.status("BSY") == 0);
  CHECK(r.words(sector2, 32) == data);
}

TEST_CASE("RM0481 7.11.11: FW programs a partly filled buffer with the unwritten bytes at 0xFF")
{
  rig r;
  r.unlock();
  r.control(pg);
  r.store_words(sector2, {0x1122'3344u, 0x5566'7788u});
  REQUIRE(r.status("WBNE") == 1);
  REQUIRE(r.status("BSY") == 0);
  r.control(pg | fw);
  CHECK(r.status("WBNE") == 0);
  CHECK(r.status("BSY") == 1);
  CHECK(r.field("FLASH_NSCR", "FW") == 0);
  r.wait_idle();
  CHECK(r.words(sector2, 4) == std::vector<std::uint32_t>{0x1122'3344u, 0x5566'7788u, ~0u, ~0u});
  CHECK(r.status("EOP") == 1);
}

TEST_CASE("RM0481 7.9: STRBERR, INCERR and PGSERR set their flag, flush the buffer, write nothing")
{
  rig r;
  r.unlock();
  SUBCASE("STRBERR: the same byte written twice")
  {
    r.control(pg);
    r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x12});
    REQUIRE(r.status("WBNE") == 1);
    r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x34});
    CHECK(r.status("STRBERR") == 1);
    CHECK(r.status("INCERR") == 0);
    CHECK(r.status("PGSERR") == 0);
  }
  SUBCASE("INCERR: a store to another flash word while the buffer fills")
  {
    r.control(pg);
    r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x12});
    r.call("store_byte", {static_cast<std::uint32_t>(sector2 + 16), 0x34});
    CHECK(r.status("INCERR") == 1);
    CHECK(r.status("STRBERR") == 0);
    CHECK(r.status("PGSERR") == 0);
  }
  SUBCASE("PGSERR: a store with PG clear")
  {
    r.control(0);
    r.store_words(sector2, {0x1234'5678u});
    CHECK(r.status("PGSERR") == 1);
    CHECK(r.status("STRBERR") == 0);
    CHECK(r.status("INCERR") == 0);
  }
  CHECK(r.status("WBNE") == 0);
  CHECK(r.status("BSY") == 0);
  CHECK_FALSE(r.pending());
  r.run_for(100 * us);
  CHECK(r.words(sector2, 8) == erased(8));
}

TEST_CASE("RM0481 7.11.11: each error with its enable raises FLASH_IRQn, with no read in between")
{
  rig r;
  r.unlock();
  SUBCASE("PGSERRIE")
  {
    r.control(pgserrie);
    r.store_words(sector2, {1});
  }
  SUBCASE("STRBERRIE")
  {
    r.control(pg | strberrie);
    r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x12});
    r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x34});
  }
  SUBCASE("INCERRIE")
  {
    r.control(pg | incerrie);
    r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x12});
    r.call("store_byte", {static_cast<std::uint32_t>(sector2 + 16), 0x34});
  }
  CHECK(r.pending());
}

TEST_CASE("an error without its enable leaves FLASH_IRQn low")
{
  rig r;
  r.unlock();
  r.control(pg | wrperrie);
  r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x12});
  r.call("store_byte", {static_cast<std::uint32_t>(sector2), 0x34});
  REQUIRE(r.status("STRBERR") == 1);
  CHECK_FALSE(r.pending());
}

TEST_CASE("RM0481 7.3.6: SER erases sector SNB of bank BKSEL, sector 130 is bank 2 sector 2")
{
  rig r;
  r.unlock();
  for(auto const line : {bank2, sector2 - 16, sector2, sector2 + 0x1ff0, sector2 + 0x2000})
  {
    r.program(line, pattern(4));
  }
  r.clear();
  r.control(ser | bksel | snb(2) | strt);
  CHECK(r.status("BSY") == 1);
  r.wait_idle();
  r.control(0);
  CHECK(r.words(sector2, 4) == erased(4));
  CHECK(r.words(sector2 + 0x1ff0, 4) == erased(4));
  CHECK(r.words(sector2 - 16, 4) == pattern(4));
  CHECK(r.words(sector2 + 0x2000, 4) == pattern(4));
  CHECK(r.words(bank2, 4) == pattern(4));
  CHECK(r.status("EOP") == 1);
}

TEST_CASE("RM0481 7.3.6: BER with BKSEL 1 erases bank 2 only, MER erases both banks")
{
  stm32::board board;
  board.flash.bank_erase = std::chrono::microseconds{200};
  board.flash.mass_erase = std::chrono::microseconds{400};
  rig        r{board};
  auto const bank1_end = bank2 - 16;
  r.unlock();
  for(auto const line : {bank1_end, bank2, address{0x081f'fff0}})
  {
    r.program(line, pattern(4));
  }
  r.clear();
  SUBCASE("BER")
  {
    r.control(ber | bksel | strt);
    r.run_for(250 * us);
    CHECK(r.words(bank2, 4) == erased(4));
    CHECK(r.words(0x081f'fff0, 4) == erased(4));
    CHECK(r.words(bank1_end, 4) == pattern(4));
  }
  SUBCASE("MER")
  {
    r.control(mer | strt);
    r.run_for(450 * us);
    CHECK(r.words(bank2, 4) == erased(4));
    CHECK(r.words(bank1_end, 4) == erased(4));
    CHECK(r.words(0x0800'0000, 4) == erased(4));
  }
  CHECK(r.status("BSY") == 0);
  CHECK(r.status("PGSERR") == 0);
}

TEST_CASE("RM0481 7.9.4: STRT with two of SER, BER and MER, or with none, is PGSERR")
{
  rig r;
  r.unlock();
  SUBCASE("SER and BER")
  {
    r.control(ser | ber | strt);
  }
  SUBCASE("none")
  {
    r.control(strt);
  }
  CHECK(r.status("PGSERR") == 1);
  CHECK(r.status("BSY") == 0);
}

TEST_CASE("RM0481 7.9.4: PG with SER takes PGSERR at a store or at STRT, not at the write")
{
  rig r;
  r.unlock();
  r.control(pg | ser);
  CHECK(r.status("PGSERR") == 0);
  SUBCASE("a store")
  {
    r.store_words(sector2, {0x1234'5678u});
  }
  SUBCASE("STRT")
  {
    r.control(pg | ser | strt);
    CHECK(r.status("BSY") == 0);
  }
  CHECK(r.status("PGSERR") == 1);
  CHECK(r.status("WBNE") == 0);
}

TEST_CASE("RM0481 7.3.12: programming with HSI off and SYSCLK on CSI keeps HSI off")
{
  rig        r;
  auto const rcc = [&r](std::string_view name) {
    auto const& desc = r.f.target->peripheral("RCC").description();
    return desc.base + popcorn::find_register(desc, name)->offset;
  };
  auto const set = [&](std::string_view name, std::uint32_t value) {
    REQUIRE(r.f.target->mem().write(rcc(name), 4, value).has_value());
  };
  auto const get = [&](std::string_view name) { return r.word(rcc(name)); };
  set("RCC_CR", get("RCC_CR") | 1u << 8);
  r.run_for(100 * us);
  REQUIRE(r.f.field("RCC", "RCC_CR", "CSIRDY") == 1);
  set("RCC_CFGR1", (get("RCC_CFGR1") & ~7u) | 5u);
  r.run_for(100 * us);
  REQUIRE(r.f.field("RCC", "RCC_CFGR1", "SWS") == 1);
  set("RCC_CR", get("RCC_CR") & ~1u);
  r.run_for(100 * us);
  REQUIRE(r.f.field("RCC", "RCC_CR", "HSIRDY") == 0);
  r.unlock();
  r.control(pg);
  r.store_words(sector2, pattern(4));
  REQUIRE(r.status("BSY") == 1);
  r.run_for(200 * us);
  CHECK(r.status("BSY") == 0);
  CHECK(r.field("FLASH_NSCR", "PG") == 1);
  CHECK(r.f.field("RCC", "RCC_CR", "HSION") == 0);
  CHECK(r.f.field("RCC", "RCC_CR", "HSIRDY") == 0);
  CHECK(r.words(sector2, 4) == pattern(4));
}

TEST_CASE("RM0481 7.9.10: a load of a torn flash word sets ECCD with its word index and the NMI")
{
  rig r;
  r.unlock();
  r.control(pg);
  r.store_words(sector2, {1, 2, 3, 4});
  REQUIRE(r.status("BSY") == 1);
  r.run_for(15 * us);
  REQUIRE(r.status("BSY") == 1);
  r.f.target->power_cycle();
  REQUIRE(r.f.run_to_stop().reason == stop_reason::user);
  r.call("read_word", {static_cast<std::uint32_t>(sector2)});
  auto const log = r.f.log();
  REQUIRE(log.size() >= 2);
  CHECK(log[log.size() - 2] == nmi_mark);
  CHECK(log.back() == (eccd | bk_ecc | 0x4000 / 16));
  CHECK(r.read("FLASH_ECCDETR") == (eccd | bk_ecc | 0x4000 / 16));
  r.write("FLASH_ECCDETR", eccd);
  CHECK(r.read("FLASH_ECCDETR") == 0);
}

TEST_CASE("RM0481 7.11.42: FLASH_ECCDETR bit 30 is reserved, the bootloader's ECCC write reads 0")
{
  rig r;
  r.write("FLASH_ECCDETR", 1u << 30);
  CHECK(r.read("FLASH_ECCDETR") == 0);
  CHECK(r.field("FLASH_ECCCORR", "ECCC") == 0);
}

TEST_CASE("RM0481 Table 48: at 32 MHz in VOS3 LATENCY 1 is enough and LATENCY 0 warns")
{
  rig r;
  r.logged.clear();
  r.write("FLASH_ACR", 1);
  CHECK(r.warnings("FLASH_ACR") == 0);
  r.write("FLASH_ACR", 0);
  REQUIRE(r.warnings("FLASH_ACR") == 1);
  CHECK(r.logged.back().message.find("RM0481 Table 48") != std::string::npos);
}

TEST_CASE("RM0481 Table 48: the bootloader's VOS0 250 MHz LATENCY 5 is enough and LATENCY 4 warns")
{
  rig r{{}, "h573_rcc_ll"};
  REQUIRE(r.f.target->virtual_clock().core_hz() == 250'000'000);
  REQUIRE(r.field("FLASH_ACR", "LATENCY") == 5);
  CHECK(r.warnings("FLASH_ACR") == 0);
  r.write("FLASH_ACR", 4);
  REQUIRE(r.warnings("FLASH_ACR") == 1);
  CHECK(r.logged.back().message.find("RM0481 Table 48") != std::string::npos);
}

namespace
{
constexpr std::uint32_t wrhighfreq_1 = 1u << 4;
constexpr std::uint32_t wrhighfreq_2 = 2u << 4;
constexpr std::uint32_t latency_5    = 5;

auto delay_warnings(rig const& r) -> std::size_t
{
  return static_cast<std::size_t>(
      std::ranges::count_if(r.logged, [](core::log_record const& record) {
        return record.reg == "FLASH_ACR" and record.message.contains("WRHIGHFREQ");
      }));
}

void set_field(
    rig const&       r,
    std::string_view peripheral,
    std::string_view reg,
    std::string_view name,
    std::uint32_t    value)
{
  auto const& desc  = r.f.target->peripheral(peripheral).description();
  auto const* found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  auto const mask = ((std::uint32_t{1} << bits->width) - 1u) << bits->lsb;
  auto const at   = desc.base + found->offset;
  auto const old  = r.word(at);
  REQUIRE(r.f.target->mem().write(at, 4, (old & ~mask) | (value << bits->lsb)).has_value());
}

auto store_warnings(rig& r, address to, std::uint32_t acr) -> std::size_t
{
  r.logged.clear();
  r.write("FLASH_ACR", acr);
  r.guest_store(to, 1);
  return delay_warnings(r);
}
}

TEST_CASE("RM0481 Table 48: a PG store at 250 MHz with WRHIGHFREQ 1 warns once")
{
  rig r{{}, "h573_rcc_ll"};
  REQUIRE(r.f.target->virtual_clock().core_hz() == 250'000'000);
  REQUIRE(r.field("FLASH_ACR", "WRHIGHFREQ") == 2);
  r.unlock();
  r.control(pg);
  r.logged.clear();
  r.guest_store(sector2, 1);
  CHECK(delay_warnings(r) == 0);
  r.write("FLASH_ACR", latency_5 | wrhighfreq_1);
  r.guest_store(sector2 + 4, 2);
  REQUIRE(delay_warnings(r) == 1);
  CHECK(r.logged.back().message.contains("RM0481 Table 48"));
  CHECK(r.logged.back().message.contains("WRHIGHFREQ 1 is below the 2"));
  r.guest_store(sector2 + 8, 3);
  CHECK(delay_warnings(r) == 1);
  r.write("FLASH_ACR", latency_5 | wrhighfreq_2);
  r.guest_store(sector2 + 12, 4);
  CHECK(delay_warnings(r) == 1);
}

TEST_CASE("RM0481 Table 48: an erase START at 250 MHz with WRHIGHFREQ 1 warns once")
{
  rig r{{}, "h573_rcc_ll"};
  r.unlock();
  r.logged.clear();
  r.control(ser | snb(2) | bksel | strt);
  CHECK(delay_warnings(r) == 0);
  r.run_for(30'000 * us);
  r.clear();
  r.write("FLASH_ACR", latency_5 | wrhighfreq_1);
  r.control(ser | snb(3) | bksel | strt);
  REQUIRE(delay_warnings(r) == 1);
  CHECK(r.logged.back().message.contains("WRHIGHFREQ 1 is below the 2"));
}

TEST_CASE("RM0481 Table 48: WRHIGHFREQ 0 is enough at 32 MHz in VOS3")
{
  rig r;
  REQUIRE(r.field("FLASH_ACR", "WRHIGHFREQ") == 1);
  r.write("FLASH_ACR", 3);
  r.unlock();
  r.logged.clear();
  r.control(pg);
  r.guest_store(sector2, 1);
  r.write("FLASH_ACR", 3);
  CHECK(delay_warnings(r) == 0);
}

TEST_CASE("RM0481 Table 48: the WRHIGHFREQ rows split after 1 and 3 wait states")
{
  constexpr std::uint32_t latency_3 = 3;
  {
    rig r;
    REQUIRE(r.field("FLASH_ACR", "LATENCY") == latency_3);
    set_field(r, "RCC", "RCC_CR", "HSIDIV", 0);
    r.run_for(1'000 * us);
    REQUIRE(r.f.target->virtual_clock().core_hz() == 64'000'000);
    r.unlock();
    r.control(pg);
    CHECK(store_warnings(r, sector2, latency_3 | wrhighfreq_1) == 0);
    CHECK(store_warnings(r, sector2 + 4, latency_3) == 1);
    CHECK(r.logged.back().message.contains("WRHIGHFREQ 0 is below the 1"));
  }
  rig r{{}, "h573_rcc_ll"};
  set_field(r, "RCC", "RCC_CFGR2", "HPRE", 0b1000);
  REQUIRE(r.f.target->virtual_clock().core_hz() == 125'000'000);
  r.unlock();
  r.control(pg);
  CHECK(store_warnings(r, sector2, latency_5 | wrhighfreq_1) == 0);
  CHECK(store_warnings(r, sector2 + 4, latency_5) == 1);
  CHECK(r.logged.back().message.contains("WRHIGHFREQ 0 is below the 1"));
  set_field(r, "PWR", "PWR_VOSCR", "VOS", 0b01);
  r.run_for(1'000 * us);
  REQUIRE(r.f.field("PWR", "PWR_VOSSR", "ACTVOS") == 0b01);
  CHECK(store_warnings(r, sector2 + 8, latency_5 | wrhighfreq_1) == 1);
  CHECK(r.logged.back().message.contains("WRHIGHFREQ 1 is below the 2"));
}

TEST_CASE("under strict_peripherals a PG store or an erase START below Table 48 raises model_error")
{
  rig r{{}, "h573_rcc_ll", true};
  r.write("FLASH_ACR", latency_5 | wrhighfreq_1);
  r.unlock();
  r.control(pg);
  CHECK_THROWS_AS(r.guest_store(sector2, 1), popcorn::model_error);
  CHECK_THROWS_AS(r.guest_store(sector2 + 4, 1), popcorn::model_error);
  CHECK(delay_warnings(r) == 2);
  r.control(0);
  CHECK_THROWS_AS(
      static_cast<void>(
          r.f.target->mem().write(r.at("FLASH_NSCR"), 4, ser | snb(2) | bksel | strt)),
      popcorn::model_error);
  CHECK(delay_warnings(r) == 3);
}

TEST_CASE("H573 silicon: 16-bit stores fill the write buffer, NSSR holds WBNE only, FW programs it")
{
  constexpr std::uint32_t wbne = 1u << 1;
  constexpr std::uint32_t eop  = 1u << 16;
  rig                     r;
  r.unlock();
  r.control(pg);
  SUBCASE("strh then strh")
  {
    r.call("store_half", {static_cast<std::uint32_t>(sector2), 0x1234u});
    CHECK(r.read("FLASH_NSSR") == wbne);
    r.call("store_half", {static_cast<std::uint32_t>(sector2 + 2), 0x5678u});
    CHECK(r.read("FLASH_NSSR") == wbne);
    r.control(pg | fw);
    r.wait_idle();
    CHECK(r.read("FLASH_NSSR") == eop);
    CHECK(r.words(sector2, 4) == std::vector<std::uint32_t>{0x5678'1234u, ~0u, ~0u, ~0u});
  }
  SUBCASE("str then strh")
  {
    r.store_words(sector2, {0xaabb'ccddu});
    CHECK(r.read("FLASH_NSSR") == wbne);
    r.call("store_half", {static_cast<std::uint32_t>(sector2 + 4), 0x1111u});
    CHECK(r.read("FLASH_NSSR") == wbne);
    r.control(pg | fw);
    r.wait_idle();
    CHECK(r.read("FLASH_NSSR") == eop);
    CHECK(r.words(sector2, 4) == std::vector<std::uint32_t>{0xaabb'ccddu, 0xffff'1111u, ~0u, ~0u});
  }
  SUBCASE("a single strh")
  {
    r.call("store_half", {static_cast<std::uint32_t>(sector2), 0x2222u});
    CHECK(r.read("FLASH_NSSR") == wbne);
    r.control(pg | fw);
    r.wait_idle();
    CHECK(r.read("FLASH_NSSR") == eop);
    CHECK(r.words(sector2, 4) == std::vector<std::uint32_t>{0xffff'2222u, ~0u, ~0u, ~0u});
  }
}

TEST_CASE("RM0481 7.4.3: OPTSTRT programs FLASH_WRP2R_PRG into FLASH_WRP2R_CUR at once")
{
  rig r;
  r.unlock();
  r.program(sector2, pattern(4));
  r.control(lock);
  REQUIRE(r.field("FLASH_OPTCR", "OPTLOCK") == 1);
  REQUIRE(r.field("FLASH_NSCR", "LOCK") == 1);
  CHECK(r.read("FLASH_WRP2R_CUR") == ~0u);
  r.write("FLASH_WRP2R_PRG", ~1u);
  CHECK(r.read("FLASH_WRP2R_PRG") != ~1u);
  r.unlock_options();
  CHECK(r.field("FLASH_OPTCR", "OPTLOCK") == 0);
  r.write("FLASH_WRP2R_PRG", ~1u);
  CHECK(r.read("FLASH_WRP2R_CUR") == ~0u);
  r.write("FLASH_OPTCR", optstrt);
  CHECK(r.read("FLASH_WRP2R_CUR") == ~1u);
  CHECK(r.field("FLASH_OPTCR", "OPTSTRT") == 0);
  CHECK(r.status("BSY") == 0);
  r.write("FLASH_OPTCR", 1);
  CHECK(r.field("FLASH_OPTCR", "OPTLOCK") == 1);
  r.write("FLASH_WRP2R_PRG", ~0u);
  CHECK(r.read("FLASH_WRP2R_PRG") == ~1u);
  r.unlock();
  SUBCASE("a store into sector 3 of bank 2 is WRPERR and raises FLASH_IRQn with WRPERRIE")
  {
    r.control(pg | wrperrie);
    r.store_words(bank2 + 3 * 0x2000, {1});
    CHECK(r.status("WRPERR") == 1);
    CHECK(r.pending());
    r.run_for(100 * us);
    CHECK(r.words(bank2 + 3 * 0x2000, 4) == erased(4));
  }
  SUBCASE("a sector erase in the group is WRPERR, one past it runs")
  {
    r.program(bank2 + 4 * 0x2000, pattern(4));
    r.clear();
    r.control(ser | bksel | snb(2) | strt);
    CHECK(r.status("WRPERR") == 1);
    CHECK(r.status("BSY") == 0);
    r.clear();
    r.control(ser | bksel | snb(4) | strt);
    r.wait_idle();
    CHECK(r.status("WRPERR") == 0);
    CHECK(r.words(bank2 + 4 * 0x2000, 4) == erased(4));
    CHECK(r.words(bank2 + 2 * 0x2000, 4) == pattern(4));
  }
}

TEST_CASE("a _PRG register other than WRP written before OPTSTRT logs a model_warning once")
{
  rig r;
  r.unlock_options();
  r.write("FLASH_OPTSR_PRG", 0x1234'5678u);
  CHECK(r.read("FLASH_OPTSR_PRG") != 0);
  r.logged.clear();
  r.write("FLASH_OPTCR", optstrt);
  CHECK(r.warnings("FLASH_OPTSR_PRG") == 1);
  r.write("FLASH_OPTCR", optstrt);
  CHECK(r.warnings("FLASH_OPTSR_PRG") == 1);
}

TEST_CASE("RM0481 7.6.1: with TZEN 0xC3 the secure option registers read zero and ignore writes")
{
  stm32::board board;
  SUBCASE("without option bytes")
  {}
  SUBCASE("with option bytes")
  {
    board.flash.option_bytes = stm32::option_bytes{};
  }
  rig r{board};
  r.unlock_options();
  for(auto const name :
      {"FLASH_SECBOOTR_CUR", "FLASH_BOOTR_PRG", "FLASH_SECWM1R_PRG", "FLASH_SECWM2R_PRG"})
  {
    CAPTURE(name);
    r.write(name, 0x0000'007fu);
    CHECK(r.read(name) == 0);
  }
  r.logged.clear();
  r.write("FLASH_OPTCR", optstrt);
  CHECK(r.warnings("FLASH_SECWM1R_PRG") == 0);
  CHECK(r.warnings("FLASH_SECWM2R_PRG") == 0);
}

TEST_CASE("RM0481 7.11.11: FLASH_NSCR bit 1 is PG, the bootloader's OPTSTART write sets PG")
{
  rig r;
  r.unlock();
  REQUIRE(r.field("FLASH_OPTCR", "OPTLOCK") == 1);
  r.control(pg);
  CHECK(r.field("FLASH_NSCR", "PG") == 1);
  CHECK(r.field("FLASH_NSCR", "LOCK") == 0);
  CHECK(r.field("FLASH_OPTCR", "OPTSTRT") == 0);
  CHECK(r.field("FLASH_OPTCR", "OPTLOCK") == 1);
  r.control(lock);
  CHECK(r.field("FLASH_NSCR", "LOCK") == 1);
  CHECK(r.field("FLASH_OPTCR", "OPTLOCK") == 1);
}

TEST_CASE("RM0481 7.11: the H5 option bytes read back in the _CUR registers")
{
  SUBCASE("watermarks, HDP and boot addresses")
  {
    auto const f = option_fixture({
        .secwm1  = stm32::page_range{.first = 0,  .last = 3 },
        .hdp1    = stm32::page_range{.first = 10, .last = 39},
        .secboot = 0x0c00'1000,
        .nsboot  = 0x0800'2000
    });
    CHECK(option_word(f, "FLASH_SECWM1R_CUR") == 0x0003'0000);
    CHECK(option_word(f, "FLASH_SECWM2R_CUR") == 0x007f'0000);
    CHECK(option_word(f, "FLASH_HDP1R_CUR") == 0x0027'000a);
    CHECK(option_word(f, "FLASH_HDP2R_CUR") == 0x0000'0001);
    CHECK(option_word(f, "FLASH_NSBOOTR_CUR") == 0x0800'20c3);
    CHECK(option_word(f, "FLASH_SECBOOTR_CUR") == 0x0c00'10c3);
    auto const& desc        = f.target->peripheral("FLASH").description();
    auto const  secure_boot = f.target->mem().read(
        desc.base + popcorn::find_register(desc, "FLASH_SECBOOTR_CUR")->offset, 4);
    REQUIRE(secure_boot.has_value());
    CHECK(*secure_boot == 0);
    CHECK(option_word(f, "FLASH_OPTSR2_CUR") == 0xc300'017c);
    CHECK(option_word(f, "FLASH_OPTSR_CUR") == 0x2d30'edf8);
  }
  SUBCASE("no area")
  {
    auto const f = option_fixture({.secwm1 = stm32::no_area{}});
    CHECK(option_word(f, "FLASH_SECWM1R_CUR") == 0x0000'007f);
  }
  SUBCASE("swap_bank")
  {
    auto const f = option_fixture({.swap_bank = true});
    CHECK(option_word(f, "FLASH_OPTSR_CUR") == (0x2d30'edf8u | 1u << 31));
    CHECK(f.field("FLASH", "FLASH_OPTCR", "SWAP_BANK") == 1);
  }
}

TEST_CASE("H5 option bytes refuse only what RM0481 cannot encode, each keyed by its field")
{
  CHECK(
      refused_key({
          .secwm1 = stm32::page_range{.first = 0, .last = 128}
  })
      == "option_bytes.secwm1");
  CHECK(
      refused_key({
          .hdp2 = stm32::page_range{.first = 5, .last = 4}
  })
      == "option_bytes.hdp2");
  CHECK(refused_key({.nsboot = 0x0800'0080}) == "option_bytes.nsboot");
  CHECK(refused_key({.dualbank = false}) == "option_bytes.dualbank");
  CHECK(stm32::check_option_bytes(h573(), {.dualbank = true}) == address{0x0800'0000});
  CHECK(
      stm32::check_option_bytes(
          h573(),
          {
              .secwm1    = stm32::page_range{.first = 0, .last = 3},
              .secwm2    = stm32::no_area{},
              .hdp1      = stm32::page_range{.first = 10, .last = 39},
              .hdp2      = stm32::page_range{.first = 0, .last = 127},
              .secboot   = 0x0c00'1000,
              .nsboot    = 0x0800'2000,
              .swap_bank = true,
              .dualbank  = true
  })
      == address{0x0800'2000});
  CHECK(stm32::check_option_bytes(h573(), {.tzen = true}) == address{0x0c00'0000});
  core::machine_options options;
  REQUIRE(stm32::configure_option_bytes(h573(), {.tzen = true}, options).has_value());
  CHECK(options.trustzone);
  CHECK(options.vector_table == address{0x0c00'0000});
}

TEST_CASE("an H573 description without FLASH_NSCR.FW or FLASH_OPTCR.OPTLOCK throws config_error")
{
  for(auto const& [reg, name] : {
          std::pair{"NSCR",  "FW"     },
          std::pair{"OPTCR", "OPTLOCK"}
  })
  {
    CAPTURE(name);
    auto desc  = h573();
    auto flash = std::ranges::find(
        desc.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
    REQUIRE(flash != desc.peripherals.end());
    auto found =
        std::ranges::find(flash->registers, std::string_view{reg}, &popcorn::register_desc::name);
    REQUIRE(found != flash->registers.end());
    std::erase_if(
        found->fields, [name](popcorn::field_desc const& field) { return field.name == name; });
    stm32::register_builtin_models();
    CHECK_THROWS_AS(
        core::machine(desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()),
        popcorn::config_error);
  }
}

TEST_CASE(
    "an H573 description without a block register its 128-sector bank needs throws config_error")
{
  for(std::string_view const name : {"SECBB2R4", "PRIVBB1R4"})
  {
    CAPTURE(name);
    auto desc  = h573();
    auto flash = std::ranges::find(
        desc.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
    REQUIRE(flash != desc.peripherals.end());
    REQUIRE(
        std::erase_if(
            flash->registers,
            [name](popcorn::register_desc const& reg) { return reg.name == name; })
        == 1);
    stm32::register_builtin_models();
    CHECK_THROWS_AS(
        core::machine(desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()),
        popcorn::config_error);
  }
}

TEST_CASE("with only the FLASH model, tzen sets FLASH_OPTSR2_CUR.TZEN to 0xB4, RM0481 7.11.23")
{
  stm32::board board;
  board.flash.option_bytes = stm32::option_bytes{.tzen = true};
  auto options             = options_for(board);
  for(auto& [name, factory] : options.models)
  {
    if(name != "FLASH")
    {
      factory = [](popcorn::periph::model_context&,
                   popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
        return nullptr;
      };
    }
  }
  options.trustzone    = true;
  options.vector_table = 0x0c00'0000;
  stm32_fixture const f{h573(), firmware, std::move(options)};
  CHECK(option_word(f, "FLASH_OPTSR2_CUR") == 0xb400'017c);
  CHECK(option_word(f, "FLASH_SECBOOTR_CUR") == 0x0c00'00c3);
}

TEST_CASE("RM0481 Table 55: without option bytes each _CUR register reads its delivery value")
{
  rig         r;
  auto const& regs = r.f.target->peripheral("FLASH");
  auto const  peek = [&](std::string_view name) {
    auto const found = popcorn::find_register(regs.description(), name);
    REQUIRE(found != nullptr);
    return regs.peek(found->offset);
  };
  for(auto const& [name, delivery] : {
          std::pair{"FLASH_OPTSR_CUR",    0x2d30'edf8u},
          std::pair{"FLASH_OPTSR2_CUR",   0xc300'017cu},
          std::pair{"FLASH_NSBOOTR_CUR",  0x0800'00c3u},
          std::pair{"FLASH_SECBOOTR_CUR", 0x0c00'00c3u},
          std::pair{"FLASH_SECWM1R_CUR",  0x0000'007fu},
          std::pair{"FLASH_SECWM2R_CUR",  0x0000'007fu},
          std::pair{"FLASH_HDP1R_CUR",    0x0000'0001u},
          std::pair{"FLASH_HDP2R_CUR",    0x0000'0001u},
          std::pair{"FLASH_EDATA1R_CUR",  0u          },
          std::pair{"FLASH_EDATA2R_CUR",  0u          },
          std::pair{"FLASH_OTPBLR_CUR",   0u          },
  })
  {
    CAPTURE(name);
    CHECK(peek(name) == delivery);
    CHECK(r.read(name) == (name == std::string_view{"FLASH_SECBOOTR_CUR"} ? 0u : delivery));
  }
}

namespace
{
constexpr std::uint32_t sector_bytes = 8 * 1024;

auto policy_at(popcorn::testing::h573_secure_rig const& r, address at) -> stm32::page_policy
{
  auto const* const pages = r.f.model_of<stm32::secure_page_controller>("FLASH");
  REQUIRE(pages != nullptr);
  auto const rule = pages->page_rule(at);
  REQUIRE(rule.has_value());
  return *rule;
}

auto without_watermarks() -> stm32::option_bytes
{
  return {.tzen = true, .secwm1 = stm32::no_area{}, .secwm2 = stm32::no_area{}};
}
}

TEST_CASE(
    "RM0481 7.6.3: a FLASH_SECBB2R3 bit secures its bank 2 sector, 64 plus the bit, its "
    "neighbours stay Non-secure")
{
  popcorn::testing::h573_secure_rig const r{firmware, without_watermarks()};
  r.write("FLASH", "FLASH_SECBB2R3", 1u << 5, true);
  CHECK(r.peek("FLASH", "FLASH_SECBB2R3") == 1u << 5);
  CHECK(policy_at(r, bank2 + 69 * sector_bytes).secure);
  CHECK_FALSE(policy_at(r, bank2 + 68 * sector_bytes).secure);
  CHECK_FALSE(policy_at(r, bank2 + 70 * sector_bytes).secure);
  CHECK_FALSE(policy_at(r, 0x0800'0000 + 69 * sector_bytes).secure);
  r.write("FLASH", "FLASH_PRIVBB1R4", 1u << 31, true);
  CHECK(policy_at(r, 0x0800'0000 + 127 * sector_bytes).privileged);
  CHECK_FALSE(policy_at(r, bank2 + 127 * sector_bytes).privileged);
}

TEST_CASE("RM0481 7.6.1 Table 62: SECWMx_STRT to SECWMx_END are Secure in each bank")
{
  stm32::option_bytes wanted = without_watermarks();
  wanted.secwm2              = stm32::page_range{.first = 3, .last = 9};
  popcorn::testing::h573_secure_rig const r{firmware, wanted};
  CHECK_FALSE(policy_at(r, bank2 + 2 * sector_bytes).secure);
  CHECK(policy_at(r, bank2 + 3 * sector_bytes).secure);
  CHECK(policy_at(r, bank2 + 9 * sector_bytes).secure);
  CHECK_FALSE(policy_at(r, bank2 + 10 * sector_bytes).secure);
  CHECK_FALSE(policy_at(r, 0x0800'0000 + 3 * sector_bytes).secure);
}

TEST_CASE(
    "RM0481 7.6.2 Table 63: the bank 2 HDP area closes at HDPL 2, and at HDPL 3 with its "
    "FLASH_HDPEXTR extension, which only grows and freezes at HDPL 3")
{
  stm32::option_bytes wanted = without_watermarks();
  wanted.hdp2                = stm32::page_range{.first = 10, .last = 39};
  popcorn::testing::h573_secure_rig const r{firmware, wanted};
  auto const                              hidden = [&](std::uint32_t sector) {
    return policy_at(r, bank2 + sector * sector_bytes).hidden;
  };
  CHECK(r.read("SBS", "SBS_HDPLSR", true) == 0x51);
  CHECK_FALSE(hidden(10));
  r.write("SBS", "SBS_HDPLCR", 0xb4, true);
  CHECK(r.read("SBS", "SBS_HDPLSR", true) == 0x51);
  r.write("SBS", "SBS_HDPLCR", 0x6a, true);
  CHECK(r.read("SBS", "SBS_HDPLSR", true) == 0x8a);
  CHECK_FALSE(hidden(9));
  CHECK(hidden(10));
  CHECK(hidden(39));
  CHECK_FALSE(hidden(40));
  CHECK_FALSE(policy_at(r, 0x0800'0000 + 10 * sector_bytes).hidden);
  r.write("FLASH", "FLASH_HDPEXTR", 3u << 16, false);
  CHECK(r.peek("FLASH", "FLASH_HDPEXTR") == 3u << 16);
  r.write("FLASH", "FLASH_HDPEXTR", 1u << 16, false);
  CHECK(r.peek("FLASH", "FLASH_HDPEXTR") == 3u << 16);
  CHECK_FALSE(hidden(40));
  r.write("SBS", "SBS_HDPLCR", 0x6a, true);
  CHECK(r.read("SBS", "SBS_HDPLSR", true) == 0x6f);
  CHECK(hidden(10));
  CHECK(hidden(42));
  CHECK_FALSE(hidden(43));
  r.write("FLASH", "FLASH_HDPEXTR", 5u << 16, false);
  CHECK(r.peek("FLASH", "FLASH_HDPEXTR") == 3u << 16);
  r.write("SBS", "SBS_HDPLCR", 0x6a, true);
  CHECK(r.read("SBS", "SBS_HDPLSR", true) == 0x6f);
}

TEST_CASE(
    "RM0481 Table 63: at HDPL 3 an empty HDP area with an extension hides HDPx_END to END+EXT")
{
  stm32::option_bytes                     wanted = without_watermarks();
  popcorn::testing::h573_secure_rig const r{firmware, wanted};
  r.write("FLASH", "FLASH_HDPEXTR", 1u, false);
  r.write("SBS", "SBS_HDPLCR", 0x6a, true);
  CHECK_FALSE(policy_at(r, 0x0800'0000).hidden);
  r.write("SBS", "SBS_HDPLCR", 0x6a, true);
  CHECK(policy_at(r, 0x0800'0000).hidden);
  CHECK(policy_at(r, 0x0800'0000 + sector_bytes).hidden);
  CHECK_FALSE(policy_at(r, 0x0800'0000 + 2 * sector_bytes).hidden);
}

TEST_CASE(
    "an SBS_HDPLCR store inside a cached Secure loop closes the HDP area for the loop's next "
    "load of a hidden sector, RM0481 7.6.2, a 0xB4 store leaves it open")
{
  for(auto const increment : {false, true})
  {
    CAPTURE(increment);
    stm32::option_bytes wanted{.tzen = true};
    wanted.hdp2 = stm32::page_range{.first = 10, .last = 39};
    popcorn::testing::h573_secure_rig const r{firmware, wanted};
    constexpr address                       hidden = 0x0c10'0000 + 10 * sector_bytes + 0x20;
    constexpr address                       code   = 0x3000'0200;
    REQUIRE(r.f.target->mem().write(bank2 + 10 * sector_bytes + 0x20, 4, 0x1234'5678));
    for(auto const& [at, half] : std::initializer_list<std::pair<std::uint32_t, std::uint16_t>>{
            {0x00, 0x3b01},
            {0x02, 0x42a3},
            {0x04, 0xd100},
            {0x06, 0x6001},
            {0x08, 0x682a},
            {0x0a, 0x2b00},
            {0x0c, 0xd1f8},
            {0x0e, 0xbe00},
    })
    {
      REQUIRE(r.f.target->mem().write(code + at, 2, half));
    }
    auto const value = [&](std::string_view name) {
      return r.f.target->regs().read(*r.f.target->regs().find(name));
    };
    r.f.set("r0", r.alias("SBS", "SBS_HDPLCR", true));
    r.f.set("r1", increment ? 0x6a : 0xb4);
    r.f.set("r5", hidden);
    r.f.set("r3", 100);
    r.f.set("r4", 50);
    r.f.set("pc", code | 1u);
    auto const stopped = r.f.run_to_stop({.max_instructions = icount{2000}});
    CHECK(stopped.reason == stop_reason::user);
    CHECK(value("r3") == 0);
    CHECK(r.read("SBS", "SBS_HDPLSR", true) == (increment ? 0x8a : 0x51));
    CHECK(value("r2") == (increment ? 0 : 0x1234'5678));
  }
}

TEST_CASE(
    "an SBS_HDPLCR store that closes the HDP area holding the running Secure loop faults its next "
    "fetch, RM0481 7.6.2 Table 64, a 0xB4 store runs the loop to its end")
{
  constexpr address code      = 0x0c10'0000 + 10 * sector_bytes;
  constexpr address vectors   = 0x3000'0400;
  constexpr address handler   = 0x3000'0600;
  constexpr address stack_top = 0x3000'2000;
  constexpr address vtor_s    = 0xe000'ed08;
  constexpr address cfsr      = 0xe000'ed28;
  constexpr address hfsr      = 0xe000'ed2c;
  constexpr auto    ibuserr   = std::uint32_t{1} << 8;
  constexpr auto    forced    = std::uint32_t{1} << 30;
  for(auto const increment : {false, true})
  {
    CAPTURE(increment);
    stm32::option_bytes wanted{.tzen = true};
    wanted.hdp2 = stm32::page_range{.first = 10, .last = 39};
    popcorn::testing::h573_secure_rig const r{
        firmware, wanted, popcorn::arch::fault_policy::enter_handler};
    auto mem = r.f.target->mem(true);
    for(auto const& [at, half] : std::initializer_list<std::pair<std::uint32_t, std::uint16_t>>{
            {0x00, 0x3b01},
            {0x02, 0x6001},
            {0x04, 0x2b00},
            {0x06, 0xd1fb},
            {0x08, 0xbe00},
    })
    {
      REQUIRE(r.f.target->mem().write(bank2 + 10 * sector_bytes + at, 2, half));
    }
    REQUIRE(mem.write(handler, 2, 0xbe00));
    REQUIRE(mem.write(vectors, 4, stack_top));
    REQUIRE(mem.write(vectors + 4 * 3, 4, handler | 1u));
    REQUIRE(mem.write(vtor_s, 4, vectors));
    auto const value = [&](std::string_view name) {
      return static_cast<std::uint32_t>(r.f.target->regs().read(*r.f.target->regs().find(name)));
    };
    auto const word = [&](address at) {
      auto const read = mem.read(at, 4);
      REQUIRE(read.has_value());
      return static_cast<std::uint32_t>(*read);
    };
    r.f.set("msp", stack_top);
    r.f.set("r0", r.alias("SBS", "SBS_HDPLCR", true));
    r.f.set("r1", increment ? 0x6a : 0xb4);
    r.f.set("r3", 100);
    r.f.set("pc", code | 1u);
    auto const stopped = r.f.run_to_stop({.max_instructions = icount{2000}});
    CHECK(stopped.reason == stop_reason::user);
    CHECK(r.read("SBS", "SBS_HDPLSR", true) == (increment ? 0x8a : 0x51));
    if(increment)
    {
      CHECK(value("pc") == handler + 2);
      CHECK((value("xpsr") & 0x1ff) == 3);
      CHECK((word(hfsr) & forced) != 0);
      CHECK((word(cfsr) & ibuserr) != 0);
      CHECK(word(value("msp") + 24) == code + 4);
      CHECK(value("r3") == 99);
    }
    else
    {
      CHECK(value("pc") == code + 10);
      CHECK(value("r3") == 0);
    }
  }
}

namespace
{
using popcorn::bus_attrs;
using popcorn::initiator;

constexpr bus_attrs secure_priv{.by = initiator::cpu, .secure = true, .privileged = true};
constexpr bus_attrs secure_unpriv{.by = initiator::cpu, .secure = true, .privileged = false};
constexpr bus_attrs nonsecure_priv{.by = initiator::cpu, .secure = false, .privileged = true};
constexpr bus_attrs nonsecure_unpriv{.by = initiator::cpu, .secure = false, .privileged = false};

constexpr address       flash_ns      = 0x0800'0000;
constexpr address       flash_s       = 0x0c00'0000;
constexpr address       unused_secure = flash_s + 2 * sector_bytes;
constexpr address       idle_s        = 0x3000'0100;
constexpr address       code_s        = 0x3000'0200;
constexpr address       vectors_s     = 0x3000'0400;
constexpr address       handler_s     = 0x3000'0600;
constexpr address       stack_s       = 0x3000'2000;
constexpr address       sau_ctrl      = 0xe000'edd0;
constexpr address       sau_rnr       = 0xe000'edd8;
constexpr address       sau_rbar      = 0xe000'eddc;
constexpr address       sau_rlar      = 0xe000'ede0;
constexpr std::uint32_t flash_s_irq   = 7;
constexpr std::uint32_t secure_key1   = 0x4567'0123;
constexpr std::uint32_t secure_key2   = 0xcdef'89ab;
constexpr std::uint16_t bkpt          = 0xbe01;

auto four_secure_sectors() -> stm32::option_bytes
{
  return {
      .tzen = true, .secwm1 = stm32::page_range{.first = 0, .last = 3},
           .secwm2 = stm32::no_area{}
  };
}

struct secure_flash
{
  popcorn::testing::h573_secure_rig r;

  explicit secure_flash(
      stm32::option_bytes const&  wanted = four_secure_sectors(),
      popcorn::arch::fault_policy faults = popcorn::arch::fault_policy::stop)
      : r{firmware, wanted, faults}
  {
    REQUIRE(r.f.target->mem().write(idle_s, 4, 0xE7FD'BF30).has_value());
  }

  [[nodiscard]] auto bus() const -> core::machine_services&
  {
    return core::machine_internals::services(*r.f.target);
  }

  [[nodiscard]] auto bit(std::string_view reg, std::string_view field) const -> std::uint32_t
  {
    return r.bit("FLASH", reg, field);
  }

  [[nodiscard]] auto sector_field(std::string_view reg, std::uint32_t sector) const -> std::uint32_t
  {
    auto const mask = bit(reg, "SNB");
    return (sector << std::countr_zero(mask)) & mask;
  }

  void write(std::string_view reg, std::uint32_t value, bus_attrs attrs) const
  {
    REQUIRE(bus().write(r.alias("FLASH", reg, attrs.secure), 4, value, attrs).has_value());
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(bus().write(r.alias(peripheral, reg, true), 4, value, secure_priv).has_value());
  }

  [[nodiscard]] auto read(std::string_view reg, bus_attrs attrs) const -> std::uint32_t
  {
    auto const value = bus().read(r.alias("FLASH", reg, attrs.secure), 4, attrs);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto peek(std::string_view reg) const -> std::uint32_t
  {
    return r.peek("FLASH", reg);
  }

  [[nodiscard]] auto word(address at) const -> std::uint32_t
  {
    auto const value = r.f.target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto value(std::string_view name) const -> std::uint32_t
  {
    return static_cast<std::uint32_t>(r.f.target->regs().read(*r.f.target->regs().find(name)));
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    return (word(0xe000'e200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
  }

  [[nodiscard]] auto tzic(std::string_view field) const -> bool
  {
    return (r.peek("GTZC1_TZIC", "SR4") & r.bit("GTZC1_TZIC", "SR4", field)) != 0;
  }

  void clear_events() const
  {
    write("GTZC1_TZIC", "FCR4", ~0u);
    REQUIRE(r.peek("GTZC1_TZIC", "SR4") == 0);
  }

  void unlock(bool secure) const
  {
    auto const attrs = secure ? secure_priv : nonsecure_priv;
    auto const key   = secure ? "FLASH_SECKEYR" : "FLASH_NSKEYR";
    write(key, secure_key1, attrs);
    write(key, secure_key2, attrs);
    auto const control = secure ? "FLASH_SECCR" : "FLASH_NSCR";
    REQUIRE((peek(control) & bit(control, "LOCK")) == 0);
  }

  void clear_flags(bool secure) const
  {
    write(secure ? "FLASH_SECCCR" : "FLASH_NSCCR", ~0u, secure ? secure_priv : nonsecure_priv);
  }

  void store(address at, std::initializer_list<std::uint32_t> words, bus_attrs attrs) const
  {
    for(auto const [index, value] : std::views::enumerate(words))
    {
      REQUIRE(bus().write(at + 4 * static_cast<address>(index), 4, value, attrs).has_value());
    }
  }

  void settle(std::chrono::microseconds wait = std::chrono::microseconds{3000}) const
  {
    r.f.set("pc", idle_s | 1u);
    auto const until = vtime{
        r.f.target->virtual_clock().time().ps + static_cast<std::uint64_t>(wait.count()) * us};
    REQUIRE(
        r.f.run_to_stop({.max_instructions = icount{50'000'000}, .until_time = until}).reason
        == stop_reason::time);
  }

  void put(address at, std::initializer_list<std::uint16_t> code) const
  {
    for(auto const [index, half] : std::views::enumerate(code))
    {
      REQUIRE(r.f.target->mem().write(at + 2 * static_cast<address>(index), 2, half));
    }
  }

  void sau_region(std::uint32_t number, address base, address limit) const
  {
    auto mem = r.f.target->mem(true);
    REQUIRE(mem.write(sau_rnr, 4, number));
    REQUIRE(mem.write(sau_rbar, 4, base));
    REQUIRE(mem.write(sau_rlar, 4, (limit & ~0x1fu) | 1u));
  }

  void sau(std::uint32_t control) const
  {
    REQUIRE(r.f.target->mem(true).write(sau_ctrl, 4, control));
  }

  void secure_vectors() const
  {
    auto mem = r.f.target->mem(true);
    put(handler_s, {bkpt});
    REQUIRE(mem.write(vectors_s, 4, stack_s));
    REQUIRE(mem.write(vectors_s + 4 * 3, 4, handler_s | 1u));
    REQUIRE(mem.write(0xe000'ed08, 4, vectors_s));
    r.f.set("msp", stack_s);
  }
};
}

TEST_CASE(
    "RM0481 7.11.10 and 7.11.14: FLASH_SECCCR clears the FLASH_SECSR flags and leaves FLASH_NSSR, "
    "FLASH_SECSR ignores writes")
{
  secure_flash const s;
  s.unlock(true);
  s.write("FLASH_SECCR", s.bit("FLASH_SECCR", "PG") | s.bit("FLASH_SECCR", "EOPIE"), secure_priv);
  s.store(unused_secure, {1, 2, 3, 4}, secure_priv);
  s.settle();
  auto const eop_s = s.bit("FLASH_SECSR", "EOP");
  REQUIRE(s.peek("FLASH_SECSR") == eop_s);
  REQUIRE(s.pending(flash_s_irq));
  s.store(unused_secure - flash_s + flash_ns + 0x10, {5}, nonsecure_priv);
  REQUIRE((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "WRPERR")) != 0);
  s.write("FLASH_SECSR", ~0u, secure_priv);
  CHECK(s.peek("FLASH_SECSR") == eop_s);
  s.write("FLASH_SECCCR", s.bit("FLASH_SECCCR", "CLR_EOP"), nonsecure_priv);
  CHECK(s.peek("FLASH_SECSR") == eop_s);
  s.write("FLASH_SECCCR", s.bit("FLASH_SECCCR", "CLR_EOP"), secure_priv);
  CHECK(s.peek("FLASH_SECSR") == 0);
  CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "WRPERR")) != 0);
  CHECK(s.read("FLASH_SECCCR", secure_priv) == 0);
  s.write("FLASH_SECCR", 0, secure_priv);
  s.write("FLASH_NSCCR", ~0u, nonsecure_priv);
  CHECK(s.peek("FLASH_NSSR") == 0);
}

TEST_CASE(
    "RM0481 7.6.1 Table 60: a program through each register set lands only on a sector of its "
    "security, SECWM1 sectors 0 to 3")
{
  secure_flash const s;
  auto const         pg_s     = s.bit("FLASH_SECCR", "PG");
  auto const         pg_ns    = s.bit("FLASH_NSCR", "PG");
  auto const         bsy_s    = s.bit("FLASH_SECSR", "BSY");
  auto const         wrperr_s = s.bit("FLASH_SECSR", "WRPERR");
  auto const         wrperr   = s.bit("FLASH_NSSR", "WRPERR");
  s.unlock(true);
  s.unlock(false);
  s.write("FLASH_SECCR", pg_s | s.bit("FLASH_SECCR", "EOPIE"), secure_priv);
  s.store(unused_secure, {1, 2, 3, 4}, secure_priv);
  CHECK((s.peek("FLASH_SECSR") & bsy_s) != 0);
  CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "BSY")) != 0);
  s.settle();
  CHECK(s.word(unused_secure + 12) == 4);
  CHECK(s.peek("FLASH_SECSR") == s.bit("FLASH_SECSR", "EOP"));
  CHECK(s.peek("FLASH_NSSR") == 0);
  CHECK(s.pending(flash_s_irq));
  CHECK_FALSE(s.pending(flash_irq));
  CHECK_FALSE(s.tzic("FLASHF"));
  s.clear_flags(true);

  s.store(flash_s + 8 * sector_bytes, {5, 6, 7, 8}, secure_priv);
  s.settle();
  CHECK(s.word(flash_ns + 8 * sector_bytes) == 0xffff'ffff);
  CHECK((s.peek("FLASH_SECSR") & wrperr_s) != 0);
  CHECK((s.peek("FLASH_NSSR") & wrperr) == 0);
  CHECK(s.tzic("FLASHF"));
  s.clear_events();
  s.write("FLASH_SECCR", 0, secure_priv);
  s.clear_flags(true);

  s.write("FLASH_NSCR", pg_ns, nonsecure_priv);
  s.store(flash_ns + 8 * sector_bytes, {5, 6, 7, 8}, nonsecure_priv);
  s.settle();
  CHECK(s.word(flash_ns + 8 * sector_bytes + 4) == 6);
  CHECK(s.peek("FLASH_NSSR") == s.bit("FLASH_NSSR", "EOP"));
  CHECK(s.peek("FLASH_SECSR") == 0);
  CHECK_FALSE(s.tzic("FLASHF"));
  s.clear_flags(false);

  s.store(unused_secure - flash_s + flash_ns + 16, {9, 10, 11, 12}, nonsecure_priv);
  s.settle();
  CHECK(s.word(unused_secure + 16) == 0xffff'ffff);
  CHECK((s.peek("FLASH_NSSR") & wrperr) != 0);
  CHECK((s.peek("FLASH_SECSR") & wrperr_s) == 0);
  CHECK(s.tzic("FLASHF"));
}

TEST_CASE(
    "RM0481 7.6.1 Table 60: the straddling erase, sector 3 through FLASH_SECCR and sector 4 "
    "through FLASH_NSCR, erases both, and each set refuses the other's sector")
{
  secure_flash const s;
  for(address const at : {flash_ns + 3 * sector_bytes, flash_ns + 4 * sector_bytes})
  {
    REQUIRE(s.r.f.target->mem().write(at, 4, 0));
  }
  auto const erase_s = [&s](std::uint32_t sector) {
    return s.bit("FLASH_SECCR", "SER")
         | s.sector_field("FLASH_SECCR", sector)
         | s.bit("FLASH_SECCR", "STRT");
  };
  auto const erase_ns = [&s](std::uint32_t sector) {
    return s.bit("FLASH_NSCR", "SER")
         | s.sector_field("FLASH_NSCR", sector)
         | s.bit("FLASH_NSCR", "STRT");
  };
  s.unlock(true);
  s.unlock(false);
  s.write("FLASH_NSCR", erase_ns(3), nonsecure_priv);
  CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "WRPERR")) != 0);
  CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "BSY")) == 0);
  CHECK((s.peek("FLASH_NSCR") & s.bit("FLASH_NSCR", "STRT")) == 0);
  CHECK(s.tzic("FLASHF"));
  s.clear_events();
  s.write("FLASH_SECCR", erase_s(4), secure_priv);
  CHECK((s.peek("FLASH_SECSR") & s.bit("FLASH_SECSR", "WRPERR")) != 0);
  CHECK(s.tzic("FLASHF"));
  s.clear_flags(false);
  s.clear_flags(true);

  s.write("FLASH_SECCR", erase_s(3), secure_priv);
  s.settle();
  s.write("FLASH_SECCR", 0, secure_priv);
  s.write("FLASH_NSCR", erase_ns(4), nonsecure_priv);
  s.settle();
  CHECK(s.word(flash_ns + 3 * sector_bytes) == 0xffff'ffff);
  CHECK(s.word(flash_ns + 4 * sector_bytes) == 0xffff'ffff);
  CHECK(s.peek("FLASH_NSSR") == s.bit("FLASH_NSSR", "EOP"));
  CHECK(s.peek("FLASH_SECSR") == s.bit("FLASH_SECSR", "EOP"));
}

TEST_CASE(
    "RM0481 7.6.1 Table 61: a bank erase over Secure and Non-secure sectors is refused whole by "
    "either set, a bank erase of an all-Secure bank runs")
{
  {
    secure_flash const s;
    REQUIRE(s.r.f.target->mem().write(flash_ns + 5 * sector_bytes, 4, 0));
    s.unlock(true);
    s.unlock(false);
    s.write("FLASH_SECCR", s.bit("FLASH_SECCR", "BER") | s.bit("FLASH_SECCR", "STRT"), secure_priv);
    CHECK((s.peek("FLASH_SECSR") & s.bit("FLASH_SECSR", "WRPERR")) != 0);
    CHECK(s.tzic("FLASHF"));
    s.clear_events();
    s.write("FLASH_NSCR", s.bit("FLASH_NSCR", "BER") | s.bit("FLASH_NSCR", "STRT"), nonsecure_priv);
    CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "WRPERR")) != 0);
    CHECK(s.tzic("FLASHF"));
    CHECK(s.word(flash_ns + 5 * sector_bytes) == 0);
  }
  {
    auto wanted   = four_secure_sectors();
    wanted.secwm2 = stm32::page_range{.first = 0, .last = 127};
    secure_flash const s{wanted};
    REQUIRE(s.r.f.target->mem().write(bank2 + 5 * sector_bytes, 4, 0));
    s.unlock(true);
    s.write(
        "FLASH_SECCR",
        s.bit("FLASH_SECCR", "BER") | s.bit("FLASH_SECCR", "BKSEL") | s.bit("FLASH_SECCR", "STRT"),
        secure_priv);
    CHECK((s.peek("FLASH_SECSR") & s.bit("FLASH_SECSR", "BSY")) != 0);
    s.settle(std::chrono::microseconds{300'000});
    CHECK(s.word(bank2 + 5 * sector_bytes) == 0xffff'ffff);
    CHECK(s.peek("FLASH_SECSR") == s.bit("FLASH_SECSR", "EOP"));
    CHECK_FALSE(s.tzic("FLASHF"));
  }
  {
    auto wanted   = four_secure_sectors();
    wanted.secwm2 = stm32::page_range{.first = 120, .last = 127};
    secure_flash const s{wanted};
    REQUIRE(s.r.f.target->mem().write(bank2 + 125 * sector_bytes, 4, 0));
    s.unlock(false);
    s.write(
        "FLASH_NSCR",
        s.bit("FLASH_NSCR", "BER") | s.bit("FLASH_NSCR", "BKSEL") | s.bit("FLASH_NSCR", "STRT"),
        nonsecure_priv);
    CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "WRPERR")) != 0);
    CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "BSY")) == 0);
    CHECK(s.tzic("FLASHF"));
    s.settle(std::chrono::microseconds{300'000});
    CHECK(s.word(bank2 + 125 * sector_bytes) == 0);
  }
}

TEST_CASE(
    "RM0481 7.6.5 Table 70: a Non-secure access to FLASH_SECCR is RAZ/WI with FLASH_REGF, SPRIV "
    "and NSPRIV make their set RAZ/WI to unprivileged accesses")
{
  secure_flash const s;
  auto const         lock_s = s.bit("FLASH_SECCR", "LOCK");
  auto const         lock   = s.bit("FLASH_NSCR", "LOCK");
  CHECK(s.read("FLASH_SECCR", nonsecure_priv) == 0);
  CHECK(s.tzic("FLASH_REGF"));
  s.clear_events();
  s.write("FLASH_SECKEYR", secure_key1, nonsecure_priv);
  s.write("FLASH_SECKEYR", secure_key2, nonsecure_priv);
  CHECK((s.peek("FLASH_SECCR") & lock_s) != 0);
  CHECK(s.tzic("FLASH_REGF"));
  s.clear_events();
  CHECK(s.read("FLASH_SECCR", secure_unpriv) == lock_s);
  CHECK(s.read("FLASH_NSCR", nonsecure_unpriv) == lock);
  CHECK_FALSE(s.tzic("FLASH_REGF"));

  auto const spriv  = s.bit("FLASH_PRIVCFGR", "SPRIV");
  auto const nspriv = s.bit("FLASH_PRIVCFGR", "NSPRIV");
  s.write("FLASH_PRIVCFGR", spriv | nspriv, nonsecure_unpriv);
  CHECK(s.peek("FLASH_PRIVCFGR") == 0);
  s.write("FLASH_PRIVCFGR", spriv | nspriv, nonsecure_priv);
  CHECK(s.peek("FLASH_PRIVCFGR") == nspriv);
  s.write("FLASH_PRIVCFGR", spriv | nspriv, secure_priv);
  CHECK(s.peek("FLASH_PRIVCFGR") == (spriv | nspriv));
  CHECK(s.read("FLASH_PRIVCFGR", nonsecure_unpriv) == (spriv | nspriv));
  CHECK(s.read("FLASH_SECCR", secure_unpriv) == 0);
  CHECK(s.read("FLASH_NSCR", nonsecure_unpriv) == 0);
  CHECK(s.read("FLASH_SECCR", secure_priv) == lock_s);
  s.write("FLASH_NSKEYR", secure_key1, nonsecure_unpriv);
  s.write("FLASH_NSKEYR", secure_key2, nonsecure_unpriv);
  CHECK((s.peek("FLASH_NSCR") & lock) != 0);
  CHECK_FALSE(s.tzic("FLASH_REGF"));
}

TEST_CASE(
    "RM0481 7.6.3 Table 65 and 7.6.4 Table 68: FLASH_SECBB1R1 takes a Secure write, from an "
    "unprivileged one only the unprivileged sectors, FLASH_PRIVBB1R1 a privileged one, from "
    "Non-secure only Non-secure sectors")
{
  secure_flash const s;
  s.write("FLASH_SECBB1R1", 0xf0, nonsecure_priv);
  CHECK(s.peek("FLASH_SECBB1R1") == 0);
  CHECK(s.tzic("FLASH_REGF"));
  CHECK(s.read("FLASH_SECBB1R1", nonsecure_unpriv) == 0);
  s.clear_events();
  s.write("FLASH_PRIVBB1R1", 0xff, nonsecure_unpriv);
  CHECK(s.peek("FLASH_PRIVBB1R1") == 0);
  s.write("FLASH_PRIVBB1R1", 0xff, nonsecure_priv);
  CHECK(s.peek("FLASH_PRIVBB1R1") == 0xf0);
  s.write("FLASH_PRIVBB1R1", 0x30, secure_priv);
  CHECK(s.peek("FLASH_PRIVBB1R1") == 0x30);
  s.write("FLASH_SECBB1R1", 0x330, secure_unpriv);
  CHECK(s.peek("FLASH_SECBB1R1") == 0x300);
  s.write("FLASH_SECBB1R1", 0x330, secure_priv);
  CHECK(s.peek("FLASH_SECBB1R1") == 0x330);
  CHECK(s.read("FLASH_SECBB1R1", nonsecure_unpriv) == 0x330);
  s.write("FLASH_PRIVBB1R1", 0x00, nonsecure_priv);
  CHECK(s.peek("FLASH_PRIVBB1R1") == 0x30);
  s.write("FLASH_PRIVBB2R2", ~0u, nonsecure_priv);
  CHECK(s.peek("FLASH_PRIVBB2R2") == ~0u);
  CHECK_FALSE(s.tzic("FLASH_REGF"));
}

struct sbs_bus
{
  secure_flash const& s;

  void write(std::string_view reg, std::uint32_t value, bus_attrs attrs) const
  {
    REQUIRE(s.bus().write(s.r.at("SBS", reg), 4, value, attrs).has_value());
  }

  [[nodiscard]] auto read(std::string_view reg, bus_attrs attrs) const -> std::uint32_t
  {
    auto const value = s.bus().read(s.r.at("SBS", reg), 4, attrs);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto peek(std::string_view reg) const -> std::uint32_t
  {
    return s.r.peek("SBS", reg);
  }

  [[nodiscard]] auto bit(std::string_view reg, std::string_view field) const -> std::uint32_t
  {
    return s.r.bit("SBS", reg, field);
  }

  [[nodiscard]] auto flips(std::string_view reg, std::string_view field, bus_attrs attrs) const
      -> bool
  {
    auto const before = peek(reg);
    write(reg, before ^ bit(reg, field), attrs);
    auto const moved = peek(reg) != before;
    write(reg, before, secure_priv);
    return moved;
  }

  [[nodiscard]] auto raised() const -> bool
  {
    auto const flag = s.tzic("SBSF");
    s.clear_events();
    return flag;
  }

  void rcc(std::string_view reg, std::uint32_t value, bus_attrs attrs) const
  {
    REQUIRE(s.bus().write(s.r.alias("RCC", reg, attrs.secure), 4, value, attrs).has_value());
  }
};

TEST_CASE(
    "RM0481 14.5.8: SBS_SECCFGR is always Secure, RAZ/WI to Non-secure, and FPUSEC takes "
    "privileged writes only")
{
  secure_flash const s;
  sbs_bus const      sbs{s};
  auto const         sbssec = sbs.bit("SBS_SECCFGR", "SBSSEC");
  auto const         fpusec = sbs.bit("SBS_SECCFGR", "FPUSEC");
  s.clear_events();
  sbs.write("SBS_SECCFGR", sbssec, nonsecure_priv);
  CHECK(sbs.peek("SBS_SECCFGR") == 0);
  CHECK(sbs.raised());
  sbs.write("SBS_SECCFGR", sbssec | fpusec, secure_unpriv);
  CHECK(sbs.peek("SBS_SECCFGR") == sbssec);
  CHECK_FALSE(sbs.raised());
  sbs.write("SBS_SECCFGR", sbssec | fpusec, secure_priv);
  CHECK(sbs.peek("SBS_SECCFGR") == (sbssec | fpusec));
  CHECK(sbs.read("SBS_SECCFGR", nonsecure_priv) == 0);
  CHECK(sbs.raised());
  CHECK(sbs.read("SBS_SECCFGR", nonsecure_unpriv) == 0);
  CHECK(sbs.raised());
  CHECK(sbs.read("SBS_SECCFGR", secure_unpriv) == (sbssec | fpusec));
  CHECK_FALSE(sbs.raised());
}

TEST_CASE(
    "RM0481 14.5.6, 14.5.7, 14.5.17 to 14.5.19 and 5.7.8: SBS_RSSCMDR is Secure, SBS_EPOCHSELCR, "
    "SBS_CSLCKR and SBS_ECCNMIR Secure and privileged, SBS_CNSLCKR privileged, SBS_PMCR BOOSTEN "
    "open while ADC12 is Non-secure, a Non-secure access to a Secure one raises SBSF")
{
  secure_flash const s;
  sbs_bus const      sbs{s};
  struct row
  {
    std::string_view reg;
    std::string_view field;
    bool             secure;
    bool             privileged;
  };
  for(auto const& [reg, field, secure, privileged] : {
          row{"SBS_RSSCMDR",    "RSSCMD",         true,  false},
          row{"SBS_EPOCHSELCR", "EPOCH_SEL",      true,  true },
          row{"SBS_CSLCKR",     "LOCKSMPU",       true,  true },
          row{"SBS_ECCNMIR",    "ECCNMI_MASK_EN", true,  true },
          row{"SBS_CNSLCKR",    "LOCKNSMPU",      false, true },
          row{"SBS_PMCR",       "BOOSTEN",        false, false},
  })
  {
    CAPTURE(reg);
    s.clear_events();
    CHECK(sbs.flips(reg, field, secure_priv));
    CHECK_FALSE(sbs.raised());
    CHECK(sbs.flips(reg, field, nonsecure_priv) == not secure);
    CHECK(sbs.raised() == secure);
    CHECK(sbs.flips(reg, field, secure_unpriv) == not privileged);
    CHECK_FALSE(sbs.raised());
    CHECK(sbs.flips(reg, field, nonsecure_unpriv) == (not secure and not privileged));
    CHECK(sbs.raised() == secure);
    sbs.write(reg, sbs.bit(reg, field), secure_priv);
    CHECK((sbs.read(reg, nonsecure_priv) != 0) == not secure);
    CHECK(sbs.raised() == secure);
    CHECK((sbs.read(reg, secure_unpriv) != 0) == not privileged);
    CHECK_FALSE(sbs.raised());
  }
}

TEST_CASE(
    "RM0481 14.5.8 to 14.5.16 and 5.7.8: FPUSEC makes SBS_FPUIMR Secure, SBSSEC the SBS_MESR "
    "group and CLASSBSEC SBS_CFGR2, SBS_FPUIMR is privileged, SBSF follows the SEC bits")
{
  secure_flash const s;
  sbs_bus const      sbs{s};
  s.clear_events();
  CHECK(sbs.flips("SBS_FPUIMR", "FPU_IE", nonsecure_priv));
  CHECK_FALSE(sbs.flips("SBS_FPUIMR", "FPU_IE", nonsecure_unpriv));
  CHECK(sbs.flips("SBS_CCSWCR", "SW_ANSRC1", nonsecure_unpriv));
  CHECK(sbs.read("SBS_CCVALR", nonsecure_unpriv) != 0);
  CHECK_FALSE(sbs.raised());
  sbs.write("SBS_SECCFGR", sbs.bit("SBS_SECCFGR", "FPUSEC"), secure_priv);
  CHECK_FALSE(sbs.flips("SBS_FPUIMR", "FPU_IE", nonsecure_priv));
  CHECK(sbs.raised());
  CHECK(sbs.read("SBS_FPUIMR", nonsecure_priv) == 0);
  CHECK(sbs.raised());
  CHECK(sbs.flips("SBS_FPUIMR", "FPU_IE", secure_priv));
  CHECK_FALSE(sbs.flips("SBS_FPUIMR", "FPU_IE", secure_unpriv));
  CHECK(sbs.flips("SBS_CCSWCR", "SW_ANSRC1", nonsecure_unpriv));
  CHECK_FALSE(sbs.raised());
  sbs.write("SBS_SECCFGR", sbs.bit("SBS_SECCFGR", "SBSSEC"), secure_priv);
  CHECK_FALSE(sbs.flips("SBS_CCSWCR", "SW_ANSRC1", nonsecure_priv));
  CHECK(sbs.raised());
  CHECK_FALSE(sbs.flips("SBS_CCCSR", "EN1", nonsecure_priv));
  CHECK(sbs.raised());
  CHECK(sbs.read("SBS_CCVALR", nonsecure_priv) == 0);
  CHECK(sbs.raised());
  CHECK(sbs.read("SBS_CCSWCR", nonsecure_priv) == 0);
  CHECK(sbs.raised());
  CHECK(sbs.flips("SBS_CCSWCR", "SW_ANSRC1", secure_unpriv));
  CHECK(sbs.read("SBS_CCVALR", secure_unpriv) != 0);
  CHECK(sbs.read("SBS_CFGR2", nonsecure_priv) == 0);
  sbs.write("SBS_CFGR2", sbs.bit("SBS_CFGR2", "CLL"), nonsecure_priv);
  CHECK(sbs.peek("SBS_CFGR2") == 1);
  CHECK_FALSE(sbs.raised());
  sbs.write("SBS_SECCFGR", sbs.bit("SBS_SECCFGR", "CLASSBSEC"), secure_priv);
  CHECK(sbs.flips("SBS_CFGR2", "PVDL", nonsecure_priv) == false);
  CHECK(sbs.raised());
  CHECK(sbs.read("SBS_CFGR2", nonsecure_priv) == 0);
  CHECK(sbs.raised());
  CHECK(sbs.read("SBS_CFGR2", secure_unpriv) == 1);
  CHECK(sbs.flips("SBS_PMCR", "BOOSTEN", nonsecure_priv));
  CHECK_FALSE(sbs.raised());
}

TEST_CASE(
    "RM0481 14.5.4 and 14.5.5: SBS_DBGCR and SBS_DBGLOCKR take accesses at HDPL1 and are RAZ/WI "
    "above it")
{
  secure_flash const s;
  sbs_bus const      sbs{s};
  sbs.write("SBS_DBGCR", 0xb4, secure_priv);
  CHECK(sbs.peek("SBS_DBGCR") == 0xb4);
  CHECK(sbs.read("SBS_DBGCR", nonsecure_unpriv) == 0xb4);
  CHECK(sbs.read("SBS_DBGLOCKR", secure_priv) == 0xb4);
  sbs.write("SBS_HDPLCR", 0x6a, secure_priv);
  REQUIRE(sbs.peek("SBS_HDPLSR") == 0x8a);
  CHECK(sbs.read("SBS_DBGCR", secure_priv) == 0);
  CHECK(sbs.read("SBS_DBGLOCKR", secure_priv) == 0);
  sbs.write("SBS_DBGCR", 0xb4b4, secure_priv);
  sbs.write("SBS_DBGLOCKR", 0xc3, secure_priv);
  CHECK(sbs.peek("SBS_DBGCR") == 0xb4);
  CHECK(sbs.peek("SBS_DBGLOCKR") == 0xb4);
  CHECK_FALSE(sbs.raised());
}

TEST_CASE(
    "STM32H573 silicon: RCC_APB3RSTR bit 1 reads 0 after a write of 1 and every SBS register "
    "keeps its value, RM0481 11.8.25 marks it reserved")
{
  secure_flash const s;
  sbs_bus const      sbs{s};
  auto const&        rcc  = s.r.f.target->peripheral("RCC").description();
  auto const*        rstr = popcorn::find_register(rcc, std::string_view{"RCC_APB3RSTR"});
  REQUIRE(rstr != nullptr);
  CHECK(
      std::ranges::none_of(rstr->fields, [](popcorn::field_desc const& f) { return f.lsb == 1; }));
  struct kept
  {
    std::string_view reg;
    std::uint32_t    value;
  };
  std::array const written{
      kept{"SBS_CNSLCKR", 2},
      kept{"SBS_CFGR2", 1},
      kept{"SBS_FPUIMR", 0},
      kept{"SBS_EPOCHSELCR", 1},
      kept{"SBS_SECCFGR", sbs.bit("SBS_SECCFGR", "CLASSBSEC")},
      kept{"SBS_CSLCKR", sbs.bit("SBS_CSLCKR", "LOCKSAU")},
      kept{"SBS_DBGLOCKR", 0xc3},
  };
  for(auto const& [reg, value] : written)
  {
    sbs.write(reg, value, secure_priv);
    REQUIRE(sbs.peek(reg) == value);
  }
  sbs.write("SBS_HDPLCR", 0x6a, secure_priv);
  for(auto const attrs : {secure_priv, nonsecure_priv})
  {
    sbs.rcc("RCC_APB3RSTR", 1u << 1, attrs);
    CHECK(s.r.peek("RCC", "RCC_APB3RSTR") == 0);
    sbs.rcc("RCC_APB3RSTR", 0, attrs);
    for(auto const& [reg, value] : written)
    {
      CAPTURE(reg);
      CHECK(sbs.peek(reg) == value);
    }
    CHECK(sbs.peek("SBS_HDPLSR") == 0x8a);
  }
  sbs.write("SBS_EPOCHSELCR", 0, secure_priv);
  CHECK(sbs.peek("SBS_EPOCHSELCR") == 0);
}

TEST_CASE("a system reset restores every SBS register but SBS_RSSCMDR, a power-on reset every one")
{
  secure_flash const s;
  sbs_bus const      sbs{s};
  sbs.write("SBS_CSLCKR", sbs.bit("SBS_CSLCKR", "LOCKSAU"), secure_priv);
  sbs.write("SBS_RSSCMDR", 1, secure_priv);
  sbs.write("SBS_HDPLCR", 0x6a, secure_priv);
  REQUIRE(sbs.peek("SBS_HDPLSR") == 0x8a);
  s.r.f.target->reset(popcorn::reset_kind::software);
  CHECK(sbs.peek("SBS_HDPLSR") == 0x51);
  CHECK(sbs.peek("SBS_HDPLCR") == 0xb4);
  CHECK(sbs.peek("SBS_CSLCKR") == 0);
  CHECK(sbs.peek("SBS_RSSCMDR") == 1);
  s.r.f.target->reset();
  CHECK(sbs.peek("SBS_RSSCMDR") == 0);
}

TEST_CASE("RM0481 11.5.1: SBSSEC makes RCC_APB3ENR.SBSEN Secure")
{
  secure_flash const s;
  sbs_bus const      sbs{s};
  auto const         sbssec = sbs.bit("SBS_SECCFGR", "SBSSEC");
  auto const         enable = s.r.bit("RCC", "RCC_APB3ENR", "SBSEN");
  auto const         clock  = [&] { return s.r.peek("RCC", "RCC_APB3ENR") & enable; };
  sbs.write("SBS_SECCFGR", sbssec, secure_priv);
  auto const before = clock();
  sbs.rcc("RCC_APB3ENR", s.r.peek("RCC", "RCC_APB3ENR") ^ enable, nonsecure_priv);
  CHECK(clock() == before);
  sbs.rcc("RCC_APB3ENR", s.r.peek("RCC", "RCC_APB3ENR") ^ enable, secure_priv);
  CHECK(clock() != before);
  sbs.write("SBS_SECCFGR", 0, secure_priv);
  sbs.rcc("RCC_APB3ENR", s.r.peek("RCC", "RCC_APB3ENR") ^ enable, nonsecure_priv);
  CHECK(clock() == before);
}

TEST_CASE(
    "coverage reports a firmware write of SBS_PMCR as storage_only and SBS_SECCFGR as served "
    "under TrustZone")
{
  secure_flash const s;
  s.r.f.target->enable_coverage({}, true);
  s.put(code_s, {0x6001, 0x601a, bkpt});
  s.r.f.set("r0", s.r.alias("SBS", "SBS_PMCR", true));
  s.r.f.set("r1", s.r.bit("SBS", "SBS_PMCR", "BOOSTEN"));
  s.r.f.set("r3", s.r.alias("SBS", "SBS_SECCFGR", true));
  s.r.f.set("r2", s.r.bit("SBS", "SBS_SECCFGR", "CLASSBSEC"));
  s.r.f.set("pc", code_s | 1u);
  REQUIRE(s.r.f.run_to_stop({.max_instructions = icount{10}}).reason == stop_reason::user);
  REQUIRE(s.r.peek("SBS", "SBS_PMCR") == s.r.bit("SBS", "SBS_PMCR", "BOOSTEN"));
  REQUIRE(s.r.peek("SBS", "SBS_SECCFGR") == s.r.bit("SBS", "SBS_SECCFGR", "CLASSBSEC"));
  auto const found = s.r.f.target->coverage().unmodelled;
  REQUIRE(found.size() == 1);
  CHECK(found.front().peripheral == "SEC_SBS");
  CHECK(found.front().reg.ends_with("PMCR"));
  CHECK(found.front().reason == popcorn::core::unmodelled_reason::storage_only);
}

TEST_CASE(
    "RM0481 14.5.6 to 14.5.8, 14.5.11 and 14.5.17 to 14.5.19: without TrustZone SBS_SECCFGR, "
    "SBS_RSSCMDR and SBS_CSLCKR are RAZ/WI, SBS_EPOCHSELCR, SBS_ECCNMIR, SBS_FPUIMR and "
    "SBS_CNSLCKR privileged")
{
  stm32_fixture const f{h573(), firmware};
  REQUIRE(f.target->model("SBS") != nullptr);
  auto&       bus  = core::machine_internals::services(*f.target);
  auto const& unit = f.target->peripheral("SBS");
  auto const  at   = [&](std::string_view reg) {
    auto const* found = popcorn::find_register(unit.description(), reg);
    REQUIRE(found != nullptr);
    return found->offset;
  };
  auto const write = [&](std::string_view reg, std::uint32_t value, bus_attrs attrs) {
    REQUIRE(bus.write(unit.description().base + at(reg), 4, value, attrs).has_value());
  };
  auto const read = [&](std::string_view reg, bus_attrs attrs) {
    auto const value = bus.read(unit.description().base + at(reg), 4, attrs);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  };
  for(auto const reg : {"SBS_SECCFGR", "SBS_RSSCMDR", "SBS_CSLCKR"})
  {
    CAPTURE(reg);
    write(reg, 0x7, nonsecure_priv);
    write(reg, 0x7, secure_priv);
    CHECK(unit.peek(at(reg)) == 0);
    CHECK(read(reg, secure_priv) == 0);
  }
  struct row
  {
    std::string_view reg;
    std::uint32_t    value;
    bool             privileged;
  };
  for(auto const& [reg, value, privileged] : {
          row{"SBS_EPOCHSELCR", 0x1,  true },
          row{"SBS_ECCNMIR",    0x1,  true },
          row{"SBS_FPUIMR",     0x3,  true },
          row{"SBS_CNSLCKR",    0x1,  true },
          row{"SBS_CFGR2",      0x1,  false},
          row{"SBS_CCSWCR",     0x12, false},
  })
  {
    CAPTURE(reg);
    auto const before = unit.peek(at(reg));
    write(reg, value, nonsecure_unpriv);
    CHECK((unit.peek(at(reg)) == value) == not privileged);
    CHECK((read(reg, nonsecure_unpriv) == value) == not privileged);
    write(reg, before, nonsecure_priv);
    write(reg, value, nonsecure_priv);
    CHECK(unit.peek(at(reg)) == value);
    CHECK(read(reg, nonsecure_priv) == value);
  }
  f.target->peripheral("SBS").poke(at("SBS_SECCFGR"), 0xb);
  write("SBS_FPUIMR", 0x5, nonsecure_priv);
  write("SBS_CCSWCR", 0x34, nonsecure_priv);
  CHECK(unit.peek(at("SBS_FPUIMR")) == 0x5);
  CHECK(unit.peek(at("SBS_CCSWCR")) == 0x34);
  write("SBS_HDPLCR", 0x6a, nonsecure_unpriv);
  CHECK(read("SBS_HDPLSR", nonsecure_unpriv) == 0x8a);
}

TEST_CASE(
    "RM0481 Table 72 note 1 and Table 64: without TrustZone HDPL 2 closes the HDP area, a load of "
    "it reads zero, a fetch from it faults and a program into it sets WRPERR, the next sector "
    "stays open with its SECWM1 range ignored")
{
  constexpr address closed = flash_ns + 10 * sector_bytes;
  constexpr address open   = flash_ns + 12 * sector_bytes;
  auto              f      = option_fixture({
      .secwm1 = stm32::page_range{.first = 12, .last = 13},
      .hdp1   = stm32::page_range{.first = 10, .last = 11}
  });
  auto&             bus    = core::machine_internals::services(*f.target);
  auto const        at     = [&](std::string_view unit, std::string_view reg) {
    auto const& desc  = f.target->peripheral(unit).description();
    auto const* found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  };
  auto const write = [&](std::string_view unit, std::string_view reg, std::uint32_t value) {
    REQUIRE(bus.write(at(unit, reg), 4, value, nonsecure_priv).has_value());
  };
  auto const value = [&](std::string_view name) {
    return static_cast<std::uint32_t>(f.target->regs().read(*f.target->regs().find(name)));
  };
  auto const load = [&](address from) {
    f.set("r0", from);
    f.set("r1", 0xdead);
    f.set("pc", loop_at | 1u);
    REQUIRE(f.run_to_stop({.max_instructions = icount{4}}).reason == stop_reason::user);
    return value("r1");
  };
  REQUIRE(f.target->mem().write(loop_at, 4, 0xbe01'6801u));
  REQUIRE(f.target->mem().write(closed, 4, 0x1234'5678));
  REQUIRE(f.target->mem().write(open, 4, 0x9abc'def0));
  REQUIRE(f.target->mem().write(closed + 0x100, 2, bkpt));
  CHECK(load(closed) == 0x1234'5678);
  write("SBS", "SBS_HDPLCR", 0x6a);
  REQUIRE(bus.read(at("SBS", "SBS_HDPLSR"), 4, nonsecure_priv) == 0x8a);
  CHECK(load(closed) == 0);
  CHECK(load(closed + sector_bytes) == 0);
  CHECK(load(open) == 0x9abc'def0);
  f.set("pc", (closed + 0x100) | 1u);
  CHECK(f.run_to_stop({.max_instructions = icount{4}}).reason == stop_reason::fault);
  write("FLASH", "FLASH_NSKEYR", secure_key1);
  write("FLASH", "FLASH_NSKEYR", secure_key2);
  write("FLASH", "FLASH_NSCR", pg);
  for(address word = 0; word < 4; ++word)
  {
    REQUIRE(bus.write(closed + 0x20 + 4 * word, 4, word, nonsecure_priv).has_value());
  }
  CHECK(f.field("FLASH", "FLASH_NSSR", "WRPERR") == 1);
  CHECK(f.target->mem().read(closed + 0x20, 4) == 0xffff'ffff);
  write("FLASH", "FLASH_NSCCR", ~0u);
  REQUIRE(f.field("FLASH", "FLASH_NSSR", "WRPERR") == 0);
  for(address word = 0; word < 4; ++word)
  {
    REQUIRE(bus.write(open + 0x20 + 4 * word, 4, word, nonsecure_priv).has_value());
  }
  CHECK(f.field("FLASH", "FLASH_NSSR", "WRPERR") == 0);
  CHECK(f.field("FLASH", "FLASH_NSSR", "BSY") == 1);
  f.target->reset(popcorn::reset_kind::software);
  REQUIRE(f.target->mem().write(loop_at, 4, 0xbe01'6801u));
  CHECK(load(closed) == 0x1234'5678);
  f.set("pc", (closed + 0x100) | 1u);
  CHECK(f.run_to_stop({.max_instructions = icount{4}}).reason == stop_reason::user);
}

TEST_CASE(
    "RM0481 7.6.3: a FLASH_SECBB1R1 bit makes sector 9 Secure, a Non-secure program into it is "
    "refused with WRPERR and FLASHF")
{
  secure_flash const s;
  constexpr address  sector9 = flash_ns + 9 * sector_bytes;
  s.unlock(false);
  s.write("FLASH_NSCR", s.bit("FLASH_NSCR", "PG"), nonsecure_priv);
  s.store(sector9 + 0x20, {1, 2, 3, 4}, nonsecure_priv);
  s.settle();
  REQUIRE(s.word(sector9 + 0x20) == 1);
  s.clear_flags(false);
  s.write("FLASH_SECBB1R1", 1u << 9, secure_priv);
  s.store(sector9, {1, 2, 3, 4}, nonsecure_priv);
  s.settle();
  CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "WRPERR")) != 0);
  CHECK(s.word(sector9) == 0xffff'ffff);
  CHECK(s.tzic("FLASHF"));
}

TEST_CASE(
    "RM0481 7.6.4: an unprivileged program or erase of a FLASH_PRIVBB1R1 sector is refused with "
    "WRPERR and no event, a privileged one lands")
{
  secure_flash const s;
  constexpr address  sector6 = flash_ns + 6 * sector_bytes;
  auto const         wrperr  = s.bit("FLASH_NSSR", "WRPERR");
  s.write("FLASH_PRIVBB1R1", 1u << 6, secure_priv);
  s.unlock(false);
  s.write("FLASH_NSCR", s.bit("FLASH_NSCR", "PG"), nonsecure_priv);
  s.store(sector6, {1, 2, 3, 4}, nonsecure_unpriv);
  CHECK((s.peek("FLASH_NSSR") & wrperr) != 0);
  CHECK_FALSE(s.tzic("FLASHF"));
  s.clear_flags(false);
  s.store(sector6, {1, 2, 3, 4}, nonsecure_priv);
  s.settle();
  CHECK(s.word(sector6 + 8) == 3);
  s.clear_flags(false);
  s.write(
      "FLASH_NSCR",
      s.bit("FLASH_NSCR", "SER") | s.sector_field("FLASH_NSCR", 6) | s.bit("FLASH_NSCR", "STRT"),
      nonsecure_unpriv);
  CHECK((s.peek("FLASH_NSSR") & wrperr) != 0);
  CHECK(s.word(sector6 + 8) == 3);
  CHECK_FALSE(s.tzic("FLASHF"));
}

TEST_CASE(
    "RM0481 7.6.2 Table 63: at HDPL 2 a Secure program and erase of the closed HDP area are "
    "refused with WRPERR, the sector past it erases")
{
  auto wanted = four_secure_sectors();
  wanted.hdp1 = stm32::page_range{.first = 0, .last = 1};
  secure_flash const s{wanted};
  auto const         wrperr_s = s.bit("FLASH_SECSR", "WRPERR");
  auto const         erase    = [&s](std::uint32_t sector) {
    return s.bit("FLASH_SECCR", "SER")
         | s.sector_field("FLASH_SECCR", sector)
         | s.bit("FLASH_SECCR", "STRT");
  };
  s.unlock(true);
  s.write("FLASH_SECCR", s.bit("FLASH_SECCR", "PG"), secure_priv);
  s.store(flash_s + sector_bytes, {1, 2, 3, 4}, secure_priv);
  s.settle();
  CHECK(s.word(flash_s + sector_bytes) == 1);
  s.clear_flags(true);
  s.r.write("SBS", "SBS_HDPLCR", 0x6a, true);
  REQUIRE(s.r.read("SBS", "SBS_HDPLSR", true) == 0x8a);
  s.store(flash_s + sector_bytes + 16, {1, 2, 3, 4}, secure_priv);
  CHECK((s.peek("FLASH_SECSR") & wrperr_s) != 0);
  CHECK_FALSE(s.tzic("FLASHF"));
  s.clear_flags(true);
  s.write("FLASH_SECCR", erase(1), secure_priv);
  CHECK((s.peek("FLASH_SECSR") & wrperr_s) != 0);
  s.clear_flags(true);
  REQUIRE(s.r.f.target->mem().write(flash_ns + 2 * sector_bytes, 4, 0));
  s.write("FLASH_SECCR", erase(2), secure_priv);
  CHECK((s.peek("FLASH_SECSR") & s.bit("FLASH_SECSR", "BSY")) != 0);
  s.settle();
  CHECK(s.word(flash_ns + 2 * sector_bytes) == 0xffff'ffff);
  CHECK((s.peek("FLASH_SECSR") & wrperr_s) == 0);
}

TEST_CASE(
    "a Secure core's store through an SAU Non-secure region programs a Non-secure H5 sector "
    "through FLASH_NSCR, without the region it is a Secure store refused with "
    "FLASH_SECSR.WRPERR, B10.2")
{
  constexpr address sector8 = flash_ns + 8 * sector_bytes;
  for(auto const sau_over : {true, false})
  {
    CAPTURE(sau_over);
    secure_flash const s;
    s.unlock(false);
    s.write("FLASH_NSCR", s.bit("FLASH_NSCR", "PG"), nonsecure_priv);
    if(not sau_over)
    {
      s.unlock(true);
      s.write("FLASH_SECCR", s.bit("FLASH_SECCR", "PG"), secure_priv);
    }
    s.sau_region(0, sector8, sector8 + sector_bytes - 32);
    s.sau(sau_over ? 1u : 0u);
    s.put(code_s, {0x6001, 0x6041, 0x6081, 0x60c1, bkpt});
    s.r.f.set("r0", sector8);
    s.r.f.set("r1", 0x0bad'f00d);
    s.r.f.set("pc", code_s | 1u);
    CHECK(s.r.f.run_to_stop({.max_instructions = icount{5}}).reason == stop_reason::user);
    s.sau(0);
    s.settle();
    CHECK(s.word(sector8 + 12) == (sau_over ? 0x0bad'f00du : 0xffff'ffffu));
    CHECK((s.peek("FLASH_NSSR") & s.bit("FLASH_NSSR", "WRPERR")) == 0);
    CHECK(((s.peek("FLASH_SECSR") & s.bit("FLASH_SECSR", "WRPERR")) != 0) == not sau_over);
  }
}

TEST_CASE(
    "a FLASH_SECBB1R1 store that makes the running Secure loop's sector Non-secure faults its "
    "next fetch in an SAU-split sector, RM0481 7.6.3 and B3.11 RJQMQ")
{
  secure_flash const s{four_secure_sectors(), popcorn::arch::fault_policy::enter_handler};
  constexpr address  sector4 = flash_ns + 4 * sector_bytes;
  s.write("FLASH_SECBB1R1", 1u << 4, secure_priv);
  s.secure_vectors();
  s.put(sector4, {0x3b01, 0x42a3, 0xd100, 0x6001, 0x2b00, 0xd1f9, bkpt});
  s.sau_region(0, sector4 + 0x200, sector4 + 0x3e0);
  s.sau(1);
  s.r.f.set("r0", s.r.at("FLASH", "FLASH_SECBB1R1"));
  s.r.f.set("r1", 0);
  s.r.f.set("r3", 100);
  s.r.f.set("r4", 50);
  s.r.f.set("pc", sector4 | 1u);
  CHECK(s.r.f.run_to_stop({.max_instructions = icount{2000}}).reason == stop_reason::user);
  CHECK(s.value("pc") == handler_s + 2);
  CHECK((s.value("xpsr") & 0x1ff) == 3);
  CHECK((s.word(0xe000'ed2c) & (1u << 30)) != 0);
  CHECK((s.word(0xe000'ed28) & (1u << 8)) != 0);
  CHECK(s.word(s.value("msp") + 24) == sector4 + 8);
  CHECK(s.value("r3") == 50);
}

TEST_CASE(
    "an H5 TrustZone snapshot taken while a Secure program is BSY ends it in FLASH_SECSR after a "
    "restore into a fresh machine, the Secure lock state kept")
{
  secure_flash const s;
  s.unlock(true);
  s.write("FLASH_SECCR", s.bit("FLASH_SECCR", "PG") | s.bit("FLASH_SECCR", "EOPIE"), secure_priv);
  s.store(unused_secure + 32, {1, 2, 3, 4}, secure_priv);
  REQUIRE((s.peek("FLASH_SECSR") & s.bit("FLASH_SECSR", "BSY")) != 0);
  auto const         taken = s.r.f.target->snapshot();
  secure_flash const fresh;
  fresh.r.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))});
  fresh.settle();
  CHECK(fresh.word(unused_secure + 44) == 4);
  CHECK(fresh.peek("FLASH_SECSR") == fresh.bit("FLASH_SECSR", "EOP"));
  CHECK((fresh.peek("FLASH_SECCR") & fresh.bit("FLASH_SECCR", "LOCK")) == 0);
  CHECK(fresh.pending(flash_s_irq));
}

TEST_CASE(
    "H5 power_cycle under TrustZone keeps the option bytes, resets FLASH_SECBBxRy, the MPCBB, the "
    "SAU and HDPL")
{
  secure_flash const s;
  auto const         secwm = s.peek("FLASH_SECWM1R_CUR");
  s.write("FLASH_SECBB1R1", 0xf0, secure_priv);
  s.write("FLASH_SECBB2R4", 0x1, secure_priv);
  s.write("GTZC1_MPCBB1", "SECCFGR0", 0xffff'fffe);
  s.r.write("SBS", "SBS_HDPLCR", 0x6a, true);
  s.sau_region(0, 0x2000'0000, 0x2000'03e0);
  s.sau(1);
  s.r.f.target->power_cycle();
  CHECK(s.peek("FLASH_SECWM1R_CUR") == secwm);
  CHECK(s.peek("FLASH_SECBB1R1") == 0);
  CHECK(s.peek("FLASH_SECBB2R4") == 0);
  CHECK(s.r.peek("GTZC1_MPCBB1", "SECCFGR0") == 0xffff'ffff);
  CHECK(s.word(sau_ctrl) == 0);
  CHECK(s.r.read("SBS", "SBS_HDPLSR", true) == 0x51);
}

TEST_CASE("an H5 flash whose banks need more block registers than the H5 traits hold throws")
{
  auto desc = h573();
  for(auto& region : desc.regions)
  {
    if(region.controller == "FLASH")
    {
      region.size *= 2;
    }
  }
  stm32::register_builtin_models();
  try
  {
    core::machine const target{
        desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
    FAIL("the machine built");
  }
  catch(popcorn::config_error const& error)
  {
    CHECK(error.expected() == "8 block registers per bank, the traits hold 4");
  }
}
