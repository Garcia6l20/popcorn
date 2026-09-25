#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>

#include <doctest/doctest.h>

#include <tuple>

using popcorn::image::image;
using popcorn::image::symbol;
using popcorn::image::symbol_kind;

namespace
{
auto make_symbol(
    std::string      name,
    popcorn::address value,
    std::uint32_t    size,
    symbol_kind      kind,
    bool             global) -> symbol
{
  return symbol{
      .name = std::move(name), .value = value, .size = size, .kind = kind, .global = global};
}
}

TEST_CASE("lookup finds an exact name")
{
  image img;
  img.symbols.push_back(make_symbol("alpha", 0x1000, 4, symbol_kind::function, true));
  img.symbols.push_back(make_symbol("beta", 0x2000, 4, symbol_kind::object, true));

  auto const& found = img.lookup("beta");
  CHECK(found.value == 0x2000);
}

TEST_CASE("lookup with no match throws symbol_error with close names")
{
  image img;
  img.symbols.push_back(make_symbol("alpha", 0x1000, 4, symbol_kind::function, true));
  img.symbols.push_back(make_symbol("alphb", 0x1004, 4, symbol_kind::function, true));
  img.symbols.push_back(make_symbol("zzzzzzzzzzz", 0x1008, 4, symbol_kind::function, true));

  try
  {
    std::ignore = img.lookup("alpha_");
    FAIL("expected symbol_error");
  }
  catch(popcorn::symbol_error const& err)
  {
    CHECK(err.name() == "alpha_");
    REQUIRE(err.close_matches().size() >= 1);
    CHECK(err.close_matches().front() == "alpha");
  }
}

TEST_CASE("lookup with an empty image throws symbol_error with no close matches")
{
  image const img;
  try
  {
    std::ignore = img.lookup("anything");
    FAIL("expected symbol_error");
  }
  catch(popcorn::symbol_error const& err)
  {
    CHECK(err.close_matches().empty());
  }
}

TEST_CASE("lookup on duplicate names returns the unique global one")
{
  image img;
  img.symbols.push_back(make_symbol("dup", 0x1000, 4, symbol_kind::function, false));
  img.symbols.push_back(make_symbol("dup", 0x2000, 4, symbol_kind::function, true));

  auto const& found = img.lookup("dup");
  CHECK(found.value == 0x2000);
  CHECK(found.global);
}

TEST_CASE("lookup on duplicate names with no global throws symbol_error listing them")
{
  image img;
  img.symbols.push_back(make_symbol("dup", 0x1000, 4, symbol_kind::function, false));
  img.symbols.push_back(make_symbol("dup", 0x2000, 4, symbol_kind::function, false));

  try
  {
    std::ignore = img.lookup("dup");
    FAIL("expected symbol_error");
  }
  catch(popcorn::symbol_error const& err)
  {
    CHECK(err.close_matches().size() == 2);
  }
}

TEST_CASE("lookup on duplicate names with several globals throws symbol_error listing them")
{
  image img;
  img.symbols.push_back(make_symbol("dup", 0x1000, 4, symbol_kind::function, true));
  img.symbols.push_back(make_symbol("dup", 0x2000, 4, symbol_kind::function, true));

  CHECK_THROWS_AS(std::ignore = img.lookup("dup"), popcorn::symbol_error);
}

TEST_CASE("locate resolves an address inside a function")
{
  image img;
  img.symbols.push_back(make_symbol("f", 0x1000, 0x10, symbol_kind::function, true));

  auto const loc = img.locate(0x1004);
  CHECK(loc.symbol == "f");
  CHECK(loc.offset == 4);
}

TEST_CASE("locate resolves the first byte of a symbol")
{
  image img;
  img.symbols.push_back(make_symbol("f", 0x1000, 0x10, symbol_kind::function, true));
  auto const loc = img.locate(0x1000);
  CHECK(loc.symbol == "f");
  CHECK(loc.offset == 0);
}

TEST_CASE("locate one past a symbol's end finds nothing at that symbol")
{
  image img;
  img.symbols.push_back(make_symbol("f", 0x1000, 0x10, symbol_kind::function, true));
  auto const loc = img.locate(0x1010);
  CHECK(loc.symbol.empty());
}

TEST_CASE("locate between two symbols with a gap finds nothing")
{
  image img;
  img.symbols.push_back(make_symbol("f", 0x1000, 0x4, symbol_kind::function, true));
  img.symbols.push_back(make_symbol("g", 0x2000, 0x4, symbol_kind::function, true));
  auto const loc = img.locate(0x1800);
  CHECK(loc.symbol.empty());
}

TEST_CASE("locate prefers function over object over other at the same address")
{
  image img;
  img.symbols.push_back(make_symbol("obj", 0x1000, 0x10, symbol_kind::object, true));
  img.symbols.push_back(make_symbol("fn", 0x1000, 0x10, symbol_kind::function, true));
  img.symbols.push_back(make_symbol("other", 0x1000, 0x10, symbol_kind::other, true));
  auto const loc = img.locate(0x1004);
  CHECK(loc.symbol == "fn");
}

TEST_CASE("locate finds a zero-size symbol only at its exact address")
{
  image img;
  img.symbols.push_back(make_symbol("z", 0x1000, 0, symbol_kind::other, true));
  CHECK(img.locate(0x1000).symbol == "z");
  CHECK(img.locate(0x1001).symbol.empty());
}

TEST_CASE("locate below the first symbol finds nothing")
{
  image img;
  img.symbols.push_back(make_symbol("f", 0x1000, 0x10, symbol_kind::function, true));
  CHECK(img.locate(0x0800).symbol.empty());
}

TEST_CASE("locate stops walking back once past the largest symbol size")
{
  image img;
  img.symbols.push_back(make_symbol("small", 0x1000, 0x4, symbol_kind::function, true));
  img.symbols.push_back(make_symbol("big", 0x2000, 0x1000, symbol_kind::function, true));
  auto const loc = img.locate(0x1800);
  CHECK(loc.symbol.empty());
}

TEST_CASE("locate on an empty image finds nothing")
{
  image const img;
  auto const  loc = img.locate(0x1000);
  CHECK(loc.symbol.empty());
  CHECK(loc.file.empty());
  CHECK(loc.line == 0);
}

TEST_CASE("locate finds the line covering an address from the line table")
{
  image img;
  img.files = {"a.c"};
  img.lines = {
      {.addr = 0x1000, .file = 0, .line = 10, .is_stmt = true},
      {.addr = 0x1010, .file = 0, .line = 20, .is_stmt = true},
  };
  auto const loc = img.locate(0x1004);
  CHECK(loc.file == "a.c");
  CHECK(loc.line == 10);
}
