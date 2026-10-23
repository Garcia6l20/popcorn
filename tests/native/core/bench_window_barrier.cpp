#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/window_barrier.hpp>
#include <popcorn/image/image.hpp>

#include "../support/test_arch.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using clock_type = std::chrono::steady_clock;

constexpr int members = 3;

struct work_size
{
  double        us;
  std::uint64_t instructions;
  int           rounds;
};

constexpr work_size sizes[] = {
    {1.3,  1240, 400'000},
    {2.0,  2300, 300'000},
    {2.8,  3500, 200'000},
    {7.75, 9300, 100'000},
};

auto alu_image() -> popcorn::image::image const&
{
  static auto const loaded = popcorn::image::load_elf(POPCORN_FW_ALU);
  return loaded;
}

auto make_machine() -> std::unique_ptr<core::machine>
{
  auto target = std::make_unique<core::machine>(
      popcorn::load_description(POPCORN_M33_DESC), popcorn::testing::test_arch_factory(),
      core::machine_options{});
  target->load(alu_image());
  target->reset();
  target->regs().write(*target->regs().find("r0"), 0xFFFF'FFFFu);
  return target;
}

struct member
{
  std::unique_ptr<core::machine> engine = make_machine();
  std::uint64_t                  executed{};

  void slice(std::uint64_t instructions)
  {
    auto const result = engine->run_slice(
        {.max_instructions = icount{instructions}, .wall_budget = std::chrono::seconds{1}});
    if(result.stopped)
    {
      throw std::runtime_error{"member stopped"};
    }
    executed += result.executed.value;
  }
};

struct outcome
{
  double                     wall{};
  std::vector<std::uint64_t> executed;
  std::uint64_t              sleeps{};
};

auto seconds_since(clock_type::time_point start) -> double
{
  return std::chrono::duration<double>(clock_type::now() - start).count();
}

auto run_serial(work_size size, int warmup) -> outcome
{
  std::array<member, members> group;
  auto const                  round = [&] {
    for(auto& m : group)
    {
      m.slice(size.instructions);
    }
  };
  for(int r = 0; r < warmup; ++r)
  {
    round();
  }
  auto const start = clock_type::now();
  for(int r = 0; r < size.rounds; ++r)
  {
    round();
  }
  outcome out{.wall = seconds_since(start), .executed = {}, .sleeps = 0};
  for(auto const& m : group)
  {
    out.executed.push_back(m.executed);
  }
  return out;
}

auto run_barrier(work_size size, int warmup) -> outcome
{
  core::window_barrier        gate{members};
  std::array<member, members> group;
  int                         completed = 0;
  clock_type::time_point      start;
  auto const                  complete = [&]() noexcept {
    if(++completed == warmup)
    {
      start = clock_type::now();
    }
  };
  {
    std::vector<std::jthread> pool;
    for(int index = 0; index < members; ++index)
    {
      pool.emplace_back([&, index] {
        for(int r = 0; r < warmup + size.rounds; ++r)
        {
          group[index].slice(size.instructions);
          std::ignore = gate.arrive_and_wait(complete);
        }
      });
    }
  }
  outcome out{.wall = seconds_since(start), .executed = {}, .sleeps = gate.sleeps()};
  for(auto const& m : group)
  {
    out.executed.push_back(m.executed);
  }
  return out;
}

auto median(std::vector<double> values) -> double
{
  std::ranges::sort(values);
  auto const n = values.size();
  return n % 2 != 0 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

auto first_line(char const* path) -> std::string
{
  std::ifstream file{path};
  std::string   line;
  std::getline(file, line);
  return line;
}

void env_line()
{
  auto const now  = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  auto       load = first_line("/proc/loadavg");
  load            = load.substr(0, load.find(' ', load.find(' ', load.find(' ') + 1) + 1));
  std::println(
      "# {:%FT%T} load={} governor={} epp={} cpus={}", now, load,
      first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
      first_line("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"),
      core::usable_cpus());
}

auto parse_int(std::string_view text) -> int
{
  int value{};
  auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if(error != std::errc{} or end != text.data() + text.size() or value <= 0)
  {
    throw std::invalid_argument{"bad count " + std::string{text}};
  }
  return value;
}

auto bench(int runs, int divisor) -> int
{
  struct variant
  {
    std::string name;
    bool        threaded;
  };
  std::vector<variant> const variants{
      {"serial",       false},
      {"futex-inline", true },
  };
  env_line();
  for(auto size : sizes)
  {
    size.rounds /= divisor;
    auto const                                        warmup = std::max(200, size.rounds / 50);
    std::map<std::string, std::vector<double>>        per_round;
    std::map<std::string, std::vector<std::uint64_t>> sleeps;
    std::vector<std::uint64_t>                        reference;
    for(int run = 0; run < runs; ++run)
    {
      for(auto const& v : variants)
      {
        auto const out = v.threaded ? run_barrier(size, warmup) : run_serial(size, warmup);
        if(reference.empty())
        {
          reference = out.executed;
        }
        else if(out.executed != reference)
        {
          std::println("MISMATCH {} executed {} vs {}", v.name, out.executed[0], reference[0]);
          return 1;
        }
        per_round[v.name].push_back(out.wall * 1e6 / size.rounds);
        sleeps[v.name].push_back(out.sleeps);
      }
    }
    auto const serial = median(per_round["serial"]);
    std::println(
        "\nwork_us={} instructions={} rounds={} runs={} executed_per_member={}", size.us,
        size.instructions, size.rounds, runs, reference[0]);
    std::println(
        "| variant | median us/round | min | max | spread % | speedup vs serial | sleeps/round |");
    std::println("|---|---|---|---|---|---|---|");
    for(auto const& v : variants)
    {
      auto const& s  = per_round[v.name];
      auto const  md = median(s);
      auto const  lo = *std::ranges::min_element(s);
      auto const  hi = *std::ranges::max_element(s);
      auto const  slept =
          static_cast<double>(*std::ranges::max_element(sleeps[v.name])) / (size.rounds + warmup);
      std::println(
          "| {} | {:.3f} | {:.3f} | {:.3f} | {:.1f} | {:.3f} | {:.3f} |", v.name, md, lo, hi,
          (hi - lo) / md * 100, serial / md, slept);
    }
    std::fflush(stdout);
  }
  env_line();
  return 0;
}
}

auto main(int argc, char** argv) -> int
{
  try
  {
    std::vector<std::string_view> const args(argv + 1, argv + argc);
    auto const                          runs    = args.size() > 0 ? parse_int(args[0]) : 5;
    auto const                          divisor = args.size() > 1 ? parse_int(args[1]) : 1;
    return bench(runs, divisor);
  }
  catch(std::exception const& e)
  {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }
}
