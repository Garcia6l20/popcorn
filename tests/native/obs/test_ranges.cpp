#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/coverage.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
namespace image = popcorn::image;
using popcorn::address_range;
using popcorn::obs::function_ranges;

auto branches_elf() -> image::image
{
  char const* const path = std::getenv("POPCORN_FW_BRANCHES");
  REQUIRE(path != nullptr);
  return image::load_elf(path);
}

auto function(std::string name, popcorn::address value, std::uint32_t size, bool global = true)
    -> image::symbol
{
  return {
      .name   = std::move(name),
      .value  = value,
      .size   = size,
      .kind   = image::symbol_kind::function,
      .global = global};
}
}

TEST_CASE("function_ranges gives a function's value and size")
{
  std::array const images{branches_elf()};
  std::array const names{std::string{"branches"}, std::string{"outside"}};
  CHECK(
      function_ranges(images, names)
      == std::vector<address_range>{
          {.name = "branches", .start = 0x0800'0048, .size = 30},
          {.name = "outside",  .start = 0x0800'0066, .size = 6 },
  });
}

TEST_CASE("a size-0 function reaches the next function symbol")
{
  std::array const images{branches_elf()};
  std::array const names{std::string{"branch_top"}, std::string{"branch_end"}};
  CHECK(
      function_ranges(images, names)
      == std::vector<address_range>{
          {.name = "branch_top", .start = 0x0800'004a, .size = 8},
          {.name = "branch_end", .start = 0x0800'0064, .size = 2},
  });
}

TEST_CASE("a size-0 last function reaches the end of its executable segment")
{
  image::image alone;
  alone.segments.push_back(
      {.name = "0", .start = 0x100, .bytes = {}, .memory_size = 0x40, .executable = true});
  alone.symbols.push_back(function("tail", 0x120, 0));
  std::array const images{alone};
  std::array const names{std::string{"tail"}};
  CHECK(
      function_ranges(images, names)
      == std::vector<address_range>{
          {.name = "tail", .start = 0x120, .size = 0x20}
  });
}

TEST_CASE("the first image holding a name wins")
{
  image::image first;
  first.symbols.push_back(function("shared", 0x100, 4));
  image::image second;
  second.symbols.push_back(function("shared", 0x200, 8));
  second.symbols.push_back(function("only_second", 0x300, 2));
  std::array const images{first, second};
  std::array const names{std::string{"shared"}, std::string{"only_second"}};
  CHECK(
      function_ranges(images, names)
      == std::vector<address_range>{
          {.name = "shared",      .start = 0x100, .size = 4},
          {.name = "only_second", .start = 0x300, .size = 2},
  });
}

TEST_CASE("an unknown name throws symbol_error")
{
  std::array const images{branches_elf()};
  std::array const names{std::string{"branchez"}};
  CHECK_THROWS_AS(static_cast<void>(function_ranges(images, names)), popcorn::symbol_error);
  std::array const missing{std::string{"branches"}};
  CHECK_THROWS_AS(
      static_cast<void>(function_ranges(std::span<image::image const>{}, missing)),
      popcorn::symbol_error);
}

TEST_CASE("a size-0 function stops at its segment's end before a function in the next segment")
{
  image::image split;
  split.segments.push_back(
      {.name = "0", .start = 0x100, .bytes = {}, .memory_size = 0x40, .executable = true});
  split.segments.push_back(
      {.name = "1", .start = 0x200, .bytes = {}, .memory_size = 0x40, .executable = true});
  split.symbols.push_back(function("first", 0x110, 0));
  split.symbols.push_back(function("second", 0x200, 0x10));
  std::array const images{split};
  std::array const names{std::string{"first"}};
  CHECK(
      function_ranges(images, names)
      == std::vector<address_range>{
          {.name = "first", .start = 0x110, .size = 0x30}
  });
}

TEST_CASE("a size-0 function outside every segment reaches the next function symbol")
{
  image::image loose;
  loose.symbols.push_back(function("first", 0x110, 0));
  loose.symbols.push_back(function("second", 0x200, 0x10));
  std::array const images{loose};
  std::array const names{std::string{"first"}};
  CHECK(
      function_ranges(images, names)
      == std::vector<address_range>{
          {.name = "first", .start = 0x110, .size = 0xf0}
  });
}

TEST_CASE("a size-0 function with no segment and no later function throws symbol_error")
{
  image::image loose;
  loose.symbols.push_back(function("alone", 0x110, 0));
  std::array const images{loose};
  std::array const names{std::string{"alone"}};
  CHECK_THROWS_AS(static_cast<void>(function_ranges(images, names)), popcorn::symbol_error);
}

TEST_CASE("an ambiguous name in the first image having it is reported, not skipped")
{
  image::image first;
  first.symbols.push_back(function("dup", 0x100, 4, false));
  first.symbols.push_back(function("dup", 0x200, 4, false));
  image::image second;
  second.symbols.push_back(function("dup", 0x300, 4));
  std::array const images{first, second};
  std::array const names{std::string{"dup"}};
  try
  {
    static_cast<void>(function_ranges(images, names));
    FAIL("no symbol_error");
  }
  catch(popcorn::symbol_error const& ambiguous)
  {
    CHECK(
        ambiguous.close_matches() == std::vector<std::string>{"dup@0x00000100", "dup@0x00000200"});
  }
}
