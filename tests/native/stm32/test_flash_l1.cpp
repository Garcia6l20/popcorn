#include "boot_alias_checks.hpp"
#include "flash_unit.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace arch = popcorn::arch;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware   = std::string_view{"l152xe_l1_flash_ll"};
constexpr auto          firmware_b = std::string_view{"l152xb_l1_flash_ll"};
constexpr address       flash      = 0x4002'3C00;
constexpr address       acr        = flash + 0x00;
constexpr address       pecr       = flash + 0x04;
constexpr address       pdkeyr     = flash + 0x08;
constexpr address       pekeyr     = flash + 0x0C;
constexpr address       prgkeyr    = flash + 0x10;
constexpr address       optkeyr    = flash + 0x14;
constexpr address       sr         = flash + 0x18;
constexpr address       target     = 0x0801'F000;
constexpr address       eeprom     = 0x0808'0000;
constexpr std::uint32_t pe_key1    = 0x89AB'CDEF;
constexpr std::uint32_t pe_key2    = 0x0203'0405;
constexpr std::uint32_t prg_key1   = 0x8C9D'AEBF;
constexpr std::uint32_t prg_key2   = 0x1314'1516;
constexpr std::uint32_t opt_key1   = 0xFBEA'D9C8;
constexpr std::uint32_t opt_key2   = 0x2425'2627;
constexpr std::uint32_t pd_key1    = 0x0415'2637;
constexpr std::uint32_t pd_key2    = 0xFAFB'FCFD;
constexpr std::uint32_t pelock     = 1u << 0;
constexpr std::uint32_t prglock    = 1u << 1;
constexpr std::uint32_t optlock    = 1u << 2;
constexpr std::uint32_t locks      = pelock | prglock | optlock;
constexpr std::uint32_t prog       = 1u << 3;
constexpr std::uint32_t data       = 1u << 4;
constexpr std::uint32_t ftdw       = 1u << 8;
constexpr std::uint32_t erase      = 1u << 9;
constexpr std::uint32_t fprg       = 1u << 10;
constexpr std::uint32_t parallel   = 1u << 15;
constexpr std::uint32_t eopie      = 1u << 16;
constexpr std::uint32_t errie      = 1u << 17;
constexpr std::uint32_t bsy        = 1u << 0;
constexpr std::uint32_t eop        = 1u << 1;
constexpr std::uint32_t endhv      = 1u << 2;
constexpr std::uint32_t ready      = 1u << 3;
constexpr std::uint32_t wrperr     = 1u << 8;
constexpr std::uint32_t pgaerr     = 1u << 9;
constexpr std::uint32_t sizerr     = 1u << 10;
constexpr std::uint32_t latency    = 1u << 0;
constexpr std::uint32_t prften     = 1u << 1;
constexpr std::uint32_t acc64      = 1u << 2;
constexpr std::uint32_t run_pd     = 1u << 4;
constexpr std::uint32_t flash_irq  = 4;
constexpr std::uint64_t tprog_ps   = 3'280'000'000;

void copy_ram_code(stm32_fixture const& f)
{
  auto const from = sym(f.firmware_name, "_sidata");
  auto const to   = sym(f.firmware_name, "_sdata");
  auto const end  = sym(f.firmware_name, "_edata");
  for(address at = 0; at < end - to; at += 4)
  {
    auto const word = f.target->mem().read(from + at, 4);
    REQUIRE(word.has_value());
    REQUIRE(f.target->mem().write(to + at, 4, *word).has_value());
  }
}

auto parked(
    part_description const& desc    = l152re(),
    std::string_view        name    = firmware,
    core::machine_options   options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{desc, name, std::move(options)};
  copy_ram_code(f);
  f.set("pc", sym(name, "idle"));
  return f;
}

auto strict() -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  return options;
}

auto read(stm32_fixture const& f, address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

auto try_write(stm32_fixture const& f, address at, std::uint32_t value) -> bool
{
  return f.target->mem().write(at, 4, value).has_value();
}

void write(stm32_fixture const& f, address at, std::uint32_t value)
{
  REQUIRE(try_write(f, at, value));
}

void unlock(stm32_fixture const& f)
{
  write(f, pekeyr, pe_key1);
  write(f, pekeyr, pe_key2);
  write(f, prgkeyr, prg_key1);
  write(f, prgkeyr, prg_key2);
  REQUIRE((read(f, pecr) & locks) == optlock);
}

void call(stm32_fixture const& f, std::string_view entry, std::initializer_list<std::uint32_t> args)
{
  auto index = 0;
  for(auto const arg : args)
  {
    f.set(std::format("r{}", index++), arg);
  }
  f.set("pc", sym(f.firmware_name, entry));
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  f.set("pc", sym(f.firmware_name, "idle"));
}

void store_words(stm32_fixture const& f, address to, std::vector<std::uint32_t> const& values)
{
  auto const buffer = sym(f.firmware_name, "words");
  for(std::size_t i = 0; i < values.size(); ++i)
  {
    write(f, buffer + 4 * i, values[i]);
  }
  call(
      f, "copy_words",
      {static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(buffer),
       static_cast<std::uint32_t>(values.size())});
}

void idle_to(stm32_fixture const& f, std::uint64_t ps, icount cap = icount{1'000'000})
{
  auto const stopped = f.run_to_stop({.max_instructions = cap, .until_time = vtime{ps}});
  REQUIRE(stopped.reason == stop_reason::time);
}

auto now_ps(stm32_fixture const& f) -> std::uint64_t
{
  return f.target->virtual_clock().time().ps;
}

auto flash_due(stm32_fixture const& f) -> std::vector<std::uint64_t>
{
  auto const& units = f.target->description().peripherals;
  auto const  owner = static_cast<std::uint32_t>(
      std::ranges::find(units, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name)
      - units.begin());
  std::vector<std::uint64_t> due;
  for(auto const& event : f.target->snapshot().data().events.pending)
  {
    if(event.id.owner == owner)
    {
      due.push_back(event.due_ps);
    }
  }
  return due;
}

auto irq_pending(stm32_fixture const& f) -> bool
{
  return (read(f, 0xE000'E200u) >> flash_irq & 1u) != 0;
}

void clear_irq(stm32_fixture const& f)
{
  write(f, 0xE000'E280u, 1u << flash_irq);
}

auto memory_write(stm32_fixture const& f, address at, std::uint8_t size, std::uint32_t value)
    -> bool
{
  auto const& regions = f.target->description().regions;
  auto const  region  = std::ranges::find_if(regions, [at](popcorn::region_desc const& r) {
    return r.controller == "FLASH" and at >= r.start and at < r.start + r.size;
  });
  REQUIRE(region != regions.end());
  return f.target->model("FLASH")
      ->on_memory_write(*region, at, size, value, {.pc = 0, .now = icount{0}})
      .has_value();
}

struct log_capture
{
  std::vector<core::log_record> records;
  core::hook_handle             sink;

  explicit log_capture(stm32_fixture const& f)
      : sink{f.target->add_log_sink(
            [this](core::log_record const& record) { records.push_back(record); })}
  {}

  [[nodiscard]] auto count(std::string_view reg) const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(records, [reg](auto const& record) {
      return record.peripheral == "FLASH" and record.reg == reg;
    }));
  }
};
}

TEST_CASE(
    "RM0038 3.4.1 and 3.4.3: the LL sequence unlocks, programs a word, flags SIZERR and WRPERR")
{
  for(auto const& [desc, name] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(desc->part);
    stm32_fixture f{*desc, name};
    REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
    auto const log = f.log();
    CHECK(
        log
        == std::vector<std::uint32_t>{
            locks, ready | endhv, prglock | optlock, optlock, bsy, 1, ready | endhv | eop,
            0x1234'5678u, ready | endhv, ready | endhv | sizerr, locks, ready | endhv | wrperr, 0,
            acc64 | prften | latency});
  }
}

TEST_CASE("RM0038 3.4.1: a wrong PEKEY2 is a bus error and PECR stays locked until reset")
{
  auto options   = popcorn::testing::stm32_options();
  options.faults = arch::fault_policy::enter_handler;
  stm32_fixture f{l152re(), firmware, options};
  copy_ram_code(f);
  f.set("pc", sym(firmware, "wrong_key"));
  REQUIRE(f.run_to_stop({.max_instructions = icount{10'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0xfa17u, locks});
  CHECK_FALSE(try_write(f, pekeyr, pe_key1));
  CHECK_FALSE(try_write(f, pekeyr, pe_key2));
  CHECK((read(f, pecr) & pelock) != 0);
  f.target->reset();
  copy_ram_code(f);
  f.set("pc", sym(firmware, "idle"));
  unlock(f);
}

TEST_CASE("RM0038 3.4.1: PEKEY1 alone unlocks nothing, PEKEY2 first and a third key are bus errors")
{
  {
    auto const f = parked();
    write(f, pekeyr, pe_key1);
    CHECK(read(f, pecr) == locks);
    CHECK(read(f, pekeyr) == 0);
    write(f, pekeyr, pe_key2);
    CHECK(read(f, pecr) == (prglock | optlock));
    write(f, pecr, eopie | ftdw);
    CHECK(read(f, pecr) == (prglock | optlock | eopie | ftdw));
    CHECK_FALSE(try_write(f, pekeyr, pe_key1));
    CHECK(read(f, pecr) == (locks | eopie));
    write(f, pecr, 0);
    CHECK(read(f, pecr) == (locks | eopie));
  }
  auto const f = parked();
  CHECK_FALSE(try_write(f, pekeyr, pe_key2));
  CHECK_FALSE(try_write(f, pekeyr, pe_key1));
  CHECK(read(f, pecr) == locks);
}

TEST_CASE(
    "RM0038 3.4.1: PRGKEYR and OPTKEYR need PECR unlocked, a wrong key holds the lock to reset")
{
  auto f = parked();
  CHECK_FALSE(try_write(f, prgkeyr, prg_key1));
  write(f, pekeyr, pe_key1);
  write(f, pekeyr, pe_key2);
  CHECK_FALSE(try_write(f, prgkeyr, prg_key1));
  CHECK_FALSE(try_write(f, prgkeyr, prg_key2));
  CHECK((read(f, pecr) & prglock) != 0);
  write(f, optkeyr, opt_key1);
  write(f, optkeyr, opt_key2);
  CHECK(read(f, pecr) == prglock);
  write(f, pecr, optlock);
  CHECK(read(f, pecr) == (prglock | optlock));
  write(f, optkeyr, opt_key1);
  CHECK_FALSE(try_write(f, optkeyr, opt_key1));
  CHECK_FALSE(try_write(f, optkeyr, opt_key1));
  CHECK(read(f, pecr) == (prglock | optlock));
  f.target->reset();
  copy_ram_code(f);
  f.set("pc", sym(firmware, "idle"));
  write(f, pekeyr, pe_key1);
  write(f, pekeyr, pe_key2);
  write(f, optkeyr, opt_key1);
  write(f, optkeyr, opt_key2);
  CHECK(read(f, pecr) == prglock);
  write(f, prgkeyr, prg_key1);
  write(f, prgkeyr, prg_key2);
  CHECK(read(f, pecr) == 0);
}

TEST_CASE(
    "RM0038 3.9.2: a third PRGKEYR or OPTKEYR write sets its lock to reset, a store then WRPERR")
{
  auto const f = parked();
  unlock(f);
  write(f, optkeyr, opt_key1);
  write(f, optkeyr, opt_key2);
  CHECK(read(f, pecr) == 0);
  CHECK_FALSE(try_write(f, prgkeyr, prg_key1));
  CHECK_FALSE(try_write(f, optkeyr, opt_key1));
  CHECK(read(f, pecr) == (prglock | optlock));
  REQUIRE(memory_write(f, target, 4, 5));
  CHECK(read(f, sr) == (ready | endhv | wrperr));
  write(f, sr, wrperr);
  CHECK_FALSE(try_write(f, prgkeyr, prg_key1));
  CHECK_FALSE(try_write(f, optkeyr, opt_key1));
}

TEST_CASE("RM0038 3.4.1: OPTKEY2 without OPTKEY1 is a bus error and OPTLOCK stays set")
{
  auto const f = parked();
  write(f, pekeyr, pe_key1);
  write(f, pekeyr, pe_key2);
  CHECK_FALSE(try_write(f, optkeyr, opt_key2));
  CHECK(read(f, pecr) == (prglock | optlock));
}

TEST_CASE("RM0038 3.9.2: setting PELOCK sets PRGLOCK and OPTLOCK and clears the mode bits")
{
  auto const f = parked();
  unlock(f);
  write(f, pecr, prog | data | ftdw | erase | fprg | eopie | errie);
  CHECK(read(f, pecr) == (optlock | prog | data | ftdw | erase | fprg | eopie | errie));
  write(f, pecr, pelock | eopie);
  CHECK(read(f, pecr) == (locks | eopie));
  unlock(f);
  write(f, pecr, prglock | eopie);
  CHECK(read(f, pecr) == (prglock | optlock | eopie));
}

TEST_CASE("RM0038 3.9.2: PARALLELBANK on the two-bank Cat.5 only, mode bits frozen while BSY")
{
  auto const rb = parked(l152rb(), firmware_b);
  unlock(rb);
  write(rb, pecr, parallel);
  CHECK(read(rb, pecr) == optlock);
  auto const re = parked();
  unlock(re);
  write(re, pecr, parallel);
  CHECK(read(re, pecr) == (optlock | parallel));
  write(re, pecr, 0);
  store_words(re, target, {0x55});
  REQUIRE((read(re, sr) & bsy) != 0);
  write(re, pecr, parallel | ftdw | data | eopie);
  CHECK(read(re, pecr) == (optlock | eopie));
}

TEST_CASE("RM0038 3.9.1: LATENCY and PRFTEN follow ACC64, RUN_PD needs PDKEYR, READY follows it")
{
  auto const f = parked();
  write(f, acr, latency | prften);
  CHECK(read(f, acr) == 0);
  write(f, acr, acc64 | latency);
  CHECK(read(f, acr) == acc64);
  write(f, acr, acc64 | latency | prften);
  CHECK(read(f, acr) == (acc64 | latency | prften));
  write(f, acr, latency);
  CHECK(read(f, acr) == (latency | prften));
  write(f, acr, run_pd);
  CHECK((read(f, acr) & run_pd) == 0);
  write(f, pdkeyr, pd_key1);
  write(f, pdkeyr, pd_key2);
  CHECK(read(f, pdkeyr) == 0);
  write(f, acr, run_pd);
  CHECK((read(f, acr) & run_pd) != 0);
  CHECK((read(f, sr) & ready) == 0);
  write(f, acr, 0);
  CHECK((read(f, acr) & run_pd) != 0);
  write(f, pdkeyr, pd_key2);
  write(f, acr, 0);
  CHECK((read(f, acr) & run_pd) != 0);
  write(f, pdkeyr, pd_key1);
  write(f, pdkeyr, pd_key2);
  write(f, acr, 0);
  CHECK((read(f, acr) & run_pd) == 0);
  CHECK((read(f, sr) & ready) != 0);
}

TEST_CASE("RM0038 Table 13: LATENCY 0 at 32 MHz in Range 1 warns once, Range 2 at 32 MHz warns")
{
  stm32_fixture f{l152re(), "l152xe_l1_rcc_ll"};
  log_capture   logged{f};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  REQUIRE(read(f, acr) == (acc64 | latency));
  CHECK(logged.count("ACR") == 0);
  write(f, acr, acc64);
  write(f, acr, acc64);
  CHECK(logged.count("ACR") == 1);
  write(f, acr, acc64 | latency);
  CHECK(logged.count("ACR") == 1);
  auto const& pwr = f.target->peripheral("PWR").description();
  auto const  cr  = pwr.base + popcorn::find_register(pwr, "CR")->offset;
  write(f, cr, (read(f, cr) & ~(3u << 11)) | (2u << 11));
  f.set("pc", sym("l152xe_l1_rcc_ll", "idle"));
  idle_to(f, now_ps(f) + 100'000'000);
  REQUIRE(logged.count("ACR") == 2);
  CHECK(logged.records.back().message.contains("above the maximum of voltage range 2"));
}

TEST_CASE("RM0038 Table 13: a LATENCY below the table raises model_error under strict")
{
  stm32_fixture f{l152re(), "l152xe_l1_rcc_ll", strict()};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto& regs = f.target->peripheral("FLASH");
  CHECK_THROWS_AS(
      (void)f.target->model("FLASH")->on_write(
          regs, 0, 4, regs.peek(0), acc64, {.pc = 0, .now = icount{0}}),
      popcorn::model_error);
}

TEST_CASE("RM0038 3.4.3 and 3.9.7: a word program holds BSY for one tprog, then EOP and ENDHV")
{
  auto const f = parked();
  unlock(f);
  store_words(f, target, {0xCAFE'F00Du});
  auto const start = now_ps(f);
  CHECK(read(f, target) == 0xCAFE'F00Du);
  CHECK(read(f, sr) == (ready | bsy));
  auto const due = flash_due(f);
  REQUIRE(due.size() == 1);
  CHECK(due[0] > start);
  CHECK(due[0] <= start + tprog_ps);
  idle_to(f, due[0] - 1'000'000);
  CHECK(read(f, sr) == (ready | bsy));
  idle_to(f, due[0] + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
  write(f, sr, eop);
  CHECK(read(f, sr) == (ready | endhv));
}

TEST_CASE("RM0038 3.4.3: the program ends one tprog after the store, by the queued event")
{
  auto const f = parked();
  unlock(f);
  auto const before = now_ps(f);
  REQUIRE(memory_write(f, target, 4, 1));
  auto const due = flash_due(f);
  REQUIRE(due.size() == 1);
  CHECK(due[0] == before + tprog_ps);
}

TEST_CASE(
    "a store while BSY is written at once and adds one tprog, decided by the PECR of its time")
{
  auto const f = parked();
  unlock(f);
  REQUIRE(memory_write(f, target, 4, 0x1111'1111u));
  REQUIRE(memory_write(f, target + 4, 4, 0x2222'2222u));
  write(f, pecr, pelock);
  auto const start = now_ps(f);
  CHECK(read(f, target) == 0x1111'1111u);
  CHECK(read(f, target + 4) == 0x2222'2222u);
  CHECK(read(f, sr) == (ready | bsy));
  idle_to(f, start + tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | bsy | eop));
  idle_to(f, start + 2 * tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
}

TEST_CASE("1000 stores while BSY keep the FLASH save size and end 1000 tprog later")
{
  auto const size = [](stm32_fixture const& f) {
    auto const saved = f.target->snapshot();
    auto const found = std::ranges::find(
        saved.data().models, std::string_view{"FLASH"}, &core::snapshot_part::owner);
    REQUIRE(found != saved.data().models.end());
    return found->data.size();
  };
  auto const two = parked(l152rb(), firmware_b);
  unlock(two);
  call(two, "store_loop", {static_cast<std::uint32_t>(target), 2});
  auto const f = parked(l152rb(), firmware_b);
  unlock(f);
  call(f, "store_loop", {static_cast<std::uint32_t>(target), 1000});
  auto const start = now_ps(f);
  CHECK(size(f) == size(two));
  CHECK(read(f, target + 4 * 999) == 999);
  CHECK(read(f, sr) == (ready | bsy));
  idle_to(f, start + 999 * tprog_ps - 1'000'000'000, icount{8'000'000});
  CHECK((read(f, sr) & bsy) != 0);
  idle_to(f, start + 1000 * tprog_ps + 1'000'000, icount{8'000'000});
  CHECK(read(f, sr) == (ready | endhv | eop));
}

TEST_CASE("RM0038 3.7.3 and 3.9.7: WRPERR while locked, SIZERR, PGAERR, cleared by a write of 1")
{
  auto const f   = parked();
  auto const old = read(f, target);
  REQUIRE(memory_write(f, target, 4, 0x77));
  CHECK(read(f, sr) == (ready | endhv | wrperr));
  write(f, pekeyr, pe_key1);
  write(f, pekeyr, pe_key2);
  write(f, sr, wrperr);
  REQUIRE(memory_write(f, target, 4, 0x77));
  CHECK(read(f, sr) == (ready | endhv | wrperr));
  write(f, prgkeyr, prg_key1);
  write(f, prgkeyr, prg_key2);
  write(f, sr, wrperr);
  call(f, "store_byte", {static_cast<std::uint32_t>(target), 0x5a});
  CHECK(read(f, sr) == (ready | endhv | sizerr));
  call(f, "store_half", {static_cast<std::uint32_t>(target), 0x5a5a});
  CHECK(read(f, sr) == (ready | endhv | sizerr));
  write(f, sr, sizerr);
  REQUIRE(memory_write(f, target + 2, 4, 0x77));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  CHECK(read(f, sr) == (ready | endhv));
  CHECK(read(f, target) == old);
  CHECK(flash_due(f).empty());
}

TEST_CASE("RM0038 3.8: FLASH_IRQn follows EOP with EOPIE and an error flag with ERRIE")
{
  auto const f = parked();
  unlock(f);
  write(f, pecr, errie);
  call(f, "store_byte", {static_cast<std::uint32_t>(target), 0x5a});
  CHECK(irq_pending(f));
  write(f, sr, sizerr);
  clear_irq(f);
  CHECK_FALSE(irq_pending(f));
  write(f, pecr, eopie);
  REQUIRE(memory_write(f, target, 4, 3));
  CHECK_FALSE(irq_pending(f));
  idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  CHECK(irq_pending(f));
  write(f, sr, eop);
  clear_irq(f);
  CHECK_FALSE(irq_pending(f));
  write(f, pecr, errie);
  REQUIRE(memory_write(f, target + 4, 4, 3));
  idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  CHECK((read(f, sr) & eop) != 0);
  CHECK_FALSE(irq_pending(f));
}

TEST_CASE("RM0038 Table 13: Range 3 at 4.194 MHz needs 1 wait state on Cat.1 only")
{
  for(auto const& [desc, name, warned] : {
          std::tuple{&l152re(), firmware,   false},
          std::tuple{&l152rb(), firmware_b, true }
  })
  {
    CAPTURE(desc->part);
    auto const  f = parked(*desc, name);
    log_capture logged{f};
    auto const  field_at = [&](std::string_view unit, std::string_view reg) {
      auto const& d = f.target->peripheral(unit).description();
      return d.base + popcorn::find_register(d, reg)->offset;
    };
    auto const icscr = field_at("RCC", "ICSCR");
    write(f, icscr, (read(f, icscr) & ~(7u << 13)) | (6u << 13));
    REQUIRE(f.target->virtual_clock().core_hz() == 4'194'304);
    CHECK(logged.count("ACR") == 0);
    auto const cr = field_at("PWR", "CR");
    write(f, cr, (read(f, cr) & ~(3u << 11)) | (3u << 11));
    idle_to(f, now_ps(f) + 100'000'000);
    CHECK(logged.count("ACR") == (warned ? 1u : 0u));
  }
}

TEST_CASE(
    "a store with ERASE or FPRG and no PROG, or both, warns once and programs nothing, "
    "model_error under strict")
{
  {
    auto const  f = parked();
    log_capture logged{f};
    unlock(f);
    auto const old = read(f, target);
    write(f, pecr, erase);
    REQUIRE(memory_write(f, target, 4, 0));
    write(f, pecr, erase | fprg | prog);
    REQUIRE(memory_write(f, target, 4, 0));
    CHECK(logged.count("PECR") == 1);
    CHECK(read(f, target) == old);
    CHECK((read(f, sr) & bsy) == 0);
  }
  auto const f = parked(l152re(), firmware, strict());
  unlock(f);
  write(f, pecr, fprg);
  CHECK_THROWS_AS((void)memory_write(f, target, 4, 1), popcorn::model_error);
}

TEST_CASE("RM0038 3.4.2: ERASE and PROG with a store to a page start erase the page to 0")
{
  auto const f = parked();
  unlock(f);
  for(std::uint32_t at = 0; at < 64; ++at)
  {
    REQUIRE(memory_write(f, target + 4 * at, 4, 0xA5A5'0000u | at));
    idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  }
  REQUIRE(memory_write(f, target + 256, 4, 0x77));
  idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  write(f, sr, eop);
  call(f, "erase_page", {static_cast<std::uint32_t>(target + 4)});
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  CHECK(read(f, target + 4) == 0xA5A5'0001u);
  write(f, sr, pgaerr);
  write(f, pecr, erase | prog);
  auto const before = now_ps(f);
  REQUIRE(memory_write(f, target, 4, 0x1234));
  CHECK(read(f, sr) == (ready | bsy));
  auto const due = flash_due(f);
  REQUIRE(due.size() == 1);
  CHECK(due[0] == before + tprog_ps);
  for(std::uint32_t at = 0; at < 64; ++at)
  {
    CHECK(read(f, target + 4 * at) == 0);
  }
  CHECK(read(f, target + 256) == 0x77);
  write(f, pecr, 0);
  call(f, "wait_idle", {});
  CHECK(read(f, sr) == (ready | endhv | eop));
}

TEST_CASE("RM0038 3.4.2: a page erase stored while BSY erases at once and adds one tprog")
{
  auto const f = parked();
  unlock(f);
  REQUIRE(memory_write(f, target + 256, 4, 7));
  idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  write(f, sr, eop);
  write(f, pecr, erase | prog);
  REQUIRE(memory_write(f, target, 4, 0));
  REQUIRE(memory_write(f, target + 256, 4, 0));
  auto const start = now_ps(f);
  CHECK(read(f, target + 256) == 0);
  idle_to(f, start + tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | bsy | eop));
  idle_to(f, start + 2 * tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
}

auto load_buffer(stm32_fixture const& f, std::uint32_t count, std::uint32_t seed) -> address
{
  auto const buffer = sym(f.firmware_name, "words");
  for(std::uint32_t i = 0; i < count; ++i)
  {
    write(f, buffer + 4 * i, seed + i);
  }
  return buffer;
}

TEST_CASE("RM0038 3.4.3: FPRG and PROG with 32 sequential stores program a half page in one tprog")
{
  auto const f = parked();
  unlock(f);
  write(f, pecr, fprg | prog);
  auto const buffer = load_buffer(f, 32, 0x100);
  call(
      f, "copy_words",
      {static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(buffer), 31});
  CHECK(read(f, sr) == (ready | endhv));
  CHECK(read(f, target) != 0x100);
  REQUIRE(memory_write(f, target + 124, 4, 0x11f));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(flash_due(f).size() == 1);
  for(std::uint32_t at = 0; at < 32; ++at)
  {
    CHECK(read(f, target + 4 * at) == 0x100 + at);
  }
  call(f, "wait_idle", {});
  CHECK(read(f, sr) == (ready | endhv | eop));
}

TEST_CASE("RM0038 3.4.3: a half page out of sequence, off its boundary or cut short sets PGAERR")
{
  auto const f = parked();
  unlock(f);
  auto const blank = read(f, target + 128);
  write(f, pecr, fprg | prog);
  REQUIRE(memory_write(f, target + 132, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  REQUIRE(memory_write(f, target + 128, 4, 1));
  REQUIRE(memory_write(f, target + 136, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  for(std::uint32_t at = 0; at < 31; ++at)
  {
    REQUIRE(memory_write(f, target + 128 + 4 * at, 4, 2));
  }
  write(f, pecr, 0);
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  write(f, pecr, fprg | prog);
  REQUIRE(memory_write(f, target + 128, 4, 3));
  CHECK(read(f, sr) == (ready | endhv));
  CHECK(read(f, target + 128) == blank);
  CHECK(flash_due(f).empty());
}

TEST_CASE(
    "RM0038 3.4.2 and 3.4.3: PARALLELBANK erases and programs a page and a half page per bank")
{
  constexpr address bank2 = 0x0804'0000;
  auto const        f     = parked();
  unlock(f);
  REQUIRE(memory_write(f, target, 4, 9));
  idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  REQUIRE(memory_write(f, bank2, 4, 9));
  idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  write(f, pecr, parallel | erase | prog);
  REQUIRE(memory_write(f, bank2, 4, 0));
  CHECK(read(f, sr) == (ready | endhv | eop | pgaerr));
  write(f, sr, pgaerr | eop);
  REQUIRE(memory_write(f, target, 4, 0));
  CHECK(read(f, sr) == (ready | endhv));
  CHECK(read(f, target) == 9);
  REQUIRE(memory_write(f, target + 256, 4, 0));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  REQUIRE(memory_write(f, target, 4, 0));
  REQUIRE(memory_write(f, bank2, 4, 0));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(flash_due(f).size() == 1);
  CHECK(read(f, target) == 0);
  CHECK(read(f, bank2) == 0);
  call(f, "wait_idle", {});
  write(f, sr, eop);
  write(f, pecr, parallel | fprg | prog);
  for(std::uint32_t at = 0; at < 32; ++at)
  {
    REQUIRE(memory_write(f, target + 4 * at, 4, 0x200 + at));
  }
  CHECK((read(f, sr) & bsy) == 0);
  for(std::uint32_t at = 0; at < 32; ++at)
  {
    REQUIRE(memory_write(f, bank2 + 4 * at, 4, 0x300 + at));
  }
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, target + 124) == 0x21f);
  CHECK(read(f, bank2 + 124) == 0x31f);
}

TEST_CASE("RM0038 3.9.2: with one bank PARALLELBANK reads 0 and a page erase runs alone")
{
  auto const f = parked(l152rb(), firmware_b);
  unlock(f);
  REQUIRE(memory_write(f, target, 4, 9));
  idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
  write(f, sr, eop);
  write(f, pecr, parallel | erase | prog);
  REQUIRE(memory_write(f, target, 4, 0));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, target) == 0);
}

TEST_CASE("HAL_FLASHEx_HalfPageProgram runs from SRAM and programs a half page")
{
  stm32_fixture const f{l152re(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  write(f, sr, wrperr);
  unlock(f);
  auto const buffer = load_buffer(f, 32, 0x400);
  call(
      f, "half_page_hal", {static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(buffer)});
  auto const all = f.log();
  REQUIRE(all.size() == 18);
  auto const log = std::span{all}.last(4);
  CHECK((log[0] & 0xFFF0'0000u) == 0x2000'0000u);
  CHECK(log[1] == 0);
  CHECK(log[2] == optlock);
  CHECK(log[3] == (ready | endhv));
  CHECK(read(f, sr) == (ready | endhv));
  for(std::uint32_t at = 0; at < 32; ++at)
  {
    CHECK(read(f, target + 4 * at) == 0x400 + at);
  }
}

TEST_CASE("RM0038 3.6: a program in voltage range 3 programs and warns once, model_error strict")
{
  auto const set_range3 = [](stm32_fixture const& f) {
    auto const& pwr = f.target->peripheral("PWR").description();
    auto const  cr  = pwr.base + popcorn::find_register(pwr, "CR")->offset;
    write(f, cr, (read(f, cr) & ~(3u << 11)) | (3u << 11));
    idle_to(f, now_ps(f) + 100'000'000);
  };
  {
    auto const  f = parked();
    log_capture logged{f};
    set_range3(f);
    unlock(f);
    REQUIRE(memory_write(f, target, 4, 9));
    idle_to(f, now_ps(f) + tprog_ps + 1'000'000);
    REQUIRE(memory_write(f, target + 4, 4, 9));
    CHECK(logged.count("PECR") == 1);
    CHECK(read(f, target) == 9);
  }
  auto const f = parked(l152re(), firmware, strict());
  set_range3(f);
  unlock(f);
  CHECK_THROWS_AS((void)memory_write(f, target, 4, 1), popcorn::model_error);
}

TEST_CASE("RM0038 6.3.5: an FLITFRST pulse with RUN_PD 0 changes nothing, a program runs on")
{
  auto const f = parked();
  unlock(f);
  write(f, acr, acc64);
  REQUIRE(memory_write(f, target, 4, 1));
  auto const due = flash_due(f);
  popcorn::testing::rcc_reset_pulse(f, "AHBRSTR", "FLITFRST");
  CHECK(read(f, acr) == acc64);
  CHECK(read(f, pecr) == optlock);
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(flash_due(f) == due);
}

TEST_CASE("the HAL_DeInit AHB reset pulse at 32 MHz with RUN_PD 0 leaves ACR, no strict error")
{
  stm32_fixture f{l152re(), "l152xe_l1_rcc_ll", strict()};
  log_capture   logged{f};
  REQUIRE(f.run_to_stop({.max_instructions = icount{1'000'000}}).reason == stop_reason::user);
  auto const& rcc     = f.target->peripheral("RCC").description();
  auto const  ahbrstr = rcc.base + popcorn::find_register(rcc, "AHBRSTR")->offset;
  write(f, ahbrstr, 0xFFFF'FFFFu);
  write(f, ahbrstr, 0);
  CHECK(read(f, acr) == (acc64 | latency));
  CHECK(logged.count("ACR") == 0);
}

TEST_CASE("RM0038 6.3.5: an FLITFRST pulse relocks FLASH and the old program's end is stale")
{
  auto const f = parked();
  unlock(f);
  write(f, pdkeyr, pd_key1);
  write(f, pdkeyr, pd_key2);
  write(f, acr, acc64 | run_pd);
  REQUIRE(memory_write(f, target, 4, 1));
  REQUIRE(memory_write(f, target + 8, 4, 3));
  auto const start = now_ps(f);
  idle_to(f, start + tprog_ps / 3);
  popcorn::testing::rcc_reset_pulse(f, "AHBRSTR", "FLITFRST");
  CHECK(read(f, pecr) == locks);
  CHECK(read(f, sr) == (ready | endhv));
  CHECK(read(f, acr) == 0);
  idle_to(f, start + 2 * tprog_ps / 3);
  unlock(f);
  REQUIRE(memory_write(f, target + 4, 4, 2));
  auto const second = now_ps(f);
  idle_to(f, start + tprog_ps + 1'000'000);
  CHECK((read(f, sr) & bsy) != 0);
  idle_to(f, second + tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
}

TEST_CASE("a FLASH snapshot keeps a PE key lock-up across a power-on reset and restore")
{
  auto const f = parked();
  write(f, pekeyr, pe_key1);
  CHECK_FALSE(try_write(f, pekeyr, 0x1234'5678u));
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  copy_ram_code(f);
  f.set("pc", sym(firmware, "idle"));
  f.target->restore(saved);
  CHECK_FALSE(try_write(f, pekeyr, pe_key1));
  CHECK(read(f, pecr) == locks);
}

TEST_CASE("RM0038 3.4.2: a never-programmed program memory word reads 0, the L1 erased value")
{
  for(auto const& [desc, name] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(desc->part);
    auto const f = parked(*desc, name);
    CHECK(read(f, target + 4) == 0);
    CHECK(read(f, 0x0801'FFFC) == 0);
  }
}

TEST_CASE("a FLASH snapshot restores a queued chain of three programs and ends it on time")
{
  auto const f = parked();
  unlock(f);
  REQUIRE(memory_write(f, target, 4, 1));
  REQUIRE(memory_write(f, target + 4, 4, 2));
  REQUIRE(memory_write(f, target + 8, 4, 3));
  auto const start = now_ps(f);
  auto const saved = f.target->snapshot();
  idle_to(f, start + 3 * tprog_ps + 1'000'000);
  REQUIRE(read(f, sr) == (ready | endhv | eop));
  f.target->restore(saved);
  CHECK(read(f, sr) == (ready | bsy));
  idle_to(f, start + 2 * tprog_ps + 1'000'000);
  CHECK((read(f, sr) & bsy) != 0);
  idle_to(f, start + 3 * tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
}

TEST_CASE("a FLASH snapshot restores the unlock, a key sequence half done and BSY")
{
  auto const f = parked();
  write(f, pekeyr, pe_key1);
  write(f, pekeyr, pe_key2);
  write(f, prgkeyr, prg_key1);
  write(f, pecr, eopie);
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  copy_ram_code(f);
  f.set("pc", sym(firmware, "idle"));
  CHECK(read(f, pecr) == locks);
  f.target->restore(saved);
  CHECK(read(f, pecr) == (prglock | optlock | eopie));
  write(f, prgkeyr, prg_key2);
  CHECK(read(f, pecr) == (optlock | eopie));
  REQUIRE(memory_write(f, target, 4, 4));
  auto const busy = f.target->snapshot();
  auto const due  = flash_due(f);
  idle_to(f, due.front() + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
  f.target->restore(busy);
  CHECK(read(f, sr) == (ready | bsy));
  idle_to(f, due.front() + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
}

namespace
{
auto cat46_variant() -> part_description
{
  auto part  = l152re();
  auto found = std::ranges::find(
      part.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != part.peripherals.end());
  found->ip_version = "STM32L1_flash_cat46";
  std::erase_if(found->registers, [](auto const& reg) { return reg.name == "WRPR4"; });
  return part;
}
}

TEST_CASE("the Cat.2 and Cat.4/6 rows: one bank and two, Range 3 at 4.194 MHz with 0 wait state")
{
  for(auto const& [desc, name, two_banks] : {
          std::tuple{popcorn::testing::description_from("POPCORN_L152RBA_DESC"), firmware_b, false},
          std::tuple{cat46_variant(),                                            firmware,   true }
  })
  {
    CAPTURE(desc.part);
    auto const  f = parked(desc, name);
    log_capture logged{f};
    unlock(f);
    write(f, pecr, parallel);
    CHECK(read(f, pecr) == (optlock | (two_banks ? parallel : 0u)));
    auto const field_at = [&](std::string_view unit, std::string_view reg) {
      auto const& d = f.target->peripheral(unit).description();
      return d.base + popcorn::find_register(d, reg)->offset;
    };
    auto const icscr = field_at("RCC", "ICSCR");
    write(f, icscr, (read(f, icscr) & ~(7u << 13)) | (6u << 13));
    auto const cr = field_at("PWR", "CR");
    write(f, cr, (read(f, cr) & ~(3u << 11)) | (3u << 11));
    idle_to(f, now_ps(f) + 100'000'000);
    CHECK(logged.count("ACR") == 0);
  }
}

TEST_CASE("every L1 part gets the FLASH model of its category, a mismatched WRPR set throws")
{
  for(auto const* desc : {&l152re(), &l152rb(), &popcorn::testing::l152rc()})
  {
    CAPTURE(desc->part);
    stm32_fixture const f{*desc, desc == &l152rb() ? firmware_b : firmware};
    CHECK(f.target->model("FLASH") != nullptr);
  }
  auto part  = l152rb();
  auto found = std::ranges::find(
      part.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != part.peripherals.end());
  found->ip_version = "STM32L1_flash_cat5";
  CHECK_THROWS_AS((stm32_fixture{std::move(part), firmware_b}), popcorn::config_error);
}

namespace
{
constexpr address       bank2       = 0x0804'0000;
constexpr address       cfsr        = 0xE000'ED28;
constexpr address       hfsr        = 0xE000'ED2C;
constexpr address       bfar        = 0xE000'ED38;
constexpr std::uint32_t ibuserr     = 1u << 8;
constexpr std::uint32_t preciserr   = 1u << 9;
constexpr std::uint32_t bfarvalid   = 1u << 15;
constexpr std::uint32_t forced      = 1u << 30;
constexpr std::uint32_t vecttbl     = 1u << 1;
constexpr std::uint32_t fault_mark  = 0xfa17;
constexpr std::uint32_t hardfault_n = 3;

auto handler_options() -> core::machine_options
{
  auto options   = popcorn::testing::stm32_options();
  options.faults = arch::fault_policy::enter_handler;
  return options;
}

void vectors_in_sram(stm32_fixture const& f)
{
  constexpr address vtor  = 0xE000'ED08;
  constexpr address table = 0x2000'C000;
  for(address at = 0; at < 0x200; at += 4)
  {
    write(f, table + at, read(f, 0x0800'0000 + at));
  }
  write(f, vtor, static_cast<std::uint32_t>(table));
}

auto ipsr(stm32_fixture const& f) -> std::uint32_t
{
  return static_cast<std::uint32_t>(f.target->regs().read(*f.target->regs().find("xpsr"))) & 0x1ffu;
}

void store_sequence(stm32_fixture const& f, address at, std::uint32_t count, std::uint32_t seed)
{
  for(std::uint32_t i = 0; i < count; ++i)
  {
    REQUIRE(memory_write(f, at + 4 * i, 4, seed + i));
  }
}

auto split_banks(std::uint32_t regions) -> part_description
{
  auto part = cat46_variant();
  auto flash_region =
      std::ranges::find(part.regions, std::string_view{"Flash"}, &popcorn::region_desc::name);
  REQUIRE(flash_region != part.regions.end());
  flash_region->size = 0x3'0000;
  auto boot =
      std::ranges::find(part.regions, std::string_view{"BOOT"}, &popcorn::region_desc::name);
  REQUIRE(boot != part.regions.end());
  boot->size          = flash_region->size;
  auto boot_bank2     = *boot;
  boot_bank2.name     = "BOOT_BANK2";
  boot_bank2.start    = bank2 - 0x0800'0000;
  boot_bank2.physical = bank2;
  auto second         = *flash_region;
  second.name         = "Flash_Bank2";
  second.start        = bank2;
  part.regions.push_back(second);
  part.regions.push_back(boot_bank2);
  if(regions > 2)
  {
    second.name  = "Flash_Bank3";
    second.start = 0x0807'0000;
    second.size  = 0x1'0000;
    part.regions.push_back(second);
  }
  return part;
}
}

TEST_CASE("RM0038 3.4.3: a half page into bank 2 with PARALLELBANK clear programs in one tprog")
{
  auto const f = parked();
  unlock(f);
  write(f, pecr, fprg | prog);
  store_sequence(f, bank2, 32, 0x500);
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(flash_due(f).size() == 1);
  CHECK(read(f, bank2) == 0x500);
  CHECK(read(f, bank2 + 124) == 0x51f);
}

TEST_CASE(
    "RM0038 2.7 Tables 6 and 7: the L152RE aliases its main flash at 0, VTOR resets to 0, a load "
    "and a fetch at 0 reach the flash and SysTick takes its vector through the alias")
{
  stm32_fixture const f{l152re(), firmware};
  popcorn::testing::check_flash_boot_alias(f, 0x0800'0000, 0x0807'0000, 0x2000'4000);
}

TEST_CASE("HAL_FLASHEx_HalfPageProgram into bank 2 from bank 1 code returns HAL_OK")
{
  stm32_fixture const f{l152re(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  write(f, sr, wrperr);
  unlock(f);
  auto const buffer = load_buffer(f, 32, 0x600);
  call(f, "half_page_hal", {static_cast<std::uint32_t>(bank2), static_cast<std::uint32_t>(buffer)});
  auto const log = f.log();
  REQUIRE(log.size() == 18);
  CHECK(log[15] == 0);
  CHECK(log[17] == (ready | endhv));
  for(std::uint32_t at = 0; at < 32; ++at)
  {
    CHECK(read(f, bank2 + 4 * at) == 0x600 + at);
  }
}

TEST_CASE(
    "RM0038 3.4.3 note 2 and 2.7: during a half page a load of its bank, at its address or through "
    "the alias at 0, is a BusFault, of the other bank it completes")
{
  for(auto const base : {address{0}, address{0x0800'0000}})
  {
    CAPTURE(base);
    auto const f = parked(l152re(), firmware, handler_options());
    vectors_in_sram(f);
    unlock(f);
    write(f, pecr, fprg | prog);
    REQUIRE(memory_write(f, target, 4, 1));
    call(f, "load_word", {static_cast<std::uint32_t>(bank2 + 8 - base)});
    CHECK(f.log().back() == read(f, bank2 + 8));
    CHECK(read(f, cfsr) == 0);
    call(f, "load_word", {static_cast<std::uint32_t>(target + 64 - base)});
    auto const log = f.log();
    REQUIRE(log.size() >= 2);
    CHECK(log[log.size() - 2] == fault_mark);
    CHECK(ipsr(f) == hardfault_n);
    CHECK((read(f, hfsr) & forced) != 0);
    CHECK((read(f, cfsr) & (preciserr | bfarvalid)) == (preciserr | bfarvalid));
    CHECK(read(f, bfar) == target + 64 - base);
  }
}

TEST_CASE(
    "RM0038 3.4.3 note 2: code in the bank a half page writes takes a BusFault, after the "
    "tprog it runs")
{
  {
    auto const f = parked(l152re(), firmware, handler_options());
    vectors_in_sram(f);
    unlock(f);
    write(f, pecr, fprg | prog);
    REQUIRE(memory_write(f, target, 4, 1));
    call(f, "read_flash", {static_cast<std::uint32_t>(target)});
    auto const log = f.log();
    REQUIRE(log.size() == 2);
    CHECK(log[0] == fault_mark);
    CHECK(ipsr(f) == hardfault_n);
    CHECK((read(f, hfsr) & forced) != 0);
    CHECK((read(f, cfsr) & ibuserr) != 0);
  }
  auto const f = parked(l152re(), firmware, handler_options());
  vectors_in_sram(f);
  unlock(f);
  write(f, pecr, fprg | prog);
  store_sequence(f, target, 32, 0x40);
  idle_to(f, flash_due(f).front() + 1'000'000);
  REQUIRE(read(f, sr) == (ready | endhv | eop));
  call(f, "read_flash", {static_cast<std::uint32_t>(target + 4)});
  CHECK(f.log() == std::vector<std::uint32_t>{0x41});
}

TEST_CASE("RM0038 3.4.4: a store to bank 2 while a half page in bank 1 is BSY sets WRPERR, aborted")
{
  auto const f = parked();
  unlock(f);
  write(f, pecr, fprg | prog);
  store_sequence(f, target, 32, 0x80);
  REQUIRE(read(f, sr) == (ready | bsy));
  auto const due = flash_due(f);
  REQUIRE(memory_write(f, bank2 + 0x100, 4, 7));
  CHECK(read(f, sr) == (ready | bsy | wrperr));
  CHECK(read(f, bank2 + 0x100) == 0);
  CHECK(flash_due(f) == due);
  idle_to(f, due.front() + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop | wrperr));
}

TEST_CASE("RM0038 3.4.3: the parallel half page takes bank 1 first, then bank 2")
{
  auto const f = parked();
  unlock(f);
  write(f, pecr, parallel | fprg | prog);
  REQUIRE(memory_write(f, bank2, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  store_sequence(f, target, 32, 0x900);
  REQUIRE(memory_write(f, target + 128, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  CHECK(read(f, target) == 0);
  CHECK(flash_due(f).empty());
}

TEST_CASE("RM0038 3.9.2: setting PELOCK or a SIZERR store drops a half page sequence")
{
  auto const f = parked();
  unlock(f);
  write(f, pecr, fprg | prog);
  store_sequence(f, target, 5, 0xa00);
  write(f, pecr, pelock | fprg | prog);
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  unlock(f);
  write(f, pecr, fprg | prog);
  store_sequence(f, target, 5, 0xb00);
  REQUIRE(memory_write(f, target + 20, 1, 1));
  CHECK(read(f, sr) == (ready | endhv | sizerr));
  write(f, sr, sizerr);
  store_sequence(f, target, 32, 0xc00);
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, target + 20) == 0xc05);
}

TEST_CASE("a FLASH snapshot keeps 16 loaded half page words and a pending parallel erase")
{
  {
    auto const f = parked();
    unlock(f);
    write(f, pecr, fprg | prog);
    store_sequence(f, target, 16, 0xd00);
    auto const saved = f.target->snapshot();
    f.target->reset(reset_kind::power_on);
    f.target->restore(saved);
    store_sequence(f, target + 64, 16, 0xd10);
    CHECK(read(f, sr) == (ready | bsy));
    CHECK(flash_due(f).size() == 1);
    CHECK(read(f, target + 124) == 0xd1f);
  }
  auto const f = parked();
  unlock(f);
  REQUIRE(memory_write(f, target + 8, 4, 5));
  REQUIRE(memory_write(f, bank2 + 8, 4, 6));
  idle_to(f, now_ps(f) + 2 * tprog_ps + 1'000'000);
  write(f, sr, eop);
  write(f, pecr, parallel | erase | prog);
  REQUIRE(memory_write(f, target, 4, 0));
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  f.target->restore(saved);
  REQUIRE(memory_write(f, bank2, 4, 0));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, target + 8) == 0);
  CHECK(read(f, bank2 + 8) == 0);
}

TEST_CASE(
    "Cat.4 bank 2 starts at half the Flash region, a parallel erase pairs the two halves"
    * doctest::skip(not popcorn::testing::has_l152rd()))
{
  constexpr address half = 0x0803'0000;
  auto const        f    = parked(popcorn::testing::description_from("POPCORN_L152RD_DESC"));
  unlock(f);
  write(f, pecr, parallel | erase | prog);
  REQUIRE(memory_write(f, half, 4, 0));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  REQUIRE(memory_write(f, half - 256, 4, 0));
  REQUIRE(memory_write(f, half, 4, 0));
  CHECK(read(f, sr) == (ready | bsy));
}

TEST_CASE("Cat.6 bank 2 starts at the second Flash region, a third region throws config_error")
{
  auto const f = parked(split_banks(2));
  unlock(f);
  write(f, pecr, parallel | erase | prog);
  REQUIRE(memory_write(f, 0x0802'FF00, 4, 0));
  REQUIRE(memory_write(f, bank2, 4, 0));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK_THROWS_AS((stm32_fixture{split_banks(3), firmware}), popcorn::config_error);
}

TEST_CASE("HAL_FLASHEx_EraseParallelPage and HAL_FLASHEx_ProgramParallelHalfPage run from SRAM")
{
  stm32_fixture const f{l152re(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  write(f, sr, wrperr);
  unlock(f);
  f.set("pc", sym(firmware, "idle"));
  REQUIRE(memory_write(f, target + 12, 4, 5));
  REQUIRE(memory_write(f, bank2 + 12, 4, 6));
  idle_to(f, now_ps(f) + 2 * tprog_ps + 1'000'000);
  write(f, sr, eop);
  call(
      f, "parallel_erase_hal",
      {static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(bank2)});
  auto log = f.log();
  REQUIRE(log.size() == 17);
  CHECK(log[14] == 0);
  CHECK(log[15] == optlock);
  CHECK(log[16] == (ready | endhv));
  CHECK(read(f, target + 12) == 0);
  CHECK(read(f, bank2 + 12) == 0);
  write(f, sr, eop);
  auto const buffer = load_buffer(f, 64, 0xe00);
  call(
      f, "parallel_half_hal",
      {static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(bank2),
       static_cast<std::uint32_t>(buffer)});
  log = f.log();
  REQUIRE(log.size() == 20);
  CHECK(log[17] == 0);
  CHECK(log[18] == optlock);
  CHECK(read(f, target + 124) == 0xe1f);
  CHECK(read(f, bank2 + 124) == 0xe3f);
}

TEST_CASE(
    "checklist 20: a half page stored by a loop in bank 1 faults at the fetch after its first "
    "store")
{
  auto const f = parked(l152re(), firmware, handler_options());
  vectors_in_sram(f);
  unlock(f);
  write(f, pecr, fprg | prog);
  auto const buffer = load_buffer(f, 32, 0x300);
  call(
      f, "copy_words_flash",
      {static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(buffer), 32});
  auto const log = f.log();
  REQUIRE(log.size() == 2);
  CHECK(log[0] == fault_mark);
  CHECK((read(f, cfsr) & ibuserr) != 0);
  CHECK(flash_due(f).empty());
  CHECK(read(f, target + 4) == 0);
}

TEST_CASE("a FLASH snapshot keeps the bank lock of a loading and of a BSY half page")
{
  auto const f = parked(l152re(), firmware, handler_options());
  vectors_in_sram(f);
  unlock(f);
  write(f, pecr, fprg | prog);
  store_sequence(f, target, 16, 0xf00);
  auto const loading = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  f.target->restore(loading);
  call(f, "load_word", {static_cast<std::uint32_t>(target + 0x200)});
  auto const faulted = f.log();
  REQUIRE(faulted.size() >= 2);
  CHECK(faulted[faulted.size() - 2] == fault_mark);
  CHECK((read(f, cfsr) & preciserr) != 0);
  f.target->restore(loading);
  store_sequence(f, target + 64, 16, 0xf10);
  REQUIRE(read(f, sr) == (ready | bsy));
  auto const due  = flash_due(f);
  auto const busy = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  f.target->restore(busy);
  idle_to(f, due.front() + 1'000'000);
  REQUIRE(read(f, sr) == (ready | endhv | eop));
  call(f, "load_word", {static_cast<std::uint32_t>(target + 4)});
  CHECK(f.log().back() == 0xf01);
}

TEST_CASE("a vector table in the bank a half page writes fails the exception entry, inferred")
{
  auto const f = parked(l152re(), firmware, handler_options());
  unlock(f);
  write(f, pecr, fprg | prog);
  REQUIRE(memory_write(f, target, 4, 1));
  write(f, 0xE000'ED04, 1u << 31);
  auto const stopped = f.run_to_stop({.max_instructions = icount{1'000}});
  CHECK(stopped.reason == stop_reason::fault);
  CHECK((read(f, hfsr) & vecttbl) != 0);
  CHECK(f.log().empty());
}

TEST_CASE("checklist 20: the store after the first half page store in the same block does not run")
{
  auto const f = parked(l152re(), firmware, handler_options());
  vectors_in_sram(f);
  unlock(f);
  write(f, pecr, fprg | prog);
  call(f, "store_pair", {static_cast<std::uint32_t>(target), 0x700, 0x701});
  auto const log = f.log();
  REQUIRE(log.size() == 2);
  CHECK(log[0] == fault_mark);
  CHECK((read(f, cfsr) & ibuserr) != 0);
  store_sequence(f, target + 4, 31, 0x801);
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, target) == 0x700);
  CHECK(read(f, target + 4) == 0x801);
}

namespace
{
constexpr address       options    = 0x1FF8'0000;
constexpr address       obr        = flash + 0x1C;
constexpr address       wrpr1      = flash + 0x20;
constexpr address       wrpr2      = flash + 0x80;
constexpr address       wrpr4      = flash + 0x88;
constexpr std::uint32_t optverr    = 1u << 11;
constexpr std::uint32_t optverrusr = 1u << 12;
constexpr std::uint32_t obl_launch = 1u << 18;
constexpr std::uint32_t virgin_obr = 0x00F8'00AA;

void unlock_data(stm32_fixture const& f)
{
  write(f, pekeyr, pe_key1);
  write(f, pekeyr, pe_key2);
}

void unlock_options(stm32_fixture const& f)
{
  unlock_data(f);
  write(f, optkeyr, opt_key1);
  write(f, optkeyr, opt_key2);
  REQUIRE((read(f, pecr) & locks) == prglock);
}

auto option_word(std::uint32_t low, std::uint32_t bits) -> std::uint32_t
{
  auto const mask = (std::uint32_t{1} << bits) - 1;
  auto const high = bits == 8 ? 0xFF00'0000u : 0u;
  return high | ((~low & mask) << 16) | (low & mask);
}

void end_operations(stm32_fixture const& f)
{
  while((read(f, sr) & bsy) != 0)
  {
    auto const due = flash_due(f);
    REQUIRE(due.size() == 1);
    idle_to(f, std::max(due.front(), now_ps(f)) + 1'000'000);
  }
}

void program_option(stm32_fixture const& f, std::uint32_t offset, std::uint32_t value)
{
  REQUIRE(memory_write(f, options + offset, 4, value));
  end_operations(f);
  write(f, sr, eop);
}

void reload(stm32_fixture const& f)
{
  f.target->reset(reset_kind::option_load);
  copy_ram_code(f);
  f.set("pc", sym(f.firmware_name, "idle"));
}
}

TEST_CASE("RM0038 Table 15: data EEPROM byte, halfword and word writes take one or two tprog")
{
  auto const f = parked();
  unlock_data(f);
  auto const timed = [&f](address at, std::uint8_t size, std::uint32_t value) {
    auto const start = now_ps(f);
    REQUIRE(memory_write(f, at, size, value));
    auto const due = flash_due(f);
    REQUIRE(due.size() == 1);
    end_operations(f);
    write(f, sr, eop);
    return (due.front() - start + tprog_ps / 2) / tprog_ps;
  };
  CHECK(read(f, eeprom) == 0);
  CHECK(timed(eeprom, 4, 0x1122'3344) == 1);
  CHECK(read(f, eeprom) == 0x1122'3344);
  CHECK(timed(eeprom, 4, 0x5566'7788) == 2);
  CHECK(timed(eeprom, 4, 0) == 1);
  CHECK(read(f, eeprom) == 0);
  CHECK(timed(eeprom + 5, 1, 0xAB) == 1);
  CHECK(timed(eeprom + 6, 2, 0xCDEF) == 2);
  CHECK(read(f, eeprom + 4) == 0xCDEF'AB00);
  write(f, pecr, ftdw);
  CHECK(timed(eeprom + 8, 4, 1) == 2);
  CHECK(read(f, sr) == (ready | endhv));
}

TEST_CASE("RM0038 3.4.2 and 3.4.3: a data EEPROM double word erases and programs in one tprog")
{
  auto const f = parked();
  unlock_data(f);
  REQUIRE(memory_write(f, eeprom + 8, 4, 7));
  REQUIRE(memory_write(f, eeprom + 12, 4, 9));
  end_operations(f);
  write(f, sr, eop);
  write(f, pecr, erase | data);
  REQUIRE(memory_write(f, eeprom + 8, 4, 0));
  CHECK(flash_due(f).empty());
  REQUIRE(memory_write(f, eeprom + 12, 4, 0));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, eeprom + 8) == 0);
  CHECK(read(f, eeprom + 12) == 0);
  end_operations(f);
  write(f, sr, eop);
  write(f, pecr, fprg | data);
  REQUIRE(memory_write(f, eeprom + 16, 4, 0x10));
  REQUIRE(memory_write(f, eeprom + 20, 4, 0x20));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, eeprom + 20) == 0x20);
  end_operations(f);
  write(f, sr, eop);
  REQUIRE(memory_write(f, eeprom + 28, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  REQUIRE(memory_write(f, eeprom + 24, 4, 1));
  REQUIRE(memory_write(f, eeprom + 32, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | pgaerr));
  write(f, sr, pgaerr);
  REQUIRE(memory_write(f, eeprom + 24, 2, 1));
  CHECK(read(f, sr) == (ready | endhv | sizerr));
  CHECK(read(f, eeprom + 24) == 0);
  CHECK(flash_due(f).empty());
}

TEST_CASE("RM0038 3.7.3: a data EEPROM store while PELOCK is set sets WRPERR, PRGLOCK does not")
{
  auto const f = parked();
  REQUIRE(memory_write(f, eeprom, 4, 5));
  CHECK(read(f, sr) == (ready | endhv | wrperr));
  CHECK(read(f, eeprom) == 0);
  write(f, sr, wrperr);
  unlock_data(f);
  REQUIRE((read(f, pecr) & prglock) != 0);
  REQUIRE(memory_write(f, eeprom, 4, 5));
  CHECK(read(f, eeprom) == 5);
}

TEST_CASE("RM0038 Table 20 note 7: on Cat.1 a byte or halfword store of 0 programs nothing")
{
  {
    auto const  f = parked(l152rb(), firmware_b);
    log_capture logged{f};
    unlock_data(f);
    REQUIRE(memory_write(f, eeprom, 4, 0x0102'0304));
    end_operations(f);
    REQUIRE(memory_write(f, eeprom + 1, 1, 0));
    REQUIRE(memory_write(f, eeprom + 2, 2, 0));
    CHECK(read(f, eeprom) == 0x0102'0304);
    CHECK(logged.count("PECR") == 1);
    CHECK(flash_due(f).empty());
  }
  {
    auto const f = parked();
    unlock_data(f);
    REQUIRE(memory_write(f, eeprom, 4, 0x0102'0304));
    end_operations(f);
    REQUIRE(memory_write(f, eeprom + 1, 1, 0));
    CHECK(read(f, eeprom) == 0x0102'0004);
  }
  auto const f = parked(l152rb(), firmware_b, strict());
  unlock_data(f);
  CHECK_THROWS_AS((void)memory_write(f, eeprom, 1, 0), popcorn::model_error);
}

TEST_CASE("HAL_FLASHEx_DATAEEPROM_Program writes a byte, a halfword and a word")
{
  stm32_fixture const f{l152re(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  write(f, sr, wrperr);
  call(f, "eeprom_hal", {0, static_cast<std::uint32_t>(eeprom + 1), 0x5A});
  call(f, "eeprom_hal", {1, static_cast<std::uint32_t>(eeprom + 6), 0xBEEF});
  call(f, "eeprom_hal", {2, static_cast<std::uint32_t>(eeprom + 8), 0x1234'5678});
  auto const log = f.log();
  REQUIRE(log.size() == 23);
  CHECK(
      std::vector(log.begin() + 14, log.end())
      == std::vector<std::uint32_t>{0, 0, ready | endhv, 1, 0, ready | endhv, 1, 0, ready | endhv});
  CHECK(read(f, eeprom) == 0x5A00);
  CHECK(read(f, eeprom + 4) == 0xBEEF'0000);
  CHECK(read(f, eeprom + 8) == 0x1234'5678);
}

TEST_CASE("RM0038 3.9.8 and 3.9.9: a virgin part loads OBR 0x00F800AA and WRPRx 0, no OPTVERR")
{
  for(auto const& [desc, name] : {
          std::pair{&l152re(),                   firmware  },
          std::pair{&l152rb(),                   firmware_b},
          std::pair{&popcorn::testing::l152rc(), firmware  }
  })
  {
    CAPTURE(desc->part);
    auto const f = parked(*desc, name);
    CHECK(read(f, obr) == virgin_obr);
    CHECK(read(f, wrpr1) == 0);
    CHECK(read(f, sr) == (ready | endhv));
  }
}

TEST_CASE("RM0038 3.5.1 and 3.7.2: a WRP word loads at OBL_LAUNCH and protects its sectors")
{
  auto const f = parked();
  unlock(f);
  write(f, optkeyr, opt_key1);
  write(f, optkeyr, opt_key2);
  REQUIRE((read(f, pecr) & locks) == 0);
  auto const start = now_ps(f);
  REQUIRE(memory_write(f, options + 0x08, 4, option_word(0x0002, 16)));
  REQUIRE(flash_due(f).size() == 1);
  CHECK(flash_due(f).front() - start == 2 * tprog_ps);
  REQUIRE(memory_write(f, options + 0x10, 4, option_word(0x0001, 16)));
  end_operations(f);
  CHECK(read(f, wrpr1) == 0);
  write(f, pecr, obl_launch);
  auto const stopped = f.run_to_stop({.max_instructions = icount{1'000}});
  REQUIRE(stopped.reason == stop_reason::reset_request);
  CHECK(stopped.detail == "option_load");
  REQUIRE(f.target->apply_reset_request());
  CHECK(read(f, wrpr1) == 2);
  CHECK(read(f, wrpr2) == 1);
  CHECK(read(f, pecr) == locks);
  unlock(f);
  REQUIRE(memory_write(f, 0x0800'1000, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | wrperr));
  CHECK(read(f, 0x0800'1000) != 1);
  write(f, sr, wrperr);
  REQUIRE(memory_write(f, 0x0802'0000, 4, 1));
  CHECK(read(f, sr) == (ready | endhv | wrperr));
  write(f, sr, wrperr);
  REQUIRE(memory_write(f, 0x0800'2000, 4, 1));
  CHECK(read(f, 0x0800'2000) == 1);
}

TEST_CASE("RM0038 3.5: OPTLOCK, factory information, byte stores and the Cat.1 bank 2 offsets")
{
  {
    auto const f = parked();
    unlock_data(f);
    REQUIRE(memory_write(f, options + 0x08, 4, option_word(1, 16)));
    CHECK(read(f, sr) == (ready | endhv | wrperr));
    CHECK(read(f, options + 0x08) == 0xFFFF'0000);
    write(f, sr, wrperr);
    write(f, optkeyr, opt_key1);
    write(f, optkeyr, opt_key2);
    REQUIRE(memory_write(f, options + 0x20, 4, 1));
    CHECK(read(f, sr) == (ready | endhv | wrperr));
    write(f, sr, wrperr);
    REQUIRE(memory_write(f, options + 0x08, 2, 1));
    CHECK(read(f, sr) == (ready | endhv | sizerr));
    write(f, sr, sizerr);
    program_option(f, 0x80, option_word(4, 16));
    program_option(f, 0x84, option_word(8, 16));
    reload(f);
    CHECK(read(f, wrpr4) == 0x0008'0004);
  }
  auto const f = parked(l152rb(), firmware_b);
  unlock_options(f);
  auto const factory = read(f, options + 0x80);
  REQUIRE(memory_write(f, options + 0x80, 4, option_word(4, 16)));
  CHECK(read(f, sr) == (ready | endhv | wrperr));
  CHECK(read(f, options + 0x80) == factory);
}

TEST_CASE("RM0038 3.5: a word whose complement is wrong loads 0 and sets OPTVERR, OPTVERRUSR")
{
  for(auto const& [desc, name, user_flag] : {
          std::tuple{&l152re(), firmware,   true },
          std::tuple{&l152rb(), firmware_b, false}
  })
  {
    CAPTURE(desc->part);
    auto const f = parked(*desc, name);
    unlock_options(f);
    program_option(f, 0x00, 0xFF00'00BBu);
    program_option(f, 0x08, 0x0000'0001u);
    reload(f);
    CHECK(read(f, obr) == 0x00F8'0000u);
    CHECK(read(f, wrpr1) == 0);
    CHECK(read(f, sr) == (ready | endhv | optverr));
    write(f, sr, optverr);
    unlock_options(f);
    program_option(f, 0x00, option_word(0xAA, 8));
    program_option(f, 0x04, 0xFF00'00F8u);
    reload(f);
    CHECK(read(f, obr) == 0xAAu);
    CHECK(read(f, sr) == (ready | endhv | optverr | (user_flag ? optverrusr : 0u)));
  }
}

TEST_CASE("RM0038 3.7.1: RDP level 2 refuses option stores, level 1 to 0 mass erases first")
{
  {
    auto const f = parked();
    unlock_options(f);
    program_option(f, 0x00, option_word(0xCC, 8));
    reload(f);
    CHECK((read(f, obr) & 0xFFu) == 0xCC);
    unlock_options(f);
    REQUIRE(memory_write(f, options + 0x08, 4, option_word(1, 16)));
    CHECK(read(f, sr) == (ready | endhv | wrperr));
    CHECK(read(f, options + 0x08) == 0xFFFF'0000);
  }
  auto const f = parked();
  unlock_options(f);
  write(f, prgkeyr, prg_key1);
  write(f, prgkeyr, prg_key2);
  REQUIRE(memory_write(f, target, 4, 0x1111));
  REQUIRE(memory_write(f, eeprom + 16, 4, 0x2222));
  end_operations(f);
  write(f, sr, eop);
  program_option(f, 0x08, option_word(3, 16));
  program_option(f, 0x00, option_word(0xBB, 8));
  auto&      rtc  = f.target->peripheral("RTC");
  auto const bkp0 = popcorn::find_register(rtc.description(), "BKP0R")->offset;
  rtc.poke(bkp0, 0x3333);
  reload(f);
  REQUIRE((read(f, obr) & 0xFFu) == 0xBB);
  unlock_options(f);
  auto const start = now_ps(f);
  REQUIRE(memory_write(f, options, 4, option_word(0xAA, 8)));
  CHECK(flash_due(f).front() - start == 3 * tprog_ps);
  CHECK(read(f, target) == 0);
  CHECK(read(f, eeprom + 16) == 0);
  CHECK(read(f, options + 0x08) == 0);
  CHECK(read(f, options) == option_word(0xAA, 8));
  CHECK(rtc.peek(bkp0) == 0);
  CHECK(f.field("PWR", "CR", "DBP") == 0);
  CHECK(read(f, rtc.description().base + bkp0) == 0);
  end_operations(f);
  reload(f);
  CHECK(read(f, obr) == 0xAAu);
  CHECK(read(f, sr) == (ready | endhv | optverr | optverrusr));
}

TEST_CASE("R-08a check m4: the HAL_DeInit AHB pulse and an FLITFRST pulse keep OBR and WRPRx")
{
  auto const f = parked();
  unlock_options(f);
  program_option(f, 0x00, option_word(0xBB, 8));
  program_option(f, 0x08, option_word(0x40, 16));
  reload(f);
  REQUIRE(read(f, obr) == 0x00F8'00BBu);
  REQUIRE(read(f, wrpr1) == 0x40);
  auto const& rcc     = f.target->peripheral("RCC").description();
  auto const  ahbrstr = rcc.base + popcorn::find_register(rcc, "AHBRSTR")->offset;
  write(f, ahbrstr, 0xFFFF'FFFFu);
  write(f, ahbrstr, 0);
  CHECK(read(f, obr) == 0x00F8'00BBu);
  CHECK(read(f, wrpr1) == 0x40);
  write(f, pdkeyr, pd_key1);
  write(f, pdkeyr, pd_key2);
  write(f, acr, acc64 | run_pd);
  popcorn::testing::rcc_reset_pulse(f, "AHBRSTR", "FLITFRST");
  CHECK(read(f, acr) == 0);
  CHECK(read(f, obr) == 0x00F8'00BBu);
  CHECK(read(f, wrpr1) == 0x40);
  write(f, obr, 0);
  write(f, wrpr1, 0);
  CHECK(read(f, obr) == 0x00F8'00BBu);
  CHECK(read(f, wrpr1) == 0x40);
  f.target->reset(reset_kind::software);
  CHECK(read(f, wrpr1) == 0x40);
}

TEST_CASE("a FLASH snapshot keeps PECR unlocked, a double word half loaded and the loaded OBR")
{
  auto const f = parked();
  unlock_options(f);
  program_option(f, 0x08, option_word(0x10, 16));
  reload(f);
  unlock_data(f);
  write(f, pecr, fprg | data);
  REQUIRE(memory_write(f, eeprom + 32, 4, 0xAA));
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  f.target->restore(saved);
  CHECK(read(f, pecr) == (prglock | optlock | fprg | data));
  CHECK(read(f, wrpr1) == 0x10);
  CHECK(read(f, options + 0x08) == option_word(0x10, 16));
  REQUIRE(memory_write(f, eeprom + 36, 4, 0xBB));
  CHECK(read(f, sr) == (ready | bsy));
  CHECK(read(f, eeprom + 32) == 0xAA);
  CHECK(read(f, eeprom + 36) == 0xBB);
}

TEST_CASE("RM0038 Table 15 note 3: the data EEPROM of the bank written reads as a bus error")
{
  constexpr address eeprom_bank2 = eeprom + 0x2000;
  auto const        f            = parked(l152re(), firmware, handler_options());
  vectors_in_sram(f);
  unlock(f);
  write(f, pecr, fprg | prog);
  REQUIRE(memory_write(f, target, 4, 1));
  call(f, "load_word", {static_cast<std::uint32_t>(eeprom_bank2)});
  CHECK(read(f, cfsr) == 0);
  call(f, "load_word", {static_cast<std::uint32_t>(eeprom + 4)});
  CHECK((read(f, cfsr) & preciserr) != 0);
  CHECK(read(f, bfar) == eeprom + 4);
  auto const g = parked(l152re(), firmware, handler_options());
  vectors_in_sram(g);
  unlock_data(g);
  write(g, pecr, erase | data);
  REQUIRE(memory_write(g, eeprom_bank2 + 8, 4, 0));
  call(g, "load_word", {static_cast<std::uint32_t>(eeprom + 4)});
  CHECK(read(g, cfsr) == 0);
  call(g, "load_word", {static_cast<std::uint32_t>(0x0805'0000)});
  CHECK((read(g, cfsr) & preciserr) != 0);
  CHECK(read(g, bfar) == 0x0805'0000);
}

TEST_CASE("RM0038 3.7.3: a data EEPROM store to the other bank of a locked operation is WRPERR")
{
  constexpr address eeprom_bank2 = eeprom + 0x2000;
  {
    auto const f = parked();
    unlock(f);
    write(f, pecr, fprg | prog);
    store_sequence(f, target, 32, 0x100);
    REQUIRE(read(f, sr) == (ready | bsy));
    auto const due = flash_due(f);
    REQUIRE(memory_write(f, eeprom_bank2, 4, 0x55));
    CHECK(read(f, sr) == (ready | bsy | wrperr));
    CHECK(read(f, eeprom_bank2) == 0);
    CHECK(flash_due(f) == due);
  }
  auto const f = parked();
  unlock_data(f);
  write(f, pecr, fprg | data);
  REQUIRE(memory_write(f, eeprom_bank2 + 8, 4, 0x11));
  REQUIRE(memory_write(f, eeprom_bank2 + 12, 4, 0x22));
  REQUIRE(read(f, sr) == (ready | bsy));
  auto const due = flash_due(f);
  REQUIRE(memory_write(f, eeprom + 8, 4, 0x33));
  CHECK(read(f, sr) == (ready | bsy | wrperr));
  CHECK(read(f, eeprom + 8) == 0);
  CHECK(flash_due(f) == due);
  end_operations(f);
  CHECK(read(f, eeprom_bank2 + 8) == 0x11);
  CHECK(read(f, eeprom_bank2 + 12) == 0x22);
}

TEST_CASE("RM0038 3.9.7: OPTVERR and OPTVERRUSR set again after a software reset and a pulse")
{
  auto const f = parked();
  unlock_options(f);
  program_option(f, 0x04, 0xFF00'00F8u);
  reload(f);
  REQUIRE(read(f, sr) == (ready | endhv | optverr | optverrusr));
  f.target->reset(reset_kind::software);
  copy_ram_code(f);
  f.set("pc", sym(f.firmware_name, "idle"));
  CHECK(read(f, sr) == (ready | endhv | optverr | optverrusr));
  write(f, sr, optverr | optverrusr);
  CHECK(read(f, sr) == (ready | endhv));
  write(f, pdkeyr, pd_key1);
  write(f, pdkeyr, pd_key2);
  write(f, acr, acc64 | run_pd);
  popcorn::testing::rcc_reset_pulse(f, "AHBRSTR", "FLITFRST");
  CHECK(read(f, sr) == (ready | endhv | optverr | optverrusr));
  write(f, sr, optverr | optverrusr);
  unlock_options(f);
  program_option(f, 0x04, option_word(0xF8, 8));
  reload(f);
  CHECK(read(f, sr) == (ready | endhv));
  f.target->reset(reset_kind::software);
  CHECK(read(f, sr) == (ready | endhv));
}

TEST_CASE("RM0038 3.8: a queued 2-tprog data EEPROM write sets EOP once, at its end")
{
  auto const f = parked();
  unlock_data(f);
  auto const start = now_ps(f);
  REQUIRE(memory_write(f, eeprom + 0x40, 4, 1));
  REQUIRE(memory_write(f, eeprom + 0x40, 4, 2));
  REQUIRE(flash_due(f) == std::vector<std::uint64_t>{start + tprog_ps});
  idle_to(f, start + tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | bsy | eop));
  auto const due = flash_due(f);
  REQUIRE(due.size() == 1);
  CHECK(due.front() >= start + 3 * tprog_ps);
  CHECK(due.front() < start + 3 * tprog_ps + 1'000'000);
  write(f, sr, eop);
  idle_to(f, start + 2 * tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | bsy));
  idle_to(f, due.front() + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
  CHECK(read(f, eeprom + 0x40) == 2);
}

TEST_CASE("a FLASH snapshot restores a 1-tprog and a 2-tprog write queued, EOP at 3 tprog")
{
  auto const f = parked();
  unlock_data(f);
  auto const start = now_ps(f);
  REQUIRE(memory_write(f, eeprom + 0x40, 4, 1));
  REQUIRE(memory_write(f, eeprom + 0x40, 4, 2));
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  f.target->restore(saved);
  copy_ram_code(f);
  f.set("pc", sym(f.firmware_name, "idle"));
  idle_to(f, start + tprog_ps + 1'000'000);
  CHECK(read(f, sr) == (ready | bsy | eop));
  auto const due = flash_due(f);
  REQUIRE(due.size() == 1);
  CHECK(due.front() >= start + 3 * tprog_ps);
  CHECK(due.front() < start + 3 * tprog_ps + 1'000'000);
  write(f, sr, eop);
  idle_to(f, due.front() + 1'000'000);
  CHECK(read(f, sr) == (ready | endhv | eop));
}

TEST_CASE("a FLASH snapshot keeps the OPTVERR verdict of a bad USER load across a software reset")
{
  auto const f = parked();
  unlock_options(f);
  program_option(f, 0x04, 0xFF00'00F8u);
  reload(f);
  REQUIRE(read(f, sr) == (ready | endhv | optverr | optverrusr));
  auto const saved = f.target->snapshot();
  unlock_options(f);
  program_option(f, 0x04, option_word(0xF8, 8));
  reload(f);
  REQUIRE(read(f, sr) == (ready | endhv));
  f.target->restore(saved);
  f.target->reset(reset_kind::software);
  CHECK(read(f, sr) == (ready | endhv | optverr | optverrusr));
}

namespace
{
namespace stm32 = popcorn::periph::stm32;

auto refusal_key(part_description const& desc, stm32::option_bytes const& wanted) -> std::string
{
  auto const result = stm32::check_option_bytes(desc, wanted);
  REQUIRE_FALSE(result.has_value());
  return result.error().key;
}

auto resized(std::uint32_t size) -> part_description
{
  auto part = cat46_variant();
  auto region =
      std::ranges::find(part.regions, std::string_view{"Flash"}, &popcorn::region_desc::name);
  REQUIRE(region != part.regions.end());
  region->size = size;
  return part;
}
}

TEST_CASE("flash_unit on an STM32L1 part gives FLASH and its Flash regions, no data or options")
{
  for(auto const& [desc, regions] : {
          std::pair{l152re(),       std::vector<address>{0x0800'0000}             },
          std::pair{split_banks(2), std::vector<address>{0x0800'0000, 0x0804'0000}}
  })
  {
    CAPTURE(desc.part);
    auto const program = stm32::flash_unit(desc);
    REQUIRE(program.has_value());
    CHECK(program->unit->name == "FLASH");
    auto const starts = program->regions
                      | std::views::transform([](auto const* r) { return r->start; })
                      | std::ranges::to<std::vector>();
    CHECK(starts == regions);
  }
}

TEST_CASE(
    "RM0038 Table 19 and 2.7 Table 7: L1 option bytes boot from address 0, where the flash is "
    "aliased, swap_bank from bank 2")
{
  CHECK(stm32::check_option_bytes(l152re(), {}) == address{0});
  CHECK(stm32::check_option_bytes(l152re(), {.swap_bank = true}) == address{0x0804'0000});
  CHECK(stm32::check_option_bytes(resized(0x6'0000), {.swap_bank = true}) == address{0x0803'0000});
  CHECK(stm32::check_option_bytes(split_banks(2), {.swap_bank = true}) == address{0x0804'0000});
  CHECK(stm32::check_option_bytes(split_banks(2), {}) == address{0});
  CHECK(stm32::check_option_bytes(l152rb(), {}) == address{0});
  CHECK(refusal_key(popcorn::testing::l152rc(), {.swap_bank = true}) == "option_bytes.swap_bank");
  CHECK(refusal_key(l152rb(), {.swap_bank = true}) == "option_bytes.swap_bank");
  CHECK(refusal_key(l152re(), {.tzen = true}) == "option_bytes.tzen");
  CHECK(refusal_key(l152re(), {.secwm1 = stm32::no_area{}}) == "option_bytes.secwm1");
  CHECK(
      refusal_key(
          l152re(),
          {
              .secwm2 = stm32::page_range{.first = 0, .last = 1}
  })
      == "option_bytes.secwm2");
  CHECK(refusal_key(l152re(), {.hdp1 = stm32::page_range{}}) == "option_bytes.hdp1");
  CHECK(refusal_key(l152re(), {.hdp2 = stm32::page_range{}}) == "option_bytes.hdp2");
  CHECK(refusal_key(l152re(), {.secboot = 0x0800'0000}) == "option_bytes.secboot");
  CHECK(refusal_key(l152re(), {.nsboot = 0x0800'0000}) == "option_bytes.nsboot");
  CHECK(refusal_key(l152re(), {.dualbank = true}) == "option_bytes.dualbank");
  CHECK(refusal_key(l152re(), {.dualbank = false}) == "option_bytes.dualbank");
  auto       options = popcorn::testing::stm32_options();
  auto const refused = stm32::configure_option_bytes(l152re(), {.tzen = true}, options);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().key == "option_bytes.tzen");
  CHECK(options.models.empty());
}

TEST_CASE("the L1 FLASH factory refuses host option bytes its encoder refuses")
{
  stm32::register_builtin_models();
  auto const refused = [](part_description const& desc, stm32::option_bytes const& wanted) {
    stm32::board board;
    board.flash.option_bytes = wanted;
    auto options             = popcorn::testing::stm32_options();
    options.models           = stm32::factories(board, desc);
    try
    {
      core::machine target{desc, arch::armv8m::factory(), std::move(options)};
    }
    catch(popcorn::config_error const& error)
    {
      return std::string{error.key()};
    }
    return std::string{};
  };
  CHECK(refused(popcorn::testing::l152rc(), {.swap_bank = true}) == "option_bytes.swap_bank");
  CHECK(refused(l152re(), {.tzen = true}) == "option_bytes.tzen");
  CHECK(refused(l152re(), {.secwm1 = stm32::no_area{}}) == "option_bytes.secwm1");
  CHECK(refused(l152re(), {.swap_bank = true}).empty());
}

TEST_CASE("option_bytes on STM32L152RETx: the boot bank and FLASH_OBR nBFB2 at every load")
{
  constexpr std::uint32_t nbfb2 = 1u << 23;
  for(auto const swap : {false, true})
  {
    CAPTURE(swap);
    auto       options    = popcorn::testing::stm32_options();
    auto const configured = stm32::configure_option_bytes(l152re(), {.swap_bank = swap}, options);
    REQUIRE(configured.has_value());
    CHECK(options.vector_table == address{swap ? 0x0804'0000u : 0u});
    CHECK_FALSE(options.trustzone);
    stm32_fixture const f{l152re(), firmware, std::move(options)};
    auto const          handler = read(f, 0x0800'0004);
    REQUIRE(f.target->mem().write(0x0804'0000, 4, 0x2000'4000).has_value());
    REQUIRE(f.target->mem().write(0x0804'0004, 4, handler).has_value());
    f.target->reset(reset_kind::power_on);
    auto const reg = [&f](std::string_view name) {
      return static_cast<std::uint32_t>(f.target->regs().read(*f.target->regs().find(name)));
    };
    CHECK(reg("pc") == (handler & ~1u));
    CHECK(reg("sp") == (swap ? 0x2000'4000u : read(f, 0x0800'0000)));
    CHECK((read(f, obr) & nbfb2) == (swap ? 0u : nbfb2));
    f.target->reset(reset_kind::option_load);
    CHECK((read(f, obr) & nbfb2) == (swap ? 0u : nbfb2));
    f.target->reset(reset_kind::software);
    CHECK((read(f, obr) & nbfb2) == (swap ? 0u : nbfb2));
    copy_ram_code(f);
    f.set("pc", sym(f.firmware_name, "idle"));
    auto const user = swap ? 0xF8u : 0x78u;
    unlock_options(f);
    program_option(f, 0x04, option_word(user, 8));
    reload(f);
    CHECK(read(f, 0x1FF8'0004) == option_word(user, 8));
    CHECK((read(f, obr) & nbfb2) == (swap ? 0u : nbfb2));
    CHECK((read(f, sr) & optverr) == 0);
  }
  auto const plain = parked();
  CHECK((read(plain, obr) & nbfb2) == nbfb2);
  unlock_options(plain);
  program_option(plain, 0x04, option_word(0x78, 8));
  reload(plain);
  CHECK((read(plain, obr) & nbfb2) == 0);
}

namespace
{
constexpr auto          firmware_c = std::string_view{"l152xc_l1_flash_ll"};
constexpr std::uint32_t sprmod     = 1u << 8;
constexpr std::uint32_t rderr      = 1u << 13;
constexpr address       pcrop_read = 0x0801'4000;
constexpr address       pcrop_code = 0x0801'5000;
constexpr address       open_page  = 0x0801'3000;
constexpr address       flash_base = 0x0800'0000;
constexpr std::uint32_t pcrop_bits = (1u << 20) | (1u << 21);

void run_entry(
    stm32_fixture const&                 f,
    std::string_view                     entry,
    std::initializer_list<std::uint32_t> args)
{
  auto index = 0;
  for(auto const arg : args)
  {
    f.set(std::format("r{}", index++), arg);
  }
  f.set("pc", sym(f.firmware_name, entry));
  REQUIRE(f.run_to_stop({.max_instructions = icount{2'000'000}}).reason == stop_reason::user);
  f.set("pc", sym(f.firmware_name, "idle"));
}

auto pcrop_rc() -> stm32_fixture
{
  stm32_fixture f{popcorn::testing::l152rc(), firmware_c};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  REQUIRE(f.target->mem().write(pcrop_read, 4, 0x1234'5678).has_value());
  REQUIRE(f.target->mem().write(open_page, 4, 0x0BAD'F00D).has_value());
  REQUIRE(f.target->mem().write(pcrop_code, 4, 0xBE01'202Au).has_value());
  write(f, sr, wrperr | pgaerr | sizerr);
  run_entry(f, "pcrop_hal", {~pcrop_bits, pcrop_bits});
  reload(f);
  return f;
}

auto loaded_log(stm32_fixture const& f, std::size_t count) -> std::vector<std::uint32_t>
{
  auto const log = f.log();
  REQUIRE(log.size() >= count);
  return {log.end() - static_cast<std::ptrdiff_t>(count), log.end()};
}
}

TEST_CASE("RM0038 3.7.4: HAL_FLASHEx_AdvOBProgram sets PCROP on RCTx, AdvOBGetConfig reads it")
{
  auto const f   = pcrop_rc();
  auto const ran = f.log();
  REQUIRE(ran.size() >= 6);
  CHECK(std::vector(ran.end() - 6, ran.end() - 1) == std::vector<std::uint32_t>{0, 0, 0, 0, 0});
  CHECK(ran.back() == (ready | endhv));
  CHECK((read(f, obr) & sprmod) == sprmod);
  CHECK(read(f, wrpr1) == ~pcrop_bits);
  CHECK(read(f, wrpr2) == 0xFFFF'FFFFu);
  run_entry(f, "pcrop_get", {});
  CHECK(loaded_log(f, 3) == std::vector<std::uint32_t>{1, ~pcrop_bits, 0xFFFF'FFFFu});
}

TEST_CASE(
    "RM0038 3.7.4 and 2.7: a data read of a PCROP sector, at its address or through the alias at "
    "0, reads 0 and sets RDERR, a fetch runs")
{
  auto const f = pcrop_rc();
  CHECK((read(f, sr) & rderr) == 0);
  run_entry(f, "read_flash", {static_cast<std::uint32_t>(open_page)});
  CHECK(loaded_log(f, 1) == std::vector<std::uint32_t>{0x0BAD'F00D});
  CHECK((read(f, sr) & rderr) == 0);
  run_entry(f, "read_flash", {static_cast<std::uint32_t>(pcrop_read)});
  CHECK(loaded_log(f, 1) == std::vector<std::uint32_t>{0});
  CHECK((read(f, sr) & rderr) == rderr);
  write(f, sr, rderr);
  run_entry(f, "read_flash", {static_cast<std::uint32_t>(pcrop_read - flash_base)});
  CHECK(loaded_log(f, 1) == std::vector<std::uint32_t>{0});
  CHECK((read(f, sr) & rderr) == rderr);
  write(f, sr, rderr);
  CHECK((read(f, sr) & rderr) == 0);
  f.set("r0", 0);
  f.set("pc", pcrop_code | 1u);
  REQUIRE(f.run_to_stop({.max_instructions = icount{10}}).reason == stop_reason::user);
  CHECK(f.target->regs().read(*f.target->regs().find("r0")) == 42);
  CHECK((read(f, sr) & rderr) == 0);
  f.set("r0", 0);
  f.set("pc", (pcrop_code - flash_base) | 1u);
  REQUIRE(f.run_to_stop({.max_instructions = icount{10}}).reason == stop_reason::user);
  CHECK(f.target->regs().read(*f.target->regs().find("r0")) == 42);
  CHECK((read(f, sr) & rderr) == 0);
  unlock(f);
  REQUIRE(memory_write(f, pcrop_read + 4, 4, 1));
  CHECK((read(f, sr) & wrperr) == wrperr);
  write(f, sr, wrperr);
  REQUIRE(memory_write(f, open_page + 4, 4, 1));
  CHECK((read(f, sr) & wrperr) == 0);
  end_operations(f);
  CHECK(read(f, open_page + 4) == 1);
}

TEST_CASE(
    "RM0038 3.7.4 and 2.7: a DAP read of a PCROP sector through m.mem.secure or m.mem.nonsecure, "
    "at its address or through the alias at 0, reads 0 and sets RDERR, the unfiltered debugger "
    "view reads the data")
{
  auto const f    = pcrop_rc();
  auto const data = f.target->mem().read(pcrop_read, 4);
  REQUIRE(data.has_value());
  CHECK(*data != 0);
  CHECK((read(f, sr) & rderr) == 0);
  for(auto const secure : {true, false})
  {
    CAPTURE(secure);
    for(auto const at : {pcrop_read, pcrop_read - flash_base})
    {
      CAPTURE(at);
      CHECK(f.target->mem(secure).read(at, 4) == 0u);
      CHECK((read(f, sr) & rderr) == rderr);
      write(f, sr, rderr);
    }
    CHECK(f.target->mem(secure).read(open_page, 4) == f.target->mem().read(open_page, 4));
    CHECK(
        f.target->mem(secure).read(open_page - flash_base, 4)
        == f.target->mem().read(open_page, 4));
    CHECK((read(f, sr) & rderr) == 0);
  }
  CHECK(f.target->mem().read(pcrop_read - flash_base, 4) == data);
}

TEST_CASE(
    "RM0038 3.7.4: an option store keeps SPRMOD and PCROP sectors, an RDP 1 to 0 store "
    "with SPRMOD 0 clears them")
{
  auto const f = pcrop_rc();
  unlock_options(f);
  program_option(f, 0x00, option_word(0xAA, 8));
  CHECK(read(f, options) == 0xFE55'01AAu);
  program_option(f, 0x0C, option_word(0xFFFF, 16));
  CHECK(read(f, options + 0x0C) == 0x0030'FFCFu);
  program_option(f, 0x00, option_word(0xBB, 8));
  reload(f);
  CHECK(read(f, obr) == (0x00F8'00BBu | sprmod));
  CHECK(read(f, wrpr1) == ~pcrop_bits);
  CHECK((read(f, sr) & optverr) == 0);
  unlock_options(f);
  REQUIRE(memory_write(f, options, 4, option_word(0xAA, 8)));
  end_operations(f);
  reload(f);
  CHECK((read(f, obr) & (sprmod | 0xFFu)) == 0xAAu);
  CHECK(read(f, wrpr1) == 0);
  f.target->load(popcorn::testing::elf(f.firmware_name));
  f.target->reset(reset_kind::software);
  copy_ram_code(f);
  REQUIRE(f.target->mem().write(pcrop_read, 4, 0x1234'5678).has_value());
  run_entry(f, "read_flash", {static_cast<std::uint32_t>(pcrop_read)});
  CHECK(loaded_log(f, 1) == std::vector<std::uint32_t>{0x1234'5678});
  CHECK((read(f, sr) & rderr) == 0);
}

TEST_CASE(
    "popcorn's result: the RDP 1 to 0 word the HAL writes keeps SPRMOD 1, the erased WRPx words "
    "load 0 with OPTVERR")
{
  auto const f = pcrop_rc();
  unlock_options(f);
  program_option(f, 0x00, option_word(0xBB, 8));
  CHECK(read(f, options) == 0xFE44'01BBu);
  reload(f);
  REQUIRE((read(f, obr) & (sprmod | 0xFFu)) == (sprmod | 0xBBu));
  unlock_options(f);
  REQUIRE(memory_write(f, options, 4, 0xFE55'01AAu));
  end_operations(f);
  reload(f);
  CHECK((read(f, obr) & (sprmod | 0xFFu)) == (sprmod | 0xAAu));
  CHECK((read(f, sr) & optverr) == optverr);
  CHECK(read(f, wrpr1) == 0);
  CHECK(read(f, wrpr2) == 0);
}

TEST_CASE("RM0038 3.8: RDERR with ERRIE raises FLASH_IRQn, the handler runs after the load")
{
  constexpr address       code  = 0x2000'1000;
  constexpr std::uint32_t iser0 = 0xE000'E100;
  constexpr std::uint32_t pcr0  = 0xE000'E280;
  auto const              f     = pcrop_rc();
  unlock_data(f);
  write(f, pecr, read(f, pecr) | errie);
  write(f, pcr0, 1u << flash_irq);
  write(f, iser0, 1u << flash_irq);
  REQUIRE(f.target->mem().write(code, 2, 0x6802).has_value());
  REQUIRE(f.target->mem().write(code + 2, 2, 0x2301).has_value());
  f.set("r0", pcrop_read);
  f.set("r3", 0);
  f.set("pc", code | 1u);
  REQUIRE_FALSE(f.run({.max_instructions = icount{2}}).stopped.has_value());
  CHECK((read(f, sr) & rderr) == rderr);
  CHECK(ipsr(f) == 16 + flash_irq);
  CHECK(f.target->regs().read(*f.target->regs().find("r3")) == 0);
}

TEST_CASE("a FLASH snapshot taken with PCROP loaded restores the read protection")
{
  auto const    saved = pcrop_rc().target->snapshot();
  stm32_fixture other{popcorn::testing::l152rc(), firmware_c};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  REQUIRE((read(other, sr) & rderr) == 0);
  run_entry(other, "read_flash", {static_cast<std::uint32_t>(pcrop_read)});
  CHECK(loaded_log(other, 1) == std::vector<std::uint32_t>{0});
  CHECK((read(other, sr) & rderr) == rderr);
}

TEST_CASE("RM0038 3.7.4: RBTxA, Cat.2, loads SPRMOD and refuses a read of a PCROP sector")
{
  auto const f = parked(popcorn::testing::description_from("POPCORN_L152RBA_DESC"), firmware_b);
  REQUIRE(f.target->mem().write(pcrop_read, 4, 0x1234'5678).has_value());
  unlock_options(f);
  program_option(f, 0x08, option_word(0xFFFF, 16));
  program_option(f, 0x0C, option_word(0xFFEF, 16));
  program_option(f, 0x00, 0xFE55'01AAu);
  reload(f);
  CHECK(read(f, obr) == (virgin_obr | sprmod));
  CHECK(read(f, wrpr1) == 0xFFEF'FFFFu);
  call(f, "read_flash", {static_cast<std::uint32_t>(pcrop_read)});
  CHECK(f.log().back() == 0);
  CHECK((read(f, sr) & rderr) == rderr);
}

TEST_CASE("RCTxA has no SPRMOD and no RDERR, its WRPx bits are plain write protection")
{
  auto const desc  = popcorn::testing::description_from("POPCORN_L152RCA_DESC");
  auto const flash = std::ranges::find(
      desc.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
  REQUIRE(flash != desc.peripherals.end());
  CHECK(flash->ip_version == "STM32L1_flash_cat3");
  for(auto const& [reg, field] : {
          std::pair{"OBR", "SPRMOD"},
          std::pair{"SR",  "RDERR" }
  })
  {
    auto const found = popcorn::find_register(*flash, reg);
    REQUIRE(found != nullptr);
    CHECK(
        std::ranges::find(found->fields, std::string_view{field}, &popcorn::field_desc::name)
        == found->fields.end());
  }
  auto const f = parked(desc, firmware_c);
  REQUIRE(f.target->mem().write(pcrop_read, 4, 0x1234'5678).has_value());
  unlock_options(f);
  program_option(f, 0x0C, option_word(0x0010, 16));
  program_option(f, 0x00, 0xFE55'01AAu);
  reload(f);
  CHECK(read(f, obr) == virgin_obr);
  CHECK(read(f, wrpr1) == 0x0010'0000u);
  run_entry(f, "read_flash", {static_cast<std::uint32_t>(pcrop_read)});
  CHECK(loaded_log(f, 1) == std::vector<std::uint32_t>{0x1234'5678});
  CHECK((read(f, sr) & rderr) == 0);
  unlock(f);
  REQUIRE(memory_write(f, pcrop_read + 4, 4, 1));
  CHECK((read(f, sr) & wrperr) == wrperr);
  write(f, sr, wrperr);
  REQUIRE(memory_write(f, open_page, 4, 1));
  CHECK((read(f, sr) & wrperr) == 0);
}

namespace
{
constexpr address       dma1      = 0x4002'6000;
constexpr address       dma_buf   = 0x2000'2000;
constexpr std::uint32_t word_copy = (1u << 14) | (2u << 10) | (2u << 8) | (1u << 7) | (1u << 6);

auto dma_copy(stm32_fixture const& f, std::uint32_t ch, address from, std::uint32_t items)
    -> std::uint32_t
{
  auto const base = dma1 + 20 * (ch - 1);
  for(std::uint32_t index = 0; index < items; ++index)
  {
    write(f, dma_buf + 0x100 * ch + 4 * index, 0xDEAD'BEEFu);
  }
  write(f, base + 0x10, static_cast<std::uint32_t>(from));
  write(f, base + 0x14, static_cast<std::uint32_t>(dma_buf + 0x100 * ch));
  write(f, base + 0x0C, items);
  write(f, base + 0x08, word_copy | 1u);
  idle_to(f, now_ps(f) + 50'000'000);
  return read(f, dma1) >> (4 * (ch - 1)) & 0xFu;
}

auto dma_copied(stm32_fixture const& f, std::uint32_t ch, std::uint32_t index) -> std::uint32_t
{
  return read(f, dma_buf + 0x100 * ch + 4 * index);
}
}

TEST_CASE(
    "RM0038 3.4.3 note 2: during a half page a DMA read of its bank sets TEIF, "
    "of bank 2 it completes")
{
  auto const f = parked();
  REQUIRE(f.target->mem().write(target + 64, 4, 0x1111'2222u).has_value());
  REQUIRE(f.target->mem().write(bank2 + 8, 4, 0x3333'4444u).has_value());
  unlock(f);
  write(f, pecr, fprg | prog);
  REQUIRE(memory_write(f, target, 4, 1));
  CHECK(dma_copy(f, 1, target + 64, 1) == 0x9u);
  CHECK((read(f, dma1 + 0x08) & 1u) == 0);
  CHECK(read(f, dma1 + 0x0C) == 1);
  CHECK(dma_copied(f, 1, 0) == 0xDEAD'BEEFu);
  CHECK(dma_copy(f, 2, bank2 + 8, 1) == 0x7u);
  CHECK(dma_copied(f, 2, 0) == 0x3333'4444u);
}

TEST_CASE("bank_filter: a DMA grant from bank 1 stops at the bank boundary while bank 2 is locked")
{
  auto const f = parked();
  REQUIRE(f.target->mem().write(bank2 - 8, 4, 0x0101'0101u).has_value());
  REQUIRE(f.target->mem().write(bank2 - 4, 4, 0x0202'0202u).has_value());
  REQUIRE(f.target->mem().write(bank2, 4, 0x0303'0303u).has_value());
  unlock(f);
  write(f, pecr, fprg | prog);
  REQUIRE(memory_write(f, bank2 + 0x80, 4, 1));
  CHECK(dma_copy(f, 1, bank2 - 8, 3) == 0xDu);
  CHECK(read(f, dma1 + 0x0C) == 1);
  CHECK(dma_copied(f, 1, 0) == 0x0101'0101u);
  CHECK(dma_copied(f, 1, 1) == 0x0202'0202u);
  CHECK(dma_copied(f, 1, 2) == 0xDEAD'BEEFu);
}

TEST_CASE(
    "RM0038 3.7.4 and 2.7: a DMA read entering a PCROP sector, at its address or through the "
    "alias at 0, reads 0 and sets RDERR, inferred")
{
  for(auto const shift : {address{0}, flash_base})
  {
    CAPTURE(shift);
    auto const f = pcrop_rc();
    REQUIRE(f.target->mem().write(pcrop_read - 8, 4, 0x0505'0505u).has_value());
    REQUIRE(f.target->mem().write(pcrop_read - 4, 4, 0x0606'0606u).has_value());
    REQUIRE((read(f, sr) & rderr) == 0);
    CHECK(dma_copy(f, 1, pcrop_read - 8 - shift, 3) == 0x7u);
    CHECK(dma_copied(f, 1, 0) == 0x0505'0505u);
    CHECK(dma_copied(f, 1, 1) == 0x0606'0606u);
    CHECK(dma_copied(f, 1, 2) == 0);
    CHECK((read(f, sr) & rderr) == rderr);
  }
}

TEST_CASE(
    "RM0038 2.7: a DMA store through the alias at 0 is a transfer error and leaves the flash, "
    "inferred, the same store at 0x08000100 reaches FLASH, which sets WRPERR while locked")
{
  constexpr std::uint32_t teif = 1u << 3;
  constexpr std::uint32_t dir  = 1u << 4;
  for(auto const at : {address{0x100}, flash_base + 0x100})
  {
    CAPTURE(at);
    auto const f      = pcrop_rc();
    auto const before = read(f, flash_base + 0x100);
    write(f, dma_buf + 0x100, 0x1234'5678u);
    write(f, dma1 + 0x10, static_cast<std::uint32_t>(at));
    write(f, dma1 + 0x14, static_cast<std::uint32_t>(dma_buf + 0x100));
    write(f, dma1 + 0x0C, 1);
    write(f, dma1 + 0x08, word_copy | dir | 1u);
    idle_to(f, now_ps(f) + 50'000'000);
    CHECK((read(f, dma1) & teif) == (at == 0x100 ? teif : 0u));
    CHECK((read(f, sr) & wrperr) == (at == 0x100 ? 0u : wrperr));
    CHECK(read(f, flash_base + 0x100) == before);
  }
}

TEST_CASE("RM0038 3.4.3 note 2: a half page started mid-copy fails the next DMA read of its bank")
{
  constexpr std::uint64_t dma_latency_ps = 4'000'000'000'000 / 2'097'152;
  auto const              f              = parked();
  for(std::uint32_t index = 0; index < 8; ++index)
  {
    REQUIRE(f.target->mem().write(target + 64 + 4 * index, 4, 0x1000u + index).has_value());
    write(f, dma_buf + 0x100 + 4 * index, 0xDEAD'BEEFu);
  }
  unlock(f);
  write(f, pecr, fprg | prog);
  auto const base = dma1 + 0x08;
  write(f, base + 0x08, static_cast<std::uint32_t>(target + 64));
  write(f, base + 0x0C, static_cast<std::uint32_t>(dma_buf + 0x100));
  write(f, base + 0x04, 8);
  auto const t0 = now_ps(f);
  write(f, base, word_copy | 1u);
  idle_to(f, t0 + 2 * dma_latency_ps + dma_latency_ps / 2);
  REQUIRE(read(f, base + 0x04) == 6);
  REQUIRE(memory_write(f, target, 4, 1));
  idle_to(f, now_ps(f) + 50'000'000);
  CHECK((read(f, dma1) & 0xFu) == 0x9u);
  CHECK((read(f, base) & 1u) == 0);
  CHECK(read(f, base + 0x04) == 6);
  CHECK(dma_copied(f, 1, 0) == 0x1000u);
  CHECK(dma_copied(f, 1, 1) == 0x1001u);
  CHECK(dma_copied(f, 1, 2) == 0xDEAD'BEEFu);
}

TEST_CASE(
    "RM0038 3.4.3: every CPU store to the non-executable option region reaches the FLASH model, "
    "which sets WRPERR for each with PELOCK set")
{
  stm32_fixture const f{l152re(), firmware};
  REQUIRE((read(f, pecr) & pelock) != 0);
  constexpr address code = 0x2000'8000;
  write(f, code, 0xbe01'6001u);
  for(auto const at :
      {address{options + 0x100}, address{options + 0x104}, address{options + 0x108}})
  {
    CAPTURE(at);
    auto const before = read(f, at);
    f.set("r0", at);
    f.set("r1", 0x33);
    f.set("pc", code | 1u);
    REQUIRE(f.run_to_stop({.max_instructions = icount{4}}).reason == stop_reason::user);
    CHECK((read(f, sr) & wrperr) == wrperr);
    CHECK(read(f, at) == before);
    write(f, sr, wrperr);
  }
}
