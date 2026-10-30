#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <initializer_list>
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

auto parked(
    part_description const& desc    = l152re(),
    std::string_view        name    = firmware,
    core::machine_options   options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{desc, name, std::move(options)};
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
  f.set("pc", sym(firmware, "wrong_key"));
  REQUIRE(f.run_to_stop({.max_instructions = icount{10'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0xfa17u, locks});
  CHECK_FALSE(try_write(f, pekeyr, pe_key1));
  CHECK_FALSE(try_write(f, pekeyr, pe_key2));
  CHECK((read(f, pecr) & pelock) != 0);
  f.target->reset();
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
  auto const f = parked(l152rb(), firmware_b);
  unlock(f);
  auto const size = [&f] {
    auto const saved = f.target->snapshot();
    auto const found = std::ranges::find(
        saved.data().models, std::string_view{"FLASH"}, &core::snapshot_part::owner);
    REQUIRE(found != saved.data().models.end());
    return found->data.size();
  };
  auto const before = size();
  call(f, "store_loop", {static_cast<std::uint32_t>(target), 1000});
  auto const start = now_ps(f);
  CHECK(size() == before);
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
    "a store with ERASE or FPRG set warns once and programs nothing, model_error under strict")
{
  {
    auto const  f = parked();
    log_capture logged{f};
    unlock(f);
    auto const old = read(f, target);
    write(f, pecr, erase | prog);
    REQUIRE(memory_write(f, target, 4, 0));
    REQUIRE(memory_write(f, target, 4, 0));
    CHECK(logged.count("PECR") == 1);
    CHECK(read(f, target) == old);
    CHECK((read(f, sr) & bsy) == 0);
  }
  auto const f = parked(l152re(), firmware, strict());
  unlock(f);
  write(f, pecr, fprg | prog);
  CHECK_THROWS_AS((void)memory_write(f, target, 4, 1), popcorn::model_error);
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

TEST_CASE("a store to the data EEPROM or the option bytes is a bus error")
{
  auto const f = parked();
  unlock(f);
  CHECK_FALSE(memory_write(f, eeprom, 4, 1));
  CHECK_FALSE(memory_write(f, 0x1FF8'0000, 4, 1));
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
