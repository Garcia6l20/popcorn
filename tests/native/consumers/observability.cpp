#include <popcorn/core/coverage.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/image/image.hpp>

#include "common.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::consumers::observability
{
auto full_trace(core::machine& target, std::vector<std::uint32_t>& file) -> core::hook_handle
{
  return target.add_trace_sink(
      core::trace_kinds::all(),
      [&file](std::span<core::trace_record const> records) {
        for(auto const& record : records)
        {
          file.push_back(
              std::uint32_t{std::to_underlying(record.kind)}
              | std::uint32_t{record.flags}
              << 8
              | std::uint32_t{record.aux}
              << 16);
          file.push_back(record.dt);
          for(std::size_t word = 0; word < core::pctr_payload_words(record.kind); ++word)
          {
            file.push_back(record.payload[word]);
          }
        }
  },
      {{.name = "drc_process", .start = 0x0800'2000, .size = 0x200}});
}

auto collect_coverage(core::machine& target) -> std::uint64_t
{
  target.enable_coverage({
      {.name = "text", .start = 0x0800'0000, .size = 0x1000}
  });
  target.enable_coverage({});
  std::uint64_t total = 0;
  for(auto const& counters : target.coverage().blocks)
  {
    for(std::uint32_t const hits : counters.hits)
    {
      total += hits;
    }
  }
  for(auto const& edge : target.coverage().edges)
  {
    total += edge.from < edge.to ? edge.hits : 0;
  }
  if(target.coverage_enabled())
  {
    target.disable_coverage();
  }
  target.clear_coverage();
  return total;
}

auto symbol_of(core::machine const& target, std::uint16_t id) -> image::symbol const*
{
  std::size_t remaining = id;
  for(auto const& loaded : target.loaded_images())
  {
    if(remaining < loaded.symbols.size())
    {
      return &loaded.symbols[remaining];
    }
    remaining -= loaded.symbols.size();
  }
  return nullptr;
}

auto ring_dump(core::machine& target) -> std::vector<std::string>
{
  std::vector<std::string> lines;
  auto                     at = target.trace().origin();
  for(auto const& record : target.trace().records())
  {
    at = record.kind == core::trace_kind::time
           ? icount{record.payload[0] | std::uint64_t{record.payload[1]} << 32}
           : icount{at.value + record.dt};
    if(record.kind == core::trace_kind::call and record.aux != core::trace_id_overflow)
    {
      if(auto const* const called = symbol_of(target, record.aux))
      {
        lines.push_back(called->name);
      }
    }
    if(record.kind == core::trace_kind::mmio)
    {
      if(auto const reg = register_at(target.description(), record.aux))
      {
        lines.push_back(target.description().peripherals[reg->peripheral].registers[reg->reg].name);
      }
    }
    if(record.kind == core::trace_kind::stop)
    {
      lines.push_back(std::format("{} at {}", static_cast<stop_reason>(record.aux), at.value));
    }
  }
  return lines;
}
}

TEST_CASE("observability consumer reads the ring, streams a full trace and collects coverage")
{
  popcorn::core::machine target{
      popcorn::testing::toy_description(), popcorn::testing::test_arch_factory(), {}};
  std::vector<std::uint32_t> file;
  CHECK_THROWS_AS(
      popcorn::consumers::observability::full_trace(target, file), popcorn::not_implemented);
  CHECK(popcorn::consumers::observability::ring_dump(target).empty());
  CHECK_FALSE(target.coverage_enabled());
  CHECK_THROWS_AS(
      popcorn::consumers::observability::collect_coverage(target), popcorn::not_implemented);

  target.trace().append(
      {.kind = popcorn::core::trace_kind::mmio, .flags = 4 | popcorn::core::trace_write_flag},
      popcorn::icount{7});
  CHECK(popcorn::core::pctr_payload_words(target.trace().records().front().kind) == 3);
  CHECK(popcorn::consumers::observability::ring_dump(target) == std::vector<std::string>{"ISR"});
}
