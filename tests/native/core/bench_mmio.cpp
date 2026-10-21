#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/image/image.hpp>

#include "../../../native/src/core/engine.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using clock_type = std::chrono::steady_clock;

constexpr std::uint32_t loops       = 1'000'000;
constexpr std::uint32_t slice_loops = 100'000;
constexpr address       testdev     = 0x4001'0000;
constexpr address       rw_register = testdev + 0x08;
constexpr address       data_offset = popcorn::testing::test_model::data_offset;
constexpr address       ctrl_offset = popcorn::testing::test_model::ctrl_offset;
constexpr address       sram_data   = 0x2008'0000;
constexpr std::uint64_t mmio_base   = 0x4001'0000;
constexpr address       flash_load  = 0x0800'8000;

struct measured
{
  double        ns{};
  std::uint64_t count{};
};

struct scenario
{
  std::string                        name;
  std::copyable_function<measured()> run;
  std::vector<double>                samples{};
};

auto image() -> popcorn::image::image const&
{
  static auto const loaded = popcorn::image::load_elf(POPCORN_FW_MMIO);
  return loaded;
}

auto symbol(std::string_view name) -> address
{
  return image().lookup(name).value;
}

auto per_access(clock_type::time_point start, std::uint64_t count) -> measured
{
  auto const ns = std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
  return {ns / static_cast<double>(count), count};
}

void check(uc_err err, std::string_view what)
{
  if(err != UC_ERR_OK)
  {
    throw std::runtime_error{std::format("{}: {}", what, uc_strerror(err))};
  }
}

auto constant_read(uc_engine*, std::uint64_t, unsigned, void*) noexcept -> std::uint64_t
{
  return 1;
}

void ignore_write(uc_engine*, std::uint64_t, unsigned, std::uint64_t, void*) noexcept
{}

auto raw_engine() -> core::engine_handle
{
  auto handle = core::open_engine(
      {.arch      = UC_ARCH_ARM,
       .mode      = UC_MODE_THUMB | UC_MODE_MCLASS,
       .cpu_model = UC_CPU_ARM_CORTEX_M33});
  check(uc_mem_map(handle.get(), 0x0800'0000, 2048 * 1024, UC_PROT_READ | UC_PROT_EXEC), "flash");
  check(uc_mem_map(handle.get(), 0x2000'0000, 768 * 1024, UC_PROT_ALL), "sram");
  check(
      uc_mmio_map(handle.get(), mmio_base, 0x400, &constant_read, nullptr, &ignore_write, nullptr),
      "mmio");
  for(auto const& seg : image().segments)
  {
    check(uc_mem_write(handle.get(), seg.start, seg.bytes.data(), seg.bytes.size()), "load");
  }
  return handle;
}

auto raw_loop(
    std::string_view                                          entry,
    std::string_view                                          done,
    address                                                   target,
    std::function_ref<void(uc_engine*, core::exact_pc_hook&)> prepare = [](uc_engine*,
                                                                           core::exact_pc_hook&) {},
    bool                                                      warm    = false) -> measured
{
  auto const          handle = raw_engine();
  core::exact_pc_hook exact;
  exact.configure(
      handle.get(), {
                        {mmio_base, mmio_base + 0x3ff}
  });
  prepare(handle.get(), exact);
  if(warm)
  {
    std::uint32_t one = 1;
    std::uint32_t r2  = target;
    check(uc_reg_write(handle.get(), UC_ARM_REG_R0, &one), "r0");
    check(uc_reg_write(handle.get(), UC_ARM_REG_R2, &r2), "r2");
    check(uc_emu_start(handle.get(), symbol(entry) | 1, symbol(done), 0, 0), "warm run");
  }
  std::uint32_t count = loops;
  std::uint32_t r2    = target;
  check(uc_reg_write(handle.get(), UC_ARM_REG_R0, &count), "r0");
  check(uc_reg_write(handle.get(), UC_ARM_REG_R2, &r2), "r2");
  auto const start = clock_type::now();
  check(uc_emu_start(handle.get(), symbol(entry) | 1, symbol(done), 0, 0), "raw run");
  return per_access(start, loops);
}

auto options(bool with_model) -> core::machine_options
{
  core::machine_options made;
  if(with_model)
  {
    made.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  }
  return made;
}

struct bench_machine
{
  popcorn::testing::test_arch*   module = nullptr;
  std::unique_ptr<core::machine> target;

  explicit bench_machine(popcorn::part_description desc, bool with_model = true)
  {
    target = std::make_unique<core::machine>(
        std::move(desc),
        [this](popcorn::core_desc const& unit) -> std::unique_ptr<popcorn::arch::arch> {
          auto made = std::make_unique<popcorn::testing::test_arch>(unit);
          module    = made.get();
          return made;
        },
        options(with_model));
    target->load(image());
    target->reset();
    if(auto* const found = dynamic_cast<popcorn::testing::test_model*>(target->model("TESTDEV")))
    {
      found->record = false;
    }
  }

  void set(std::string_view name, std::uint64_t value) const
  {
    target->regs().write(*target->regs().find(name), value);
  }

  auto model() const -> popcorn::testing::test_model&
  {
    return dynamic_cast<popcorn::testing::test_model&>(*target->model("TESTDEV"));
  }

  auto run_until(address done) const -> void
  {
    for(;;)
    {
      auto const slice = target->run_slice(
          {.max_instructions = icount{10'000'000},
           .wall_budget      = std::chrono::seconds{10},
           .until            = done});
      if(slice.stopped)
      {
        if(slice.stopped->reason != popcorn::stop_reason::address)
        {
          throw std::runtime_error{std::format(
              "stopped as {} at {:#x}, fault address {:#x}", slice.stopped->reason,
              slice.stopped->where.pc,
              slice.stopped->fault ? slice.stopped->fault->fault_address : 0)};
        }
        return;
      }
    }
  }
};

auto machine_loop(
    std::string_view                        entry,
    std::string_view                        done,
    address                                 target,
    std::uint32_t                           count,
    std::function_ref<void(bench_machine&)> prepare = [](bench_machine&) {}) -> measured
{
  bench_machine m{popcorn::load_description(POPCORN_M33_DESC)};
  prepare(m);
  m.set("pc", symbol(entry));
  m.set("r0", count);
  m.set("r2", target);
  auto const start = clock_type::now();
  m.run_until(symbol(done));
  return per_access(start, count);
}

auto dma_writes(bool executable) -> measured
{
  auto desc = popcorn::load_description(POPCORN_M33_DESC);
  if(not executable)
  {
    desc.regions[2].acc.execute = false;
  }
  bench_machine m{std::move(desc)};
  auto&         services = m.module->attached_services();
  auto const    start    = clock_type::now();
  for(std::uint32_t i = 0; i < loops; ++i)
  {
    if(not services.write(sram_data, 4, i, popcorn::initiator::dma))
    {
      throw std::runtime_error{"dma write refused"};
    }
  }
  return per_access(start, loops);
}

auto flash_loads(std::vector<address> const& watched, std::size_t hooks, std::uint32_t size = 16)
    -> measured
{
  auto desc                  = popcorn::load_description(POPCORN_M33_DESC);
  desc.regions[0].controller = "TESTDEV";
  bench_machine m{std::move(desc)};
  for(auto const at : watched)
  {
    m.module->attached_services().watch_reads(at, size, true);
  }
  if(m.target->controller_watch_hooks() != hooks)
  {
    throw std::runtime_error{
        std::format("{} hooks, {} expected", m.target->controller_watch_hooks(), hooks)};
  }
  m.set("pc", symbol("read_entry"));
  m.set("r0", loops);
  m.set("r2", flash_load);
  auto const start = clock_type::now();
  m.run_until(symbol("read_done"));
  return per_access(start, loops);
}

auto spaced_ranges(std::uint32_t count) -> measured
{
  std::vector<address> watched;
  for(std::uint32_t slot = 0; slot < count; ++slot)
  {
    watched.push_back(flash_load + 8 + 16 * slot);
  }
  return flash_loads(watched, 2, 4);
}

auto percentile(std::vector<double> samples, double fraction) -> double
{
  std::ranges::sort(samples);
  return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1))];
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
}

auto main(int argc, char** argv) -> int
{
  auto const args = std::span{argv, static_cast<std::size_t>(argc)};
  int        reps = 21;
  if(auto const value = option(args, "--reps"))
  {
    std::from_chars(value->data(), value->data() + value->size(), reps);
  }
  auto const acquire = [](uc_engine*, core::exact_pc_hook& exact) { exact.acquire(); };
  auto const cycle   = [](uc_engine*, core::exact_pc_hook& exact) {
    exact.acquire();
    exact.release();
  };
  std::vector<scenario> scenarios{
      {"raw-read",            [] { return raw_loop("read_entry", "read_done", mmio_base); }             },
      {"raw-write",           [] { return raw_loop("write_entry", "write_done", mmio_base); }           },
      {"storage-read",        [] { return machine_loop("read_entry", "read_done", rw_register, loops); }},
      {"storage-write",
       [] { return machine_loop("write_entry", "write_done", rw_register, loops); }                     },
      {"model-read",
       [] { return machine_loop("read_entry", "read_done", testdev + data_offset, loops); }             },
      {"model-write",
       [] { return machine_loop("write_entry", "write_done", testdev + data_offset, loops); }           },
      {"dma-write-exec",      [] { return dma_writes(true); }                                           },
      {"dma-write-data",      [] { return dma_writes(false); }                                          },
      {"write-ctrl",
       [] {
         return machine_loop(
             "write_entry", "write_done", testdev + ctrl_offset, slice_loops,
             [](bench_machine& m) { m.set("primask", 1); });
       }                                                                                                },
      {"dma-mid-slice",
       [] {
         return machine_loop(
             "write_entry", "write_done", testdev + ctrl_offset, slice_loops, [](bench_machine& m) {
               m.set("primask", 1);
               m.model().patch_on_go = sram_data;
               m.model().patch_value = 0x1234;
             });
       }                                                                                                },
      {"irq-heavy",
       [] {
         return machine_loop(
             "write_entry", "write_done", rw_register, slice_loops, [](bench_machine& m) {
               m.module->write_system(popcorn::testing::test_arch::timer_period_offset, 1000);
               m.module->write_system(popcorn::testing::test_arch::timer_control_offset, 1);
             });
       }                                                                                                },
      {"store-no-hook",       [] { return raw_loop("store_entry", "store_done", sram_data); }           },
      {"store-exact-pc",
       [acquire] { return raw_loop("store_entry", "store_done", sram_data, acquire); }                  },
      {"store-released",
       [cycle] { return raw_loop("store_entry", "store_done", sram_data, cycle); }                      },
      {"store-no-hook-warm",
       [] {
         return raw_loop(
             "store_entry", "store_done", sram_data, [](uc_engine*, core::exact_pc_hook&) {}, true);
       }                                                                                                },
      {"store-exact-pc-warm",
       [acquire] { return raw_loop("store_entry", "store_done", sram_data, acquire, true); }            },
      {"store-released-warm",
       [cycle] { return raw_loop("store_entry", "store_done", sram_data, cycle, true); }                },
      {"load-no-hook",        [] { return flash_loads({}, 0); }                                         },
      {"load-read-hook",      [] { return flash_loads({flash_load}, 4); }                               },
      {"load-read-hook-miss", [] { return flash_loads({flash_load + 0x100}, 2); }                       },
      {"load-read-hook-far",  [] { return flash_loads({flash_load + 0x1'0000}, 4); }                    },
      {"load-ranges-1",       [] { return spaced_ranges(1); }                                           },
      {"load-ranges-8",       [] { return spaced_ranges(8); }                                           },
      {"load-ranges-64",      [] { return spaced_ranges(64); }                                          },
  };
  if(auto const only = option(args, "--only"))
  {
    std::erase_if(scenarios, [&](scenario const& entry) { return entry.name != *only; });
  }
  for(int rep = 0; rep < reps; ++rep)
  {
    for(auto& entry : scenarios)
    {
      entry.samples.push_back(entry.run().ns);
    }
  }
  for(auto const& entry : scenarios)
  {
    std::println(
        R"({{"scenario": "{}", "reps": {}, "median_ns": {:.2f}, "p95_ns": {:.2f}}})", entry.name,
        reps, percentile(entry.samples, 0.5), percentile(entry.samples, 0.95));
  }
  return 0;
}
