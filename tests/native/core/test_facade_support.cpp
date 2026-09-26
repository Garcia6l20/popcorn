#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
namespace arch = popcorn::arch;
using popcorn::address;
using popcorn::stop;
using popcorn::stop_reason;
using popcorn::testing::elf;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address       sram_code = 0x2000'1000;
constexpr std::uint16_t bkpt      = 0xbe00;

auto booted() -> fixture
{
  fixture f{m33(), "branches", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto join() -> address
{
  return sym("branches", "branch_join");
}

auto join_line() -> std::uint32_t
{
  auto const line = elf("branches").locate(join()).line;
  REQUIRE(line != 0);
  return line;
}

auto stop_at_join(fixture const& f) -> stop
{
  f.set("pc", sym("branches", "branches"));
  f.set("r0", 3);
  auto const stopped = f.run_to_stop({.until = join()});
  REQUIRE(stopped.reason == stop_reason::address);
  REQUIRE(stopped.where.pc == join());
  return stopped;
}

auto overlaid() -> popcorn::image::image
{
  auto overlay = elf("branches");
  for(auto& file : overlay.files)
  {
    file = "/overlay/" + file;
  }
  return overlay;
}

auto abi_key(std::function<void()> const& action) -> std::string
{
  try
  {
    action();
  }
  catch(popcorn::config_error const& e)
  {
    return e.key();
  }
  return {};
}
}

TEST_CASE("a stop inside a function with a line table carries its file and line")
{
  auto const f       = booted();
  auto const stopped = stop_at_join(f);
  CHECK(stopped.where.symbol == "branch_join");
  CHECK(stopped.where.file.ends_with("/branches.c"));
  CHECK(stopped.where.line == join_line());
}

TEST_CASE("a stop at the reset handler names the source line of its breakpoint")
{
  fixture const f{m33(), "branches", {}};
  auto const    stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(stopped.where.symbol == "Reset_Handler");
  CHECK(stopped.where.file.ends_with("/branches.c"));
  CHECK(stopped.where.line == elf("branches").locate(stopped.where.pc).line);
  CHECK(stopped.where.line != 0);
}

TEST_CASE("a stop outside every image keeps an empty file and line 0")
{
  auto const f = booted();
  REQUIRE(f.target->mem().write(sram_code, 2, bkpt).has_value());
  f.set("pc", sram_code);
  auto const stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(stopped.where.pc == sram_code + 2);
  CHECK(stopped.where.file.empty());
  CHECK(stopped.where.line == 0);
}

TEST_CASE("the last loaded image covering pc gives the file and line")
{
  auto const f = booted();
  f.target->load(overlaid());
  auto const stopped = stop_at_join(f);
  CHECK(stopped.where.file.starts_with("/overlay/"));
  CHECK(stopped.where.line == join_line());
}

TEST_CASE(
    "an image loaded later that does not cover pc leaves the file and line of the one that does")
{
  auto const            f = booted();
  popcorn::image::image data;
  data.segments.push_back(
      {.name        = "data",
       .start       = 0x2000'2000,
       .bytes       = std::vector<std::byte>(16),
       .memory_size = 16,
       .executable  = false,
       .writable    = true});
  f.target->load(data);
  auto const stopped = stop_at_join(f);
  CHECK(stopped.where.file.ends_with("/branches.c"));
  CHECK(stopped.where.line == join_line());
}

TEST_CASE("a later image with lines whose segment ends at or below pc does not locate it")
{
  for(auto const gap : {address{0x40}, address{0}})
  {
    CAPTURE(gap);
    auto const f       = booted();
    auto       partial = overlaid();
    auto const first   = partial.segments.front();
    REQUIRE(first.start + gap < join());
    auto const kept = static_cast<std::uint32_t>(join() - first.start - gap);
    REQUIRE(kept <= first.bytes.size());
    partial.segments = {first};
    partial.segments.front().bytes.resize(kept);
    partial.segments.front().memory_size = kept;
    f.target->load(partial);
    auto const stopped = stop_at_join(f);
    CHECK(stopped.where.file.ends_with("/branches.c"));
    CHECK_FALSE(stopped.where.file.starts_with("/overlay/"));
    CHECK(stopped.where.line == join_line());
  }
}

TEST_CASE("the test arch refuses floating-point values under a hard float ABI")
{
  auto const f = booted();
  auto const f32 =
      arch::call_arg{.kind = arch::value_kind::f32, .bits = std::bit_cast<std::uint32_t>(1.0f)};
  auto const f64 =
      arch::call_arg{.kind = arch::value_kind::f64, .bits = std::bit_cast<std::uint64_t>(2.0)};
  auto hard = arch::call_frame{
      .entry           = sym("branches", "branches"),
      .args            = {f32},
      .abi             = arch::float_abi::hard_double,
      .return_sentinel = sram_code};
  CHECK(abi_key([&] { f.module->prepare_call(hard); }) == "abi");
  CHECK(abi_key([&] { f.module->read_args(hard); }) == "abi");
  CHECK(abi_key([&] { f.module->return_to_caller(f64, arch::float_abi::hard_double); }) == "abi");
  hard.args    = {};
  hard.returns = arch::value_kind::f64;
  CHECK(abi_key([&] { std::ignore = f.module->call_result(hard); }) == "abi");
  hard.args = {
      arch::call_arg{.kind = arch::value_kind::i32, .bits = 7}
  };
  hard.returns = arch::value_kind::i32;
  CHECK(abi_key([&] { f.module->read_args(hard); }).empty());
  CHECK(abi_key([&] { std::ignore = f.module->call_result(hard); }).empty());
}
