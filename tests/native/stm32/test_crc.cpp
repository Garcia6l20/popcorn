#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint32_t crc_base     = 0x4002'3000;
constexpr std::uint32_t sec_crc_base = 0x5002'3000;
constexpr std::uint32_t dr           = 0x00;
constexpr std::uint32_t idr          = 0x04;
constexpr std::uint32_t cr           = 0x08;
constexpr std::uint32_t init         = 0x10;
constexpr std::uint32_t pol          = 0x14;
constexpr std::uint32_t cr_reset     = 1u << 0;
constexpr std::uint32_t cr_rev_out   = 1u << 7;
constexpr std::uint32_t one_word     = 0xDF8A'8A2B;
constexpr std::uint32_t two_words    = 0x7D24'A31B;

auto idle(bool strict) -> stm32_fixture
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = strict;
  stm32_fixture f{u575(), "rcc_ll", options};
  f.set("pc", popcorn::testing::sym("rcc_ll", "idle"));
  return f;
}

struct crc_rig
{
  explicit crc_rig(bool strict = true) : f{idle(strict)}
  {}

  [[nodiscard]] auto read(std::uint32_t offset, std::uint32_t base = crc_base) const
      -> std::uint32_t
  {
    auto const value = f.target->mem().read(base + offset, 4);
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::uint32_t offset, std::uint32_t value, std::uint32_t base = crc_base) const
  {
    REQUIRE(f.target->mem().write(base + offset, 4, value).has_value());
  }

  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink =
      f.target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
};

void check_reset_values(crc_rig const& r)
{
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  CHECK(r.read(idr) == 0);
  CHECK(r.read(cr) == 0);
  CHECK(r.read(init) == 0xFFFF'FFFF);
  CHECK(r.read(pol) == 0x04C1'1DB7);
}
}

TEST_CASE("CRC registers read their reset values")
{
  crc_rig const r;
  REQUIRE(r.f.target->model("CRC") != nullptr);
  check_reset_values(r);
  CHECK(r.logged.empty());
}

TEST_CASE("a 32-bit DR write feeds one word through the reset polynomial")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  CHECK(r.read(dr) == one_word);
  r.write(dr, 0x9ABC'DEF0);
  CHECK(r.read(dr) == two_words);
  CHECK(r.logged.empty());
}

TEST_CASE("CR.RESET reloads DR from INIT and reads 0")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  r.write(cr, cr_reset);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  CHECK(r.read(cr) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("an INIT write reloads DR at once and RESET reloads the new INIT")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  r.write(init, 0);
  CHECK(r.read(dr) == 0);
  r.write(dr, 0x1234'5678);
  CHECK(r.read(dr) == 0x188E'5750);
  r.write(cr, cr_reset);
  CHECK(r.read(dr) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("a POL write keeps DR")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  r.write(pol, 0x1021);
  CHECK(r.read(pol) == 0x1021);
  CHECK(r.read(dr) == one_word);
  CHECK(r.logged.empty());
}

TEST_CASE("IDR survives RESET and an INIT write")
{
  crc_rig const r;
  r.write(idr, 0xA5A5'A5A5);
  r.write(cr, cr_reset);
  r.write(init, 0x1234'5678);
  CHECK(r.read(idr) == 0xA5A5'A5A5);
  CHECK(r.logged.empty());
}

TEST_CASE("SEC_CRC reaches the same unit as CRC")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678, sec_crc_base);
  CHECK(r.read(dr) == one_word);
  r.write(dr, 0x9ABC'DEF0);
  CHECK(r.read(dr, sec_crc_base) == two_words);
  r.write(idr, 0x5A5A'5A5A, sec_crc_base);
  CHECK(r.read(idr) == 0x5A5A'5A5A);
  r.write(cr, cr_reset, sec_crc_base);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  CHECK(r.logged.empty());
}

TEST_CASE("a snapshot after one word restores the running CRC")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  auto const saved = r.f.target->snapshot();
  r.write(dr, 0x9ABC'DEF0);
  REQUIRE(r.read(dr) == two_words);
  r.f.target->restore(saved);
  CHECK(r.read(dr) == one_word);
  r.write(dr, 0x9ABC'DEF0);
  CHECK(r.read(dr) == two_words);

  crc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.write(dr, 0x9ABC'DEF0);
  CHECK(other.read(dr) == two_words);
  CHECK(r.logged.empty());
  CHECK(other.logged.empty());
}

TEST_CASE("a software reset restores every reset value")
{
  crc_rig const r;
  r.write(idr, 0xA5A5'A5A5);
  r.write(pol, 0x1021);
  r.write(init, 0);
  r.write(dr, 0x1234'5678);
  r.f.target->reset(popcorn::reset_kind::software);
  check_reset_values(r);
  CHECK(r.logged.empty());
}

TEST_CASE("narrow DR writes and non-default CR modes feed nothing and log once")
{
  crc_rig const r{false};
  REQUIRE(r.f.target->mem().write(crc_base + dr, 1, 0x31).has_value());
  REQUIRE(r.f.target->mem().write(crc_base + dr, 2, 0x3132).has_value());
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
  CHECK(r.logged[0].reg == "DR");

  r.write(cr, cr_rev_out);
  r.write(dr, 0x1234'5678);
  r.write(dr, 0x1234'5678);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  REQUIRE(r.logged.size() == 2);
  CHECK(r.logged[1].reg == "CR");

  r.write(cr, 0);
  r.write(dr, 0x1234'5678);
  CHECK(r.read(dr) == one_word);
  CHECK(r.logged.size() == 2);
}

TEST_CASE("a narrow write to a register other than DR is ignored and logs once")
{
  crc_rig const r{false};
  REQUIRE(r.f.target->mem().write(crc_base + idr, 1, 0x5A).has_value());
  REQUIRE(r.f.target->mem().write(crc_base + init + 2, 2, 0).has_value());
  CHECK(r.read(idr) == 0);
  CHECK(r.read(init) == 0xFFFF'FFFF);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
  CHECK(r.logged[0].reg == "IDR");
}

TEST_CASE("unmodelled CRC accesses raise model_error under strict peripherals")
{
  crc_rig const r;
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().write(crc_base + dr, 1, 0x31)), popcorn::model_error);
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().write(crc_base + idr, 2, 0x31)), popcorn::model_error);
  r.write(cr, cr_rev_out);
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().write(crc_base + dr, 4, 0x1234'5678)),
      popcorn::model_error);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
}
