#include "thumb.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/collect.hpp>

#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace image = popcorn::image;
namespace obs   = popcorn::obs;
namespace thumb = popcorn::obs::thumb;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::firmware;
using popcorn::testing::m33;
using fixture = popcorn::testing::machine_fixture<>;

auto code(std::initializer_list<std::uint16_t> halfwords) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  for(auto const halfword : halfwords)
  {
    bytes.push_back(static_cast<std::byte>(halfword & 0xff));
    bytes.push_back(static_cast<std::byte>(halfword >> 8));
  }
  return bytes;
}

struct row
{
  address       at;
  std::uint32_t line;
};

auto synthetic(
    std::vector<std::byte>     bytes,
    std::initializer_list<row> rows,
    std::uint32_t              function_size) -> image::image
{
  image::image made;
  made.segments.push_back(
      {.name        = "code",
       .start       = 0x100,
       .bytes       = std::move(bytes),
       .memory_size = 0x100,
       .executable  = true});
  made.symbols.push_back(
      {.name = "f", .value = 0x100, .size = function_size, .kind = image::symbol_kind::function});
  made.files = {"a.c"};
  for(auto const& entry : rows)
  {
    made.lines.push_back({.addr = entry.at, .file = 0, .line = entry.line, .is_stmt = true});
  }
  made.digest = "d";
  return made;
}

auto group_of(
    std::vector<obs::block_hit> blocks,
    std::vector<obs::edge_hit>  edges  = {},
    std::uint32_t               scope  = 0x10,
    std::string                 digest = "d") -> obs::coverage_group
{
  return {
      .images = {{.kind = obs::image_kind::elf, .path = "/a.elf", .digest = std::move(digest)}},
      .ranges = {{.name = "f", .start = 0x100, .size = scope}},
      .blocks = std::move(blocks),
      .edges  = std::move(edges),
  };
}

auto lcov_of(
    image::image const&              made,
    std::vector<obs::coverage_group> groups,
    obs::lcov_options const&         options = {}) -> std::string
{
  obs::coverage_set const                set{.groups = std::move(groups)};
  std::vector<std::vector<image::image>> images(set.groups.size(), std::vector{made});
  return obs::lcov(set, images, options);
}

auto records(std::string const& text) -> std::vector<std::string>
{
  std::vector<std::string> lines;
  std::istringstream       in{text};
  for(std::string line; std::getline(in, line);)
  {
    lines.push_back(line);
  }
  return lines;
}

auto has(std::string const& text, std::string_view record) -> bool
{
  return std::ranges::contains(records(text), record);
}

auto starting(std::string const& text, std::string_view prefix) -> std::vector<std::string>
{
  return records(text)
       | std::views::filter([&](std::string const& line) { return line.starts_with(prefix); })
       | std::ranges::to<std::vector>();
}

auto nops(std::size_t count) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  for(std::size_t i = 0; i < count; ++i)
  {
    bytes.push_back(std::byte{0x00});
    bytes.push_back(std::byte{0xbf});
  }
  return bytes;
}
}

TEST_CASE("conditional_branch knows B T1, B T3, CBZ and CBNZ only")
{
  for(std::uint16_t cond = 0; cond < 14; ++cond)
  {
    CAPTURE(cond);
    auto const t1 = thumb::insn{
        .at = 0x100, .bits = static_cast<std::uint16_t>(0xd000 | cond << 8 | 0x02), .size = 2};
    CHECK(
        thumb::conditional_branch(t1)
        == thumb::cond_branch{.at = 0x100, .size = 2, .taken = 0x108, .fallthrough = 0x102});
    auto const t3 =
        thumb::insn{.at = 0x100, .bits = (0xf000u | cond << 6) << 16 | 0x8002, .size = 4};
    CHECK(
        thumb::conditional_branch(t3)
        == thumb::cond_branch{.at = 0x100, .size = 4, .taken = 0x108, .fallthrough = 0x104});
  }
  for(std::uint16_t const cond : {14, 15})
  {
    CHECK_FALSE(
        thumb::conditional_branch(
            {.at = 0x100, .bits = static_cast<std::uint16_t>(0xd000 | cond << 8), .size = 2}));
    CHECK_FALSE(
        thumb::conditional_branch(
            {.at = 0x100, .bits = (0xf000u | cond << 6) << 16 | 0x8000, .size = 4}));
  }
  CHECK(
      thumb::conditional_branch({.at = 0x100, .bits = 0xb3f8, .size = 2})
      == thumb::cond_branch{
          .at = 0x100, .size = 2, .taken = 0x100 + 4 + 126, .fallthrough = 0x102});
  CHECK(
      thumb::conditional_branch({.at = 0x100, .bits = 0xbbf8, .size = 2})
      == thumb::cond_branch{
          .at = 0x100, .size = 2, .taken = 0x100 + 4 + 126, .fallthrough = 0x102});
  CHECK_FALSE(thumb::conditional_branch({.at = 0x100, .bits = 0xe7fd, .size = 2}));
  CHECK_FALSE(thumb::conditional_branch({.at = 0x100, .bits = 0xf001'b856, .size = 4}));
  CHECK_FALSE(thumb::conditional_branch({.at = 0x100, .bits = 0xf001'f854, .size = 4}));
  CHECK(thumb::is_call({.at = 0, .bits = 0xf001'f854, .size = 4}));
  CHECK(thumb::is_call({.at = 0, .bits = 0x4798, .size = 2}));
  CHECK_FALSE(thumb::is_call({.at = 0, .bits = 0x4770, .size = 2}));
  CHECK(thumb::it_length({.at = 0, .bits = 0xbf08, .size = 2}) == 1);
  CHECK(thumb::it_length({.at = 0, .bits = 0xbf0c, .size = 2}) == 2);
  CHECK(thumb::it_length({.at = 0, .bits = 0xbf01, .size = 2}) == 4);
}

TEST_CASE("a line counts the most executed instruction, not the sum of its blocks")
{
  auto const made = synthetic(
      nops(8),
      {
          {0x100, 7},
          {0x108, 0}
  },
      0x10);
  CHECK(
      lcov_of(
          made,
          {
              group_of(
                  {{.start = 0x100, .size = 4, .hits = 1},
                    {.start = 0x104, .size = 4, .hits = 3}}
                  )
  })
      == "TN:\n"
         "SF:a.c\n"
         "FN:7,f\n"
         "FNDA:1,f\n"
         "FNF:1\n"
         "FNH:1\n"
         "BRF:0\n"
         "BRH:0\n"
         "DA:7,3\n"
         "LF:1\n"
         "LH:1\n"
         "end_of_record\n");
}

TEST_CASE("overlapping blocks add up on their shared instructions")
{
  auto const made = synthetic(
      nops(8),
      {
          {0x100, 8},
          {0x10c, 0}
  },
      0x10);
  auto const text = lcov_of(
      made,
      {
          group_of(
              {{.start = 0x100, .size = 12, .hits = 1}, {.start = 0x104, .size = 8, .hits = 3}}
              )
  });
  CHECK(has(text, "DA:8,4"));
}

TEST_CASE("scope, unexecuted rows, line-0 rows and empty rows")
{
  auto const made = synthetic(
      nops(8),
      {
          {0x100, 5},
          {0x104, 4},
          {0x104, 6},
          {0x108, 0},
          {0x10a, 9},
          {0x10c, 0}
  },
      0x10);
  auto const text = lcov_of(
      made, {
                group_of({{.start = 0x100, .size = 4, .hits = 2}},
                {                                     },
                8)
  });
  CHECK(starting(text, "DA:") == std::vector<std::string>{"DA:5,2", "DA:6,0"});
  CHECK(has(text, "LF:2"));
  CHECK(has(text, "LH:1"));
}

TEST_CASE("rows of one line keep their largest count")
{
  auto const made = synthetic(
      nops(8),
      {
          {0x100, 3},
          {0x104, 4},
          {0x108, 3},
          {0x10c, 0}
  },
      0x10);
  auto const text = lcov_of(
      made,
      {
          group_of(
              {{.start = 0x100, .size = 4, .hits = 5}, {.start = 0x108, .size = 4, .hits = 2}}
              )
  });
  CHECK(starting(text, "DA:") == std::vector<std::string>{"DA:3,5", "DA:4,0"});
}

TEST_CASE("groups of different images add their line, function and branch counts")
{
  auto const made = synthetic(
      code({
          0x2800, 0xd001, 0xbf00, 0x4770, 0x4770
  }),
      {{0x100, 7}, {0x104, 8}, {0x108, 9}, {0x10a, 0}}, 0x0a);
  auto const first = group_of(
      {
          {.start = 0x104, .size = 4, .hits = 1}
  },
      {}, 0x10, "one");
  auto const second = group_of(
      {
          {.start = 0x100, .size = 4, .hits = 5},
          {.start = 0x104, .size = 4, .hits = 3},
          {.start = 0x108, .size = 2, .hits = 2}
  },
      {{.from = 0x100, .to = 0x104, .hits = 3}, {.from = 0x100, .to = 0x108, .hits = 2}}, 0x10,
      "two");
  auto const text = lcov_of(made, {first, second});
  CHECK(starting(text, "DA:") == std::vector<std::string>{"DA:7,5", "DA:8,4", "DA:9,2"});
  CHECK(has(text, "FNDA:5,f"));
  CHECK(starting(text, "BRDA:") == std::vector<std::string>{"BRDA:7,0,0,2", "BRDA:7,0,1,3"});
  CHECK(has(text, "BRH:2"));
}

TEST_CASE("branch counts: never reached, blocks sharing the branch, branch to the next instruction")
{
  auto const made = synthetic(
      code({
          0x2800, 0xd001, 0xbf00, 0x4770, 0x4770
  }),
      {{0x100, 7}, {0x10a, 0}}, 0x0a);
  CHECK(
      starting(lcov_of(made, {group_of({})}), "BRDA:")
      == std::vector<std::string>{"BRDA:7,0,0,-", "BRDA:7,0,1,-"});
  auto const shared = lcov_of(
      made,
      {
          group_of(
              {{.start = 0x100, .size = 4, .hits = 3}, {.start = 0x102, .size = 2, .hits = 4}},
              {{.from = 0x100, .to = 0x104, .hits = 2},
                {.from = 0x100, .to = 0x108, .hits = 1},
                {.from = 0x102, .to = 0x104, .hits = 1},
                {.from = 0x102, .to = 0x108, .hits = 3}}
              )
  });
  CHECK(starting(shared, "BRDA:") == std::vector<std::string>{"BRDA:7,0,0,4", "BRDA:7,0,1,3"});

  auto const next = synthetic(
      code({
          0x2800, 0xd0ff, 0x4770
  }),
      {{0x100, 7}, {0x106, 0}}, 6);
  auto const text = lcov_of(
      next, {
                group_of(
                    {{.start = 0x100, .size = 4, .hits = 5} },
                    {{.from = 0x100, .to = 0x104, .hits = 5}}
                    )
  });
  CHECK(starting(text, "BRDA:") == std::vector<std::string>{"BRDA:7,0,0,5", "BRDA:7,0,1,0"});
}

TEST_CASE("a literal pool after a return is not decoded for branches")
{
  auto const made = synthetic(
      code({
          0x4770, 0xd001
  }),
      {{0x100, 3}, {0x104, 0}}, 4);
  auto const text = lcov_of(
      made, {
                group_of({{.start = 0x100, .size = 2, .hits = 1}}
                )
  });
  CHECK(starting(text, "BRDA:").empty());
  CHECK(has(text, "BRF:0"));
}

TEST_CASE(
    "branches inside IT blocks are not reported, and a conditional return does not stop descent")
{
  auto const predicated = synthetic(
      code({
          0xbf0c, 0xf000, 0x8002, 0x2001, 0xd1fa, 0x4770
  }),
      {{0x100, 3}, {0x10c, 0}}, 0x0c);
  CHECK(
      starting(lcov_of(predicated, {group_of({})}), "BRDA:")
      == std::vector<std::string>{"BRDA:3,0,0,-", "BRDA:3,0,1,-"});
  auto const ran = group_of(
      {
          {.start = 0x108, .size = 2, .hits = 1}
  },
      {{.from = 0x108, .to = 0x100, .hits = 1}});
  auto const text = lcov_of(predicated, {ran});
  CHECK(starting(text, "BRDA:") == std::vector<std::string>{"BRDA:3,0,0,1", "BRDA:3,0,1,0"});

  auto const returning = synthetic(
      code({
          0xbf08, 0x4770, 0xd1fc, 0x4770
  }),
      {{0x100, 3}, {0x108, 0}}, 8);
  CHECK(
      starting(lcov_of(returning, {group_of({})}), "BRDA:")
      == std::vector<std::string>{"BRDA:3,0,0,-", "BRDA:3,0,1,-"});
}

TEST_CASE("without branches there are no branch records")
{
  auto const made = synthetic(
      code({
          0x2800, 0xd001, 0xbf00, 0x4770, 0x4770
  }),
      {{0x100, 7}, {0x10a, 0}}, 0x0a);
  auto const text = lcov_of(
      made,
      {
          group_of({{.start = 0x100, .size = 4, .hits = 1}}
          )
  },
      {.branches = false, .test_name = "t"});
  CHECK(text.starts_with("TN:t\n"));
  CHECK(starting(text, "BR").empty());
}

TEST_CASE("lines.elf: every record equals the hand-derived table")
{
  auto const loaded = image::load_elf(firmware("lines"));
  auto const path   = std::ranges::find_if(
      loaded.files, [](std::string const& file) { return file.ends_with("lines.c"); });
  REQUIRE(path != loaded.files.end());

  fixture          f{m33(), "lines", {}};
  std::array const functions{
      std::string{"clamp"}, std::string{"count_bits"}, std::string{"classify"}};
  f.target->enable_coverage(obs::function_ranges(std::span{&loaded, 1}, functions));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  std::array const sources{
      obs::image_source{.kind = obs::image_kind::elf, .path = firmware("lines").string()}
  };
  auto const                                   set = obs::collect(*f.target, sources);
  std::vector<std::vector<image::image>> const images{{loaded}};
  auto const                                   text = obs::lcov(set, images, {});

  CHECK(
      text
      == "TN:\n"
         "SF:"
      + *path
      + "\n"
        "FN:8,clamp\n"
        "FN:21,count_bits\n"
        "FN:32,classify\n"
        "FNDA:2,clamp\n"
        "FNDA:1,count_bits\n"
        "FNDA:2,classify\n"
        "FNF:3\n"
        "FNH:3\n"
        "BRDA:9,0,0,1\n"
        "BRDA:9,0,1,1\n"
        "BRDA:23,0,0,4\n"
        "BRDA:23,0,1,1\n"
        "BRDA:34,0,0,1\n"
        "BRDA:34,0,1,1\n"
        "BRF:6\n"
        "BRH:6\n"
        "DA:9,2\n"
        "DA:13,1\n"
        "DA:18,2\n"
        "DA:21,1\n"
        "DA:22,1\n"
        "DA:23,5\n"
        "DA:25,4\n"
        "DA:26,4\n"
        "DA:29,1\n"
        "DA:34,2\n"
        "DA:40,1\n"
        "DA:44,1\n"
        "DA:47,1\n"
        "LF:13\n"
        "LH:13\n"
        "end_of_record\n");

  char const* const genhtml = std::getenv("POPCORN_GENHTML");
  if(genhtml == nullptr)
  {
    MESSAGE("genhtml check skipped, set POPCORN_GENHTML to run it");
    return;
  }
  char const* const project = std::getenv("POPCORN_PROJECT_DIR");
  char const* const scratch = std::getenv("POPCORN_OBS_SCRATCH");
  REQUIRE(project != nullptr);
  REQUIRE(scratch != nullptr);
  auto const out = std::filesystem::path{scratch} / "genhtml";
  std::filesystem::create_directories(out);
  auto const info = out / "lines.info";
  std::ofstream{info} << text;
  auto const log     = out / "genhtml.log";
  auto const command = std::format(
      "cd '{}' && '{}' --branch-coverage --function-coverage -o '{}' '{}' > '{}' 2>&1", project,
      genhtml, (out / "html").string(), info.string(), log.string());
  CHECK(std::system(command.c_str()) == 0);
  std::ifstream      logged{log};
  std::ostringstream content;
  content << logged.rdbuf();
  CHECK(content.str().find("WARNING") == std::string::npos);
  MESSAGE(content.str());
}

TEST_CASE("a switch case reached only through TBB reports its branch once no IT precedes it")
{
  auto const made = synthetic(
      code({
          0xe8df, 0xf000, 0x0401, 0x2900, 0xd100, 0x4770, 0x4770
  }),
      {{0x100, 3}, {0x106, 5}, {0x10c, 6}, {0x10e, 0}}, 0x0e);
  CHECK(
      starting(lcov_of(made, {group_of({})}), "BRDA:")
      == std::vector<std::string>{"BRDA:5,0,0,-", "BRDA:5,0,1,-"});
}

TEST_CASE("a pool word after a call that never returns is decoded, a known limit")
{
  auto const made = synthetic(
      code({
          0xf000, 0xf800, 0xd1fe
  }),
      {{0x100, 3}, {0x108, 0}}, 8);
  CHECK(
      starting(lcov_of(made, {group_of({})}), "BRDA:")
      == std::vector<std::string>{"BRDA:3,0,0,-", "BRDA:3,0,1,-"});
}

TEST_CASE("FN takes the smallest line among the rows at the function's address")
{
  auto const made = synthetic(
      nops(4),
      {
          {0x100, 9},
          {0x100, 4},
          {0x100, 6},
          {0x108, 0}
  },
      8);
  CHECK(starting(lcov_of(made, {group_of({})}), "FN:") == std::vector<std::string>{"FN:4,f"});
}

TEST_CASE("functions of one name in one file add their entries")
{
  auto made = synthetic(
      nops(8),
      {
          {0x100, 3},
          {0x108, 7},
          {0x110, 0}
  },
      8);
  made.symbols.push_back(
      {.name = "f", .value = 0x108, .size = 8, .kind = image::symbol_kind::function});
  auto const text = lcov_of(
      made,
      {
          group_of(
              {{.start = 0x100, .size = 8, .hits = 2}, {.start = 0x108, .size = 8, .hits = 3}}
              )
  });
  CHECK(starting(text, "FNDA:") == std::vector<std::string>{"FNDA:5,f"});
}

TEST_CASE("every file record starts with its TN record")
{
  auto made = synthetic(
      nops(8),
      {
          {0x100, 3},
          {0x108, 0}
  },
      8);
  made.files = {"a.c", "b.c"};
  made.lines.insert(made.lines.begin() + 1, {.addr = 0x104, .file = 1, .line = 2, .is_stmt = true});
  auto const text = lcov_of(
      made,
      {
          group_of({{.start = 0x100, .size = 8, .hits = 1}}
          )
  },
      {.test_name = "t"});
  CHECK(starting(text, "TN:") == std::vector<std::string>{"TN:t", "TN:t"});
  auto const lines = records(text);
  for(auto const& [index, line] : std::views::enumerate(lines))
  {
    if(line.starts_with("SF:"))
    {
      REQUIRE(index > 0);
      CHECK(lines[static_cast<std::size_t>(index) - 1] == "TN:t");
    }
  }
}

TEST_CASE("a function is filed under the file of its entry row, not under an inlined header")
{
  auto made  = synthetic(nops(8), {}, 8);
  made.files = {"c.c", "h.h"};
  made.lines = {
      {.addr = 0x100, .file = 0, .line = 747, .is_stmt = true},
      {.addr = 0x100, .file = 1, .line = 76,  .is_stmt = true},
      {.addr = 0x100, .file = 0, .line = 737, .is_stmt = true},
      {.addr = 0x100, .file = 0, .line = 747, .is_stmt = true},
      {.addr = 0x108, .file = 0, .line = 0,   .is_stmt = true},
  };
  auto const text = lcov_of(
      made, {
                group_of({{.start = 0x100, .size = 8, .hits = 1}}
                )
  });
  CHECK(starting(text, "FN:") == std::vector<std::string>{"FN:737,f"});
  CHECK(starting(text, "SF:") == std::vector<std::string>{"SF:c.c"});
}
