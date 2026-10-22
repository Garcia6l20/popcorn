#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint32_t    crc_base     = 0x4002'3000;
constexpr std::uint32_t    sec_crc_base = 0x5002'3000;
constexpr std::uint32_t    dr           = 0x00;
constexpr std::uint32_t    idr          = 0x04;
constexpr std::uint32_t    cr           = 0x08;
constexpr std::uint32_t    init         = 0x10;
constexpr std::uint32_t    pol          = 0x14;
constexpr std::uint32_t    cr_reset     = 1u << 0;
constexpr std::uint32_t    cr_poly16    = 1u << 3;
constexpr std::uint32_t    cr_poly8     = 2u << 3;
constexpr std::uint32_t    cr_poly7     = 3u << 3;
constexpr std::uint32_t    cr_rev_byte  = 1u << 5;
constexpr std::uint32_t    cr_rev_half  = 2u << 5;
constexpr std::uint32_t    cr_rev_word  = 3u << 5;
constexpr std::uint32_t    cr_rev_out   = 1u << 7;
constexpr std::string_view check_input  = "123456789";
constexpr std::uint32_t    one_word     = 0xDF8A'8A2B;
constexpr std::uint32_t    two_words    = 0x7D24'A31B;

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

  void store(std::uint32_t offset, std::uint8_t size, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(crc_base + offset, size, value).has_value());
  }

  void feed_bytes(std::string_view bytes) const
  {
    for(auto const byte : bytes)
    {
      store(dr, 1, static_cast<std::uint8_t>(byte));
    }
  }

  void configure(std::uint32_t control, std::uint32_t polynomial, std::uint32_t initial) const
  {
    write(cr, control);
    write(pol, polynomial);
    write(init, initial);
    write(cr, control | cr_reset);
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

TEST_CASE("byte writes feed CRC-32/MPEG-2")
{
  crc_rig const r;
  r.feed_bytes(check_input);
  CHECK(r.read(dr) == 0x0376'E6E7);
  CHECK(r.logged.empty());
}

TEST_CASE("word, half-word and byte writes mix in one CRC")
{
  crc_rig const r;
  r.feed_bytes("12345");
  CHECK(r.read(dr) == 0xBD9A'B747);
  r.write(cr, cr_reset);
  r.write(dr, 0x3132'3334);
  r.store(dr, 1, 0x35);
  CHECK(r.read(dr) == 0xBD9A'B747);
  r.write(cr, cr_reset);
  r.store(dr, 2, 0x3132);
  r.store(dr, 2, 0x3334);
  r.store(dr, 1, 0x35);
  CHECK(r.read(dr) == 0xBD9A'B747);
  CHECK(r.logged.empty());
}

TEST_CASE("REV_IN reverses a word by byte, half-word or word, RM0456 24.3.3")
{
  crc_rig const r;
  for(auto const& [control, expected] : {
          std::pair{cr_rev_byte, 0xD35E'9975u},
          std::pair{cr_rev_half, 0xADC6'814Bu},
          std::pair{cr_rev_word, 0x271D'6588u},
  })
  {
    r.write(cr, control | cr_reset);
    r.write(dr, 0x1A2B'3C4D);
    CHECK(r.read(dr) == expected);
  }
  CHECK(r.logged.empty());
}

TEST_CASE("REV_OUT reverses the DR read and keeps the running CRC")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  r.write(cr, cr_rev_out);
  CHECK(r.read(dr) == 0xD451'51FB);
  CHECK(r.f.target->mem().read(crc_base + dr, 2) == 0x51FB);
  r.write(cr, 0);
  CHECK(r.read(dr) == one_word);
  r.write(dr, 0x9ABC'DEF0);
  CHECK(r.read(dr) == two_words);
  CHECK(r.logged.empty());
}

TEST_CASE("REV_IN by byte and REV_OUT feed CRC-32/ISO-HDLC")
{
  crc_rig const r;
  r.write(cr, cr_rev_byte | cr_rev_out | cr_reset);
  r.feed_bytes(check_input);
  CHECK((r.read(dr) ^ 0xFFFF'FFFF) == 0xCBF4'3926);
  CHECK(r.logged.empty());
}

TEST_CASE("POLYSIZE 16, 8 and 7 compute in the low bits of DR")
{
  crc_rig const r;
  r.configure(cr_poly16, 0x1021, 0xFFFF);
  CHECK(r.read(dr) == 0xFFFF);
  r.feed_bytes(check_input);
  CHECK(r.read(dr) == 0x0000'29B1);
  CHECK(r.f.target->mem().read(crc_base + dr, 2) == 0x29B1);

  r.configure(cr_poly8, 0x07, 0);
  r.feed_bytes(check_input);
  CHECK(r.read(dr) == 0x0000'00F4);

  r.configure(cr_poly7, 0x09, 0);
  r.feed_bytes(check_input);
  CHECK(r.read(dr) == 0x0000'0075);
  CHECK(r.logged.empty());
}

TEST_CASE("INIT and RESET load the low POLYSIZE bits of INIT")
{
  crc_rig const r;
  r.write(cr, cr_poly8);
  r.write(init, 0x1234'5678);
  CHECK(r.read(dr) == 0x78);
  r.write(dr, 0xFFFF'FFFF);
  r.write(cr, cr_poly8 | cr_reset);
  CHECK(r.read(dr) == 0x78);
  CHECK(r.logged.empty());
}

TEST_CASE("an even polynomial logs once and computes, a model_error when strict")
{
  {
    crc_rig const r{false};
    r.write(pol, 0x04C1'1DB6);
    r.write(dr, 0x1234'5678);
    CHECK(r.read(dr) != 0xFFFF'FFFF);
    r.write(dr, 0x1234'5678);
    REQUIRE(r.logged.size() == 1);
    CHECK(r.logged[0].kind == core::log_kind::model_warning);
    CHECK(r.logged[0].reg == "POL");
  }
  crc_rig const r;
  r.write(pol, 0x04C1'1DB6);
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().write(crc_base + dr, 4, 0x1234'5678)),
      popcorn::model_error);
}

TEST_CASE("a snapshot with POLYSIZE 16, REV_IN and REV_OUT restores the running CRC")
{
  crc_rig const r;
  r.configure(cr_poly16 | cr_rev_byte | cr_rev_out, 0x8005, 0);
  r.feed_bytes("1234");
  auto const saved = r.f.target->snapshot();
  r.feed_bytes("56789");
  REQUIRE(r.read(dr) == 0xBB3D);
  r.f.target->restore(saved);
  r.feed_bytes("56789");
  CHECK(r.read(dr) == 0xBB3D);

  crc_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.feed_bytes("56789");
  CHECK(other.read(dr) == 0xBB3D);
  CHECK(r.logged.empty());
  CHECK(other.logged.empty());
}

TEST_CASE("a DR write that is not right-aligned is ignored and logs once")
{
  crc_rig const r{false};
  r.store(dr + 1, 1, 0x31);
  r.store(dr + 2, 2, 0x3132);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
  CHECK(r.logged[0].reg == "DR");
}

TEST_CASE("REV_IN reverses a byte write by the byte whatever its group")
{
  crc_rig const r;
  for(auto const control : {cr_rev_byte, cr_rev_half, cr_rev_word})
  {
    r.write(cr, control | cr_rev_out | cr_reset);
    r.feed_bytes(check_input);
    CHECK((r.read(dr) ^ 0xFFFF'FFFF) == 0xCBF4'3926);
  }
  CHECK(r.logged.empty());
}

TEST_CASE("REV_OUT reverses the low POLYSIZE bits, CRC-16/ARC")
{
  crc_rig const r;
  r.configure(cr_poly16 | cr_rev_byte | cr_rev_out, 0x8005, 0);
  r.feed_bytes(check_input);
  CHECK(r.read(dr) == 0x0000'BB3D);
  r.write(cr, cr_poly16 | cr_rev_byte);
  CHECK(r.read(dr) == 0x0000'BCDD);
  CHECK(r.logged.empty());
}

TEST_CASE("a narrow access to a register other than DR logs once per direction")
{
  crc_rig const r{false};
  r.store(idr, 1, 0x5A);
  r.store(init + 2, 2, 0);
  CHECK(r.read(idr) == 0);
  CHECK(r.read(init) == 0xFFFF'FFFF);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
  CHECK(r.logged[0].reg == "IDR");

  r.write(pol, 0x1021);
  CHECK(r.f.target->mem().read(crc_base + pol, 2) == 0x1021);
  CHECK(r.f.target->mem().read(crc_base + idr, 1) == 0);
  REQUIRE(r.logged.size() == 2);
  CHECK(r.logged[1].reg == "POL");
}

TEST_CASE("forbidden CRC accesses raise model_error under strict peripherals")
{
  crc_rig const r;
  auto const    throws_on_write = [&](std::uint32_t offset, std::uint8_t size) {
    CHECK_THROWS_AS(
        static_cast<void>(r.f.target->mem().write(crc_base + offset, size, 0x31)),
        popcorn::model_error);
  };
  throws_on_write(dr + 1, 1);
  throws_on_write(idr, 2);
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().read(crc_base + pol, 2)), popcorn::model_error);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  CHECK(r.read(idr) == 0);
}

TEST_CASE("CRCRST held ignores a DR write, DR reads the reset value after release")
{
  crc_rig const r;
  r.write(dr, 0x1234'5678);
  REQUIRE(r.read(dr) != 0xFFFF'FFFF);
  rcc_reset_write(r.f, "RCC_AHB1RSTR", "CRCRST", 1);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
  r.write(dr, 0x1234'5678);
  rcc_reset_write(r.f, "RCC_AHB1RSTR", "CRCRST", 0);
  CHECK(r.read(dr) == 0xFFFF'FFFF);
}
