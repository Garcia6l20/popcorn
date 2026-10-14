#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/collect.hpp>
#include <popcorn/obs/coverage.hpp>

#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
namespace image = popcorn::image;
namespace obs   = popcorn::obs;

[[maybe_unused]] auto const builtin_arch = (popcorn::arch::armv8m::register_builtin(), true);
using popcorn::stop_reason;
using popcorn::testing::firmware;
using popcorn::testing::m33;
using fixture = popcorn::testing::machine_fixture<>;

struct parsed
{
  std::vector<std::string>                                             header;
  std::vector<std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>> entries;
};

auto parse(std::vector<std::byte> const& bytes) -> parsed
{
  parsed      out;
  std::size_t at = 0;
  while(true)
  {
    auto const begin = at;
    while(at < bytes.size() and bytes[at] != std::byte{'\n'})
    {
      ++at;
    }
    REQUIRE(at < bytes.size());
    std::string line(reinterpret_cast<char const*>(bytes.data() + begin), at - begin);
    ++at;
    out.header.push_back(line);
    if(line.starts_with("BB Table: "))
    {
      break;
    }
  }
  REQUIRE((bytes.size() - at) % 8 == 0);
  for(; at < bytes.size(); at += 8)
  {
    std::uint32_t offset{};
    std::uint16_t size{};
    std::uint16_t module{};
    std::memcpy(&offset, bytes.data() + at, 4);
    std::memcpy(&size, bytes.data() + at + 4, 2);
    std::memcpy(&module, bytes.data() + at + 6, 2);
    out.entries.emplace_back(offset, size, module);
  }
  return out;
}

auto ref(
    obs::image_kind  kind,
    std::string      path,
    std::string      digest,
    popcorn::address base,
    popcorn::address end) -> obs::image_ref
{
  return {
      .kind = kind, .path = std::move(path), .digest = std::move(digest), .base = base, .end = end};
}
}

TEST_CASE("drcov of the lines firmware: header fields exact, one entry per block that ran")
{
  auto const loaded = image::load_elf(firmware("lines"));
  auto const code   = std::ranges::find_if(loaded.segments, &image::segment::executable);
  REQUIRE(code != loaded.segments.end());

  fixture f{m33(), "lines", {}};
  f.target->enable_coverage({});
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  std::array const sources{
      obs::image_source{.kind = obs::image_kind::elf, .path = firmware("lines").string()}
  };
  auto const set  = obs::collect(*f.target, sources);
  auto const file = obs::drcov(set);
  CHECK(file.dropped == 0);
  auto const read = parse(file.bytes);

  auto const& blocks = set.groups.at(0).blocks;
  REQUIRE(read.header.size() == 6);
  CHECK(read.header[0] == "DRCOV VERSION: 2");
  CHECK(read.header[1] == "DRCOV FLAVOR: drcov");
  CHECK(read.header[2] == "Module Table: version 2, count 1");
  CHECK(read.header[3] == "Columns: id, base, end, entry, checksum, timestamp, path");
  CHECK(
      read.header[4]
      == std::format(
          " 0, 0x{:016x}, 0x{:016x}, 0x0000000000000000, 0x00000000, 0x00000000, {}", code->start,
          code->start + code->memory_size,
          std::filesystem::absolute(firmware("lines")).lexically_normal().string()));
  CHECK(read.header[5] == std::format("BB Table: {} bbs", blocks.size()));
  REQUIRE(read.entries.size() == blocks.size());
  for(auto const& [entry, block] : std::views::zip(read.entries, blocks))
  {
    CHECK(
        entry
        == std::tuple{
            block.start - code->start, static_cast<std::uint16_t>(block.size), std::uint16_t{0}});
  }
}

TEST_CASE("modules per image and group, raw paths, blocks outside every module dropped")
{
  obs::coverage_set const set{
      .groups = {
                 {.images =
               {ref(obs::image_kind::elf, "/a.elf", "aa", 0x1000, 0x2000),
                ref(obs::image_kind::elf, "/b.elf", "bb", 0x4000, 0x5000)},
           .blocks =
               {{.start = 0x1000, .size = 4, .hits = 1},
                {.start = 0x4010, .size = 8, .hits = 2},
                {.start = 0x9000, .size = 2, .hits = 1}}},
                 {.images = {ref(obs::image_kind::raw, "", "0123456789abcdef", 0x8000, 0x8100)},
           .blocks = {{.start = 0x8004, .size = 6, .hits = 1}}},
                 }
  };
  auto const file = obs::drcov(set);
  CHECK(file.dropped == 1);
  auto const read = parse(file.bytes);
  REQUIRE(read.header.size() == 8);
  CHECK(read.header[2] == "Module Table: version 2, count 3");
  CHECK(read.header[4].ends_with(", /a.elf"));
  CHECK(read.header[5].starts_with(" 1, 0x0000000000004000, 0x0000000000005000, "));
  CHECK(read.header[6].ends_with(", raw-0123456789ab"));
  CHECK(read.header[7] == "BB Table: 3 bbs");
  CHECK(
      read.entries
      == std::vector<std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>>{
          {0x0,  4, 0},
          {0x10, 8, 1},
          {0x4,  6, 2}
  });
}

TEST_CASE("two groups sharing a base and a block give one entry per module")
{
  obs::coverage_set const set{
      .groups = {
                 {.images = {ref(obs::image_kind::elf, "/one.elf", "aa", 0x1000, 0x2000)},
           .blocks = {{.start = 0x1010, .size = 4, .hits = 1}}},
                 {.images = {ref(obs::image_kind::elf, "/two.elf", "bb", 0x1000, 0x2000)},
           .blocks = {{.start = 0x1010, .size = 4, .hits = 5}}},
                 }
  };
  auto const read = parse(obs::drcov(set).bytes);
  CHECK(
      read.entries
      == std::vector<std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>>{
          {0x10, 4, 0},
          {0x10, 4, 1}
  });
}

TEST_CASE("an empty set has an empty module table and no entries")
{
  auto const read = parse(obs::drcov({}).bytes);
  CHECK(read.header[2] == "Module Table: version 2, count 0");
  CHECK(read.header[4] == "BB Table: 0 bbs");
  CHECK(read.entries.empty());
}

TEST_CASE("overlapping images of one group: the last one holding a block owns it")
{
  obs::coverage_set const set{
      .groups = {
                 {.images =
               {ref(obs::image_kind::elf, "/a.elf", "aa", 0x1000, 0x3000),
                ref(obs::image_kind::elf, "/b.elf", "bb", 0x2000, 0x2800)},
           .blocks = {{.start = 0x2100, .size = 4, .hits = 1}}},
                 }
  };
  auto const read = parse(obs::drcov(set).bytes);
  CHECK(
      read.entries
      == std::vector<std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>>{
          {0x100, 4, 1}
  });
}
