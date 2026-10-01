#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace
{
using popcorn::icount;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint32_t csrst      = 1u << 0;
constexpr std::uint32_t psrst      = 1u << 1;
constexpr std::uint32_t fcrst      = 1u << 2;
constexpr std::uint32_t rxfflsh    = 1u << 4;
constexpr std::uint32_t txfflsh    = 1u << 5;
constexpr std::uint32_t all_fifos  = 0x10u << 6;
constexpr std::uint32_t ahbidl     = 1u << 31;
constexpr std::uint32_t reset_word = ahbidl;

struct otg_fixture : stm32_fixture
{
  otg_fixture() : stm32_fixture{u575(), "boot_ll"}
  {}

  [[nodiscard]] auto offset() const -> std::uint32_t
  {
    auto const& registers = target->peripheral("OTG_FS").description().registers;
    auto const  found =
        std::ranges::find(registers, std::string_view{"GRSTCTL"}, &popcorn::register_desc::name);
    REQUIRE(found != registers.end());
    return found->offset;
  }

  void write(std::uint32_t value) const
  {
    auto& regs = target->peripheral("OTG_FS");
    REQUIRE(
        target->model("OTG_FS")
            ->on_write(regs, offset(), 4, regs.peek(offset()), value, {.pc = 0, .now = icount{0}})
            .has_value());
  }

  [[nodiscard]] auto read() const -> std::uint32_t
  {
    auto&      regs = target->peripheral("OTG_FS");
    auto const value =
        target->model("OTG_FS")->on_read(regs, offset(), 4, {.pc = 0, .now = icount{0}});
    REQUIRE(value.has_value());
    return *value;
  }
};
}

TEST_CASE("OTG_GRSTCTL reads AHBIDL set after reset")
{
  otg_fixture const f;
  REQUIRE(f.target->model("OTG_FS") != nullptr);
  CHECK(f.read() == reset_word);
}

TEST_CASE("RXFFLSH reads 0 right after it is written, AHBIDL stays set")
{
  otg_fixture const f;
  f.write(rxfflsh);
  CHECK(f.read() == reset_word);
}

TEST_CASE("TXFFLSH reads 0 right after it is written, TXFNUM keeps the FIFO number")
{
  otg_fixture const f;
  f.write(txfflsh | all_fifos);
  CHECK(f.read() == (reset_word | all_fifos));
  f.write(f.read() | rxfflsh | txfflsh);
  CHECK(f.read() == (reset_word | all_fifos));
}

TEST_CASE("a core soft reset reads done at once, AHBIDL stays set")
{
  otg_fixture const f;
  f.write(f.read() | csrst);
  CHECK(f.read() == reset_word);
}

TEST_CASE("a partial soft reset and a frame counter reset read done at once")
{
  otg_fixture const f;
  f.write(psrst | all_fifos);
  CHECK(f.read() == (reset_word | all_fifos));
  f.write(fcrst | all_fifos);
  CHECK(f.read() == (reset_word | all_fifos));
}

TEST_CASE("a machine reset brings OTG_GRSTCTL back to its reset value")
{
  otg_fixture const f;
  f.write(all_fifos);
  REQUIRE(f.read() == (reset_word | all_fifos));
  f.target->reset();
  CHECK(f.read() == reset_word);
}
