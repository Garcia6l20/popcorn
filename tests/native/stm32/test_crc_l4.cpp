#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware = std::string_view{"l476_l4_crc_ll"};
constexpr std::uint32_t crc_base = 0x4002'3000;
constexpr std::uint32_t dr       = 0x00;
constexpr std::uint32_t idr      = 0x04;
constexpr std::uint32_t cr       = 0x08;
constexpr std::uint32_t init     = 0x10;
constexpr std::uint32_t pol      = 0x14;

auto options(bool strict) -> core::machine_options
{
  auto out               = popcorn::testing::stm32_options();
  out.strict_peripherals = strict;
  return out;
}

struct crc_rig
{
  explicit crc_rig(bool strict = false, part_description desc = l476rc())
      : f{std::move(desc), firmware, options(strict)}
  {
    f.set("pc", sym(firmware, "idle"));
  }

  [[nodiscard]] auto read(std::uint32_t offset, std::uint8_t size = 4) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(crc_base + offset, size);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::uint32_t offset, std::uint32_t value, std::uint8_t size = 4) const
  {
    REQUIRE(f.target->mem().write(crc_base + offset, size, value).has_value());
  }

  [[nodiscard]] auto run(std::string_view entry) const -> std::vector<std::uint32_t>
  {
    if(entry == "main")
    {
      f.target->reset();
    }
    else
    {
      f.set("pc", sym(firmware, entry));
    }
    auto const stopped = f.run_to_stop({.max_instructions = icount{1'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f.log();
  }

  [[nodiscard]] auto citing(std::string_view rule) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.kind == core::log_kind::model_warning
         and record.message.find(rule) != std::string::npos;
    });
  }

  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink =
      f.target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
};
}

TEST_CASE("RM0351 15.4.6: the L4 CRC resets DR and INIT to 0xFFFFFFFF, POL to 0x04C11DB7")
{
  crc_rig const r;
  CHECK(r.read(dr) == 0xFFFF'FFFFu);
  CHECK(r.read(idr) == 0);
  CHECK(r.read(cr) == 0);
  CHECK(r.read(init) == 0xFFFF'FFFFu);
  CHECK(r.read(pol) == 0x04C1'1DB7u);
}

TEST_CASE("LL CRC: two words give CRC-32/MPEG-2, RESET reloads INIT, IDR keeps its byte")
{
  crc_rig const r;
  CHECK(r.run("main") == std::vector<std::uint32_t>{0x7D24'A31Bu, 0xFFFF'FFFFu, 0xA5});
}

TEST_CASE("RM0351 15.3.3: POLYSIZE 7 with POL 0x09 and INIT 0 gives CRC-7/MMC 0x75")
{
  crc_rig const r;
  CHECK(r.run("poly7") == std::vector<std::uint32_t>{0x75});
}

TEST_CASE("RM0351 15.4.3: REV_IN by byte with REV_OUT gives the reflected CRC-32 of the check")
{
  crc_rig const r;
  CHECK(r.run("reversed") == std::vector<std::uint32_t>{0x340B'C6D9u});
}

TEST_CASE("RM0351 15.4.2: CRC_IDR holds 8 bits and takes byte and half-word accesses, no log")
{
  crc_rig const r{true};
  r.write(idr, 0x1A5);
  CHECK(r.read(idr) == 0xA5);
  r.write(idr, 0x3C, 1);
  CHECK(r.read(idr, 1) == 0x3C);
  r.write(idr, 0x5A, 2);
  CHECK(r.read(idr, 2) == 0x5A);
  CHECK(r.logged.empty());
}

TEST_CASE("RM0351 15.4: a narrow CR read and an even polynomial log once citing RM0351")
{
  crc_rig const r;
  static_cast<void>(r.read(cr, 1));
  static_cast<void>(r.read(cr, 1));
  CHECK(r.citing("RM0351 15.4") == 1);
  r.write(pol, 0x04C1'1DB6u);
  r.write(dr, 0x1234'5678u);
  r.write(dr, 0x1234'5678u);
  CHECK(r.citing("RM0351 15.3.3") == 1);
  CHECK(r.citing("RM0456") == 0);
  crc_rig const strict{true};
  CHECK_THROWS_AS(
      static_cast<void>(strict.f.target->mem().read(crc_base + cr, 1)), popcorn::model_error);
}

TEST_CASE("checklist 39: the L4 CRC row needs CRC_INIT, an L4 part takes the L4 row")
{
  auto desc = l476rc();
  auto found =
      std::ranges::find(desc.peripherals, std::string_view{"CRC"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  REQUIRE(std::erase_if(found->registers, [](auto const& one) { return one.name == "INIT"; }) == 1);
  CHECK_THROWS_WITH_AS((crc_rig{false, desc}), doctest::Contains("INIT"), popcorn::config_error);
}
