#include "../support/inert_model.hpp"
#include "read_protection.hpp"
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
#include <memory>
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
constexpr std::uint32_t fstpg           = 1u << 18;
constexpr std::uint32_t miserr          = 1u << 8;
constexpr std::uint32_t fasterr         = 1u << 9;
constexpr std::uint32_t pgaerr          = 1u << 5;
constexpr std::uint32_t sizerr          = 1u << 6;
constexpr std::uint32_t pgserr          = 1u << 7;
constexpr std::uint32_t eccd            = 1u << 31;
constexpr std::uint32_t eccie           = 1u << 24;
constexpr std::uint32_t bk_ecc          = 1u << 19;
constexpr std::uint64_t row_ps          = 1'910'000'000;
constexpr std::uint64_t double_word_ps  = row_ps / 32;
constexpr std::uint32_t optstrt         = 1u << 17;
constexpr std::uint32_t obl_launch      = 1u << 27;
constexpr std::uint32_t wrperr          = 1u << 4;
constexpr std::uint32_t optr_production = 0xffef'f8aa;
constexpr std::uint32_t wrp_production  = 0xff00'ffff;
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

  void unlock_options() const
  {
    write("OPTKEYR", option_key1);
    write("OPTKEYR", option_key2);
    REQUIRE(f.field("FLASH", "CR", "OPTLOCK") == 0);
  }

  void program_options() const
  {
    control(optstrt);
    REQUIRE(f.field("FLASH", "CR", "OPTSTRT") == 0);
  }

  void load_options() const
  {
    f.target->reset(popcorn::reset_kind::option_load);
    unlock();
  }

  [[nodiscard]] auto requested_reset() const -> bool
  {
    f.set("pc", sym(firmware, "idle"));
    auto const stopped = f.run_to_stop({.max_instructions = icount{1'000}});
    return stopped.reason == stop_reason::reset_request and stopped.detail == "option_load";
  }

  void msi_hz(std::uint32_t range, std::uint64_t hz) const
  {
    write("CR", (read("CR", "RCC") & ~0xf0u) | range << 4 | 1u << 3, "RCC");
    REQUIRE(f.target->virtual_clock().core_hz() == hz);
  }

  void erase_bank2() const
  {
    control(mer2 | strt);
    wait_idle();
    control(0);
    REQUIRE((read("SR") & all_errors & ~1u) == 0);
  }

  void hclk_16mhz() const
  {
    write("CR", (read("CR", "RCC") & ~0xf0u) | 0x80u | 1u << 3, "RCC");
    REQUIRE(f.target->virtual_clock().core_hz() == 16 * mhz);
  }

  void tear(address line) const
  {
    control(pg);
    store_words(line, {0x1u, 0x2u});
    idle_for_ps(40'000'000);
    REQUIRE(status("BSY") == 1);
    f.target->power_cycle();
    unlock();
  }

  struct loaded
  {
    std::uint32_t value{};
    std::uint32_t nmi_count{};
    std::uint32_t nmi_eccr{};

    friend auto operator==(loaded const&, loaded const&) -> bool = default;
  };

  [[nodiscard]] auto load(address from) const -> loaded
  {
    REQUIRE(f.target->mem().write(sym(firmware, "nmi_count"), 4, 0).has_value());
    REQUIRE(f.target->mem().write(sym(firmware, "nmi_eccr"), 4, 0).has_value());
    call("read_word", {static_cast<std::uint32_t>(from)});
    auto const log = f.log();
    REQUIRE(log.size() >= 2);
    REQUIRE(log.back() == 0);
    return {
        .value     = log[log.size() - 2],
        .nmi_count = word(sym(firmware, "nmi_count")),
        .nmi_eccr  = word(sym(firmware, "nmi_eccr"))};
  }

  [[nodiscard]] static auto row(std::uint32_t seed, std::size_t count = 64)
      -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> out;
    for(std::size_t i = 0; i < count; ++i)
    {
      out.push_back(seed + static_cast<std::uint32_t>(i));
    }
    return out;
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

TEST_CASE(
    "RM0351 3.3.7: FSTPG programs a row of 32 double words with BSY set for the DS10198 Table 63 "
    "row time and EOP at its end")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  auto const data    = rig::row(0x100);
  auto const elapsed = r.elapsed_ps([&] {
    r.control(fstpg | eopie);
    r.store_words(bank2, data);
    REQUIRE(r.status("BSY") == 1);
    r.idle_for_ps(row_ps / 2);
    auto const saved = r.f.target->snapshot();
    r.f.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    r.idle_for_ps(row_ps / 2 - double_word_ps);
    CHECK(r.status("BSY") == 1);
    CHECK(r.status("EOP") == 0);
    CHECK(r.words(bank2, 64) != data);
    r.wait_idle();
  });
  CHECK(elapsed >= row_ps);
  CHECK(elapsed < row_ps + double_word_ps);
  CHECK(r.words(bank2, 64) == data);
  CHECK(r.status("EOP") == 1);
  CHECK((r.read("SR") & all_errors & ~1u) == 0);
  CHECK(r.pending());
  CHECK(r.warnings("CR") == 0);
  r.clear_errors();
  r.store_words(bank2 + 256, rig::row(0x200));
  r.wait_idle();
  CHECK(r.words(bank2 + 256, 64) == rig::row(0x200));
  CHECK((r.read("SR") & all_errors & ~1u) == 0);
}

TEST_CASE(
    "RM0351 3.3.7: a row whose next double word is missing when one ends sets MISERR and "
    "FASTERR, the double words before it stay programmed")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  r.control(fstpg | eopie);
  auto const elapsed = r.elapsed_ps([&] {
    r.store_words(bank2, rig::row(0x10, 5));
    r.wait_idle();
  });
  CHECK(elapsed >= 2 * double_word_ps);
  CHECK(elapsed < 3 * double_word_ps);
  CHECK((r.read("SR") & all_errors) == (miserr | fasterr));
  CHECK(r.status("EOP") == 0);
  CHECK(r.words(bank2, 6) == std::vector<std::uint32_t>{0x10, 0x11, 0x12, 0x13, ~0u, ~0u});
  r.store_words(bank2 + 16, {0x20, 0x21});
  CHECK((r.read("SR") & pgserr) != 0);
  CHECK(r.words(bank2 + 16, 2) == std::vector<std::uint32_t>{~0u, ~0u});
  r.clear_errors();
  r.store_words(bank2 + 256, rig::row(0x30, 2));
  r.idle_for_ps(2 * double_word_ps);
  r.store_words(bank2 + 264, rig::row(0x40, 2));
  CHECK((r.read("SR") & all_errors) == (miserr | fasterr | pgserr));
  CHECK(r.words(bank2 + 256, 4) == std::vector<std::uint32_t>{0x30, 0x31, ~0u, ~0u});
}

TEST_CASE(
    "RM0351 3.3.7: PGAERR and FASTERR for a double word outside the row or not above the last, "
    "PGSERR for its second word, SIZERR and FASTERR for a half-word")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  r.control(fstpg);
  r.store_words(bank2, rig::row(0x50, 4));
  r.store_words(bank2 + 256, {0x60, 0x61});
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == (pgaerr | fasterr | pgserr));
  CHECK(r.words(bank2, 4) == rig::row(0x50, 4));
  CHECK(r.words(bank2 + 256, 2) == std::vector<std::uint32_t>{~0u, ~0u});
  r.clear_errors();
  r.store_words(bank2 + 512 + 16, {0x70, 0x71});
  r.store_words(bank2 + 512 + 8, {0x72, 0x73});
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == (pgaerr | fasterr | pgserr));
  CHECK(r.words(bank2 + 512 + 8, 4) == std::vector<std::uint32_t>{~0u, ~0u, 0x70, 0x71});
  r.clear_errors();
  r.call("store_half", {static_cast<std::uint32_t>(bank2 + 1024), 0x1234});
  CHECK((r.read("SR") & all_errors) == (sizerr | fasterr));
  r.clear_errors();
  r.control(pg);
  r.call("store_half", {static_cast<std::uint32_t>(bank2 + 1024), 0x1234});
  CHECK((r.read("SR") & all_errors) == sizerr);
}

TEST_CASE(
    "RM0351 3.3.7, inferred: a fast row over programmed bits clears only the bits it programs, "
    "with no PROGERR")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  r.program(bank2 + 8, {0x0f0f'0f0fu, 0xffff'0000u});
  r.control(fstpg);
  std::vector<std::uint32_t> const data(64, 0xff00'ff00u);
  r.store_words(bank2, data);
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == 0);
  CHECK(
      r.words(bank2, 4)
      == std::vector<std::uint32_t>{0xff00'ff00u, 0xff00'ff00u, 0x0f00'0f00u, 0xff00'0000u});
}

TEST_CASE("RM0351 3.3.7: FSTPG with HCLK below 8 MHz warns once, under strict_peripherals raises")
{
  rig r;
  REQUIRE(r.f.target->virtual_clock().core_hz() == 4 * mhz);
  r.erase_bank2();
  r.control(fstpg);
  r.store_words(bank2, rig::row(0x80));
  r.wait_idle();
  CHECK(r.words(bank2, 64) == rig::row(0x80));
  REQUIRE(r.warnings("CR") == 1);
  CHECK(r.logged.back().message.contains("below the 8000000 Hz"));
  r.store_words(bank2 + 256, rig::row(0x90));
  r.wait_idle();
  CHECK(r.warnings("CR") == 1);
  rig strict{strict_options()};
  strict.control(fstpg);
  strict.f.set("r0", bank2);
  strict.f.set("r1", sym(firmware, "pattern"));
  strict.f.set("r2", 2);
  strict.f.set("pc", sym(firmware, "copy_words"));
  CHECK_THROWS_AS(
      (void)strict.f.run_to_stop({.max_instructions = icount{1'000}}), popcorn::model_error);
  CHECK(strict.word(bank2) == ~0u);
  CHECK(strict.status("BSY") == 0);
}

TEST_CASE("a power cut in the third double word of a fast row leaves two programmed and one torn")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  r.control(fstpg);
  auto const start = r.f.target->virtual_clock().time().ps;
  r.store_words(bank2, rig::row(0xa0));
  r.idle_for_ps(
      start + 2 * double_word_ps + double_word_ps / 2 - r.f.target->virtual_clock().time().ps);
  REQUIRE(r.status("BSY") == 1);
  r.f.target->power_cycle();
  r.unlock();
  CHECK(
      r.words(bank2, 8) == std::vector<std::uint32_t>{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, ~0u, ~0u, ~0u});
  CHECK(r.load(bank2 + 16) == rig::loaded{0xa4, 1, eccd | bk_ecc | 16u});
}

TEST_CASE(
    "RM0351 3.3.2: a CPU load of a torn double word sets ECCD, BK_ECC and ADDR_ECC "
    "and takes the NMI")
{
  for(address const line : {bank2_last, bank1_last})
  {
    CAPTURE(line);
    rig r;
    r.tear(line);
    CHECK(r.words(line, 2) == std::vector<std::uint32_t>{0x1, ~0u});
    REQUIRE(r.read("ECCR") == 0);
    auto const reported = eccd | (line >= bank2 ? bk_ecc : 0u) | 0x1'f800u;
    CHECK(r.load(line + 4) == rig::loaded{~0u, 1, reported});
    CHECK(r.read("ECCR") == (reported & ~eccd));
    CHECK(r.load(line - 8) == rig::loaded{~0u, 0, 0});
    CHECK(r.load(line) == rig::loaded{0x1, 1, reported});
  }
}

TEST_CASE(
    "RM0351 3.7.7: FLASH_ECCR stores ECCIE and keeps its read-only fields, a reset clears it, "
    "the torn double word stays until a page erase")
{
  rig r;
  r.tear(bank2_last);
  r.write("ECCR", eccie);
  CHECK(r.read("ECCR") == eccie);
  auto const record = bk_ecc | eccie | 0x1'f800u;
  CHECK(r.load(bank2_last) == rig::loaded{0x1, 1, eccd | record});
  CHECK(r.read("ECCR") == record);
  r.write("ECCR", ~0u);
  CHECK(r.read("ECCR") == record);
  CHECK_FALSE(r.pending());
  r.f.target->reset(popcorn::reset_kind::software);
  CHECK(r.read("ECCR") == 0);
  r.unlock();
  r.program(bank2_last, {0, 0});
  CHECK(r.status("PROGERR") == 1);
  r.clear_errors();
  r.control(per | bker | ((bank2_last - bank2) / page_bytes) << 3 | strt);
  r.wait_idle();
  r.control(0);
  r.program(bank2_last, {0x5, 0x6});
  CHECK((r.read("SR") & all_errors) == 0);
  CHECK(r.load(bank2_last) == rig::loaded{0x5, 0, 0});
}

TEST_CASE("RM0351 3.3.7: FSTPG set for 7 ms sets FASTERR, a store after it sets PGSERR, inferred")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  r.control(fstpg);
  r.idle_for_ps(6'990'000'000);
  CHECK((r.read("SR") & all_errors) == 0);
  r.idle_for_ps(20'000'000);
  CHECK((r.read("SR") & all_errors) == fasterr);
  r.store_words(bank2, {0x1, 0x2});
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == (fasterr | pgserr));
  CHECK(r.words(bank2, 2) == std::vector<std::uint32_t>{~0u, ~0u});
}

TEST_CASE("RM0351 3.3.7: four rows with FSTPG never cleared set FASTERR inside the fourth")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  auto const start = r.f.target->virtual_clock().time().ps;
  r.control(fstpg);
  for(std::uint32_t row = 0; row < 3; ++row)
  {
    r.store_words(bank2 + 256 * row, rig::row(0x1000 * row));
    r.wait_idle();
    REQUIRE((r.read("SR") & all_errors) == 0);
  }
  r.store_words(bank2 + 768, rig::row(0x3000));
  r.wait_idle();
  CHECK(r.f.target->virtual_clock().time().ps - start >= 7'000'000'000);
  CHECK(r.f.target->virtual_clock().time().ps - start < 7'000'000'000 + double_word_ps);
  CHECK((r.read("SR") & all_errors) == fasterr);
  CHECK(r.status("BSY") == 0);
  for(std::uint32_t row = 0; row < 3; ++row)
  {
    CHECK(r.words(bank2 + 256 * row, 64) == rig::row(0x1000 * row));
  }
  CHECK(r.words(bank2 + 768, 2) == rig::row(0x3000, 2));
  CHECK(r.words(bank2 + 768 + 248, 2) == std::vector<std::uint32_t>{~0u, ~0u});
}

TEST_CASE("RM0351 3.3.7: clearing FSTPG and setting it again restarts the 7 ms count")
{
  rig r;
  r.control(fstpg);
  r.idle_for_ps(5'000'000'000);
  r.control(0);
  r.control(fstpg);
  r.idle_for_ps(5'000'000'000);
  CHECK((r.read("SR") & all_errors) == 0);
  r.idle_for_ps(2'100'000'000);
  CHECK((r.read("SR") & all_errors) == fasterr);
  r.clear_errors();
  r.control(0);
  r.idle_for_ps(8'000'000'000);
  CHECK((r.read("SR") & all_errors) == 0);
}

TEST_CASE(
    "RM0351 3.3.7: a load of the bank under a fast row aborts it with no flag, inferred, a load "
    "of the other bank does not")
{
  for(auto const other : {false, true})
  {
    CAPTURE(other);
    rig r;
    r.hclk_16mhz();
    r.erase_bank2();
    r.control(fstpg | eopie);
    auto const start = r.f.target->virtual_clock().time().ps;
    r.store_words(bank2, rig::row(0xc0));
    r.idle_for_ps(
        start + 3 * double_word_ps + double_word_ps / 2 - r.f.target->virtual_clock().time().ps);
    REQUIRE(r.status("BSY") == 1);
    auto const from = other ? static_cast<address>(sym(firmware, "pattern")) : bank2 + 1024;
    r.call("read_word", {static_cast<std::uint32_t>(from)});
    r.wait_idle();
    if(other)
    {
      CHECK(r.words(bank2, 64) == rig::row(0xc0));
      CHECK(r.status("EOP") == 1);
      CHECK(r.warnings("CR") == 0);
    }
    else
    {
      CHECK(
          r.words(bank2, 8)
          == std::vector<std::uint32_t>{0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, ~0u, ~0u});
      CHECK(r.words(bank2 + 248, 2) == std::vector<std::uint32_t>{~0u, ~0u});
      CHECK(r.status("EOP") == 0);
      REQUIRE(r.warnings("CR") == 1);
      CHECK(r.logged.back().message.contains("aborts the row"));
    }
    CHECK((r.read("SR") & all_errors & ~1u) == 0);
  }
  rig strict{strict_options()};
  strict.hclk_16mhz();
  strict.erase_bank2();
  strict.control(fstpg);
  strict.store_words(bank2, rig::row(0xd0));
  strict.f.set("r0", bank2 + 1024);
  strict.f.set("pc", sym(firmware, "read_word"));
  CHECK_THROWS_AS(
      (void)strict.f.run_to_stop({.max_instructions = icount{1'000}}), popcorn::model_error);
}

TEST_CASE(
    "RM0351 3.3.7: a row on a bank with no mass erase since the last reset warns once, "
    "strict raises, a page erase does not count")
{
  rig r;
  r.hclk_16mhz();
  r.control(per | bker | strt);
  r.wait_idle();
  r.control(fstpg);
  r.store_words(bank2, rig::row(0xe0));
  r.wait_idle();
  CHECK(r.words(bank2, 64) == rig::row(0xe0));
  REQUIRE(r.warnings("CR") == 1);
  CHECK(r.logged.back().message.contains("no mass erase since the last reset"));
  rig strict{strict_options()};
  strict.hclk_16mhz();
  strict.erase_bank2();
  strict.control(fstpg);
  strict.store_words(bank2, rig::row(0xf0));
  strict.wait_idle();
  CHECK(strict.words(bank2, 64) == rig::row(0xf0));
  strict.f.set("r0", bank1_last);
  strict.f.set("r1", sym(firmware, "pattern"));
  strict.f.set("r2", 2);
  strict.f.set("pc", sym(firmware, "copy_words"));
  CHECK_THROWS_AS(
      (void)strict.f.run_to_stop({.max_instructions = icount{1'000}}), popcorn::model_error);
  CHECK(strict.words(bank1_last, 2) == std::vector<std::uint32_t>{~0u, ~0u});
  strict.f.target->reset(popcorn::reset_kind::software);
  strict.unlock();
  strict.hclk_16mhz();
  strict.control(fstpg);
  strict.f.set("r0", bank2 + 256);
  strict.f.set("r1", sym(firmware, "pattern"));
  strict.f.set("r2", 2);
  strict.f.set("pc", sym(firmware, "copy_words"));
  CHECK_THROWS_AS(
      (void)strict.f.run_to_stop({.max_instructions = icount{1'000}}), popcorn::model_error);
}

TEST_CASE("RM0351 3.4.1: FLASH_OPTR and the option registers read their ST production values")
{
  rig        r;
  auto const check = [&] {
    CHECK(r.read("OPTR") == optr_production);
    CHECK(r.f.field("FLASH", "OPTR", "RDP") == 0xaa);
    CHECK(r.f.field("FLASH", "OPTR", "IWDG_SW") == 1);
    CHECK(r.f.field("FLASH", "OPTR", "DUALBANK") == 1);
    CHECK(r.f.field("FLASH", "OPTR", "BFB2") == 0);
    for(auto const name : {"PCROP1SR", "PCROP2SR"})
    {
      CHECK(r.read(name) == 0xffff'ffffu);
    }
    for(auto const name : {"PCROP1ER", "PCROP2ER"})
    {
      CHECK(r.read(name) == 0xffff'0000u);
    }
    for(auto const name : {"WRP1AR", "WRP1BR", "WRP2AR", "WRP2BR"})
    {
      CHECK(r.read(name) == wrp_production);
    }
  };
  check();
  r.f.target->power_cycle();
  check();
}

TEST_CASE(
    "RM0351 3.4.2: OPTSTRT programs the option registers, a system reset restores the loaded "
    "words, an option byte load the programmed ones")
{
  rig r;
  r.unlock_options();
  r.write("OPTR", 0);
  CHECK(r.read("OPTR") == 0xfc40'8800u);
  r.write("WRP2AR", 0xffff'ffffu);
  CHECK(r.read("WRP2AR") == 0xffff'ffffu);
  r.write("PCROP1ER", 0x7fff'1234u);
  CHECK(r.read("PCROP1ER") == 0x7fff'1234u);
  r.f.target->reset(popcorn::reset_kind::software);
  CHECK(r.read("OPTR") == optr_production);
  CHECK(r.read("WRP2AR") == wrp_production);
  r.unlock();
  r.unlock_options();
  r.write("OPTR", optr_production & ~(1u << 16));
  r.write("WRP2AR", 0xff03'ff02u);
  r.program_options();
  CHECK(r.status("BSY") == 0);
  CHECK((r.read("SR") & all_errors) == 0);
  r.f.target->reset(popcorn::reset_kind::software);
  CHECK(r.read("OPTR") == optr_production);
  CHECK(r.read("WRP2AR") == wrp_production);
  r.load_options();
  CHECK(r.read("OPTR") == (optr_production & ~(1u << 16)));
  CHECK(r.f.field("FLASH", "OPTR", "IWDG_SW") == 0);
  CHECK(r.read("WRP2AR") == 0xff03'ff02u);
  auto const saved = r.f.target->snapshot();
  r.f.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  r.f.target->power_cycle();
  CHECK(r.read("WRP2AR") == 0xff03'ff02u);
  CHECK(r.read("OPTR") == (optr_production & ~(1u << 16)));
}

TEST_CASE(
    "RM0351 3.4.1 Table 13: the option byte area holds the production words and their "
    "complements, Unused fields inferred")
{
  rig        r;
  auto const check = [&] {
    CHECK(
        r.words(0x1fff'7800, 10)
        == std::vector<std::uint32_t>{
            optr_production, 0x0010'0755u, 0xffff'ffffu, 0u, 0xffff'0000u, 0x0000'ffffu,
            wrp_production, 0x00ff'0000u, wrp_production, 0x00ff'0000u});
    CHECK(
        r.words(0x1fff'f800, 10)
        == std::vector<std::uint32_t>{
            ~0u, ~0u, 0xffff'ffffu, 0u, 0xffff'0000u, 0x0000'ffffu, wrp_production, 0x00ff'0000u,
            wrp_production, 0x00ff'0000u});
  };
  check();
  r.f.target->power_cycle();
  check();
}

TEST_CASE(
    "RM0351 3.4.1 and 3.4.2: OPTSTRT writes the programmed words and their complements into the "
    "option byte area before the option byte load, a snapshot keeps the area")
{
  rig        r;
  auto const saved = r.f.target->snapshot();
  r.unlock_options();
  r.write("OPTR", optr_production & ~(1u << 16));
  r.write("WRP1AR", 0xff10'ff10u);
  r.write("PCROP2ER", 0xffff'0123u);
  CHECK(r.words(0x1fff'7800, 2) == std::vector<std::uint32_t>{optr_production, 0x0010'0755u});
  r.program_options();
  CHECK(r.words(0x1fff'7800, 2) == std::vector<std::uint32_t>{0xffee'f8aau, 0x0011'0755u});
  CHECK(r.words(0x1fff'7818, 2) == std::vector<std::uint32_t>{0xff10'ff10u, 0x00ef'00efu});
  CHECK(r.words(0x1fff'f810, 2) == std::vector<std::uint32_t>{0xffff'0123u, 0x0000'fedcu});
  CHECK(r.words(0x1fff'f800, 2) == std::vector<std::uint32_t>{~0u, ~0u});
  r.f.target->reset(popcorn::reset_kind::software);
  CHECK(r.read("OPTR") == optr_production);
  CHECK(r.words(0x1fff'7800, 2) == std::vector<std::uint32_t>{0xffee'f8aau, 0x0011'0755u});
  r.load_options();
  CHECK(r.read("OPTR") == 0xffee'f8aau);
  r.f.target->power_cycle();
  CHECK(r.words(0x1fff'7818, 2) == std::vector<std::uint32_t>{0xff10'ff10u, 0x00ef'00efu});
  CHECK(r.read("WRP1AR") == 0xff10'ff10u);
  r.f.target->restore(saved);
  CHECK(r.words(0x1fff'7800, 2) == std::vector<std::uint32_t>{optr_production, 0x0010'0755u});
  CHECK(r.words(0x1fff'7818, 2) == std::vector<std::uint32_t>{wrp_production, 0x00ff'0000u});
}

TEST_CASE(
    "every CPU store to the non-executable information block reaches the FLASH model, a store "
    "to a non-executable rom with no controller faults every time")
{
  rig r;
  for(auto const at : {address{0x1fff'7400}, address{0x1fff'7404}, address{0x1fff'7408}})
  {
    CAPTURE(at);
    r.store_words(at, {0x33});
    CHECK((r.read("SR") & all_errors) != 0);
    CHECK(r.word(at) == ~0u);
    r.clear_errors();
  }
  auto desc = l476rc();
  auto plain =
      std::ranges::find(desc.regions, std::string_view{"OPTION2"}, &popcorn::region_desc::name);
  REQUIRE(plain != desc.regions.end());
  plain->controller.reset();
  stm32_fixture     f{std::move(desc), firmware};
  constexpr address code = 0x2000'8000;
  REQUIRE(f.target->mem().write(code, 4, 0xbe01'6001u).has_value());
  for(auto const at : {address{0x1fff'f400}, address{0x1fff'f404}, address{0x1fff'f408}})
  {
    CAPTURE(at);
    f.set("r0", at);
    f.set("r1", 0x44);
    f.set("pc", code | 1u);
    auto const stop = f.run_to_stop({.max_instructions = icount{4}});
    CHECK(stop.reason == stop_reason::fault);
    REQUIRE(stop.fault.has_value());
    CHECK(stop.fault->fault_address == at);
    CHECK(f.target->mem().read(at, 4) == 0xffff'ffffu);
  }
}

TEST_CASE("RM0351 3.4.2, inferred: option register writes while OPTLOCK is set are ignored")
{
  rig r;
  REQUIRE(r.f.field("FLASH", "CR", "OPTLOCK") == 1);
  r.write("OPTR", 0);
  r.write("WRP1AR", 0);
  CHECK(r.read("OPTR") == optr_production);
  CHECK(r.read("WRP1AR") == wrp_production);
  r.control(optstrt);
  r.load_options();
  CHECK(r.read("WRP1AR") == wrp_production);
}

TEST_CASE(
    "RM0351 3.5.3: loaded WRP areas refuse program, page erase and bank erase with WRPERR, "
    "programmed ones protect nothing before the option byte load")
{
  rig r;
  r.unlock_options();
  r.write("WRP2AR", 0xff03'ff02u);
  r.write("WRP1BR", 0xff3f'ff3fu);
  r.program_options();
  r.program(bank2 + 2 * page_bytes, {0x1, 0x2});
  CHECK((r.read("SR") & all_errors) == 0);
  CHECK(r.words(bank2 + 2 * page_bytes, 2) == std::vector<std::uint32_t>{0x1, 0x2});
  r.load_options();
  r.program(bank2 + 3 * page_bytes, {0x3, 0x4});
  CHECK((r.read("SR") & all_errors) == wrperr);
  CHECK(r.words(bank2 + 3 * page_bytes, 2) == std::vector<std::uint32_t>{~0u, ~0u});
  r.clear_errors();
  r.program(bank2 + 4 * page_bytes, {0x5, 0x6});
  CHECK((r.read("SR") & all_errors) == 0);
  r.program(bank2 + page_bytes, {0x7, 0x8});
  CHECK((r.read("SR") & all_errors) == 0);
  r.program(bank1_last, {0x9, 0xa});
  CHECK((r.read("SR") & all_errors) == wrperr);
  r.clear_errors();
  r.control(per | bker | 2u << 3 | strt);
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == wrperr);
  CHECK(r.words(bank2 + 2 * page_bytes, 2) == std::vector<std::uint32_t>{0x1, 0x2});
  r.clear_errors();
  r.control(mer2 | strt);
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == wrperr);
  CHECK(r.words(bank2 + 4 * page_bytes, 2) == std::vector<std::uint32_t>{0x5, 0x6});
  r.clear_errors();
  r.control(per | bker | 1u << 3 | strt);
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == 0);
  CHECK(r.words(bank2 + page_bytes, 2) == std::vector<std::uint32_t>{~0u, ~0u});
}

TEST_CASE("RM0351 3.7.6: OBL_LAUNCH requests an option byte loader reset that loads the options")
{
  rig r;
  r.unlock_options();
  r.write("WRP1AR", 0xff10'ff10u);
  r.program_options();
  r.control(obl_launch);
  CHECK(r.f.field("FLASH", "CR", "OBL_LAUNCH") == 0);
  REQUIRE(r.requested_reset());
  REQUIRE(r.f.target->apply_reset_request());
  CHECK(r.read("WRP1AR") == 0xff10'ff10u);
  CHECK(r.f.field("FLASH", "CR", "OPTLOCK") == 1);
  CHECK(r.warnings("CR") == 0);
}

TEST_CASE(
    "ES0250 2.2.23: OBL_LAUNCH with MSI at 8 MHz or more warns once and loads, under "
    "strict_peripherals raises with FLASH_CR kept")
{
  rig r;
  r.unlock_options();
  r.msi_hz(7, 8 * mhz);
  r.control(obl_launch);
  REQUIRE(r.warnings("CR") == 1);
  CHECK(r.logged.back().message.contains("ES0250 2.2.23"));
  CHECK(r.requested_reset());
  rig strict{strict_options()};
  strict.unlock_options();
  strict.msi_hz(7, 8 * mhz);
  auto const before = strict.read("CR");
  CHECK_THROWS_AS((void)strict.try_write("CR", before | obl_launch), popcorn::model_error);
  CHECK(strict.read("CR") == before);
  CHECK_FALSE(strict.requested_reset());
  strict.msi_hz(6, 4 * mhz);
  REQUIRE(strict.try_write("CR", before | obl_launch));
  CHECK(strict.requested_reset());
}

constexpr address sram1     = 0x2000'0000;
constexpr address sram2     = 0x1000'0000;
constexpr address otp       = 0x1fff'7000;
constexpr address option1   = 0x1fff'7800;
constexpr address system1   = 0x1fff'0000;
constexpr address bank1_low = 0x0800'0000;

class regression_counter final : public popcorn::testing::inert_model,
                                 public popcorn::periph::stm32::read_protection_listener
{
public:
  explicit regression_counter(int& count) : count_{count}
  {}

  void on_read_protection_regression() override
  {
    ++count_;
  }

private:
  int& count_;
};

auto counted_options(int& count) -> core::machine_options
{
  auto options = popcorn::testing::stm32_options();
  options.models["SYSCFG"] =
      [&count](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
        return std::make_shared<regression_counter>(count);
      };
  return options;
}

void program_rdp(rig const& r, std::uint32_t rdp)
{
  r.unlock_options();
  r.write("OPTR", (optr_production & ~0xffu) | rdp);
  r.program_options();
  REQUIRE((r.read("SR") & all_errors) == 0);
  r.load_options();
  REQUIRE(r.f.field("FLASH", "OPTR", "RDP") == rdp);
}

auto dap_refused(rig const& r, bool secure, address at) -> bool
{
  auto const read = r.f.target->mem(secure).read(at, 4);
  return not read.has_value() and read.error() == popcorn::bus_error::denied;
}

TEST_CASE(
    "RM0351 3.5.1 Table 15: at RDP level 1 a DAP read or write of the flash, the OTP area or SRAM2 "
    "is a bus error, the host view, the option bytes, the system memory, SRAM1 and the "
    "peripherals stay open")
{
  rig r;
  REQUIRE(r.f.target->mem().write(bank2, 4, 0x2222'2222u).has_value());
  REQUIRE(r.f.target->mem().write(sram2, 4, 0x1111'1111u).has_value());
  CHECK(r.f.target->mem(true).read(bank2, 4) == 0x2222'2222u);
  CHECK(r.f.target->mem(false).read(sram2, 4) == 0x1111'1111u);
  program_rdp(r, 0xbb);
  for(auto const secure : {true, false})
  {
    CAPTURE(secure);
    for(auto const at : {bank1_low, bank2, bank2_last, otp, otp + 0x3fc, sram2, sram2 + 0x7ffc})
    {
      CAPTURE(at);
      CHECK(dap_refused(r, secure, at));
    }
    CHECK_FALSE(r.f.target->mem(secure).write(sram2 + 4, 4, 1).has_value());
    CHECK_FALSE(r.f.target->mem(secure).write(bank2 + 4, 4, 1).has_value());
    CHECK(r.f.target->mem(secure).read(option1, 4) == 0xffef'f8bbu);
    CHECK(r.f.target->mem(secure).read(otp + 0x400, 4).has_value());
    CHECK(r.f.target->mem(secure).read(system1, 4).has_value());
    CHECK(r.f.target->mem(secure).read(sram1, 4).has_value());
    CHECK(r.f.target->mem(secure).read(r.at("SR"), 4).has_value());
    CHECK_FALSE(r.f.target->mem(secure).read(0, 4).has_value());
  }
  CHECK(r.f.target->mem().read(bank2, 4) == 0x2222'2222u);
  CHECK(r.f.target->mem().read(sram2, 4) == 0x1111'1111u);
  CHECK(r.f.target->mem().read(sram2 + 4, 4) == 0u);
  program_rdp(r, 0xbc);
  CHECK(r.f.target->mem().read(bank2, 4) == 0x2222'2222u);
  CHECK(dap_refused(r, true, bank2));
}

TEST_CASE(
    "RM0351 3.5.1: at RDP level 2 every DAP access is a bus error, OPTSTRT sets WRPERR and "
    "programs nothing")
{
  rig r;
  program_rdp(r, 0xcc);
  for(auto const secure : {true, false})
  {
    CAPTURE(secure);
    for(auto const at : {sram1, option1, system1, r.at("SR")})
    {
      CAPTURE(at);
      CHECK(dap_refused(r, secure, at));
    }
    CHECK_FALSE(r.f.target->mem(secure).write(sram1, 4, 1).has_value());
  }
  CHECK(r.f.target->mem().read(sram1, 4).has_value());
  r.unlock_options();
  r.write("OPTR", optr_production);
  r.write("WRP1AR", 0xff05'ff05u);
  r.program_options();
  CHECK(r.status("WRPERR") == 1);
  CHECK(r.read("OPTR") == 0xffef'f8ccu);
  CHECK(r.read("WRP1AR") == wrp_production);
  CHECK(r.word(option1) == 0xffef'f8ccu);
  CHECK(r.word(option1 + 0x18) == wrp_production);
  r.f.target->power_cycle();
  CHECK(r.f.field("FLASH", "OPTR", "RDP") == 0xcc);
  CHECK(dap_refused(r, true, sram1));
}

TEST_CASE(
    "RM0351 3.5.1: OPTSTRT from level 1 to RDP 0xAA with PCROP_RDP set erases both banks, "
    "keeps the OTP area, disables PCROP, programs the other options, calls the hook once")
{
  int count = 0;
  rig r{counted_options(count)};
  r.unlock_options();
  r.write("PCROP1SR", 0xffff'0004u);
  r.write("PCROP1ER", 0xffff'0008u);
  r.program_options();
  r.load_options();
  for(auto const at : {bank1_low + 0x5000, bank2 + 0x1800, otp, sram1 + 0x1'0000})
  {
    REQUIRE(r.f.target->mem().write(at, 4, 0x1234'5678u).has_value());
  }
  program_rdp(r, 0xbb);
  CHECK(r.f.target->mem().read(bank2 + 0x1800, 4) == 0x1234'5678u);
  CHECK(count == 0);
  r.unlock_options();
  r.write("OPTR", optr_production & ~(1u << 16));
  r.write("WRP1AR", 0xff05'ff05u);
  r.program_options();
  CHECK((r.read("SR") & all_errors) == 0);
  CHECK(count == 1);
  CHECK(r.word(bank1_low + 0x5000) == ~0u);
  CHECK(r.word(bank2 + 0x1800) == ~0u);
  CHECK(r.word(otp) == 0x1234'5678u);
  CHECK(r.word(sram1 + 0x1'0000) == 0x1234'5678u);
  CHECK(r.words(option1, 2) == std::vector<std::uint32_t>{0xffee'f8aau, 0x0011'0755u});
  CHECK(r.word(option1 + 0x08) == 0xffff'ffffu);
  CHECK(r.word(option1 + 0x10) == 0xffff'0000u);
  CHECK(r.word(option1 + 0x18) == 0xff05'ff05u);
  CHECK(r.f.field("FLASH", "OPTR", "RDP") == 0xaa);
  CHECK(dap_refused(r, true, bank2));
  r.load_options();
  CHECK(r.f.field("FLASH", "OPTR", "RDP") == 0xaa);
  CHECK(r.f.field("FLASH", "OPTR", "IWDG_SW") == 0);
  CHECK(r.read("PCROP1SR") == 0xffff'ffffu);
  CHECK(r.f.target->mem(true).read(bank2 + 0x1800, 4) == 0xffff'ffffu);
  program_rdp(r, 0xaa);
  CHECK(count == 1);
}

TEST_CASE(
    "RM0351 3.5.1 and 3.5.2: with PCROP_RDP clear the level 1 to level 0 regression keeps the "
    "pages of the bank 1 PCROP area and its PCROP words, and erases the rest of both banks")
{
  int count = 0;
  rig r{counted_options(count)};
  r.unlock_options();
  r.write("PCROP1SR", 0xffff'0200u);
  r.write("PCROP1ER", 0x7fff'037fu);
  r.program_options();
  r.load_options();
  REQUIRE(r.read("PCROP1ER") == 0x7fff'037fu);
  for(auto const page : {1u, 2u, 3u, 4u})
  {
    REQUIRE(r.f.target->mem().write(bank1_low + page * page_bytes + 8, 4, page).has_value());
  }
  REQUIRE(r.f.target->mem().write(bank2 + 2 * page_bytes, 4, 7).has_value());
  program_rdp(r, 0xbb);
  r.unlock_options();
  r.write("OPTR", optr_production);
  r.program_options();
  CHECK(count == 1);
  CHECK(r.word(bank1_low + page_bytes + 8) == ~0u);
  CHECK(r.word(bank1_low + 2 * page_bytes + 8) == 2u);
  CHECK(r.word(bank1_low + 3 * page_bytes + 8) == 3u);
  CHECK(r.word(bank1_low + 4 * page_bytes + 8) == ~0u);
  CHECK(r.word(bank2 + 2 * page_bytes) == ~0u);
  CHECK(r.word(option1 + 0x08) == 0xffff'0200u);
  CHECK(r.word(option1 + 0x10) == 0x7fff'037fu);
  r.load_options();
  CHECK(r.read("PCROP1ER") == 0x7fff'037fu);
  CHECK(r.f.field("FLASH", "OPTR", "RDP") == 0xaa);
}

constexpr std::uint32_t rderr    = 1u << 14;
constexpr std::uint32_t rderrie  = 1u << 26;
constexpr std::uint32_t progerr  = 1u << 3;
constexpr address       pcrop_at = bank1_low + 0x1'0000;

void program_pcrop1(rig const& r, std::uint32_t start, std::uint32_t end)
{
  r.unlock_options();
  r.write("PCROP1SR", 0xffff'0000u | start);
  r.write("PCROP1ER", 0xffff'0000u | end);
  r.program_options();
  r.load_options();
}

TEST_CASE(
    "RM0351 3.5.2 and 3.6: a CPU or DAP load of a PCROP area reads 0 and sets RDERR, RDERRIE "
    "raises FLASH_IRQn, a fetch from it runs, m.mem reads the data")
{
  rig r;
  REQUIRE(r.f.target->mem().write(pcrop_at, 4, 0x1234'5678u).has_value());
  REQUIRE(r.f.target->mem().write(pcrop_at + 0x100, 4, 0xbe01'202au).has_value());
  REQUIRE(r.f.target->mem().write(pcrop_at + page_bytes, 4, 0xabcdu).has_value());
  CHECK(r.load(pcrop_at).value == 0x1234'5678u);
  program_pcrop1(r, 0x2000, 0x20ff);
  CHECK(r.load(pcrop_at).value == 0u);
  CHECK(r.status("RDERR") == 1);
  CHECK(r.status("OPERR") == 0);
  CHECK_FALSE(r.pending());
  r.write("SR", rderr);
  CHECK(r.status("RDERR") == 0);
  CHECK(r.load(pcrop_at + page_bytes).value == 0xabcdu);
  CHECK(r.status("RDERR") == 0);
  r.control(rderrie);
  CHECK(r.load(pcrop_at + 0x7fc).value == 0u);
  CHECK(r.pending());
  r.control(0);
  r.write("SR", rderr);
  for(auto const secure : {true, false})
  {
    CAPTURE(secure);
    CHECK(r.f.target->mem(secure).read(pcrop_at, 4) == 0u);
    CHECK(r.status("RDERR") == 1);
    r.write("SR", rderr);
  }
  REQUIRE(r.f.target->mem(true).write(pcrop_at, 4, 0).has_value());
  CHECK(r.f.target->mem().read(pcrop_at, 4) == 0x1234'5678u);
  CHECK(r.status("RDERR") == 0);
  r.f.set("r0", 0);
  r.f.set("pc", (pcrop_at + 0x100) | 1u);
  REQUIRE(r.f.run_to_stop({.max_instructions = icount{4}}).reason == stop_reason::user);
  CHECK(r.f.target->regs().read(*r.f.target->regs().find("r0")) == 42);
  CHECK(r.status("RDERR") == 0);
  auto const saved = r.f.target->snapshot();
  rig        fresh;
  CHECK(fresh.load(pcrop_at).value == ~0u);
  fresh.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(fresh.load(pcrop_at).value == 0u);
}

TEST_CASE(
    "RM0351 3.5.2: a PCROP area refuses a program and the erase of its pages or bank with "
    "WRPERR, a shrinking option change keeps it, inferred, a growing one programs")
{
  rig r;
  REQUIRE(r.f.target->mem().write(pcrop_at, 4, 0x1234'5678u).has_value());
  program_pcrop1(r, 0x2010, 0x20ff);
  r.program(pcrop_at + 0x10, {1, 2});
  CHECK((r.read("SR") & all_errors) == 0);
  r.program(pcrop_at + 0x80, {1, 2});
  CHECK((r.read("SR") & all_errors) == wrperr);
  CHECK(r.words(pcrop_at + 0x80, 2) == erased(2));
  r.clear_errors();
  for(auto const erase : {per | pnb(32) | strt, mer1 | strt, mer1 | mer2 | strt})
  {
    CAPTURE(erase);
    r.control(erase);
    r.wait_idle();
    CHECK((r.read("SR") & all_errors) == wrperr);
    CHECK(r.word(pcrop_at + 0x10) == 1u);
    r.clear_errors();
  }
  r.control(per | pnb(33) | strt);
  r.wait_idle();
  CHECK((r.read("SR") & all_errors) == 0);
  r.erase_bank2();
  r.unlock_options();
  r.write("PCROP1SR", 0xffff'2020u);
  r.program_options();
  CHECK(r.word(0x1fff'7808) == 0xffff'2010u);
  r.write("PCROP1ER", 0xffff'20f0u);
  r.program_options();
  CHECK(r.word(0x1fff'7810) == 0xffff'20ffu);
  r.write("PCROP1SR", 0xffff'2000u);
  r.write("PCROP1ER", 0xffff'2100u);
  r.program_options();
  CHECK(r.word(0x1fff'7808) == 0xffff'2000u);
  CHECK(r.word(0x1fff'7810) == 0xffff'2100u);
}

TEST_CASE(
    "RM0351 3.3.1: an OTP double word programs once, a second program sets PROGERR even with "
    "zeros, a store to the rest of the information block sets WRPERR, inferred")
{
  rig r;
  r.program(otp + 8, {0x11, 0x22});
  CHECK((r.read("SR") & all_errors) == 0);
  CHECK(r.words(otp + 8, 2) == std::vector<std::uint32_t>{0x11, 0x22});
  r.program(otp + 8, {0, 0});
  CHECK((r.read("SR") & all_errors) == progerr);
  CHECK(r.words(otp + 8, 2) == std::vector<std::uint32_t>{0x11, 0x22});
  r.clear_errors();
  for(auto const at : {option1, address{0x1fff'7500}, address{otp + 0x400}, address{0x1fff'f808}})
  {
    CAPTURE(at);
    auto const before = r.words(at, 2);
    r.program(at, {0, 0});
    CHECK((r.read("SR") & all_errors) == wrperr);
    CHECK(r.words(at, 2) == before);
    r.clear_errors();
  }
  r.control(fstpg);
  r.store_words(otp + 0x10, {0x1, 0x2});
  r.wait_idle();
  CHECK((r.read("SR") & wrperr) == wrperr);
  CHECK(r.words(otp + 0x10, 2) == erased(2));
}

TEST_CASE(
    "RM0351 3.7.8: FLASH_OPTR.DUALBANK 0 makes the 256 KB flash one bank of 128 pages, a bank 2 "
    "erase erases nothing, warns once, raises under strict_peripherals, inferred")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    rig r{strict ? strict_options() : popcorn::testing::stm32_options()};
    r.unlock_options();
    r.write("OPTR", optr_production & ~(1u << 21));
    r.program_options();
    r.load_options();
    REQUIRE(r.f.field("FLASH", "OPTR", "DUALBANK") == 0);
    REQUIRE(r.f.target->mem().write(bank1_low + 100 * page_bytes, 4, 5).has_value());
    REQUIRE(r.f.target->mem().write(bank2 + page_bytes, 4, 6).has_value());
    REQUIRE(r.f.target->mem().write(bank2_last, 4, 7).has_value());
    r.control(per | pnb(100) | strt);
    r.wait_idle();
    CHECK((r.read("SR") & all_errors) == 0);
    CHECK(r.word(bank1_low + 100 * page_bytes) == ~0u);
    for(auto const erase : {per | bker | pnb(1) | strt, mer2 | strt})
    {
      CAPTURE(erase);
      if(strict)
      {
        auto const before = r.read("CR");
        CHECK_THROWS_AS((void)r.try_write("CR", erase), popcorn::model_error);
        CHECK(r.read("CR") == before);
        CHECK(r.status("BSY") == 0);
        continue;
      }
      r.control(erase);
      r.wait_idle();
      CHECK(r.word(bank2 + page_bytes) == 6u);
      CHECK(r.warnings("CR") == 1);
    }
    if(strict)
    {
      continue;
    }
    r.control(per | pnb(127) | strt);
    r.wait_idle();
    CHECK(r.word(bank2_last) == ~0u);
    constexpr address spin_at = 0x2000'8000;
    REQUIRE(r.f.target->mem().write(spin_at, 2, 0xe7feu).has_value());
    r.control(mer1 | strt);
    r.f.set("pc", spin_at | 1u);
    REQUIRE(
        r.f.run_to_stop(
               {.max_instructions = icount{10'000'000},
                .until_time =
                    popcorn::vtime{r.f.target->virtual_clock().time().ps + 30'000'000'000}})
            .reason
        == stop_reason::time);
    CHECK(r.status("BSY") == 0);
    CHECK((r.read("SR") & all_errors) == 0);
    CHECK(r.word(bank1_low) == ~0u);
    CHECK(r.word(bank2 + page_bytes) == ~0u);
  }
}

TEST_CASE(
    "popcorn deviation, services.hpp: a DAP load of the bank under a fast row lets the row "
    "finish, a DAP load of a torn double word takes no NMI")
{
  rig r;
  r.hclk_16mhz();
  r.erase_bank2();
  r.control(fstpg | eopie);
  auto const start = r.f.target->virtual_clock().time().ps;
  r.store_words(bank2, rig::row(0xc0));
  r.idle_for_ps(
      start + 3 * double_word_ps + double_word_ps / 2 - r.f.target->virtual_clock().time().ps);
  REQUIRE(r.status("BSY") == 1);
  CHECK(r.f.target->mem(true).read(bank2 + 1024, 4) == ~0u);
  r.wait_idle();
  CHECK(r.words(bank2, 64) == rig::row(0xc0));
  CHECK(r.status("EOP") == 1);
  CHECK(r.warnings("CR") == 0);
  r.control(0);
  constexpr address line = bank2 + 0x800;
  r.tear(line);
  CHECK(r.f.target->mem(true).read(line, 4).has_value());
  CHECK(r.read("ECCR") == 0);
  CHECK(r.load(line).nmi_count == 1);
}
}
