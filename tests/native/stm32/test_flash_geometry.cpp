#include "gtzc.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::description_from;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

auto u5_2m() -> popcorn::part_description const&
{
  static auto const desc = description_from("POPCORN_U575_2M_DESC");
  return desc;
}

auto h5_1m() -> popcorn::part_description const&
{
  static auto const desc = description_from("POPCORN_H5_1M_DESC");
  return desc;
}

constexpr address       flash_start = 0x0800'0000;
constexpr address       low_line    = flash_start + 0x4'0000;
constexpr std::uint32_t page_bytes  = 0x2000;
constexpr address       u5_bank2    = 0x0810'0000;
constexpr address       h5_bank2    = 0x0808'0000;
constexpr address       loop_at     = 0x2000'0500;
constexpr std::uint32_t secure_bit  = 0x1000'0000;

constexpr std::uint32_t u5_per   = 1u << 1;
constexpr std::uint32_t u5_mer1  = 1u << 2;
constexpr std::uint32_t u5_bker  = 1u << 11;
constexpr std::uint32_t u5_mer2  = 1u << 15;
constexpr std::uint32_t u5_strt  = 1u << 16;
constexpr std::uint32_t u5_pg    = 1u << 0;
constexpr std::uint32_t h5_pg    = 1u << 1;
constexpr std::uint32_t h5_ser   = 1u << 2;
constexpr std::uint32_t h5_ber   = 1u << 3;
constexpr std::uint32_t h5_strt  = 1u << 5;
constexpr std::uint32_t h5_mer   = 1u << 15;
constexpr std::uint32_t h5_bksel = 1u << 31;

auto u5_pnb(std::uint32_t page) -> std::uint32_t
{
  return page << 3;
}

auto h5_snb(std::uint32_t sector) -> std::uint32_t
{
  return sector << 6;
}

auto erased(std::size_t count) -> std::vector<std::uint32_t>
{
  return std::vector<std::uint32_t>(count, ~0u);
}

auto pattern() -> std::vector<std::uint32_t>
{
  return {0x1111'1111, 0x2222'2222, 0x3333'3333, 0x4444'4444};
}

auto timed_board(stm32::option_bytes const& wanted = {}) -> stm32::board
{
  stm32::board board;
  board.flash.option_bytes = wanted;
  board.flash.bank_erase   = std::chrono::microseconds{200};
  board.flash.mass_erase   = std::chrono::microseconds{400};
  return board;
}

struct family
{
  std::string_view firmware;
  std::string_view key;
  std::string_view control;
  std::string_view status;
  std::uint32_t    busy;
  std::uint32_t    program;
};

constexpr family u5_family{"flash_ll", "FLASH_NSKEYR", "FLASH_NSCR", "FLASH_NSSR", 1u << 16, u5_pg};
constexpr family h5_family{"h573_flash_ll", "FLASH_NSKEYR", "FLASH_NSCR",
                           "FLASH_NSSR",    1u << 16,       h5_pg};

struct rig
{
  family const&                 kind;
  stm32_fixture                 f;
  std::vector<core::log_record> logged{};
  core::hook_handle             sink{};

  rig(popcorn::part_description const& desc, family const& which, stm32::board const& board)
      : kind{which}, f{desc, which.firmware, options_for(desc, board)}
  {
    sink = f.target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(f.target->mem().write(loop_at, 4, 0xE7FD'BF30).has_value());
    write(kind.key, 0x4567'0123u);
    write(kind.key, 0xcdef'89abu);
  }

  static auto options_for(popcorn::part_description const& desc, stm32::board const& board)
      -> core::machine_options
  {
    auto options = popcorn::testing::stm32_options();
    for(auto& [name, factory] : stm32::factories(board, desc))
    {
      options.models.emplace(name, std::move(factory));
    }
    return options;
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

  void write(std::string_view name, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(name), 4, value).has_value());
  }

  void control(std::uint32_t value) const
  {
    write(kind.control, value);
  }

  [[nodiscard]] auto busy() const -> bool
  {
    return (read(kind.status) & kind.busy) != 0;
  }

  void call(std::string_view entry, std::initializer_list<std::uint32_t> args) const
  {
    auto index = 0;
    for(auto const arg : args)
    {
      f.set(std::format("r{}", index++), arg);
    }
    f.set("pc", sym(kind.firmware, entry));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
  }

  void wait_idle() const
  {
    call("wait_idle", {});
  }

  void program(address to, std::vector<std::uint32_t> const& values) const
  {
    control(kind.program);
    auto const buffer = sym(kind.firmware, "words");
    for(std::size_t i = 0; i < values.size(); ++i)
    {
      REQUIRE(f.target->mem().write(buffer + 4 * i, 4, values[i]).has_value());
    }
    call(
        "copy_words", {static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(buffer),
                       static_cast<std::uint32_t>(values.size())});
    wait_idle();
    control(0);
  }

  void run(std::uint32_t command) const
  {
    control(command);
    f.set("pc", loop_at | 1u);
    auto const until = popcorn::vtime{f.target->virtual_clock().time().ps + 3'000'000'000u};
    REQUIRE(
        f.run_to_stop({.max_instructions = popcorn::icount{50'000'000}, .until_time = until}).reason
        == stop_reason::time);
    control(0);
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

  [[nodiscard]] auto holds(address line) const -> bool
  {
    return words(line, 4) == pattern();
  }

  [[nodiscard]] auto is_erased(address line) const -> bool
  {
    return words(line, 4) == erased(4);
  }

  [[nodiscard]] auto warnings(std::string_view text) const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count_if(logged, [text](core::log_record const& record) {
          return record.kind == core::log_kind::model_warning
             and record.peripheral == "FLASH"
             and record.message.contains(text);
        }));
  }
};

struct secure_rig
{
  std::unique_ptr<core::machine> target;

  secure_rig(popcorn::part_description const& desc, stm32::option_bytes const& wanted)
  {
    stm32::register_builtin_models();
    auto options = popcorn::testing::stm32_options();
    REQUIRE(stm32::configure_option_bytes(desc, wanted, options).has_value());
    options.raz_wi = stm32::raz_wi_map(desc);
    target = std::make_unique<core::machine>(desc, popcorn::arch::armv8m::factory(), options);
    target->reset();
  }

  [[nodiscard]] auto at(std::string_view name) const -> address
  {
    auto const& desc  = target->peripheral("FLASH").description();
    auto const  found = popcorn::find_register(desc, name);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  void write(std::string_view name, std::uint32_t value) const
  {
    REQUIRE(target->mem(true).write(at(name) + secure_bit, 4, value).has_value());
  }

  [[nodiscard]] auto peek(std::string_view name) const -> std::uint32_t
  {
    auto const& regs  = target->peripheral("FLASH");
    auto const  found = popcorn::find_register(regs.description(), name);
    REQUIRE(found != nullptr);
    return regs.peek(found->offset & ~3u);
  }

  [[nodiscard]] auto policy(address page) const -> stm32::page_policy
  {
    auto const* const pages = dynamic_cast<stm32::secure_page_controller*>(target->model("FLASH"));
    REQUIRE(pages != nullptr);
    auto const rule = pages->page_rule(page);
    REQUIRE(rule.has_value());
    return *rule;
  }

  [[nodiscard]] auto secure(address page) const -> bool
  {
    return policy(page).secure;
  }
};

auto without_register(popcorn::part_description desc, std::string_view name)
    -> popcorn::part_description
{
  auto flash = std::ranges::find(
      desc.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
  REQUIRE(flash != desc.peripherals.end());
  REQUIRE(
      std::erase_if(
          flash->registers, [name](popcorn::register_desc const& reg) { return reg.name == name; })
      == 1);
  return desc;
}

auto builds(popcorn::part_description const& desc) -> bool
{
  stm32::register_builtin_models();
  try
  {
    core::machine const target{
        desc, popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()};
    return true;
  }
  catch(popcorn::config_error const&)
  {
    return false;
  }
}
}

TEST_CASE("a 2 MB U5 reads FLASHSIZE 0x0800 from a 2 MB flash region")
{
  rig const  r{u5_2m(), u5_family, timed_board()};
  auto const size = r.f.target->mem().read(0x0bfa'07a0, 2);
  REQUIRE(size.has_value());
  CHECK(*size == 0x0800);
  CHECK(std::ranges::any_of(u5_2m().regions, [](popcorn::region_desc const& region) {
    return region.controller == "FLASH"
       and region.start == flash_start
       and region.size == 0x20'0000;
  }));
}

TEST_CASE("a 2 MB U5 erases page 127 of bank 2 with PNB 127 and BKER, bank 1 page 127 stays")
{
  rig const         r{u5_2m(), u5_family, timed_board()};
  constexpr address bank2_last = u5_bank2 + 127 * page_bytes;
  constexpr address bank1_last = flash_start + 127 * page_bytes;
  REQUIRE(bank2_last == 0x081f'e000);
  for(auto const line : {bank2_last, bank1_last, u5_bank2, bank2_last - page_bytes})
  {
    r.program(line, pattern());
  }
  r.run(u5_per | u5_bker | u5_pnb(127) | u5_strt);
  CHECK(r.is_erased(bank2_last));
  CHECK(r.is_erased(bank2_last + page_bytes - 16));
  CHECK(r.holds(bank1_last));
  CHECK(r.holds(u5_bank2));
  CHECK(r.holds(bank2_last - page_bytes));
  r.run(u5_per | u5_pnb(127) | u5_strt);
  CHECK(r.is_erased(bank1_last));
  CHECK(r.holds(u5_bank2));
}

TEST_CASE("a 2 MB U5 warns once for a PNB of 128 and erases nothing")
{
  rig const r{u5_2m(), u5_family, timed_board()};
  r.program(u5_bank2, pattern());
  r.program(low_line, pattern());
  r.control(u5_per | u5_bker | u5_pnb(128) | u5_strt);
  CHECK_FALSE(r.busy());
  CHECK(r.warnings("PNB 128 is past the 128 pages of a bank") == 1);
  r.control(0);
  r.control(u5_per | u5_pnb(128) | u5_strt);
  CHECK(r.warnings("PNB 128") == 1);
  r.control(0);
  CHECK(r.holds(u5_bank2));
  CHECK(r.holds(low_line));
}

TEST_CASE("a 2 MB U5 MER2 erases 0x08100000 to 0x081FFFFF and MER1 the other half")
{
  rig const         r{u5_2m(), u5_family, timed_board()};
  constexpr address lines[] = {low_line, u5_bank2 - 16, u5_bank2, 0x081f'fff0};
  for(auto const line : lines)
  {
    r.program(line, pattern());
  }
  r.run(u5_mer2 | u5_strt);
  CHECK(r.holds(low_line));
  CHECK(r.holds(u5_bank2 - 16));
  CHECK(r.is_erased(u5_bank2));
  CHECK(r.is_erased(0x081f'fff0));
  for(auto const line : {u5_bank2, 0x081f'fff0u})
  {
    r.program(line, pattern());
  }
  r.run(u5_mer1 | u5_strt);
  CHECK(r.is_erased(low_line));
  CHECK(r.is_erased(u5_bank2 - 16));
  CHECK(r.holds(u5_bank2));
  CHECK(r.holds(0x081f'fff0));
}

TEST_CASE("a 2 MB U5 with SWAP_BANK 1 swaps the two halves for PNB, BKER, MER1 and MER2")
{
  stm32::option_bytes const swapped{.swap_bank = true};
  rig const                 r{u5_2m(), u5_family, timed_board(swapped)};
  constexpr address         low_last  = flash_start + 127 * page_bytes;
  constexpr address         high_last = u5_bank2 + 127 * page_bytes;
  for(auto const line : {low_last, high_last, low_line, u5_bank2})
  {
    r.program(line, pattern());
  }
  r.run(u5_per | u5_pnb(127) | u5_strt);
  CHECK(r.is_erased(high_last));
  CHECK(r.holds(low_last));
  r.run(u5_per | u5_bker | u5_pnb(127) | u5_strt);
  CHECK(r.is_erased(low_last));
  CHECK(r.holds(u5_bank2));
  r.program(low_last, pattern());
  r.program(high_last, pattern());
  r.run(u5_mer1 | u5_strt);
  CHECK(r.is_erased(u5_bank2));
  CHECK(r.is_erased(high_last));
  CHECK(r.holds(low_line));
  CHECK(r.holds(low_last));
  r.program(u5_bank2, pattern());
  r.run(u5_mer2 | u5_strt);
  CHECK(r.is_erased(low_line));
  CHECK(r.is_erased(low_last));
  CHECK(r.holds(u5_bank2));
}

TEST_CASE("a 2 MB U5 SECWM2R range 0 to 127 secures every page of bank 2 and none of bank 1")
{
  secure_rig const r{
      u5_2m(),
      {.tzen   = true,
          .secwm1 = stm32::no_area{},
          .secwm2 = stm32::page_range{.first = 0, .last = 127}}
  };
  CHECK(((r.peek("FLASH_SECWM2R1") >> 16) & 0x7f) == 127);
  CHECK((r.peek("FLASH_SECWM2R1") & 0x7f) == 0);
  CHECK(r.secure(u5_bank2));
  CHECK(r.secure(u5_bank2 + 127 * page_bytes));
  CHECK(r.secure(u5_bank2 + 126 * page_bytes));
  CHECK_FALSE(r.secure(flash_start + 127 * page_bytes));
  CHECK_FALSE(r.secure(flash_start));
}

TEST_CASE("a 2 MB U5 SEC2BBR4 bit 31 secures page 127 of bank 2 with the watermarks off")
{
  secure_rig const r{
      u5_2m(), {.tzen = true, .secwm1 = stm32::no_area{}, .secwm2 = stm32::no_area{}}
  };
  CHECK_FALSE(r.secure(u5_bank2 + 127 * page_bytes));
  r.write("SEC2BBR4", 1u << 31);
  CHECK(r.peek("SEC2BBR4") == 1u << 31);
  CHECK(r.secure(u5_bank2 + 127 * page_bytes));
  CHECK_FALSE(r.secure(u5_bank2 + 126 * page_bytes));
  CHECK_FALSE(r.secure(u5_bank2 + 95 * page_bytes));
  CHECK_FALSE(r.secure(flash_start + 127 * page_bytes));
  r.write("SEC1BBR4", 1u << 30);
  CHECK(r.secure(flash_start + 126 * page_bytes));
  CHECK_FALSE(r.secure(flash_start + 127 * page_bytes));
}

TEST_CASE(
    "a 2 MB U5 description without a block register its 128-page bank needs throws config_error, "
    "one past the need builds")
{
  for(auto const& [name, throws] : {
          std::pair{std::string_view{"SEC2BBR4"},  true },
          std::pair{std::string_view{"PRIV1BBR4"}, true },
          std::pair{std::string_view{"SEC2BBR5"},  false},
          std::pair{std::string_view{"PRIV1BBR5"}, false},
  })
  {
    CAPTURE(name);
    CHECK(builds(without_register(u5_2m(), name)) == not throws);
  }
}

TEST_CASE("a 1 MB H5 erases sector 63 of bank 2 at 0x080FE000, sector 64 is past the bank")
{
  rig const         r{h5_1m(), h5_family, timed_board()};
  constexpr address bank2_last = h5_bank2 + 63 * page_bytes;
  constexpr address bank1_last = flash_start + 63 * page_bytes;
  REQUIRE(bank2_last == 0x080f'e000);
  for(auto const line : {bank2_last, bank1_last, h5_bank2, bank2_last - page_bytes})
  {
    r.program(line, pattern());
  }
  r.run(h5_ser | h5_bksel | h5_snb(63) | h5_strt);
  CHECK(r.is_erased(bank2_last));
  CHECK(r.is_erased(bank2_last + page_bytes - 16));
  CHECK(r.holds(bank1_last));
  CHECK(r.holds(h5_bank2));
  CHECK(r.holds(bank2_last - page_bytes));
  r.control(h5_ser | h5_bksel | h5_snb(64) | h5_strt);
  CHECK(r.warnings("SNB 64 is past the 64 pages of a bank") == 1);
  r.control(0);
  CHECK(r.holds(h5_bank2));
  CHECK(r.holds(bank1_last));
}

TEST_CASE("a 1 MB H5 BER with BKSEL erases 0x08080000 to 0x080FFFFF, MER both banks")
{
  rig const r{h5_1m(), h5_family, timed_board()};
  for(auto const line : {low_line, h5_bank2 - 16, h5_bank2, 0x080f'fff0u})
  {
    r.program(line, pattern());
  }
  r.run(h5_ber | h5_bksel | h5_strt);
  CHECK(r.holds(low_line));
  CHECK(r.holds(h5_bank2 - 16));
  CHECK(r.is_erased(h5_bank2));
  CHECK(r.is_erased(0x080f'fff0));
  r.program(h5_bank2, pattern());
  r.run(h5_mer | h5_strt);
  CHECK(r.is_erased(low_line));
  CHECK(r.is_erased(h5_bank2 - 16));
  CHECK(r.is_erased(h5_bank2));
}

TEST_CASE(
    "a 1 MB H5 secures sector 63 of bank 2 by FLASH_SECBB2R2 bit 31 or by SECWM2R range 0 to 63")
{
  {
    secure_rig const r{
        h5_1m(), {.tzen = true, .secwm1 = stm32::no_area{}, .secwm2 = stm32::no_area{}}
    };
    r.write("SECBB2R2", 1u << 31);
    CHECK(r.secure(h5_bank2 + 63 * page_bytes));
    CHECK_FALSE(r.secure(h5_bank2 + 62 * page_bytes));
    CHECK_FALSE(r.secure(flash_start + 63 * page_bytes));
  }
  secure_rig const r{
      h5_1m(),
      {.tzen   = true,
          .secwm1 = stm32::no_area{},
          .secwm2 = stm32::page_range{.first = 0, .last = 63}}
  };
  CHECK(r.secure(h5_bank2));
  CHECK(r.secure(h5_bank2 + 63 * page_bytes));
  CHECK_FALSE(r.secure(flash_start + 63 * page_bytes));
}

TEST_CASE("a 1 MB H5 refuses a SECWM2 sector 64 naming 0 to 63 and accepts 63")
{
  auto const refused = stm32::check_option_bytes(
      h5_1m(), {
                   .tzen = true, .secwm2 = stm32::page_range{.first = 0, .last = 64}
  });
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().key == "option_bytes.secwm2");
  CHECK(refused.error().expected.contains("from 0 to 63"));
  CHECK(
      stm32::check_option_bytes(
          h5_1m(),
          {
              .tzen = true, .secwm2 = stm32::page_range{.first = 0, .last = 63}
  })
          .has_value());
}

TEST_CASE(
    "a 1 MB H5 description expects block registers 1 and 2 of 4 and needs no 3 and 4: removing "
    "the second throws, the third builds")
{
  for(auto const& [name, throws] : {
          std::pair{std::string_view{"SECBB2R2"},  true },
          std::pair{std::string_view{"PRIVBB1R2"}, true },
          std::pair{std::string_view{"SECBB2R3"},  false},
          std::pair{std::string_view{"PRIVBB1R3"}, false},
          std::pair{std::string_view{"SECBB2R4"},  false},
          std::pair{std::string_view{"PRIVBB1R4"}, false},
  })
  {
    CAPTURE(name);
    CHECK(builds(without_register(h5_1m(), name)) == not throws);
  }
}
