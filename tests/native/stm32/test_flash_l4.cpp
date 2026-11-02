#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
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
