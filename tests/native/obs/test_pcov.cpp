#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/collect.hpp>
#include <popcorn/obs/coverage.hpp>
#include <popcorn/obs/errors.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
namespace image = popcorn::image;
namespace obs   = popcorn::obs;

[[maybe_unused]] auto const builtin_arch = (popcorn::arch::armv8m::register_builtin(), true);
using popcorn::address;
using popcorn::address_range;
using popcorn::stop_reason;
using popcorn::testing::firmware;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_arch;

struct elf_machine
{
  std::unique_ptr<popcorn::core::machine> target;

  explicit elf_machine(std::string_view name)
  {
    target = std::make_unique<popcorn::core::machine>(
        m33(),
        [](popcorn::core_desc const& core) -> std::unique_ptr<popcorn::arch::arch> {
          return std::make_unique<test_arch>(core);
        },
        popcorn::core::machine_options{});
    target->load(image::load_elf(firmware(name)));
    target->reset();
  }

  void set(std::string_view name, std::uint64_t value) const
  {
    target->regs().write(*target->regs().find(name), value);
  }

  auto run_to_stop() const -> popcorn::stop
  {
    auto const result = target->run_slice({.wall_budget = std::chrono::seconds{60}});
    REQUIRE(result.stopped.has_value());
    return *result.stopped;
  }
};

using fixture = elf_machine;

auto scratch() -> std::filesystem::path
{
  char const* const path = std::getenv("POPCORN_OBS_SCRATCH");
  REQUIRE(path != nullptr);
  auto const dir = std::filesystem::path{path} / "pcov";
  std::filesystem::create_directories(dir);
  return dir;
}

auto sources(std::string_view name) -> std::vector<obs::image_source>
{
  return {
      {.kind = obs::image_kind::elf, .path = firmware(name).string()}
  };
}

auto branches_range() -> address_range
{
  return {
      .name  = "branches",
      .start = sym("branches", "branches"),
      .size  = sym("branches", "branch_end") + 2 - sym("branches", "branches")};
}

auto segment_range(fixture const& f) -> address_range
{
  auto const& segment = f.target->loaded_images()[0].segments.at(0);
  return {.name = "text", .start = segment.start, .size = segment.memory_size};
}

void run_branches(fixture const& f, std::uint32_t n)
{
  f.set("pc", sym("branches", "branches") | 1);
  f.set("r0", n);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
}

auto booted(std::vector<address_range> ranges) -> std::unique_ptr<fixture>
{
  auto f = std::make_unique<fixture>("branches");
  REQUIRE(f->run_to_stop().reason == stop_reason::user);
  f->target->enable_coverage(std::move(ranges));
  return f;
}

auto one(obs::coverage_group group) -> obs::coverage_set
{
  return {.groups = {std::move(group)}};
}

auto sample() -> obs::coverage_set
{
  return one({
      .images =
          {{.kind   = obs::image_kind::elf,
            .path   = "/fw.elf",
            .digest = "aa",
            .base   = 0x100,
            .end    = 0x200}},
      .ranges  = {{.name = "f", .start = 0x100, .size = 0x20}},
      .blocks  = {{.start = 0x100, .size = 4, .hits = 3}, {.start = 0x104, .size = 8, .hits = 1}},
      .edges   = {{.from = 0x100, .to = 0x104, .hits = 1}},
      .split   = 1,
      .dropped = 2,
  });
}

auto merged(std::initializer_list<obs::coverage_set> sets) -> obs::coverage_set
{
  std::vector<obs::coverage_set> const held{sets};
  return obs::merge(held);
}

auto patch_u32(std::vector<std::byte> bytes, std::size_t at, std::uint32_t value)
    -> std::vector<std::byte>
{
  std::memcpy(bytes.data() + at, &value, sizeof value);
  return bytes;
}

auto throws_format(std::span<std::byte const> bytes) -> bool
{
  try
  {
    static_cast<void>(obs::decode_pcov(bytes, "test"));
  }
  catch(obs::format_error const&)
  {
    return true;
  }
  return false;
}
}

TEST_CASE("decode inverts encode and encode inverts decode, byte for byte")
{
  auto f = booted({});
  run_branches(*f, 10);
  auto const collected = obs::collect(*f->target, sources("branches"));
  auto       two       = sample();
  two.groups.push_back(collected.groups.at(0));
  for(auto const& set :
      {obs::coverage_set{}, one({.images = {{.path = "/x", .digest = "d"}}}), sample(), collected,
       two})
  {
    auto const bytes = obs::encode_pcov(set);
    auto const back  = obs::decode_pcov(bytes);
    CHECK(back == obs::merge(std::span{&set, 1}));
    CHECK(obs::encode_pcov(back) == bytes);
  }
}

TEST_CASE("every strict prefix of a file is refused with an offset inside it")
{
  auto const bytes = obs::encode_pcov(sample());
  for(std::size_t size = 0; size < bytes.size(); ++size)
  {
    CAPTURE(size);
    try
    {
      static_cast<void>(obs::decode_pcov(std::span{bytes}.first(size), "test"));
      FAIL("accepted");
    }
    catch(obs::format_error const& refused)
    {
      CHECK(refused.offset() <= size);
      CHECK(refused.file() == "test");
    }
  }
}

TEST_CASE("malformed and non-canonical files are refused")
{
  auto const bytes = obs::encode_pcov(sample());
  auto       bad   = bytes;
  bad[0]           = std::byte{'X'};
  CHECK(throws_format(bad));
  bad    = bytes;
  bad[4] = std::byte{2};
  CHECK(throws_format(bad));
  auto trailing = bytes;
  trailing.push_back(std::byte{0});
  CHECK(throws_format(trailing));

  auto const with = [](obs::block_hit block) {
    auto set             = sample();
    set.groups[0].blocks = {block};
    return set;
  };
  auto const block_at = obs::encode_pcov(sample()).size() - 16 - 4 - 2 * 16;
  auto const valid    = obs::encode_pcov(with({.start = 0x100, .size = 4, .hits = 1}));
  auto const size_at  = valid.size() - 16 - 4 - 16 + 4;
  CHECK_FALSE(throws_format(valid));
  CHECK(throws_format(patch_u32(valid, size_at, 0)));
  CHECK(throws_format(patch_u32(valid, size_at, 1025)));
  CHECK_FALSE(throws_format(patch_u32(valid, size_at, 1024)));
  CHECK(throws_format(patch_u32(valid, size_at + 4, 0)));

  auto const two         = obs::encode_pcov(sample());
  auto const first_block = block_at;
  auto       swapped     = two;
  std::memcpy(swapped.data() + first_block, two.data() + first_block + 16, 16);
  std::memcpy(swapped.data() + first_block + 16, two.data() + first_block, 16);
  CHECK(throws_format(swapped));
  auto repeated = two;
  std::memcpy(repeated.data() + first_block + 16, two.data() + first_block, 16);
  CHECK(throws_format(repeated));

  auto const kind_at = std::size_t{4 + 2 + 2 + 4 + 8 + 8 + 4};
  CHECK(throws_format(patch_u32(bytes, kind_at, 2)));
  CHECK(throws_format(patch_u32(bytes, kind_at, 1)));
}

TEST_CASE("merging two machines equals one machine running both, byte for byte")
{
  auto a = booted({});
  a->target->enable_coverage({branches_range(), segment_range(*a)});
  run_branches(*a, 10);
  auto b = booted({});
  b->target->enable_coverage({segment_range(*b), branches_range()});
  run_branches(*b, 7);
  auto c = booted({});
  c->target->enable_coverage({branches_range(), segment_range(*c)});
  run_branches(*c, 10);
  run_branches(*c, 7);

  auto const set_a = obs::collect(*a->target, sources("branches"));
  auto const set_b = obs::collect(*b->target, sources("branches"));
  auto const set_c = obs::collect(*c->target, sources("branches"));
  auto const sum   = merged({set_a, set_b});
  CHECK(obs::encode_pcov(sum) == obs::encode_pcov(set_c));
  CHECK(obs::encode_pcov(merged({set_b, set_a})) == obs::encode_pcov(sum));
  auto const images = obs::load_images(sum);
  CHECK(obs::lcov(sum, images, {}) == obs::lcov(set_c, obs::load_images(set_c), {}));

  auto const x = sample();
  CHECK(
      obs::encode_pcov(merged({merged({set_a, set_b}), x}))
      == obs::encode_pcov(merged({set_a, merged({set_b, x})})));
  CHECK(obs::encode_pcov(merged({x})) == obs::encode_pcov(x));
  CHECK(obs::merge({}) == obs::coverage_set{});
}

TEST_CASE("different images stay in separate groups, equal ones sum")
{
  auto a = booted({});
  run_branches(*a, 10);
  auto const branches = obs::collect(*a->target, sources("branches"));
  auto const both     = merged({branches, sample()});
  REQUIRE(both.groups.size() == 2);
  auto const again = merged({both, branches});
  REQUIRE(again.groups.size() == 2);
  auto const group = std::ranges::find_if(again.groups, [&](obs::coverage_group const& held) {
    return held.images == branches.groups[0].images;
  });
  REQUIRE(group != again.groups.end());
  CHECK(group->blocks.front().hits == 2 * branches.groups[0].blocks.front().hits);
}

TEST_CASE("merge_into adds to the file of a worker")
{
  auto const path = scratch() / "worker.pcov";
  std::filesystem::remove(path);
  auto const first = obs::merge_into(path, sample());
  CHECK(first == obs::merge(std::array{sample()}));
  auto const second = obs::merge_into(path, sample());
  CHECK(second == merged({sample(), sample()}));
  CHECK(obs::load_pcov(path) == second);
  for(auto const& entry : std::filesystem::directory_iterator{path.parent_path()})
  {
    CHECK_FALSE(entry.path().filename().string().starts_with("worker.pcov.tmp"));
  }
}

TEST_CASE("paths keep the smallest, so the order of merged sets does not matter")
{
  auto other                     = sample();
  other.groups[0].images[0].path = "/a.elf";
  CHECK(obs::encode_pcov(merged({sample(), other})) == obs::encode_pcov(merged({other, sample()})));
  CHECK(merged({sample(), other}).groups[0].images[0].path == "/a.elf");
}

TEST_CASE("an ELF that changed since collect is refused")
{
  auto set = one(
      {.images = {
           {.kind = obs::image_kind::elf, .path = firmware("branches").string(), .digest = "0"}}});
  CHECK_THROWS_AS(static_cast<void>(obs::load_images(set)), obs::format_error);
  set.groups[0].images[0].digest = image::load_elf(firmware("branches")).digest;
  CHECK(obs::load_images(set).at(0).size() == 1);
  set.groups[0].images.push_back({.kind = obs::image_kind::raw, .digest = "r"});
  CHECK(obs::load_images(set).at(0).at(1).segments.empty());
}

TEST_CASE("hits saturate at the largest u64")
{
  auto big                     = sample();
  big.groups[0].blocks[0].hits = std::numeric_limits<std::uint64_t>::max() - 1;
  auto const sum               = merged({big, sample()});
  CHECK(sum.groups[0].blocks[0].hits == std::numeric_limits<std::uint64_t>::max());
}

namespace
{
auto swap_records(
    std::vector<std::byte> bytes,
    std::size_t            first,
    std::size_t            second,
    std::size_t            size) -> std::vector<std::byte>
{
  std::ranges::swap_ranges(
      std::span{bytes}.subspan(first, size), std::span{bytes}.subspan(second, size));
  return bytes;
}

auto copy_record(std::vector<std::byte> bytes, std::size_t from, std::size_t to, std::size_t size)
    -> std::vector<std::byte>
{
  std::ranges::copy(
      std::span{bytes}.subspan(from, size), bytes.begin() + static_cast<std::ptrdiff_t>(to));
  return bytes;
}

auto small_group(std::string digest) -> obs::coverage_group
{
  return {.images = {{.kind = obs::image_kind::elf, .path = "/x", .digest = std::move(digest)}}};
}
}

TEST_CASE("each rejection of a patched byte: header, counts, padding")
{
  auto const bytes = obs::encode_pcov(sample());
  REQUIRE(bytes.size() == 140);
  CHECK_FALSE(throws_format(bytes));
  auto flags = bytes;
  flags[6]   = std::byte{1};
  CHECK(throws_format(flags));
  CHECK(throws_format(patch_u32(bytes, 28, 0x1000'0000)));
  CHECK(throws_format(patch_u32(bytes, 64, 0x1000'0000)));
  CHECK(throws_format(patch_u32(bytes, 84, 0x1000'0000)));
  CHECK(throws_format(patch_u32(bytes, 120, 0x1000'0000)));
  auto padding = bytes;
  padding[55]  = std::byte{'x'};
  CHECK(throws_format(padding));
}

TEST_CASE("each rejection of a patched byte: ranges, edges and groups out of order")
{
  auto ranged             = sample();
  ranged.groups[0].ranges = {
      {.name = "a", .start = 0x100, .size = 0x20},
      {.name = "b", .start = 0x200, .size = 0x10}
  };
  auto const ranges = obs::encode_pcov(ranged);
  CHECK_FALSE(throws_format(ranges));
  CHECK(throws_format(swap_records(ranges, 68, 84, 16)));
  CHECK(throws_format(copy_record(ranges, 68, 84, 16)));

  auto edged            = sample();
  edged.groups[0].edges = {
      {.from = 0x100, .to = 0x104, .hits = 1},
      {.from = 0x104, .to = 0x100, .hits = 2}
  };
  auto const edges = obs::encode_pcov(edged);
  CHECK_FALSE(throws_format(edges));
  CHECK(throws_format(swap_records(edges, 124, 140, 16)));
  CHECK(throws_format(copy_record(edges, 124, 140, 16)));
  CHECK(throws_format(patch_u32(edges, 124 + 8, 0)));

  auto const groups = obs::encode_pcov({
      .groups = {small_group("aa"), small_group("bb")}
  });
  REQUIRE(groups.size() == 132);
  CHECK_FALSE(throws_format(groups));
  CHECK(throws_format(swap_records(groups, 12, 72, 60)));
  CHECK(throws_format(copy_record(groups, 12, 72, 60)));
}
