#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;

constexpr auto          firmware    = std::string_view{"l476_l4_smoke"};
constexpr address       stub        = 0x2000'8000;
constexpr address       spin        = 0x2000'8010;
constexpr address       bank1_last  = 0x0801'f800;
constexpr address       bank2       = 0x0802'0000;
constexpr address       bank2_last  = 0x0803'f800;
constexpr std::uint32_t flash_irq   = 4;
constexpr std::uint32_t pg          = 1u << 0;
constexpr std::uint32_t per         = 1u << 1;
constexpr std::uint32_t mer1        = 1u << 2;
constexpr std::uint32_t bker        = 1u << 11;
constexpr std::uint32_t mer2        = 1u << 15;
constexpr std::uint32_t strt        = 1u << 16;
constexpr std::uint32_t eopie       = 1u << 24;
constexpr std::uint32_t errie       = 1u << 25;
constexpr std::uint32_t optlock_bit = 1u << 30;
constexpr std::uint32_t lock_bit    = 1u << 31;
constexpr std::uint32_t all_errors  = 0xc3fb;
constexpr std::uint32_t key1        = 0x4567'0123;
constexpr std::uint32_t key2        = 0xcdef'89ab;
constexpr std::uint32_t option_key1 = 0x0819'2a3b;
constexpr std::uint32_t option_key2 = 0x4c5d'6e7f;

auto pnb(std::uint32_t page) -> std::uint32_t
{
  return page << 3;
}

auto erased(std::size_t count) -> std::vector<std::uint32_t>
{
  return std::vector<std::uint32_t>(count, 0xffff'ffffu);
}

auto strict_options() -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  return options;
}

struct rig
{
  stm32_fixture                 f;
  std::vector<core::log_record> logged{};
  core::hook_handle             sink{};

  explicit rig(core::machine_options options = popcorn::testing::stm32_options(), bool open = true)
      : f{l476rc(), firmware, std::move(options)}
  {
    sink = f.target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(f.target->mem().write(spin, 2, 0xe7feu).has_value());
    if(open)
    {
      unlock();
    }
  }

  [[nodiscard]] auto at(std::string_view reg) const -> address
  {
    auto const& desc  = f.target->peripheral("FLASH").description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(reg), 4, value).has_value());
  }

  [[nodiscard]] auto try_write(std::string_view reg, std::uint32_t value) const -> bool
  {
    auto&      regs = f.target->peripheral("FLASH");
    auto const word = popcorn::find_register(regs.description(), reg);
    REQUIRE(word != nullptr);
    return f.target->model("FLASH")
        ->on_write(
            regs, word->offset, 4, regs.peek(word->offset), value, {.pc = 0, .now = icount{0}})
        .has_value();
  }

  [[nodiscard]] auto word(address from) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(from, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
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

  [[nodiscard]] auto status(std::string_view name) const -> std::uint32_t
  {
    return f.field("FLASH", "SR", name);
  }

  [[nodiscard]] auto control_bit(std::string_view name) const -> std::uint32_t
  {
    return f.field("FLASH", "CR", name);
  }

  void unlock() const
  {
    write("KEYR", key1);
    write("KEYR", key2);
    REQUIRE(control_bit("LOCK") == 0);
  }

  void control(std::uint32_t value) const
  {
    write("CR", value);
  }

  void clear_errors() const
  {
    write("SR", all_errors | 1u);
  }

  void store(address to, std::uint32_t value, std::uint8_t size = 4) const
  {
    auto const opcode = size == 4 ? 0x6001u : size == 2 ? 0x8001u : 0x7001u;
    REQUIRE(f.target->mem().write(stub, 2, opcode).has_value());
    REQUIRE(f.target->mem().write(stub + 2, 2, 0xbe01u).has_value());
    f.set("r0", to);
    f.set("r1", value);
    f.set("pc", stub | 1u);
    REQUIRE(f.run_to_stop({.max_instructions = icount{10}}).reason == stop_reason::user);
  }

  void store_words(address to, std::vector<std::uint32_t> const& values) const
  {
    for(std::size_t i = 0; i < values.size(); ++i)
    {
      store(to + 4 * i, values[i]);
    }
  }

  void wait_idle() const
  {
    f.set("pc", spin | 1u);
    auto const stopped = f.run_to_stop({.max_instructions = icount{1'000'000}, .stop_when = [this] {
                                          return status("BSY") == 0;
                                        }});
    REQUIRE(stopped.reason == stop_reason::user);
  }

  void program(address to, std::vector<std::uint32_t> const& values) const
  {
    control(pg);
    store_words(to, values);
    wait_idle();
  }

  [[nodiscard]] auto pending() const -> bool
  {
    return (word(0xe000'e200u) >> flash_irq & 1u) != 0;
  }

  [[nodiscard]] auto elapsed_ps(auto&& action) const -> std::uint64_t
  {
    auto const start = f.target->virtual_clock().time().ps;
    action();
    return f.target->virtual_clock().time().ps - start;
  }
};
}

TEST_CASE("RM0351 3.3.5: KEY1 then KEY2 clears LOCK, setting LOCK sets OPTLOCK too")
{
  rig r{popcorn::testing::stm32_options(), false};
  CHECK(r.control_bit("LOCK") == 1);
  CHECK(r.control_bit("OPTLOCK") == 1);
  r.write("KEYR", key1);
  CHECK(r.control_bit("LOCK") == 1);
  CHECK(r.word(r.at("KEYR")) == 0);
  r.write("KEYR", key2);
  CHECK(r.control_bit("LOCK") == 0);
  r.control(0);
  CHECK(r.control_bit("OPTLOCK") == 1);
  r.control(lock_bit);
  CHECK(r.control_bit("LOCK") == 1);
  r.control(0);
  CHECK(r.control_bit("LOCK") == 1);
}

TEST_CASE("RM0351 3.3.5: a wrong key is a bus error and keeps FLASH_CR locked until reset")
{
  rig r{popcorn::testing::stm32_options(), false};
  REQUIRE(r.try_write("KEYR", key1));
  CHECK_FALSE(r.try_write("KEYR", 0x1234'5678u));
  CHECK_FALSE(r.try_write("KEYR", key1));
  CHECK_FALSE(r.try_write("KEYR", key2));
  CHECK(r.control_bit("LOCK") == 1);
  r.control(pg);
  CHECK(r.control_bit("PG") == 0);
  r.f.target->reset(popcorn::reset_kind::software);
  r.unlock();
}

TEST_CASE("RM0351 3.3.7: the second word of a double word launches it for tprog, DS10198 Table 63")
{
  rig r;
  r.control(pg);
  r.store(bank1_last, 0x11);
  CHECK(r.status("BSY") == 0);
  CHECK(r.words(bank1_last, 2) == erased(2));
  auto const took = r.elapsed_ps([&] {
    r.store(bank1_last + 4, 0x22);
    CHECK(r.status("BSY") == 1);
    CHECK(r.words(bank1_last, 2) == erased(2));
    r.wait_idle();
  });
  CHECK(took >= 81'690'000u);
  CHECK(took < 82'500'000u);
  CHECK(r.words(bank1_last, 2) == std::vector<std::uint32_t>{0x11, 0x22});
  r.program(bank2_last, {0xb1, 0xb2, 0xb3, 0xb4});
  CHECK(r.words(bank2_last, 4) == std::vector<std::uint32_t>{0xb1, 0xb2, 0xb3, 0xb4});
  CHECK((r.word(r.at("SR")) & all_errors) == 0);
}

TEST_CASE("RM0351 3.7.5: PROGERR over a programmed double word, zeros program over any line")
{
  rig r;
  r.program(bank2_last, {0x1, 0x2});
  r.program(bank2_last, {0x3, 0x4});
  CHECK(r.status("PROGERR") == 1);
  CHECK(r.words(bank2_last, 2) == std::vector<std::uint32_t>{0x1, 0x2});
  r.clear_errors();
  r.program(bank2_last, {0, 0});
  CHECK(r.status("PROGERR") == 0);
  CHECK(r.words(bank2_last, 2) == std::vector<std::uint32_t>{0, 0});
  r.program(bank2_last, {0, 0});
  CHECK(r.status("PROGERR") == 0);
}

TEST_CASE("RM0351 3.3.7: byte and half-word stores set SIZERR, a split double word PGAERR")
{
  rig r;
  r.control(pg);
  r.store(bank2_last, 1, 1);
  CHECK(r.status("SIZERR") == 1);
  r.clear_errors();
  r.store(bank2_last, 1, 2);
  CHECK(r.status("SIZERR") == 1);
  r.clear_errors();
  r.store(bank2_last + 4, 0x1);
  CHECK(r.status("PGAERR") == 1);
  r.clear_errors();
  r.store(bank2_last, 0x1);
  r.store(bank2_last + 8, 0x2);
  CHECK(r.status("PGAERR") == 1);
  CHECK(r.status("BSY") == 0);
  CHECK(r.words(bank2_last, 4) == erased(4));
}

TEST_CASE("RM0351 3.7.5: PGSERR for a store without PG, a store with an error pending, a bare STRT")
{
  rig r;
  r.store_words(bank2_last, {0x1, 0x2});
  CHECK(r.status("PGSERR") == 1);
  CHECK(r.words(bank2_last, 2) == erased(2));
  r.clear_errors();
  r.control(pg);
  r.store(bank2_last, 1, 1);
  REQUIRE(r.status("SIZERR") == 1);
  r.store_words(bank2_last, {0x1, 0x2});
  CHECK(r.status("PGSERR") == 1);
  CHECK(r.words(bank2_last, 2) == erased(2));
  r.clear_errors();
  r.control(strt);
  CHECK(r.status("PGSERR") == 1);
  CHECK(r.status("BSY") == 0);
}

TEST_CASE("RM0351 3.3.6: a page erase of either bank holds BSY for tERASE, EOP only with EOPIE")
{
  rig r;
  r.program(bank1_last, {1, 2});
  r.program(bank2, {3, 4});
  r.control(0);
  auto const took = r.elapsed_ps([&] {
    r.control(per | pnb(63) | strt);
    CHECK(r.status("BSY") == 1);
    CHECK(r.control_bit("STRT") == 1);
    r.wait_idle();
  });
  CHECK(took >= 22'020'000'000u);
  CHECK(took < 22'100'000'000u);
  CHECK(r.words(bank1_last, 2) == erased(2));
  CHECK(r.words(bank2, 2) == std::vector<std::uint32_t>{3, 4});
  CHECK(r.control_bit("STRT") == 0);
  CHECK(r.status("EOP") == 0);
  CHECK_FALSE(r.pending());
  r.control(per | bker | pnb(0) | strt | eopie);
  r.wait_idle();
  CHECK(r.words(bank2, 2) == erased(2));
  CHECK(r.status("EOP") == 1);
  CHECK(r.pending());
  r.write("SR", 1);
  CHECK(r.status("EOP") == 0);
}

TEST_CASE("RM0351 3.7.6: a PNB past the 64 pages of a bank erases nothing and warns once")
{
  rig r;
  r.program(bank2, {3, 4});
  r.control(per | bker | pnb(64) | strt);
  CHECK(r.status("BSY") == 0);
  CHECK(r.words(bank2, 2) == std::vector<std::uint32_t>{3, 4});
  CHECK(
      std::ranges::count_if(
          r.logged,
          [](core::log_record const& record) {
            return record.peripheral == "FLASH" and record.reg == "CR";
          })
      == 1);
  rig strict{strict_options()};
  CHECK_THROWS_AS(
      (void)strict.f.target->mem().write(strict.at("CR"), 4, per | bker | pnb(64) | strt),
      popcorn::model_error);
}

TEST_CASE("RM0351 3.3.6: MER2 erases bank 2 alone, MER1 with MER2 both banks, for tME")
{
  rig r;
  r.program(bank1_last, {1, 2});
  r.program(bank2_last, {3, 4});
  r.control(0);
  auto const took = r.elapsed_ps([&] {
    r.control(mer2 | strt);
    r.wait_idle();
  });
  CHECK(took >= 22'130'000'000u);
  CHECK(took < 22'200'000'000u);
  CHECK(r.words(bank1_last, 2) == std::vector<std::uint32_t>{1, 2});
  CHECK(r.words(bank2_last, 2) == erased(2));
  r.program(bank2_last, {3, 4});
  r.control(0);
  r.control(mer1 | mer2 | strt);
  r.wait_idle();
  CHECK(r.words(bank1_last, 2) == erased(2));
  CHECK(r.words(bank2_last, 2) == erased(2));
  CHECK(r.word(0x0800'0000u) == 0xffff'ffffu);
}

TEST_CASE("RM0351 3.7.5: ERRIE adds OPERR and the FLASH interrupt, IRQ 4, to an error")
{
  rig r;
  r.control(pg);
  r.store(bank2_last, 1, 1);
  CHECK(r.status("SIZERR") == 1);
  CHECK(r.status("OPERR") == 0);
  CHECK_FALSE(r.pending());
  r.clear_errors();
  r.control(pg | errie);
  r.store(bank2_last, 1, 1);
  CHECK(r.status("OPERR") == 1);
  CHECK(r.pending());
  r.clear_errors();
  CHECK(r.status("OPERR") == 0);
}

TEST_CASE(
    "RM0351 3.4.2: OPTKEY1 then OPTKEY2 clear OPTLOCK only with LOCK clear, a key with LOCK set "
    "a bus error, inferred")
{
  rig r;
  r.control(lock_bit);
  CHECK_FALSE(r.try_write("OPTKEYR", option_key1));
  CHECK_FALSE(r.try_write("OPTKEYR", option_key2));
  CHECK(r.control_bit("OPTLOCK") == 1);
  rig open;
  open.write("OPTKEYR", option_key1);
  open.write("OPTKEYR", option_key2);
  CHECK(open.control_bit("OPTLOCK") == 0);
  CHECK(open.control_bit("LOCK") == 0);
  open.control(optlock_bit);
  CHECK(open.control_bit("OPTLOCK") == 1);
}

namespace ll
{
namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware        = std::string_view{"l476_l4_flash_ll"};
constexpr address       bank2           = 0x0802'0000;
constexpr address       bank1_last      = 0x0801'f800;
constexpr address       bank2_last      = 0x0803'f800;
constexpr std::uint32_t page_bytes      = 0x800;
constexpr std::uint32_t flash_irq       = 4;
constexpr std::uint32_t pg              = 1u << 0;
constexpr std::uint32_t per             = 1u << 1;
constexpr std::uint32_t mer1            = 1u << 2;
constexpr std::uint32_t bker            = 1u << 11;
constexpr std::uint32_t mer2            = 1u << 15;
constexpr std::uint32_t strt            = 1u << 16;
constexpr std::uint32_t eopie           = 1u << 24;
constexpr std::uint32_t errie           = 1u << 25;
constexpr std::uint32_t lock_bit        = 1u << 31;
constexpr std::uint32_t all_errors      = 0xc3fb;
constexpr std::uint32_t key1            = 0x4567'0123;
constexpr std::uint32_t key2            = 0xcdef'89ab;
constexpr std::uint32_t option_key1     = 0x0819'2a3b;
constexpr std::uint32_t option_key2     = 0x4c5d'6e7f;
constexpr std::uint32_t dcen            = 1u << 10;
constexpr std::uint32_t power_down_key1 = 0x0415'2637;
constexpr std::uint32_t power_down_key2 = 0xfafb'fcfd;
constexpr address       cfsr            = 0xE000'ED28;
constexpr address       hfsr            = 0xE000'ED2C;
constexpr std::uint32_t impreciserr     = 1u << 10;
constexpr std::uint32_t forced          = 1u << 30;
constexpr std::uint32_t hardfault_n     = 3;
constexpr std::uint32_t run_pd          = 1u << 13;
constexpr std::uint64_t mhz             = 1'000'000;

auto strict_options() -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  return options;
}

struct rig
{
  stm32_fixture                 f;
  std::vector<core::log_record> logged{};
  core::hook_handle             sink{};

  explicit rig(core::machine_options options = popcorn::testing::stm32_options())
      : f{l476rc(), firmware, std::move(options)}
  {
    sink = f.target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
    f.set("pc", sym(firmware, "idle"));
    unlock();
  }

  [[nodiscard]] auto at(std::string_view reg, std::string_view peripheral = "FLASH") const
      -> address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  void write(std::string_view reg, std::uint32_t value, std::string_view peripheral = "FLASH") const
  {
    REQUIRE(f.target->mem().write(at(reg, peripheral), 4, value).has_value());
  }

  [[nodiscard]] auto try_write(std::string_view reg, std::uint32_t value) const -> bool
  {
    auto&      regs = f.target->peripheral("FLASH");
    auto const word = popcorn::find_register(regs.description(), reg);
    REQUIRE(word != nullptr);
    return f.target->model("FLASH")
        ->on_write(
            regs, word->offset, 4, regs.peek(word->offset), value, {.pc = 0, .now = icount{0}})
        .has_value();
  }

  [[nodiscard]] auto word(address from) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(from, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto read(std::string_view reg, std::string_view peripheral = "FLASH") const
      -> std::uint32_t
  {
    return word(at(reg, peripheral));
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

  [[nodiscard]] auto status(std::string_view name) const -> std::uint32_t
  {
    return f.field("FLASH", "SR", name);
  }

  void unlock() const
  {
    write("KEYR", key1);
    write("KEYR", key2);
    REQUIRE(f.field("FLASH", "CR", "LOCK") == 0);
  }

  void control(std::uint32_t value) const
  {
    write("CR", value);
  }

  void clear_errors() const
  {
    write("SR", all_errors | 1u);
  }

  void call(
      std::string_view                     entry,
      std::initializer_list<std::uint32_t> args,
      icount                               bound = icount{1'000'000}) const
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

  void power_down_keys() const
  {
    write("PDKEYR", power_down_key1);
    write("PDKEYR", power_down_key2);
  }

  void program(address to, std::vector<std::uint32_t> const& values, std::uint32_t extra = 0) const
  {
    control(pg | extra);
    store_words(to, values);
    wait_idle();
  }

  void idle_for_ps(std::uint64_t span) const
  {
    f.set("pc", sym(firmware, "idle"));
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000},
                       .until_time = popcorn::vtime{f.target->virtual_clock().time().ps + span}})
            .reason
        == stop_reason::time);
  }

  [[nodiscard]] auto pending() const -> bool
  {
    return (word(0xe000'e200u) >> flash_irq & 1u) != 0;
  }

  [[nodiscard]] auto warnings(std::string_view reg) const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(logged, [&](core::log_record const& r) {
      return r.peripheral == "FLASH" and r.reg == reg and r.kind == core::log_kind::model_warning;
    }));
  }

  [[nodiscard]] auto elapsed_ps(auto&& action) const -> std::uint64_t
  {
    auto const start = f.target->virtual_clock().time().ps;
    action();
    return f.target->virtual_clock().time().ps - start;
  }
};
}

TEST_CASE("RM0351 3.3.5: KEY1 then KEY2 clears LOCK, LOCK sets it again, OPTLOCK stays set")
{
  stm32_fixture f{l476rc(), firmware};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{4, 1, 0, 1, 1});
  CHECK(f.field("FLASH", "ACR", "LATENCY") == 4);
}

TEST_CASE(
    "RM0351 3.3.5: a wrong key from the CPU enters HardFault through an imprecise BusFault, "
    "FLASH_CR locked until reset")
{
  auto options   = popcorn::testing::stm32_options();
  options.faults = popcorn::arch::fault_policy::enter_handler;
  stm32_fixture f{l476rc(), firmware, std::move(options)};
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  f.set("pc", sym(firmware, "wrong_key"));
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  auto const log = f.log();
  REQUIRE(log.size() >= 2);
  CHECK(log[log.size() - 2] == 0xfa17u);
  CHECK(log.back() == 1);
  auto const word = [&](address at) {
    return static_cast<std::uint32_t>(f.target->mem().read(at, 4).value_or(0));
  };
  CHECK((f.target->regs().read(*f.target->regs().find("xpsr")) & 0x1ffu) == hardfault_n);
  CHECK((word(hfsr) & forced) != 0);
  CHECK(word(cfsr) == impreciserr);
  f.target->reset(popcorn::reset_kind::software);
  REQUIRE(f.target->mem().write(0x4002'2008u, 4, key1).has_value());
  REQUIRE(f.target->mem().write(0x4002'2008u, 4, key2).has_value());
  CHECK(f.field("FLASH", "CR", "LOCK") == 0);
}

TEST_CASE("RM0351 3.3.8: code in RAM programs bank 1, code in bank 1 programs bank 2")
{
  rig r;
  r.control(pg);
  auto const buffer = sym(firmware, "words");
  REQUIRE(r.f.target->mem().write(buffer, 4, 0xa1u).has_value());
  REQUIRE(r.f.target->mem().write(buffer + 4, 4, 0xa2u).has_value());
  r.call(
      "copy_words_ram",
      {static_cast<std::uint32_t>(bank1_last), static_cast<std::uint32_t>(buffer), 2});
  CHECK(r.words(bank1_last, 2) == std::vector<std::uint32_t>{0xa1, 0xa2});
  r.program(bank2_last, {0xb1, 0xb2, 0xb3, 0xb4});
  CHECK(r.words(bank2_last, 4) == std::vector<std::uint32_t>{0xb1, 0xb2, 0xb3, 0xb4});
  CHECK((r.read("SR") & all_errors) == 0);
}

TEST_CASE("RM0351 3.3.3 Table 11: LATENCY against HCLK in range 1 and range 2")
{
  rig r;
  r.write("ACR", 0x600u | 2u);
  r.write("CR", (r.read("CR", "RCC") & ~0xf0u) | 0xb0u | 1u << 3, "RCC");
  REQUIRE(r.f.target->virtual_clock().core_hz() == 48 * mhz);
  CHECK(r.warnings("ACR") == 0);
  r.write("ACR", 0x600u | 1u);
  REQUIRE(r.warnings("ACR") == 1);
  CHECK(r.logged.back().message.contains("LATENCY 1 is below the 2 wait states"));
  r.write("ACR", 0x600u | 4u);
  r.write("CR1", (r.read("CR1", "PWR") & ~(3u << 9)) | 2u << 9, "PWR");
  REQUIRE(r.f.field("PWR", "SR2", "VOSF") == 1);
  auto const before = r.warnings("ACR");
  r.idle_for_ps(60'000'000);
  REQUIRE(r.warnings("ACR") == before + 1);
  CHECK(
      r.logged.back().message.contains("HCLK 48000000 Hz is above the maximum of voltage range 2"));
}

TEST_CASE("RM0351 3.3.3 Table 11: range 2 asks LATENCY 2 for 16 MHz and 0 for 4 MHz")
{
  rig r;
  r.write("CR1", (r.read("CR1", "PWR") & ~(3u << 9)) | 2u << 9, "PWR");
  r.idle_for_ps(60'000'000);
  CHECK(r.warnings("ACR") == 0);
  r.write("ACR", 0x600u);
  CHECK(r.warnings("ACR") == 0);
  r.write("CR", (r.read("CR", "RCC") & ~0xf0u) | 0x80u | 1u << 3, "RCC");
  REQUIRE(r.f.target->virtual_clock().core_hz() == 16 * mhz);
  REQUIRE(r.warnings("ACR") == 1);
  CHECK(r.logged.back().message.contains("below the 2 wait states"));
}

TEST_CASE("RM0351 3.3.3: under strict_peripherals a latency below Table 11 raises model_error")
{
  rig r{strict_options()};
  r.write("ACR", 0x600u);
  auto const at = r.at("CR", "RCC");
  CHECK_THROWS_AS(
      (void)r.f.target->mem().write(at, 4, (r.read("CR", "RCC") & ~0xf0u) | 0xb0u | 1u << 3),
      popcorn::model_error);
}

TEST_CASE(
    "RM0351 3.7.1: RUN_PD set by the CPU after the PDKEYR pair warns once, by the debugger "
    "never, strict raises")
{
  rig r;
  r.power_down_keys();
  REQUIRE(r.try_write("ACR", r.read("ACR") | run_pd));
  CHECK((r.read("ACR") & run_pd) != 0);
  CHECK(r.warnings("ACR") == 1);
  r.power_down_keys();
  REQUIRE(r.try_write("ACR", r.read("ACR") & ~run_pd));
  CHECK((r.read("ACR") & run_pd) == 0);
  r.power_down_keys();
  REQUIRE(r.try_write("ACR", r.read("ACR") | run_pd));
  CHECK(r.warnings("ACR") == 1);
  r.power_down_keys();
  r.write("ACR", r.read("ACR") & ~run_pd);
  r.power_down_keys();
  r.write("ACR", r.read("ACR") | run_pd);
  CHECK(r.warnings("ACR") == 1);
  rig host;
  host.power_down_keys();
  host.write("ACR", host.read("ACR") | run_pd);
  CHECK((host.read("ACR") & run_pd) != 0);
  CHECK(host.warnings("ACR") == 0);
  rig        strict{strict_options()};
  auto const before = strict.read("ACR");
  strict.power_down_keys();
  CHECK_THROWS_AS((void)strict.try_write("ACR", before | run_pd), popcorn::model_error);
  CHECK(strict.read("ACR") == before);
}

TEST_CASE(
    "RM0351 3.7.2: RUN_PD keeps its value without PDKEY1 then PDKEY2, the pair unlocks one "
    "FLASH_ACR write, inferred")
{
  rig        r;
  auto const before = r.read("ACR");
  REQUIRE(r.try_write("ACR", before | run_pd | dcen));
  CHECK(r.read("ACR") == (before | dcen));
  CHECK(r.warnings("ACR") == 0);
  REQUIRE(r.try_write("ACR", before));
  r.write("PDKEYR", power_down_key2);
  REQUIRE(r.try_write("ACR", before | run_pd));
  CHECK(r.read("ACR") == before);
  r.write("PDKEYR", power_down_key1);
  r.write("PDKEYR", 0x1234'5678u);
  r.write("PDKEYR", power_down_key2);
  REQUIRE(r.try_write("ACR", before | run_pd));
  CHECK(r.read("ACR") == before);
  r.power_down_keys();
  REQUIRE(r.try_write("ACR", before | dcen));
  REQUIRE(r.try_write("ACR", before | run_pd));
  CHECK(r.read("ACR") == before);
  CHECK(r.read("PDKEYR") == 0);
  r.write("PDKEYR", power_down_key1);
  auto const saved = r.f.target->snapshot();
  r.f.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  r.write("PDKEYR", power_down_key2);
  REQUIRE(r.try_write("ACR", before | run_pd));
  CHECK(r.read("ACR") == (before | run_pd));
  CHECK(r.warnings("ACR") == 1);
  r.power_down_keys();
  r.f.target->reset(popcorn::reset_kind::software);
  r.write("ACR", r.read("ACR") | run_pd);
  CHECK((r.read("ACR") & run_pd) == 0);
  rig strict{strict_options()};
  REQUIRE(strict.try_write("ACR", strict.read("ACR") | run_pd));
  CHECK((strict.read("ACR") & run_pd) == 0);
}

TEST_CASE(
    "ES0250 2.2.14, popcorn rule: with DCEN set a read of bank 1 during a bank 2 program "
    "reads the stored data")
{
  rig r;
  r.write("ACR", r.read("ACR") | dcen);
  r.control(pg);
  r.store_words(bank2_last, {0x5, 0x6});
  REQUIRE(r.status("BSY") == 1);
  r.call("read_word", {static_cast<std::uint32_t>(sym(firmware, "pattern"))});
  auto const log = r.f.log();
  REQUIRE(log.size() >= 2);
  CHECK(log[log.size() - 2] == 0x0123'4567u);
  CHECK(log.back() == 1);
  r.wait_idle();
  CHECK(r.words(bank2_last, 2) == std::vector<std::uint32_t>{0x5, 0x6});
}
}
