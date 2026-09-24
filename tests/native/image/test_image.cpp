#include <popcorn/image/image.hpp>

#include <popcorn/core/errors.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <format>
#include <tuple>

TEST_CASE("image is plain data loaders and the object linker fill")
{
  popcorn::image::image built;
  built.segments.push_back({
      .name        = "0",
      .start       = 0x0800'0000,
      .bytes       = {std::byte{0x00}, std::byte{0x80}},
      .memory_size = 4,
      .executable  = true
  });
  built.symbols.push_back(
      {.name  = "main",
       .value = 0x0800'0100,
       .size  = 12,
       .kind  = popcorn::image::symbol_kind::function});
  built.entry     = 0x0800'0101;
  built.abi       = popcorn::arch::float_abi::hard_double;
  built.digest    = "00";
  auto const copy = built;
  CHECK(copy == built);
  CHECK(std::format("{}", popcorn::image::symbol_kind::object) == "object");
}

TEST_CASE("image loading and lookups are stubs")
{
  popcorn::image::image const empty;
  CHECK_THROWS_AS(std::ignore = empty.lookup("main"), popcorn::not_implemented);
  CHECK_THROWS_AS(std::ignore = empty.locate(0x0800'0100), popcorn::not_implemented);
  CHECK_THROWS_AS(popcorn::image::load_elf("missing.elf"), popcorn::not_implemented);
}
