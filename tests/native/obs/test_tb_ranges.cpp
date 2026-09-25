#include "tb.hpp"
#include "thumb.hpp"

#include <popcorn/core/coverage.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/obs/coverage.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace obs  = popcorn::obs;
using popcorn::address;
using popcorn::address_range;
using popcorn::stop_reason;
using popcorn::testing::firmware;
using popcorn::testing::m33;
using popcorn::testing::m33_code_alias;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

struct block_records
{
  std::map<address, std::map<std::uint32_t, std::uint64_t>> by_pc;

  auto sink() -> core::trace_sink
  {
    return [this](std::span<core::trace_record const> records) {
      for(auto const& record : records)
      {
        if(record.kind == core::trace_kind::block)
        {
          ++by_pc[record.payload[0]][record.aux];
        }
      }
    };
  }
};

auto memory_of(fixture const& f) -> obs::memory_reader
{
  return [&f](address at, std::span<std::byte> out) -> std::size_t {
    auto const read = f.target->mem().read_block(at, out);
    return read ? out.size() : read.error().at - at;
  };
}

auto code_of(fixture const& f) -> obs::code_bytes
{
  return obs::code_bytes{f.target->loaded_images(), memory_of(f)};
}

auto insns_in(address start, std::uint32_t size, obs::code_bytes& code) -> std::uint32_t
{
  auto const    bytes  = code.read(start, size);
  std::uint32_t offset = 0;
  std::uint32_t count  = 0;
  while(offset < size)
  {
    auto const decoded = popcorn::obs::thumb::read(start + offset, bytes.subspan(offset));
    REQUIRE(decoded.has_value());
    offset += decoded->size;
    ++count;
  }
  CHECK(offset == size);
  return count;
}

auto executable_ranges(fixture const& f) -> std::vector<address_range>
{
  std::vector<address_range> ranges;
  for(auto const& loaded : f.target->loaded_images())
  {
    for(auto const& segment : loaded.segments)
    {
      if(segment.executable and segment.memory_size != 0)
      {
        ranges.push_back(
            {.name = segment.name, .start = segment.start, .size = segment.memory_size});
      }
    }
  }
  return ranges;
}

auto inside(std::span<address_range const> ranges, address at) -> bool
{
  return std::ranges::any_of(ranges, [at](address_range const& range) {
    return range.start <= at and std::uint64_t{at} < std::uint64_t{range.start} + range.size;
  });
}

auto sources_of(std::string_view name) -> std::vector<obs::image_source>
{
  return {
      {.kind = obs::image_kind::elf, .path = firmware(name).string()}
  };
}

auto blocks_at(obs::coverage_set const& set, address start)
    -> std::map<std::uint32_t, std::uint64_t>
{
  std::map<std::uint32_t, std::uint64_t> sizes;
  for(auto const& block : set.groups.at(0).blocks)
  {
    if(block.start == start)
    {
      sizes[block.size] += block.hits;
    }
  }
  return sizes;
}

auto oracle_mismatches(
    fixture const&                 f,
    obs::coverage_set const&       set,
    block_records const&           seen,
    std::span<address_range const> ranges) -> std::vector<std::string>
{
  auto                     code = code_of(f);
  std::vector<std::string> mismatches;
  for(auto const& [pc, counts] : seen.by_pc)
  {
    if(not inside(ranges, pc))
    {
      continue;
    }
    auto const                             sizes = blocks_at(set, pc);
    std::uint64_t                          held  = 0;
    std::map<std::uint32_t, std::uint64_t> decoded;
    for(auto const& [size, hits] : sizes)
    {
      decoded[insns_in(pc, size, code)] += hits;
      held += hits;
    }
    std::uint64_t engine = 0;
    for(auto const& [insns, count] : counts)
    {
      engine += count;
      if(not decoded.contains(insns))
      {
        mismatches.push_back(
            std::format(
                "{:#010x}: engine ran {} instructions {} times, collect has {} sizes", pc, insns,
                count, sizes.size()));
      }
    }
    for(auto const& [insns, hits] : decoded)
    {
      if(not counts.contains(insns))
      {
        mismatches.push_back(
            std::format(
                "{:#010x}: collect has a {}-instruction size the engine never ran", pc, insns));
      }
    }
    if(engine != held)
    {
      mismatches.push_back(
          std::format("{:#010x}: engine ran {} blocks, collect holds {}", pc, engine, held));
    }
  }
  return mismatches;
}

auto pure_mismatches(
    fixture const&                 f,
    block_records const&           seen,
    std::span<address_range const> skip) -> std::vector<std::string>
{
  auto                     code = code_of(f);
  std::vector<std::string> mismatches;
  for(auto const& [pc, counts] : seen.by_pc)
  {
    if(inside(skip, pc))
    {
      continue;
    }
    auto const decoded = obs::decode_tb(pc, code).insns;
    for(auto const& [insns, count] : counts)
    {
      if(insns != decoded)
      {
        mismatches.push_back(
            std::format("{:#010x}: engine {} instructions, decoded {}", pc, insns, decoded));
      }
    }
  }
  return mismatches;
}

auto multi_size_starts(obs::coverage_set const& set) -> std::size_t
{
  std::map<address, std::size_t> sizes;
  for(auto const& block : set.groups.at(0).blocks)
  {
    ++sizes[block.start];
  }
  return static_cast<std::size_t>(
      std::ranges::count_if(sizes, [](auto const& entry) { return entry.second > 1; }));
}

auto require_none(std::vector<std::string> const& mismatches)
{
  for(auto const& mismatch : mismatches)
  {
    MESSAGE(mismatch);
  }
  CHECK(mismatches.empty());
}

auto tb_ranges(fixture const& f) -> std::vector<address_range>
{
  auto       ranges = executable_ranges(f);
  auto const ram    = sym("tb", "ram_code") - 0x2000'0000 + 0x0a00'0000;
  ranges.push_back({.name = "ram_code", .start = ram, .size = 8});
  return ranges;
}

auto ldm_range() -> address_range
{
  return {.name = "ldm_run", .start = sym("tb", "ldm_run"), .size = 0x400};
}

struct tb_run
{
  fixture                    f{m33_code_alias(), "tb", {}};
  block_records              seen;
  obs::coverage_set          set;
  std::vector<address_range> ranges;
};

void run_tb(tb_run& run, bool hooked)
{
  run.ranges = tb_ranges(run.f);
  run.f.target->enable_coverage(run.ranges);
  auto const sink = run.f.target->add_trace_sink({core::trace_kind::block}, run.seen.sink());
  std::optional<core::intercept_handle> hook;
  if(hooked)
  {
    hook = run.f.target->add_intercept(sym("tb", "hooked_site"), [](core::intercept_call const&) {
      return core::intercept_result::proceed();
    });
  }
  auto const stopped = run.f.run_to_stop();
  INFO(std::format("stop at {:#010x}: {}", stopped.where.pc, stopped.detail));
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(run.f.target->regs().read(*run.f.target->regs().find("pc")) == sym("tb", "tb_done") + 2);
  run.set = obs::collect(*run.f.target, sources_of("tb"));
}

auto thumb_bytes(std::initializer_list<std::uint16_t> halfwords) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  for(auto const halfword : halfwords)
  {
    bytes.push_back(static_cast<std::byte>(halfword & 0xff));
    bytes.push_back(static_cast<std::byte>(halfword >> 8));
  }
  return bytes;
}

auto synthetic(std::vector<std::byte> bytes) -> popcorn::image::image
{
  popcorn::image::image code;
  code.segments.push_back(
      {.name        = "code",
       .start       = 0x1000,
       .bytes       = std::move(bytes),
       .memory_size = 0x40,
       .executable  = true});
  return code;
}

auto counters_at(address start, std::uint32_t hits, address base = 0x1000) -> core::block_counters
{
  core::block_counters counters{
      .range = {.name = "code", .start = base, .size = 0x40}
  };
  counters.hits.resize(0x20);
  counters.hits[(start - base) >> 1] = hits;
  return counters;
}
}

TEST_CASE("the oracle agrees with collect on every block of tb")
{
  tb_run run;
  run_tb(run, false);
  require_none(oracle_mismatches(run.f, run.set, run.seen, run.ranges));
  auto const skip = std::array{ldm_range()};
  require_none(pure_mismatches(run.f, run.seen, skip));
  auto code = code_of(run.f);
  for(auto const& block : run.set.groups.at(0).blocks)
  {
    if(auto const full = obs::decode_tb(block.start, code).size; block.size != full)
    {
      MESSAGE(
          std::format(
              "split start {:#010x}, ldm_run+{:#x}: {} bytes of {}", block.start,
              block.start - sym("tb", "ldm_run"), block.size, full));
      CHECK(inside(skip, block.start));
    }
  }
  MESSAGE(
      std::format(
          "tb without hook: {} block starts, {} split, {} with several sizes",
          run.seen.by_pc.size(), run.set.groups.at(0).split, multi_size_starts(run.set)));
}

TEST_CASE("page rule: a block ends at its start's page end")
{
  tb_run run;
  run_tb(run, false);
  CHECK(
      blocks_at(run.set, sym("tb", "page_cross"))
      == std::map<std::uint32_t, std::uint64_t>{
          {8, 1}
  });
  CHECK(
      blocks_at(run.set, sym("tb", "page_cross_next"))
      == std::map<std::uint32_t, std::uint64_t>{
          {6, 1}
  });
  CHECK(
      blocks_at(run.set, sym("tb", "wide_at_end"))
      == std::map<std::uint32_t, std::uint64_t>{
          {4, 1}
  });
  CHECK(
      blocks_at(run.set, sym("tb", "wide_insn"))
      == std::map<std::uint32_t, std::uint64_t>{
          {4, 1}
  });
  CHECK(
      blocks_at(run.set, sym("tb", "wide_insn") + 4)
      == std::map<std::uint32_t, std::uint64_t>{
          {2, 1}
  });
}

TEST_CASE("a backward branch into its own block is a loop, not a split")
{
  tb_run run;
  run_tb(run, false);
  CHECK(
      blocks_at(run.set, sym("tb", "loop_back"))
      == std::map<std::uint32_t, std::uint64_t>{
          {10, 1}
  });
  CHECK(
      blocks_at(run.set, sym("tb", "loop_top"))
      == std::map<std::uint32_t, std::uint64_t>{
          {8, 2}
  });
}

TEST_CASE("code run from RAM decodes through memory")
{
  tb_run run;
  run_tb(run, false);
  auto const ram = sym("tb", "ram_code") - 0x2000'0000 + 0x0a00'0000;
  CHECK(
      blocks_at(run.set, ram)
      == std::map<std::uint32_t, std::uint64_t>{
          {4, 1}
  });
}

TEST_CASE("the oracle agrees with collect under an intercept code hook")
{
  tb_run run;
  run_tb(run, true);
  require_none(oracle_mismatches(run.f, run.set, run.seen, run.ranges));
  MESSAGE(
      std::format(
          "tb with hook: {} block starts, {} split, {} with several sizes", run.seen.by_pc.size(),
          run.set.groups.at(0).split, multi_size_starts(run.set)));
}

TEST_CASE("P2 firmware: collect and pure decode agree with the engine")
{
  struct workload
  {
    std::string_view    name;
    core::slice_request request;
    std::uint32_t       r0;
    std::string_view    entry;
  };
  std::array const workloads{
      workload{"branches", {},                                          10,   "branches" },
      workload{"alu",      {.budget_left = popcorn::icount{50'000}},    1000, "alu_entry"},
      workload{"fnv",      {.budget_left = popcorn::icount{2'000'000}}, 0,    "fnv_entry"},
  };
  for(auto const& load : workloads)
  {
    CAPTURE(load.name);
    fixture       f{m33(), load.name, {}};
    block_records seen;
    auto const    ranges = executable_ranges(f);
    f.target->enable_coverage(ranges);
    auto const sink = f.target->add_trace_sink({core::trace_kind::block}, seen.sink());
    f.set("pc", sym(load.name, load.entry) | 1);
    f.set("r0", load.r0);
    auto request        = load.request;
    request.wall_budget = std::chrono::seconds{10};
    static_cast<void>(f.run(request));
    auto const set = obs::collect(*f.target, sources_of(load.name));
    REQUIRE_FALSE(seen.by_pc.empty());
    require_none(oracle_mismatches(f, set, seen, ranges));
    require_none(pure_mismatches(f, seen, {}));
    CHECK(set.groups.at(0).split == 0);
  }
}

TEST_CASE("overlapping ranges keep the larger count of a start")
{
  fixture f{m33(), "branches", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const function = address_range{
      .name  = "branches",
      .start = sym("branches", "branches"),
      .size  = sym("branches", "branch_end") + 2 - sym("branches", "branches")};
  auto const run = [&] {
    f.set("pc", sym("branches", "branches") | 1);
    f.set("r0", 10);
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
  };
  f.target->enable_coverage({function});
  run();
  auto ranges = executable_ranges(f);
  ranges.push_back(function);
  f.target->enable_coverage(ranges);
  run();
  auto const set  = obs::collect(*f.target, sources_of("branches"));
  auto const join = blocks_at(set, sym("branches", "branch_join"));
  REQUIRE(join.size() == 1);
  CHECK(join.begin()->second == 20);
}

TEST_CASE("image references carry kind, path, digest and the executable extent")
{
  fixture    f{m33(), "branches", {}};
  auto const set    = obs::collect(*f.target, sources_of("branches"));
  auto const loaded = f.target->loaded_images()[0];
  REQUIRE(set.groups.size() == 1);
  REQUIRE(set.groups[0].images.size() == 1);
  auto const& ref = set.groups[0].images[0];
  CHECK(ref.kind == obs::image_kind::elf);
  CHECK(ref.path == firmware("branches").string());
  CHECK(ref.digest == loaded.digest);
  auto const executable =
      std::ranges::find_if(loaded.segments, &popcorn::image::segment::executable);
  REQUIRE(executable != loaded.segments.end());
  CHECK(ref.base == executable->start);
  CHECK(ref.end == executable->start + executable->memory_size);
}

TEST_CASE("hit split, loop: a backward branch to the block's own middle keeps one size")
{
  std::array const images{synthetic(thumb_bytes({0x3001, 0x3001, 0x3001, 0x3001, 0x3001, 0xd1fb}))};
  obs::code_bytes  code{images, {}};
  core::coverage_data const data{
      .blocks = {counters_at(0x1000, 10)},
      .edges  = {{.from = 0x1000, .to = 0x1004, .hits = 9}},
  };
  auto const sized = obs::size_blocks(data, code);
  CHECK(
      sized.blocks
      == std::vector<obs::block_hit>{
          {.start = 0x1000, .size = 12, .hits = 10}
  });
  CHECK(sized.split == 0);
}

TEST_CASE("hit split, split: an edge into the block's middle splits its hits")
{
  std::array const images{synthetic(thumb_bytes({0x3001, 0x3001, 0x3001, 0x3001, 0x3001, 0xe019}))};
  obs::code_bytes  code{images, {}};
  core::coverage_data const data{
      .blocks = {counters_at(0x1000,                        10)                                      },
      .edges =
          {{.from = 0x1000, .to = 0x1008, .hits = 4}, {.from = 0x1000, .to = 0x1040, .hits = 6}},
  };
  auto const sized = obs::size_blocks(data, code);
  CHECK(
      sized.blocks
      == std::vector<obs::block_hit>{
          {.start = 0x1000, .size = 8,  .hits = 4},
          {.start = 0x1000, .size = 12, .hits = 6},
  });
  CHECK(sized.split == 1);
}

TEST_CASE("overlapping counters of one start keep the largest, not the sum")
{
  std::array const          images{synthetic(thumb_bytes({0x3001, 0x4770}))};
  obs::code_bytes           code{images, {}};
  core::coverage_data const data{
      .blocks = {counters_at(0x1000, 10), counters_at(0x1000, 4)}
  };
  CHECK(
      obs::size_blocks(data, code).blocks
      == std::vector<obs::block_hit>{
          {.start = 0x1000, .size = 4, .hits = 10}
  });
}

TEST_CASE("decode_tb stops before a 32-bit instruction in a page's last halfword")
{
  auto bytes                         = std::vector<std::byte>(0x400 + 8, std::byte{0});
  auto code_image                    = synthetic({});
  code_image.segments[0].start       = 0x0;
  code_image.segments[0].memory_size = 0x800;
  auto const put                     = [&](std::size_t at, std::uint16_t halfword) {
    bytes[at]     = static_cast<std::byte>(halfword & 0xff);
    bytes[at + 1] = static_cast<std::byte>(halfword >> 8);
  };
  put(0x3fa, 0x3001);
  put(0x3fc, 0x3001);
  put(0x3fe, 0xf100);
  put(0x400, 0x0001);
  put(0x402, 0x4770);
  code_image.segments[0].bytes = bytes;
  std::array const images{code_image};
  obs::code_bytes  code{images, {}};
  CHECK(obs::decode_tb(0x3fa, code).size == 4);
  CHECK(obs::decode_tb(0x3fe, code).size == 4);
  CHECK(obs::decode_tb(0x3fe, code).insns == 1);
}

TEST_CASE("hits of a start that cannot be read are counted as dropped")
{
  std::array const images{synthetic(thumb_bytes({0x3001, 0x4770}))};
  obs::code_bytes  code{images, [](address, std::span<std::byte>) -> std::size_t { return 0; }};
  core::coverage_data const data{.blocks = {counters_at(0x2000, 7, 0x2000)}};
  auto const                sized = obs::size_blocks(data, code);
  CHECK(sized.blocks.empty());
  CHECK(sized.dropped == 7);
  CHECK(sized.split == 0);
}

TEST_CASE("a saturated counter gives way to the larger sum of early edges")
{
  std::array const images{synthetic(thumb_bytes({0x3001, 0x3001, 0x3001, 0x3001, 0x3001, 0xe019}))};
  obs::code_bytes  code{images, {}};
  auto const       many = std::uint64_t{0xffff'ffff} + 5;
  core::coverage_data const data{
      .blocks = {counters_at(0x1000, 0xffff'ffff)},
      .edges  = {{.from = 0x1000, .to = 0x1008, .hits = many}},
  };
  auto const sized = obs::size_blocks(data, code);
  CHECK(
      sized.blocks
      == std::vector<obs::block_hit>{
          {.start = 0x1000, .size = 8, .hits = many}
  });
  CHECK(sized.split == 1);
}

TEST_CASE(
    "a page-end read stays in its page unless a 32-bit instruction starts in the last halfword")
{
  std::vector<std::pair<address, std::size_t>> asked;
  auto const reader = [&](address at, std::span<std::byte> out) -> std::size_t {
    asked.emplace_back(at, out.size());
    for(std::size_t i = 0; i < out.size(); i += 2)
    {
      auto const halfword = at + i == 0x13fe ? 0xf100 : at + i == 0x1400 ? 0x0001 : 0x3001;
      out[i]              = static_cast<std::byte>(halfword & 0xff);
      if(i + 1 < out.size())
      {
        out[i + 1] = static_cast<std::byte>(halfword >> 8);
      }
    }
    return out.size();
  };
  obs::code_bytes code{{}, reader};
  CHECK(obs::decode_tb(0x13f0, code).size == 14);
  REQUIRE(asked.size() == 1);
  CHECK(asked[0] == std::pair<address, std::size_t>{0x13f0, 0x10});
  asked.clear();
  auto const wide = obs::decode_tb(0x13fe, code);
  CHECK(wide.size == 4);
  CHECK(wide.insns == 1);
  CHECK(
      asked
      == std::vector<std::pair<address, std::size_t>>{
          {0x13fe, 2},
          {0x13fe, 4}
  });
}

TEST_CASE("reads at the top of the address space stop at its end")
{
  std::vector<std::pair<address, std::size_t>> asked;
  auto const reader = [&](address at, std::span<std::byte> out) -> std::size_t {
    asked.emplace_back(at, out.size());
    std::ranges::fill(out, std::byte{0xf1});
    return out.size();
  };
  obs::code_bytes code{{}, reader};
  CHECK(obs::decode_tb(0xffff'fffe, code).size == 0);
  CHECK(
      asked
      == std::vector<std::pair<address, std::size_t>>{
          {0xffff'fffe, 2},
          {0xffff'fffe, 2}
  });
}

TEST_CASE("collect checks each source's path against its kind")
{
  fixture          f{m33(), "branches", {}};
  std::array const empty_elf{
      obs::image_source{.kind = obs::image_kind::elf, .path = ""}
  };
  CHECK_THROWS_AS(static_cast<void>(obs::collect(*f.target, empty_elf)), popcorn::config_error);
  std::array const raw_with_path{
      obs::image_source{.kind = obs::image_kind::raw, .path = "x.bin"}
  };
  CHECK_THROWS_AS(static_cast<void>(obs::collect(*f.target, raw_with_path)), popcorn::config_error);
  std::array const raw{
      obs::image_source{.kind = obs::image_kind::raw, .path = ""}
  };
  auto const set = obs::collect(*f.target, raw);
  REQUIRE(set.groups.at(0).images.size() == 1);
  CHECK(set.groups[0].images[0].kind == obs::image_kind::raw);
  CHECK(set.groups[0].images[0].path.empty());
  CHECK(set.groups[0].images[0].digest == f.target->loaded_images()[0].digest);
}

TEST_CASE("collect stores an ELF source's path made absolute")
{
  fixture          f{m33(), "branches", {}};
  auto const       absolute = firmware("branches");
  auto const       relative = std::filesystem::relative(absolute);
  std::array const sources{
      obs::image_source{.kind = obs::image_kind::elf, .path = relative.string()}
  };
  auto const set = obs::collect(*f.target, sources);
  CHECK(
      set.groups.at(0).images.at(0).path
      == std::filesystem::absolute(relative).lexically_normal().string());
}
