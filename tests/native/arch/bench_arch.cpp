#include "cortex_m.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using clock_type = std::chrono::steady_clock;

constexpr std::uint32_t alu_loops  = 10'000'000;
constexpr std::uint32_t irq_count  = 100'000;
constexpr std::uint64_t ps_per_ms  = 1'000'000'000;
constexpr auto          long_slice = std::chrono::seconds{600};

struct measured
{
  double        ms{};
  std::uint64_t executed{};
};

struct scenario
{
  std::string               name;
  std::function<measured()> run;
  std::vector<double>       samples{};
  std::uint64_t             executed{};
};

auto rtos_image() -> popcorn::image::image const&
{
  static auto const loaded = popcorn::image::load_elf(POPCORN_FW_RTOS);
  return loaded;
}

auto symbol(std::string_view name) -> address
{
  return rtos_image().lookup(name).value;
}

auto booted() -> std::unique_ptr<core::machine>
{
  auto target = std::make_unique<core::machine>(
      popcorn::load_description(POPCORN_M33_DESC),
      [](popcorn::core_desc const& core) -> std::unique_ptr<popcorn::arch::arch> {
        return std::make_unique<popcorn::arch::armv8m::cortex_m>(core);
      },
      core::machine_options{});
  target->load(rtos_image());
  target->reset();
  auto const boot = target->run_slice({.wall_budget = std::chrono::seconds{10}});
  if(not boot.stopped or boot.stopped->reason != stop_reason::user)
  {
    throw std::runtime_error{"the RTOS firmware did not boot"};
  }
  return target;
}

void set(core::machine& target, std::string_view name, std::uint64_t value)
{
  target.regs().write(*target.regs().find(name), value);
}

auto timed(core::machine& target, core::slice_request request, stop_reason expected) -> measured
{
  request.wall_budget = long_slice;
  auto const start    = clock_type::now();
  auto const result   = target.run_slice(request);
  auto const ms = std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
  if(not result.stopped or result.stopped->reason != expected)
  {
    throw std::runtime_error{"a benchmark run did not reach its stop"};
  }
  return {ms, result.executed.value};
}

auto entry_run(std::string_view entry, std::uint32_t argument) -> measured
{
  auto target = booted();
  set(*target, "pc", symbol(entry));
  set(*target, "r0", argument);
  return timed(*target, {.max_instructions = icount{1'000'000'000}}, stop_reason::user);
}

auto rtos_run(std::string_view entry, std::uint64_t virtual_ms) -> measured
{
  auto target = booted();
  set(*target, "pc", symbol(entry));
  auto const until = vtime{target->virtual_clock().time().ps + virtual_ms * ps_per_ms};
  return timed(
      *target, {.max_instructions = icount{4'000'000'000}, .until_time = until}, stop_reason::time);
}

auto percentile(std::vector<double> samples, double fraction) -> double
{
  std::ranges::sort(samples);
  auto const index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
  return samples[index];
}

auto option(std::span<char*> args, std::string_view name) -> std::optional<std::string_view>
{
  for(std::size_t i = 1; i + 1 < args.size(); ++i)
  {
    if(std::string_view{args[i]} == name)
    {
      return std::string_view{args[i + 1]};
    }
  }
  return std::nullopt;
}

auto reps_from(std::span<char*> args) -> int
{
  int reps = 21;
  if(auto const value = option(args, "--reps"))
  {
    std::from_chars(value->data(), value->data() + value->size(), reps);
  }
  return std::max(reps, 1);
}
}

auto main(int argc, char** argv) -> int
{
  auto const            args = std::span{argv, static_cast<std::size_t>(argc)};
  auto const            reps = reps_from(args);
  std::vector<scenario> scenarios{
      {"alu",            [] { return entry_run("bench_loop", alu_loops); }      },
      {"irq-rate",       [] { return entry_run("bench_irq_rate", alu_loops); }  },
      {"masked-pending", [] { return entry_run("bench_masked", alu_loops); }    },
      {"plain-irq",      [] { return entry_run("bench_plain_irq", irq_count); } },
      {"tail-chain",     [] { return entry_run("bench_tail_chain", irq_count); }},
      {"rtos-idle",      [] { return rtos_run("rtos_idle", 10'000); }           },
      {"rtos-busy",      [] { return rtos_run("rtos_busy", 1'000); }            },
  };
  if(auto const only = option(args, "--only"))
  {
    auto const base = *only == "plain-irq" or *only == "tail-chain" ? "plain-irq" : "alu";
    std::erase_if(scenarios, [&](scenario const& entry) {
      return entry.name != *only and entry.name != base;
    });
  }
  try
  {
    for(int rep = 0; rep < reps; ++rep)
    {
      for(auto& entry : scenarios)
      {
        auto const result = entry.run();
        entry.samples.push_back(result.ms);
        entry.executed = result.executed;
      }
    }
  }
  catch(std::exception const& failure)
  {
    std::println(stderr, "bench-arch: {}", failure.what());
    return 1;
  }
  for(auto const& entry : scenarios)
  {
    std::println(
        R"({{"scenario": "{}", "reps": {}, "median": {:.4f}, "p95": {:.4f}, "unit": "ms", "executed": {}}})",
        entry.name, reps, percentile(entry.samples, 0.5), percentile(entry.samples, 0.95),
        entry.executed);
  }
  return 0;
}
