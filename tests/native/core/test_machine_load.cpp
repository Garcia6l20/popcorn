#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/elf_image.hpp"
#include "../support/firmware.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <meta>
#include <optional>
#include <string>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace image = popcorn::image;
using popcorn::address;

constexpr address estack = 0x200c'0000;

auto m33() -> popcorn::mcu_description
{
  char const* const path = std::getenv("POPCORN_M33_DESC");
  REQUIRE(path != nullptr);
  return popcorn::load_description(path);
}

auto basic() -> image::image
{
  return popcorn::testing::read_elf(popcorn::testing::firmware("basic"));
}

auto filled(address start, std::size_t size, std::byte value, std::uint32_t memory_size = 0)
    -> image::image
{
  image::image out;
  out.segments.push_back(
      {.name        = std::format("seg@{:#x}", start),
       .start       = start,
       .bytes       = std::vector<std::byte>(size, value),
       .memory_size = memory_size == 0 ? static_cast<std::uint32_t>(size) : memory_size,
       .executable  = false,
       .writable    = true});
  return out;
}

auto load_error(core::machine& target, image::image const& loaded)
    -> std::optional<popcorn::image_error>
{
  try
  {
    target.load(loaded);
  }
  catch(popcorn::image_error const& e)
  {
    return e;
  }
  return std::nullopt;
}

auto sp_of(core::machine& target) -> std::uint64_t
{
  return target.regs().read(*target.regs().find("sp"));
}
}

TEST_CASE("the test ELF reader finds segments, symbols and the digest of basic")
{
  auto const loaded = basic();
  REQUIRE_FALSE(loaded.segments.empty());
  CHECK(loaded.segments.front().start == 0x0800'0000);
  CHECK(loaded.segments.front().executable);
  CHECK(popcorn::testing::find_symbol(loaded, "Reset_Handler") % 2 == 0);
  CHECK(popcorn::testing::find_symbol(loaded, "patched_ret") > 0x0800'0000);
  CHECK(popcorn::testing::find_symbol(loaded, "pattern") >= 0x2000'0000);
  CHECK(loaded.digest.size() == 64);
  CHECK_THROWS_AS(popcorn::testing::find_symbol(loaded, "missing"), popcorn::symbol_error);
}

TEST_CASE("load then reset starts basic at Reset_Handler with the stack at _estack")
{
  auto const    loaded = basic();
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};
  target.load(loaded);
  target.reset();
  CHECK(target.regs().pc() == popcorn::testing::find_symbol(loaded, "Reset_Handler"));
  CHECK(sp_of(target) == estack);

  auto const memory = target.mem();
  CHECK(memory.read(0x0800'0000, 4) == estack);
  CHECK(memory.read(0x0000'0000, 4) == estack);
  CHECK(memory.read(0x0800'0004, 4) == memory.read(0x0000'0004, 4));
  CHECK(
      memory.read(0x0800'0004, 4) == (popcorn::testing::find_symbol(loaded, "Reset_Handler") | 1u));
  REQUIRE(target.loaded_images().size() == 1);
  CHECK(target.loaded_images().front() == loaded);
}

TEST_CASE("reset reads the vector table machine_options::vector_table names")
{
  auto const            loaded = basic();
  core::machine_options options;
  options.vector_table = 0x0801'0000;
  core::machine target{m33(), popcorn::testing::test_arch_factory(), options};
  target.load(loaded);
  auto const entry = popcorn::testing::find_symbol(loaded, "patched");
  REQUIRE(target.mem().write(0x0801'0000, 4, 0x2008'0000));
  REQUIRE(target.mem().write(0x0801'0004, 4, entry | 1));
  target.reset();
  CHECK(target.regs().pc() == entry);
  CHECK(sp_of(target) == 0x2008'0000);
}

TEST_CASE("load refuses segments outside the regions and keeps what it accepted")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};

  auto const outside = load_error(target, filled(0x6000'0000, 16, std::byte{1}));
  REQUIRE(outside.has_value());
  CHECK(outside->segment() == "seg@0x60000000");
  CHECK(outside->addr() == 0x6000'0000);

  auto const across = load_error(target, filled(0x081f'fff0, 32, std::byte{1}));
  REQUIRE(across.has_value());
  CHECK(across->addr() == 0x081f'fff0);
  CHECK(across->region().contains("FLASH"));

  auto short_segment                         = filled(0x2000'0000, 16, std::byte{1});
  short_segment.segments.front().memory_size = 8;
  CHECK(load_error(target, short_segment).has_value());
  CHECK(target.loaded_images().empty());

  target.load({});
  CHECK(target.loaded_images().size() == 1);
}

TEST_CASE("an image with one bad segment writes none of its segments")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};
  auto          mixed = filled(0x2000'0000, 16, std::byte{0x77});
  mixed.segments.push_back(filled(0x6000'0000, 16, std::byte{0x77}).segments.front());
  CHECK(load_error(target, mixed).has_value());
  CHECK(target.mem().read(0x2000'0000, 4) == 0u);
  CHECK(target.loaded_images().empty());
}

TEST_CASE("later images overwrite earlier ones and memory past the bytes is zeroed")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};
  auto const    first  = filled(0x2000'0000, 16, std::byte{0x11});
  auto const    second = filled(0x2000'0004, 4, std::byte{0x22}, 8);
  target.load(first);
  target.load(second);
  auto const memory = target.mem();
  CHECK(memory.read(0x2000'0000, 4) == 0x1111'1111u);
  CHECK(memory.read(0x2000'0004, 4) == 0x2222'2222u);
  CHECK(memory.read(0x2000'0008, 4) == 0u);
  CHECK(memory.read(0x2000'000c, 4) == 0x1111'1111u);
  REQUIRE(target.loaded_images().size() == 2);
  CHECK(target.loaded_images()[0] == first);
  CHECK(target.loaded_images()[1] == second);
}

TEST_CASE("every reset kind restarts the clock at instruction 0 and time 0 and keeps memory")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};
  target.load(basic());
  REQUIRE(target.mem().write(0x2000'1000, 4, 0x600d'f00d));
  template for(constexpr auto kind :
               std::define_static_array(std::meta::enumerators_of(^^popcorn::reset_kind)))
  {
    popcorn::reset_kind const value = [:kind:];
    std::string const         name  = std::format("{}", value);
    INFO(name);
    target.reset(value);
    CHECK(target.virtual_clock().now() == popcorn::icount{0});
    CHECK(target.virtual_clock().time() == popcorn::vtime{0});
    CHECK(target.virtual_clock().core_hz() == 4'000'000);
    CHECK(target.mem().read(0x2000'1000, 4) == 0x600d'f00du);
  }
}
