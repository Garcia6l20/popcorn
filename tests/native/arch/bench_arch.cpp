#include "../../../native/src/core/machine_impl.hpp"
#include "cortex_m.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
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
  std::string                        name;
  std::copyable_function<measured()> run;
  std::vector<double>                samples{};
  std::uint64_t                      executed{};
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

bool trustzone = false;

auto description() -> popcorn::part_description
{
  auto desc = popcorn::load_description(POPCORN_M33_DESC);
  if(trustzone)
  {
    desc.core.trustzone          = true;
    desc.core.sau_regions        = 8;
    desc.core.mpu_regions_secure = 8;
  }
  return desc;
}

auto options() -> core::machine_options
{
  core::machine_options out;
  if(trustzone)
  {
    out.trustzone = true;
    out.idau      = {
        {.start = 0x0000'0000, .end = 0x0C00'0000, .nsc = false, .exempt = false},
        {.start = 0x0C00'0000, .end = 0x1000'0000, .nsc = true,  .exempt = false},
        {.start = 0x1000'0000, .end = 0xE000'0000, .nsc = false, .exempt = false},
    };
  }
  return out;
}

auto booted() -> std::unique_ptr<core::machine>
{
  auto target = std::make_unique<core::machine>(
      description(),
      [](popcorn::core_desc const& core) -> std::unique_ptr<popcorn::arch::arch> {
        return std::make_unique<popcorn::arch::armv8m::cortex_m>(core);
      },
      options());
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

auto nonsecure_irq_run(std::uint32_t count) -> measured
{
  constexpr address flash   = 0x0800'0000;
  constexpr address ns_ram  = 0x200B'0000;
  constexpr address sau     = 0xE000'EDD0;
  constexpr address vtor_ns = 0xE002'ED08;
  constexpr address itns0   = 0xE000'E380;
  auto              target  = booted();
  auto const        poke    = [&](address at, std::uint32_t value) {
    if(not target->mem().write(at, 4, value))
    {
      throw std::runtime_error{"a Non-secure IRQ setup write failed"};
    }
  };
  poke(sau + 8, 0);
  poke(sau + 0xc, 0);
  poke(sau + 0x10, 0x0001'FFE0 | 1u);
  poke(sau + 8, 1);
  poke(sau + 0xc, ns_ram);
  poke(sau + 0x10, (ns_ram + 0xFE0) | 1u);
  poke(sau, 1);
  poke(itns0, 1u << 3);
  poke(vtor_ns, ns_ram);
  poke(ns_ram + 4 * (16 + 3), (symbol("bench_empty") - flash) | 1u);
  std::uint32_t stack = ns_ram + 0x1000;
  uc_reg_write(core::machine_internals::engine(*target), UC_ARM_REG_MSP_NS, &stack);
  set(*target, "pc", symbol("bench_plain_irq"));
  set(*target, "r0", count);
  return timed(*target, {.max_instructions = icount{1'000'000'000}}, stop_reason::user);
}

auto probe_run(std::optional<bool> secure) -> measured
{
  auto       target = booted();
  auto const probe  = target->add_probe(symbol("bench_empty"), secure);
  set(*target, "pc", symbol("bench_plain_irq"));
  set(*target, "r0", irq_count);
  auto const result =
      timed(*target, {.max_instructions = icount{1'000'000'000}}, stop_reason::user);
  if(probe.hits->count() < irq_count)
  {
    throw std::runtime_error{"the probe missed interrupt handler arrivals"};
  }
  return result;
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
  auto const args = std::span{argv, static_cast<std::size_t>(argc)};
  auto const reps = reps_from(args);
  trustzone       = std::ranges::contains(args | std::views::drop(1), std::string_view{"--tz"});
  std::vector<scenario> scenarios{
      {"alu",            [] { return entry_run("bench_loop", alu_loops); }      },
      {"irq-rate",       [] { return entry_run("bench_irq_rate", alu_loops); }  },
      {"masked-pending", [] { return entry_run("bench_masked", alu_loops); }    },
      {"plain-irq",      [] { return entry_run("bench_plain_irq", irq_count); } },
      {"tail-chain",     [] { return entry_run("bench_tail_chain", irq_count); }},
      {"probe",          [] { return probe_run(std::nullopt); }                 },
      {"rtos-idle",      [] { return rtos_run("rtos_idle", 10'000); }           },
      {"rtos-busy",      [] { return rtos_run("rtos_busy", 1'000); }            },
  };
  if(trustzone)
  {
    std::erase_if(scenarios, [](scenario const& entry) { return entry.name.starts_with("rtos-"); });
    scenarios.push_back({"ns-irq", [] { return nonsecure_irq_run(irq_count); }});
    scenarios.push_back({"probe-state", [] { return probe_run(true); }});
  }
  if(auto const only = option(args, "--only"))
  {
    auto const base = *only == "plain-irq"
                           or *only == "tail-chain"
                           or *only == "ns-irq"
                           or only->starts_with("probe")
                        ? "plain-irq"
                        : "alu";
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
