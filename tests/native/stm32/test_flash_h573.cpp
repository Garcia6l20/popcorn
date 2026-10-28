#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
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
