#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/image/image.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace arch = popcorn::arch;
namespace core = popcorn::core;
using popcorn::address;
using popcorn::stop;
using popcorn::stop_reason;
using popcorn::testing::elf;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

auto config_key(std::function<void()> const& action) -> std::string
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

constexpr address       sram_code  = 0x2000'1000;
constexpr std::uint16_t bkpt       = 0xbe00;
constexpr address       arena      = 0x6f00'0000;
constexpr std::uint32_t arena_size = 0x1'0000;

auto with_scratch(popcorn::address_range range) -> core::machine_options
{
  core::machine_options options;
  options.scratch = std::move(range);
  return options;
}

auto scratch_error(popcorn::address_range range) -> std::string
{
  return config_key([&] { fixture const f{m33(), "branches", with_scratch(std::move(range))}; });
}

struct format_only
{
  std::uint32_t format{};
};

auto booted(popcorn::core::machine_options options = {}) -> fixture
{
  fixture f{m33(), "branches", std::move(options)};
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
  CHECK(config_key([&] { f.module->prepare_call(hard); }) == "abi");
  CHECK(config_key([&] { f.module->read_args(hard); }) == "abi");
  CHECK(
      config_key([&] { f.module->return_to_caller(f64, arch::float_abi::hard_double); }) == "abi");
  hard.args    = {};
  hard.returns = arch::value_kind::f64;
  CHECK(config_key([&] { std::ignore = f.module->call_result(hard); }) == "abi");
  hard.args = {
      arch::call_arg{.kind = arch::value_kind::i32, .bits = 7}
  };
  hard.returns = arch::value_kind::i32;
  CHECK(config_key([&] { f.module->read_args(hard); }).empty());
  CHECK(config_key([&] { std::ignore = f.module->call_result(hard); }).empty());
}

TEST_CASE("the scratch region is RAM outside the description that a restore resets")
{
  auto const f      = booted(with_scratch({.name = "", .start = arena, .size = arena_size}));
  auto const memory = f.target->mem();
  REQUIRE(memory.write(arena, 4, 0x1234'5678).has_value());
  REQUIRE(memory.write(arena + arena_size - 4, 4, 0xa5a5'a5a5).has_value());
  CHECK(f.word(arena) == 0x1234'5678);
  auto const  saved   = f.target->snapshot();
  auto const& regions = saved.data().memory;
  CHECK(
      std::ranges::find(regions, std::string_view{"scratch"}, &core::memory_image::region)
      != regions.end());
  REQUIRE(memory.write(arena, 4, 0xdead'beef).has_value());
  f.target->restore(saved);
  CHECK(f.word(arena) == 0x1234'5678);
  CHECK(f.word(arena + arena_size - 4) == 0xa5a5'a5a5);

  auto const other = booted(with_scratch({.name = "", .start = arena, .size = arena_size}));
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.word(arena) == 0x1234'5678);

  auto const plain = booted();
  CHECK(
      config_key([&] {
        plain.target->restore(
            core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
      })
      == "memory.scratch");
}

TEST_CASE("the scratch region is not executable")
{
  auto const f = booted(with_scratch({.name = "arena", .start = arena, .size = arena_size}));
  REQUIRE(f.target->mem().write(arena, 2, bkpt).has_value());
  f.set("pc", arena);
  CHECK(f.run_to_stop().reason == stop_reason::fault);
}

TEST_CASE("a scratch region that overlaps, is unaligned or empty is a config_error")
{
  CHECK(scratch_error({.name = "", .start = 0x0800'0000, .size = 0x1000}) == "scratch");
  CHECK(scratch_error({.name = "", .start = 0x081f'f000, .size = 0x2000}) == "scratch");
  CHECK(scratch_error({.name = "", .start = 0x2000'0000, .size = 0x1000}) == "scratch");
  CHECK(scratch_error({.name = "", .start = 0x4001'0000, .size = 0x1000}) == "scratch");
  CHECK(scratch_error({.name = "", .start = arena + 0x100, .size = 0x1000}) == "scratch");
  CHECK(scratch_error({.name = "", .start = arena, .size = 0x1100}) == "scratch");
  CHECK(scratch_error({.name = "", .start = arena, .size = 0}) == "scratch");
  CHECK(scratch_error({.name = "SRAM", .start = arena, .size = 0x1000}) == "scratch");
  CHECK(scratch_error({.name = "", .start = 0x2000'0000 - 0x1000, .size = 0x1000}).empty());
  CHECK(scratch_error({.name = "", .start = 0x200c'0000, .size = 0x1000}).empty());
}

TEST_CASE("a snapshot carries the machine's config digest through its saved form")
{
  core::machine_options options;
  options.config_digest = "project-digest";
  auto const f          = booted(std::move(options));
  auto const saved      = f.target->snapshot();
  CHECK(saved.data().format == core::snapshot_format_version);
  CHECK(saved.data().config_digest == "project-digest");
  auto const back = core::decode_snapshot(core::encode_snapshot(saved.data()));
  CHECK(back.config_digest == "project-digest");
  CHECK(back == saved.data());
}

TEST_CASE("format 1 snapshot bytes are refused with a config_error on format")
{
  auto data        = booted().target->snapshot().data();
  data.format      = 1;
  auto const bytes = core::encode_snapshot(data);
  CHECK(config_key([&] { std::ignore = core::decode_snapshot(bytes); }) == "format");

  std::vector<std::byte> header;
  reflex::serde::bson::serializer{header}.dump(format_only{.format = 1});
  CHECK(config_key([&] { std::ignore = core::decode_snapshot(header); }) == "format");
}

TEST_CASE("machine::locate answers what a stop's where gives")
{
  auto const f       = booted();
  auto const stopped = stop_at_join(f);
  auto const where   = f.target->locate(stopped.where.pc);
  CHECK(where.pc == stopped.where.pc);
  CHECK(where.symbol == stopped.where.symbol);
  CHECK(where.offset == stopped.where.offset);
  CHECK(where.file == stopped.where.file);
  CHECK(where.line == stopped.where.line);

  auto const vectors = f.target->locate(0x0800'0000);
  CHECK(vectors.file.empty());
  CHECK(vectors.line == 0);
  CHECK(f.target->locate(sram_code).symbol.empty());
}

TEST_CASE("an intercept reads and returns floating-point values under an explicit soft ABI")
{
  auto const f = booted();
  REQUIRE(elf("branches").abi == arch::float_abi::hard_double);
  REQUIRE(f.target->mem().write(sram_code, 2, bkpt).has_value());
  auto const kinds = std::vector<arch::value_kind>{arch::value_kind::f64, arch::value_kind::i32};
  auto const answer =
      arch::call_arg{.kind = arch::value_kind::f64, .bits = std::bit_cast<std::uint64_t>(99.0)};
  auto const run = [&](std::optional<arch::float_abi> abi) {
    std::vector<std::uint64_t> seen;
    auto const                 hook =
        f.target->add_intercept(sym("branches", "branches"), [&](core::intercept_call const& call) {
          seen = call.args(kinds, abi);
          return core::intercept_result::returns(answer, abi);
        });
    f.set("pc", sym("branches", "branches"));
    f.set("lr", sram_code | 1);
    f.set("r0", 0x0000'0000);
    f.set("r1", 0x4000'0000);
    f.set("r2", 7);
    std::ignore = f.run_to_stop();
    return seen;
  };
  auto const seen = run(arch::float_abi::soft);
  CHECK(seen == std::vector<std::uint64_t>{std::bit_cast<std::uint64_t>(2.0), 7});
  CHECK(f.reg("r0") == static_cast<std::uint32_t>(answer.bits));
  CHECK(f.reg("r1") == static_cast<std::uint32_t>(answer.bits >> 32));
  CHECK(f.target->regs().pc() == sram_code + 2);

  CHECK_THROWS_AS(std::ignore = run(std::nullopt), popcorn::config_error);
}

TEST_CASE("a description alias never lands in the scratch region")
{
  auto desc = m33();
  desc.regions.push_back({
      .name       = "ALIAS",
      .start      = 0x6000'0000,
      .size       = 0x1000,
      .kind       = popcorn::region_kind::ram,
      .acc        = {.read = true, .write = true, .execute = false},
      .secure     = false,
      .physical   = arena,
      .controller = std::nullopt,
  });
  CHECK(
      config_key([&] {
        fixture const f{
            std::move(desc), "branches",
            with_scratch({.name = "", .start = arena, .size = arena_size})};
      })
      == "regions.ALIAS.physical");
}

TEST_CASE("a snapshot read back from bytes restores only under the same config digest")
{
  core::machine_options options;
  options.config_digest = "project-a";
  auto const saved      = booted(std::move(options)).target->snapshot();
  auto const read_back  = [&] {
    return core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))};
  };

  core::machine_options same;
  same.config_digest = "project-a";
  auto const f       = booted(std::move(same));
  CHECK(config_key([&] { f.target->restore(read_back()); }).empty());

  core::machine_options changed;
  changed.config_digest = "project-b";
  auto const g          = booted(std::move(changed));
  CHECK(config_key([&] { g.target->restore(read_back()); }) == "config_digest");
  CHECK(config_key([&] { booted().target->restore(read_back()); }) == "config_digest");
}
