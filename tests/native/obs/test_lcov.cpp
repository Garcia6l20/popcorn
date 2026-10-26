#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/arch/armv8m/thumb.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/collect.hpp>

#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <elf.h>

#include <algorithm>
#include <array>
#include <chrono>
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
#include <tuple>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace image = popcorn::image;
namespace obs   = popcorn::obs;
namespace thumb = popcorn::arch::armv8m::thumb;

[[maybe_unused]] auto const builtin_arch = (popcorn::arch::armv8m::register_builtin(), true);
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
  made.elf_machine = EM_ARM;
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

void check_genhtml(std::string const& text, std::string_view name)
{
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
  auto const out = std::filesystem::path{scratch} / "genhtml" / name;
  std::filesystem::create_directories(out);
  auto const info = out / std::format("{}.info", name);
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

auto cut_line(std::string_view text) -> std::uint32_t
{
  char const* const project = std::getenv("POPCORN_PROJECT_DIR");
  REQUIRE(project != nullptr);
  std::ifstream source{std::filesystem::path{project} / "tests" / "firmware" / "obs" / "cut.c"};
  REQUIRE(source.is_open());
  std::uint32_t number = 0;
  for(std::string line; std::getline(source, line);)
  {
    ++number;
    if(line.find(text) != std::string::npos)
    {
      return number;
    }
  }
  FAIL("no line of cut.c holds ", text);
  return 0;
}

auto first_address(image::image const& loaded, std::uint32_t line) -> address
{
  auto const found = std::ranges::find(loaded.lines, line, &image::line_row::line);
  REQUIRE(found != loaded.lines.end());
  return found->addr;
}

auto cut_lcov(core::machine& target) -> std::string
{
  std::array const sources{
      obs::image_source{.kind = obs::image_kind::elf, .path = firmware("cut").string()}
  };
  std::vector<std::vector<image::image>> const images{{image::load_elf(firmware("cut"))}};
  return obs::lcov(obs::collect(target, sources), images, {});
}

auto call_to_stop(fixture const& f, std::string_view function, std::uint32_t argument)
    -> stop_reason
{
  f.target->begin_call(
      {.entry           = popcorn::testing::sym("cut", function),
       .args            = {{.kind = popcorn::arch::value_kind::i32, .bits = argument}},
       .returns         = popcorn::arch::value_kind::i32,
       .abi             = popcorn::arch::float_abi::soft,
       .return_sentinel = 0x3000'0000});
  auto const reason = f.run_to_stop().reason;
  std::ignore       = f.target->end_call();
  return reason;
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
         "FN:7,7,f\n"
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

TEST_CASE("a branch on a line whose own rows lie outside the scope still gets a DA record")
{
  auto made = synthetic(
      code({
          0xbf00, 0xbf00, 0x2800, 0xd000, 0xbf00, 0x4770
  }),
      {{0x100, 5}, {0x102, 6}}, 0x0c);
  made.files = {"tests/firmware/obs/lines.c"};
  obs::coverage_group group{
      .images = {{.kind = obs::image_kind::elf, .path = "/a.elf", .digest = "d"}},
      .ranges =
          {{.name = "head", .start = 0x100, .size = 2},
                 {.name = "tail", .start = 0x106, .size = 8}},
  };
  auto const text = lcov_of(made, {group});
  CHECK(starting(text, "BRDA:") == std::vector<std::string>{"BRDA:6,0,0,-", "BRDA:6,0,1,-"});
  CHECK(starting(text, "DA:") == std::vector<std::string>{"DA:5,0", "DA:6,0"});
  check_genhtml(text, "branch_row");

  group.blocks = {
      {.start = 0x100, .size = 8, .hits = 3}
  };
  auto const hit = lcov_of(made, {group});
  CHECK(starting(hit, "DA:") == std::vector<std::string>{"DA:5,3", "DA:6,3"});

  group.blocks = {
      {.start = 0x104, .size = 4, .hits = 3}
  };
  auto const late = lcov_of(made, {group});
  CHECK(starting(late, "DA:") == std::vector<std::string>{"DA:5,0", "DA:6,3"});

  group.blocks = {
      {.start = 0x100, .size = 8, .hits = 3}
  };
  auto const plain = lcov_of(made, {group}, {.branches = false});
  CHECK(starting(plain, "DA:") == std::vector<std::string>{"DA:5,3"});
  CHECK(starting(plain, "BRDA:").empty());
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

TEST_CASE("a UDF ends descent, an SVC falls through to the branch after it")
{
  auto const narrow = synthetic(
      code({
          0xde00, 0xd1fd, 0x4770
  }),
      {{0x100, 3}, {0x106, 0}}, 6);
  CHECK(starting(lcov_of(narrow, {group_of({})}), "BRDA:").empty());
  auto const wide = synthetic(
      code({
          0xf7f0, 0xa000, 0xd1fc, 0x4770
  }),
      {{0x100, 3}, {0x108, 0}}, 8);
  CHECK(starting(lcov_of(wide, {group_of({})}), "BRDA:").empty());
  auto const call = synthetic(
      code({
          0xdf00, 0xd1fd, 0x4770
  }),
      {{0x100, 3}, {0x106, 0}}, 6);
  CHECK(
      starting(lcov_of(call, {group_of({})}), "BRDA:")
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
               "FN:7,18,clamp\n"
               "FN:20,29,count_bits\n"
               "FN:31,47,classify\n"
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
               "DA:7,2\n"
               "DA:9,2\n"
               "DA:13,1\n"
               "DA:18,2\n"
               "DA:20,1\n"
               "DA:21,1\n"
               "DA:22,1\n"
               "DA:23,5\n"
               "DA:25,4\n"
               "DA:26,4\n"
               "DA:29,1\n"
               "DA:31,2\n"
               "DA:34,2\n"
               "DA:40,1\n"
               "DA:44,1\n"
               "DA:47,1\n"
               "LF:16\n"
               "LH:16\n"
               "end_of_record\n");

  check_genhtml(text, "lines");
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

TEST_CASE("without a debug function, FN takes the smallest line among the rows at its address")
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
  CHECK(starting(lcov_of(made, {group_of({})}), "FN:") == std::vector<std::string>{"FN:4,9,f"});
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

TEST_CASE("without a debug function, FN takes the file of its entry row, not an inlined header")
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
  CHECK(starting(text, "FN:") == std::vector<std::string>{"FN:737,747,f"});
  CHECK(starting(text, "SF:") == std::vector<std::string>{"SF:c.c"});
}

TEST_CASE("FN takes its debug function's declaration, its end the last line of that file in it")
{
  auto made  = synthetic(nops(8), {}, 8);
  made.files = {"c.c", "h.h"};
  made.lines = {
      {.addr = 0x100, .file = 1, .line = 976, .is_stmt = true},
      {.addr = 0x104, .file = 0, .line = 745, .is_stmt = true},
      {.addr = 0x106, .file = 0, .line = 740, .is_stmt = true},
      {.addr = 0x108, .file = 0, .line = 990, .is_stmt = true},
      {.addr = 0x10a, .file = 0, .line = 0,   .is_stmt = true},
  };
  made.functions = {
      {.low = 0x100, .high = 0x108, .file = 0, .line = 737}
  };
  auto const text = lcov_of(
      made, {
                group_of({{.start = 0x100, .size = 8, .hits = 3}}
                )
  });
  CHECK(starting(text, "FN:") == std::vector<std::string>{"FN:737,745,f"});
  CHECK(has(text, "DA:737,3"));
  CHECK(starting(text, "SF:") == std::vector<std::string>{"SF:c.c", "SF:h.h"});
}

TEST_CASE("FN ends before the next function of its file starts, never before its own start")
{
  auto made = synthetic(
      nops(8),
      {
          {0x100, 10},
          {0x104, 30},
          {0x108, 4 },
          {0x10c, 0 }
  },
      8);
  made.symbols.push_back(
      {.name = "g", .value = 0x108, .size = 4, .kind = image::symbol_kind::function});
  made.functions = {
      {.low = 0x108, .high = 0x10c, .file = 0, .line = 20},
  };
  CHECK(
      starting(lcov_of(made, {group_of({})}), "FN:")
      == std::vector<std::string>{"FN:10,19,f", "FN:20,20,g"});
}

TEST_CASE("FN of one name keeps the largest end over its functions and groups")
{
  auto first = synthetic(
      nops(8),
      {
          {0x100, 12},
          {0x108, 0 }
  },
      8);
  first.functions = {
      {.low = 0x100, .high = 0x108, .file = 0, .line = 10}
  };
  auto second  = first;
  second.lines = {
      {.addr = 0x100, .file = 0, .line = 15, .is_stmt = true},
      {.addr = 0x108, .file = 0, .line = 0,  .is_stmt = true}
  };
  second.digest = "e";
  auto twice    = first;
  twice.symbols.push_back(
      {.name = "f", .value = 0x108, .size = 4, .kind = image::symbol_kind::function});
  twice.lines = {
      {.addr = 0x100, .file = 0, .line = 12, .is_stmt = true},
      {.addr = 0x108, .file = 0, .line = 14, .is_stmt = true},
      {.addr = 0x10c, .file = 0, .line = 0,  .is_stmt = true}
  };
  twice.functions.push_back({.low = 0x108, .high = 0x10c, .file = 0, .line = 10});
  CHECK(starting(lcov_of(twice, {group_of({})}), "FN:") == std::vector<std::string>{"FN:10,14,f"});
  obs::coverage_set const set{
      .groups = {group_of({}), group_of({}, {}, 0x10, "e")}
  };
  std::vector<std::vector<image::image>> const images{{first}, {second}};
  CHECK(starting(obs::lcov(set, images, {}), "FN:") == std::vector<std::string>{"FN:10,15,f"});
}

TEST_CASE("cut.elf: an exit, an intercept and a fault inside a block count only what ran")
{
  using popcorn::testing::sym;
  fixture f{m33(), "cut", {}};
  f.target->enable_coverage({});
  REQUIRE(f.run_to_stop({.until = sym("cut", "cut_mid")}).reason == stop_reason::address);
  auto const hook = f.target->add_intercept(sym("cut", "pick"), [](core::intercept_call const&) {
    return core::intercept_result::returns(
        popcorn::arch::call_arg{.kind = popcorn::arch::value_kind::i32, .bits = 7});
  });
  CHECK(call_to_stop(f, "use_pick", 5) == stop_reason::returned);
  CHECK(call_to_stop(f, "probe", 0x6000'0000) == stop_reason::fault);

  auto const loaded = image::load_elf(firmware("cut"));
  auto const probed = cut_line("if(*(volatile uint32_t const*)at == 5u)");
  CHECK(
      f.target->coverage().cuts
      == std::vector<core::cut_counter>{
          {.start = sym("cut", "pick"),     .at = sym("cut",            "pick"),    .hits = 1},
          {.start = sym("cut", "straight"), .at = sym("cut",            "cut_mid"), .hits = 1},
          {.start = sym("cut", "probe"),    .at = first_address(loaded, probed),    .hits = 1},
  });

  auto const text = cut_lcov(*f.target);
  for(auto const line : {cut_line("if(v > 3u)"), cut_line("if(sink > 9u)"), probed})
  {
    CAPTURE(line);
    CHECK(has(text, std::format("DA:{},0", line)));
    CHECK(has(text, std::format("BRDA:{},0,0,-", line)));
    CHECK(has(text, std::format("BRDA:{},0,1,-", line)));
  }
  CHECK(has(text, std::format("DA:{},1", cut_line("sink = v;"))));
  CHECK(has(text, std::format("DA:{},1", cut_line("sink = 1u;"))));
  CHECK(has(text, "FNDA:0,pick"));
  CHECK(has(text, "FNDA:1,probe"));
  check_genhtml(text, "cut");

  f.target->clear_coverage();
  CHECK(f.target->coverage().cuts.empty());
}

TEST_CASE("cut.elf: a fault that enters its handler leaves the faulting line not run")
{
  using popcorn::testing::sym;
  core::machine target{
      m33(),
      popcorn::arch::armv8m::factory(),
      {.faults = popcorn::arch::fault_policy::enter_handler}};
  target.load(popcorn::testing::elf("cut"));
  target.reset();
  target.enable_coverage({});
  auto const write = [&](std::string_view name, std::uint64_t value) {
    target.regs().write(*target.regs().find(name), value);
  };
  write("r0", 0x6000'0000);
  write("lr", sym("cut", "Reset_Handler") | 1u);
  write("pc", sym("cut", "probe"));
  core::slice_request request{.until = sym("cut", "HardFault_Handler")};
  request.wall_budget = std::chrono::seconds{60};
  auto const ran      = target.run_slice(request);
  REQUIRE(ran.stopped.has_value());
  CHECK(ran.stopped->reason == stop_reason::address);

  auto const loaded = image::load_elf(firmware("cut"));
  auto const probed = cut_line("if(*(volatile uint32_t const*)at == 5u)");
  CHECK(
      target.coverage().cuts
      == std::vector<core::cut_counter>{
          {.start = sym("cut", "probe"), .at = first_address(loaded, probed), .hits = 1}
  });
  auto const text = cut_lcov(target);
  CHECK(has(text, std::format("DA:{},0", probed)));
  CHECK(has(text, std::format("DA:{},1", cut_line("sink = 1u;"))));
}

TEST_CASE("cut.elf: a stop at the entry of a block that loops on itself records no cut")
{
  fixture f{m33(), "cut", {}};
  f.target->enable_coverage({});
  REQUIRE(f.run_to_stop({.budget_left = popcorn::icount{40}}).reason == stop_reason::budget);
  CHECK(f.target->coverage().cuts.empty());
}

TEST_CASE("cut.elf: an exit where a block ended records no cut")
{
  fixture    f{m33(), "cut", {}};
  auto const loaded = image::load_elf(firmware("cut"));
  f.target->enable_coverage({});
  auto const after_branch = first_address(loaded, cut_line("note(0u);"));
  REQUIRE(f.run_to_stop({.until = after_branch}).reason == stop_reason::address);
  CHECK(f.target->coverage().cuts.empty());
}

TEST_CASE("cut.elf: a block outside every range closes the block counted before it")
{
  using popcorn::testing::sym;
  fixture    f{m33(), "cut", {}};
  auto const loaded = image::load_elf(firmware("cut"));
  auto const head   = sym("cut", "cut_mid");
  f.target->enable_coverage({
      {.name  = "head",
       .start = head,
       .size  = first_address(loaded, cut_line("note(0u);")) - head}
  });
  REQUIRE(f.run_to_stop({.until = head}).reason == stop_reason::address);
  f.set("lr", sym("cut", "straight") | 1u);
  auto       visits = 0;
  auto const hook   = f.target->add_intercept(head + 2, [&visits](core::intercept_call const&) {
    return ++visits == 1 ? core::intercept_result::proceed()
                         : core::intercept_result::stop(stop_reason::user);
  });
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(visits == 2);
  CHECK(f.target->coverage().cuts.empty());
}
