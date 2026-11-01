#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::testing::l152re;
using popcorn::testing::rcc_reset_write;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr auto          firmware  = std::string_view{"l152xe_l1_crc_ll"};
constexpr std::uint32_t crc_base  = 0x4002'3000;
constexpr std::uint32_t dr        = 0x00;
constexpr std::uint32_t idr       = 0x04;
constexpr std::uint32_t cr        = 0x08;
constexpr std::uint32_t one_word  = 0xDF8A'8A2B;
constexpr std::uint32_t two_words = 0x7D24'A31B;

auto options(bool strict) -> core::machine_options
{
  auto out               = popcorn::testing::stm32_options();
  out.strict_peripherals = strict;
  return out;
}

struct crc_rig
{
  explicit crc_rig(bool strict = true, part_description desc = l152re())
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
    f.set("pc", sym(firmware, entry));
    auto const stopped = f.run_to_stop({.max_instructions = icount{1'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f.log();
  }

  [[nodiscard]] auto warnings() const -> std::ptrdiff_t
  {
    return std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind);
  }

  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink =
      f.target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
};

auto with_crc(part_description desc, auto&& change) -> part_description
{
  auto found =
      std::ranges::find(desc.peripherals, std::string_view{"CRC"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  change(found->registers);
  return desc;
}
}

TEST_CASE("RM0038 Table 25: the L1 CRC has DR 0xFFFFFFFF, IDR 0 and CR 0, no INIT or POL")
{
  crc_rig const r;
  REQUIRE(r.f.target->model("CRC") != nullptr);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  CHECK(r.read(idr) == 0);
  CHECK(r.read(cr) == 0);
  auto const& desc = r.f.target->peripheral("CRC").description();
  CHECK(popcorn::find_register(desc, "INIT") == nullptr);
  CHECK(popcorn::find_register(desc, "POL") == nullptr);
  CHECK(r.logged.empty());
}

TEST_CASE("LL CRC: two words give CRC-32/MPEG-2, RESET reloads 0xFFFFFFFF, IDR keeps 8 bits")
{
  crc_rig const r;
  CHECK(
      r.run("words")
      == std::vector<std::uint32_t>{0xFFFF'FFFF, one_word, two_words, 0xFFFF'FFFF, 0xA5, 0x80});
  CHECK(r.logged.empty());
}

TEST_CASE("RM0038 4.3: CR.RESET reloads DR with 0xFFFFFFFF, keeps IDR, and reads 0")
{
  crc_rig const r;
  r.write(idr, 0x5A);
  r.write(dr, 0x1234'5678);
  REQUIRE(r.read(dr) == one_word);
  r.write(cr, 1);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  CHECK(r.read(cr) == 0);
  CHECK(r.read(idr) == 0x5A);
  r.write(dr, 0x1234'5678);
  CHECK(r.read(dr) == one_word);
}

TEST_CASE("a narrow DR write feeds the word its byte lanes repeat and logs once, inferred")
{
  for(auto const& [size, offset, value, expected] : {
          std::tuple{std::uint8_t{1}, 0u, 0xB0u,   0xB996'68F7u},
          std::tuple{std::uint8_t{1}, 3u, 0xB0u,   0xB996'68F7u},
          std::tuple{std::uint8_t{2}, 0u, 0xBEEFu, 0xCFBA'B472u},
          std::tuple{std::uint8_t{2}, 2u, 0xBEEFu, 0xCFBA'B472u}
  })
  {
    CAPTURE(size);
    CAPTURE(offset);
    crc_rig const r{false};
    r.write(dr + offset, value, size);
    CHECK(r.read(dr) == expected);
    r.write(dr + offset, value, size);
    CHECK(r.warnings() == 1);
  }
}

TEST_CASE("checklist 53: under strict peripherals a narrow DR write raises model_error, DR kept")
{
  crc_rig const r;
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().write(crc_base + dr, 1, 0xB0)), popcorn::model_error);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
}

TEST_CASE(
    "R-09b n4: a DMA byte copy into CRC_DR feeds one word per byte, its lanes repeated as RM0038 "
    "11.3.4 states")
{
  crc_rig const r{false};
  CHECK(r.run("dma_bytes") == std::vector<std::uint32_t>{0xBB06'E699});
  CHECK(r.warnings() == 1);
}

TEST_CASE("RM0038 4.4: a narrow DR or CR read logs once and returns the lanes, strict raises")
{
  crc_rig const r{false};
  r.write(dr, 0x1234'5678);
  CHECK(r.read(dr + 2, 2) == one_word >> 16);
  CHECK(r.read(dr + 1, 1) == ((one_word >> 8) & 0xFFu));
  CHECK(r.read(cr, 1) == 0);
  CHECK(r.warnings() == 1);

  crc_rig const strict;
  CHECK_THROWS_AS(
      static_cast<void>(strict.f.target->mem().read(crc_base + dr, 1)), popcorn::model_error);
  CHECK_THROWS_AS(
      static_cast<void>(strict.f.target->mem().read(crc_base + cr, 2)), popcorn::model_error);
  CHECK_THROWS_AS(
      static_cast<void>(strict.f.target->mem().write(crc_base + cr, 1, 1)), popcorn::model_error);
  CHECK(strict.read(dr) == 0xFFFF'FFFF);
}

TEST_CASE("CRC_IDR takes byte and half-word accesses with no log, as the CMSIS header declares")
{
  crc_rig const r;
  r.write(idr, 0xC3, 1);
  CHECK(r.read(idr, 1) == 0xC3);
  r.write(idr, 0x1234, 2);
  CHECK(r.read(idr, 2) == 0x34);
  CHECK(r.read(idr) == 0x34);
  CHECK(r.logged.empty());
}

TEST_CASE("CRCRST restores DR and IDR to their reset values")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  r.write(idr, 0x5A);
  rcc_reset_write(r.f, "AHBRSTR", "CRCRST", 1);
  rcc_reset_write(r.f, "AHBRSTR", "CRCRST", 0);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  CHECK(r.read(idr) == 0);
}

TEST_CASE("a snapshot after one word restores the running L1 CRC")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  auto const    saved = r.f.target->snapshot();
  crc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.read(dr) == one_word);
  other.write(dr, 0x9ABC'DEF0);
  CHECK(other.read(dr) == two_words);
}

TEST_CASE("checklist 39: the L1 CRC row refuses INIT, POL, POLYSIZE, REV_IN and REV_OUT")
{
  auto const add_register = [](std::string_view name, std::uint32_t offset) {
    return [=](std::vector<popcorn::register_desc>& registers) {
      auto& added  = registers.emplace_back();
      added.name   = name;
      added.offset = offset;
      added.width  = 32;
    };
  };
  auto const add_control = [](std::string_view name, std::uint8_t lsb) {
    return [=](std::vector<popcorn::register_desc>& registers) {
      auto control =
          std::ranges::find(registers, std::string_view{"CR"}, &popcorn::register_desc::name);
      REQUIRE(control != registers.end());
      auto& added = control->fields.emplace_back();
      added.name  = name;
      added.lsb   = lsb;
      added.width = 1;
    };
  };
  CHECK_THROWS_AS(
      crc_rig(true, with_crc(l152re(), add_register("INIT", 0x10))), popcorn::config_error);
  CHECK_THROWS_AS(
      crc_rig(true, with_crc(l152re(), add_register("POL", 0x14))), popcorn::config_error);
  for(auto const& [name, lsb] : {
          std::pair{std::string_view{"POLYSIZE"}, std::uint8_t{3}},
          std::pair{std::string_view{"REV_IN"},   std::uint8_t{5}},
          std::pair{std::string_view{"REV_OUT"},  std::uint8_t{7}}
  })
  {
    CAPTURE(name);
    CHECK_THROWS_AS(
        crc_rig(true, with_crc(l152re(), add_control(name, lsb))), popcorn::config_error);
  }
}
