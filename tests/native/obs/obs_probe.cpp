#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/collect.hpp>
#include <popcorn/obs/coverage.hpp>

#include "../support/test_arch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace image = popcorn::image;
namespace obs   = popcorn::obs;

auto median_ms(int reps, std::function_ref<void()> work) -> double
{
  std::vector<double> times;
  for(int rep = 0; rep < reps; ++rep)
  {
    auto const start = std::chrono::steady_clock::now();
    work();
    times.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count());
  }
  std::ranges::sort(times);
  return times[times.size() / 2];
}

auto option(std::span<char const* const> args, std::string_view name)
    -> std::optional<std::string_view>
{
  for(std::size_t at = 0; at + 1 < args.size(); ++at)
  {
    if(std::string_view{args[at]} == name)
    {
      return std::string_view{args[at + 1]};
    }
  }
  return std::nullopt;
}

auto reps_of(std::span<char const* const> args) -> int
{
  auto const value = option(args, "--reps");
  return value ? std::max(1, std::atoi(std::string{*value}.c_str())) : 1;
}

auto probe_lcov(std::span<char const* const> args) -> int
{
  auto const load_start = std::chrono::steady_clock::now();
  auto const loaded     = image::load_elf(args[0]);
  auto const load_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_start)
          .count();
  obs::coverage_group group{
      .images = {{.kind = obs::image_kind::elf, .path = args[0], .digest = loaded.digest}}};
  for(auto const& segment : loaded.segments)
  {
    if(segment.executable and segment.memory_size != 0)
    {
      group.ranges.push_back(
          {.name = segment.name, .start = segment.start, .size = segment.memory_size});
    }
  }
  obs::coverage_set const                      set{.groups = {group}};
  std::vector<std::vector<image::image>> const images{{loaded}};
  std::string                                  text;
  auto const lcov_ms = median_ms(reps_of(args), [&] { text = obs::lcov(set, images, {}); });
  std::ofstream{args[1]} << text;
  auto const functions = std::ranges::count_if(loaded.symbols, [](image::symbol const& symbol) {
    return symbol.kind == image::symbol_kind::function;
  });
  std::cout << std::format(
      R"({{"probe":"lcov","rows":{},"files":{},"functions":{},"load_ms":{:.1f},"lcov_ms":{:.1f}}})"
      "\n",
      loaded.lines.size(), loaded.files.size(), functions, load_ms, lcov_ms);
  return 0;
}

auto probe_collect(std::span<char const* const> args) -> int
{
  core::machine target{
      popcorn::load_description(args[0]),
      [](popcorn::core_desc const& core) -> std::unique_ptr<popcorn::arch::arch> {
        return std::make_unique<popcorn::testing::test_arch>(core);
      },
      core::machine_options{}};
  auto const loaded = image::load_elf(args[1]);
  target.load(loaded);
  target.reset();
  if(auto const entry = option(args, "--entry"))
  {
    target.regs().write(*target.regs().find("pc"), loaded.lookup(*entry).value | 1);
  }
  if(auto const r0 = option(args, "--r0"))
  {
    target.regs().write(
        *target.regs().find("r0"), std::strtoull(std::string{*r0}.c_str(), nullptr, 0));
  }
  target.enable_coverage({});
  auto const ran = target.run_slice(
      {.max_instructions = popcorn::icount{10'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .budget_left      = popcorn::icount{10'000'000}});
  auto const reason = ran.stopped ? std::format("{}", ran.stopped->reason) : std::string{"none"};
  std::vector<obs::image_source> const sources{
      {.kind = obs::image_kind::elf, .path = std::filesystem::absolute(args[1]).string()}
  };
  obs::coverage_set set;
  auto const  collect_ms = median_ms(reps_of(args), [&] { set = obs::collect(target, sources); });
  auto const& group      = set.groups.at(0);
  auto        starts =
      group.blocks | std::views::transform(&obs::block_hit::start) | std::ranges::to<std::vector>();
  starts.erase(std::ranges::unique(starts).begin(), starts.end());
  std::cout << std::format(
      R"({{"probe":"collect","stop":"{}","executed":{},"block_starts":{},"block_hits":{},"edges":{},"split":{},"dropped":{},"collect_ms":{:.3f}}})"
      "\n",
      reason, ran.executed.value, starts.size(), group.blocks.size(), group.edges.size(),
      group.split, group.dropped, collect_ms);
  return 0;
}
}

auto main(int argc, char** argv) -> int
{
  popcorn::arch::armv8m::register_builtin();
  std::span<char const* const> const args{argv, static_cast<std::size_t>(argc)};
  try
  {
    if(args.size() >= 4 and std::string_view{args[1]} == "lcov")
    {
      return probe_lcov(args.subspan(2));
    }
    if(args.size() >= 4 and std::string_view{args[1]} == "collect")
    {
      return probe_collect(args.subspan(2));
    }
  }
  catch(std::exception const& failure)
  {
    std::cerr << failure.what() << '\n';
    return 1;
  }
  std::cerr
      << "usage: obs-probe lcov <elf> <out.info> [--reps N]\n"
         "       obs-probe collect <description.json> <firmware.elf> [--entry SYMBOL] [--r0 N] "
         "[--reps N]\n";
  return 2;
}
