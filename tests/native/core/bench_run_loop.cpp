#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/elf_image.hpp"
#include "../support/test_arch.hpp"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
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
using clock_type = std::chrono::steady_clock;

constexpr std::uint32_t alu_loops   = 20'000'000;
constexpr std::uint32_t store_loops = 2'000'000;
constexpr address       store_base  = 0x2000'1000;
constexpr int           exit_swaps  = 10'000;
constexpr int           steps       = 100;

struct measured
{
  double        ms{};
  std::uint64_t executed{};
};

struct scenario
{
  std::string               name;
  std::uint64_t             expected{};
  std::function<measured()> run;
  std::vector<double>       samples{};
};

void check(uc_err err, std::string_view what)
{
  if(err != UC_ERR_OK)
  {
    throw std::runtime_error{std::format("{}: {}", what, uc_strerror(err))};
  }
}

auto elapsed_ms(clock_type::time_point start) -> double
{
  return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
}

auto alu_image() -> popcorn::image::image const&
{
  static auto const loaded = popcorn::testing::read_elf(POPCORN_FW_ALU);
  return loaded;
}

auto symbol(std::string_view name) -> address
{
  return popcorn::testing::find_symbol(alu_image(), name);
}

auto alu_count(std::uint64_t loops) -> std::uint64_t
{
  return 3 + 5 * loops + 1;
}

auto store_count(std::uint64_t loops) -> std::uint64_t
{
  return 1 + 5 * loops + 1;
}

struct engine_closer
{
  void operator()(uc_engine* engine) const noexcept
  {
    uc_close(engine);
  }
};
using engine_handle = std::unique_ptr<uc_engine, engine_closer>;

auto raw_engine(bool host_buffers = false) -> engine_handle
{
  uc_engine* raw = nullptr;
  check(uc_open(UC_ARCH_ARM, static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS), &raw), "open");
  engine_handle engine{raw};
  check(uc_ctl_set_cpu_model(raw, UC_CPU_ARM_CORTEX_M33), "cpu model");
  if(host_buffers)
  {
    static std::vector<std::byte> flash(2048 * 1024);
    static std::vector<std::byte> sram(768 * 1024);
    check(
        uc_mem_map_ptr(raw, 0x0800'0000, flash.size(), UC_PROT_READ | UC_PROT_EXEC, flash.data()),
        "map flash");
    check(uc_mem_map_ptr(raw, 0x2000'0000, sram.size(), UC_PROT_ALL, sram.data()), "map sram");
  }
  else
  {
    check(uc_mem_map(raw, 0x0800'0000, 2048 * 1024, UC_PROT_READ | UC_PROT_EXEC), "map flash");
    check(uc_mem_map(raw, 0x2000'0000, 768 * 1024, UC_PROT_ALL), "map sram");
  }
  for(auto const& seg : alu_image().segments)
  {
    check(uc_mem_write(raw, seg.start, seg.bytes.data(), seg.bytes.size()), "load");
  }
  return engine;
}

struct raw_budget
{
  std::uint64_t executed{};
  std::uint64_t deadline{};
};

void empty_block(uc_engine*, std::uint64_t, std::uint32_t, void*) noexcept
{}

void budget_block(uc_engine* engine, std::uint64_t, std::uint32_t size, void* user) noexcept
{
  auto& budget = *static_cast<raw_budget*>(user);
  if(budget.executed >= budget.deadline)
  {
    uc_emu_stop(engine);
    return;
  }
  budget.executed += size;
}

enum class raw_hook
{
  none,
  empty,
  budget,
};

auto raw_alu(raw_hook hook, bool exits, bool host_buffers = false) -> measured
{
  auto const engine = raw_engine(host_buffers);
  raw_budget budget{.executed = 0, .deadline = std::uint64_t{1} << 62};
  uc_hook    handle = 0;
  switch(hook)
  {
    case raw_hook::none:
      break;
    case raw_hook::empty:
      check(
          uc_hook_add(
              engine.get(), &handle, UC_HOOK_BLOCK_ICOUNT, reinterpret_cast<void*>(&empty_block),
              nullptr, 1, 0),
          "empty hook");
      break;
    case raw_hook::budget:
      check(
          uc_hook_add(
              engine.get(), &handle, UC_HOOK_BLOCK_ICOUNT, reinterpret_cast<void*>(&budget_block),
              &budget, 1, 0),
          "budget hook");
      break;
  }
  std::uint64_t until = symbol("alu_done");
  if(exits)
  {
    check(uc_ctl_exits_enable(engine.get()), "exits");
    check(uc_ctl_set_exits(engine.get(), &until, 1), "set exits");
  }
  std::uint32_t loops = alu_loops;
  check(uc_reg_write(engine.get(), UC_ARM_REG_R0, &loops), "r0");
  auto const start = clock_type::now();
  check(uc_emu_start(engine.get(), symbol("alu_entry") | 1, until, 0, 0), "raw run");
  return {elapsed_ms(start), alu_count(alu_loops)};
}

auto make_machine(char const* description) -> std::unique_ptr<core::machine>
{
  auto target = std::make_unique<core::machine>(
      popcorn::load_description(description), popcorn::testing::test_arch_factory(),
      core::machine_options{});
  target->load(alu_image());
  target->reset();
  return target;
}

void set(core::machine& target, std::string_view name, std::uint64_t value)
{
  target.regs().write(*target.regs().find(name), value);
}

auto run_budget(core::machine& target, core::slice_request request, std::uint64_t budget)
    -> measured
{
  request.budget_left = icount{budget};
  std::uint64_t total = 0;
  auto const    start = clock_type::now();
  for(;;)
  {
    auto const slice = target.run_slice(request);
    total += slice.executed.value;
    if(slice.stopped)
    {
      break;
    }
    request.budget_left = icount{request.budget_left->value - slice.executed.value};
  }
  return {elapsed_ms(start), total};
}

auto machine_alu(core::slice_request request) -> measured
{
  auto const target = make_machine(POPCORN_M33_DESC);
  set(*target, "r0", alu_loops);
  return run_budget(*target, std::move(request), alu_count(alu_loops));
}

auto machine_alu_block_sink() -> measured
{
  auto const    target = make_machine(POPCORN_M33_DESC);
  std::uint64_t blocks = 0;
  auto const    sink   = target->add_trace_sink(
      {core::trace_kind::block},
      [&blocks](std::span<core::trace_record const> records) { blocks += records.size(); });
  set(*target, "r0", alu_loops);
  auto const result = run_budget(
      *target, {.max_instructions = icount{1'000'000}, .wall_budget = std::chrono::seconds{1}},
      alu_count(alu_loops));
  if(blocks == 0)
  {
    throw std::runtime_error{"the block sink received nothing"};
  }
  return result;
}

auto machine_alu_coverage() -> measured
{
  auto const target = make_machine(POPCORN_M33_DESC);
  target->enable_coverage({});
  set(*target, "r0", alu_loops);
  auto const result = run_budget(
      *target, {.max_instructions = icount{1'000'000}, .wall_budget = std::chrono::seconds{1}},
      alu_count(alu_loops));
  if(target->coverage().edges.empty())
  {
    throw std::runtime_error{"coverage counted no edge"};
  }
  return result;
}

auto machine_store(char const* description) -> measured
{
  auto const target = make_machine(description);
  set(*target, "pc", symbol("store_entry"));
  set(*target, "r0", store_loops);
  set(*target, "r2", store_base);
  return run_budget(
      *target, {.max_instructions = icount{1'000'000}, .wall_budget = std::chrono::seconds{10}},
      store_count(store_loops));
}

auto set_exits() -> measured
{
  auto const engine = raw_engine();
  check(uc_ctl_exits_enable(engine.get()), "exits");
  std::array<std::uint64_t, 1> first{symbol("alu_done")};
  std::array<std::uint64_t, 1> second{symbol("store_done")};
  auto const                   start = clock_type::now();
  for(int i = 0; i < exit_swaps; ++i)
  {
    auto& list = i % 2 == 0 ? first : second;
    check(uc_ctl_set_exits(engine.get(), list.data(), list.size()), "set exits");
  }
  return {elapsed_ms(start) * 1000.0 / exit_swaps, 0};
}

auto step_one() -> measured
{
  auto const target = make_machine(POPCORN_M33_DESC);
  set(*target, "r0", alu_loops);
  std::uint64_t total = 0;
  auto const    start = clock_type::now();
  for(int i = 0; i < steps; ++i)
  {
    total += target->run_slice({.exact_instructions = icount{1}}).executed.value;
  }
  return {elapsed_ms(start) / steps, total};
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
  return reps;
}
}

auto main(int argc, char** argv) -> int
{
  auto const            args      = std::span{argv, static_cast<std::size_t>(argc)};
  auto const            reps      = reps_from(args);
  auto const            long_wall = std::chrono::seconds{1};
  std::vector<scenario> scenarios{
      {"raw",                   alu_count(alu_loops),     [] { return raw_alu(raw_hook::none, false); }  },
      {"raw-host-buffers",      alu_count(alu_loops),
       [] { return raw_alu(raw_hook::none, false, true); }                                               },
      {"raw-exits",             alu_count(alu_loops),     [] { return raw_alu(raw_hook::none, true); }   },
      {"raw-empty-hook",        alu_count(alu_loops),     [] { return raw_alu(raw_hook::empty, false); } },
      {"raw-budget-hook",       alu_count(alu_loops),     [] { return raw_alu(raw_hook::budget, false); }},
      {"raw-budget-hook-exits", alu_count(alu_loops),
       [] { return raw_alu(raw_hook::budget, true); }                                                    },
      {"machine-1e4",           alu_count(alu_loops),
       [&] { return machine_alu({.max_instructions = icount{10'000}, .wall_budget = long_wall}); }       },
      {"machine-1e6",           alu_count(alu_loops),
       [&] {
         return machine_alu({.max_instructions = icount{1'000'000}, .wall_budget = long_wall});
       }                                                                                                 },
      {"machine-wall",          alu_count(alu_loops),     [] { return machine_alu({}); }                 },
      {"machine-block-sink",    alu_count(alu_loops),     machine_alu_block_sink                         },
      {"machine-coverage",      alu_count(alu_loops),     machine_alu_coverage                           },
      {"machine-1e4-until",     alu_count(alu_loops),
       [&] {
         return machine_alu(
             {.max_instructions = icount{10'000},
              .wall_budget      = long_wall,
              .until            = symbol("store_done")});
       }                                                                                                 },
      {"set-exits",             0,                        set_exits                                      },
      {"step-1",                steps,                    step_one                                       },
      {"store-plain",           store_count(store_loops), [] { return machine_store(POPCORN_M33_DESC); } },
      {"store-rw-alias",        store_count(store_loops),
       [] { return machine_store(POPCORN_M33_CODE_ALIAS_DESC); }                                         },
  };
  if(auto const only = option(args, "--only"))
  {
    std::erase_if(scenarios, [&](scenario const& entry) {
      return entry.name != "raw" and entry.name != *only;
    });
  }
  for(int rep = 0; rep < reps; ++rep)
  {
    for(auto& entry : scenarios)
    {
      auto const result = entry.run();
      if(result.executed != entry.expected)
      {
        std::println(
            stderr, "{}: executed {} instead of {}", entry.name, result.executed, entry.expected);
        return 1;
      }
      entry.samples.push_back(result.ms);
    }
  }
  auto const raw_median = percentile(scenarios.front().samples, 0.5);
  for(auto const& entry : scenarios)
  {
    auto const median = percentile(entry.samples, 0.5);
    std::println(
        R"({{"scenario": "{}", "reps": {}, "median": {:.4f}, "p95": {:.4f}, "unit": "{}", "ratio_raw": {:.3f}, "executed": {}}})",
        entry.name, reps, median, percentile(entry.samples, 0.95),
        entry.name == "set-exits" ? "us" : "ms", median / raw_median, entry.expected);
  }
  return 0;
}
