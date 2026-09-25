#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <popcorn/core/description.hpp>

#include "../support/elf_image.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace arch = popcorn::arch;
using popcorn::address;
using popcorn::icount;
using clock_type = std::chrono::steady_clock;

constexpr int           iterations    = 200;
constexpr address       unmapped_page = 0x3000'0000;
constexpr std::uint32_t sram_size     = 0xc'0000;

auto options() -> core::machine_options
{
  core::machine_options made;
  made.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  return made;
}

auto image() -> popcorn::image::image const&
{
  static auto const loaded = popcorn::testing::read_elf(POPCORN_FW_SNAPSHOT);
  return loaded;
}

auto symbol(std::string_view name) -> address
{
  return image().lookup(name).value;
}

void dirty(core::machine& target, address at, std::uint32_t bytes)
{
  auto const result = target.call(
      {
          .entry = symbol("dirty") | 1,
          .args =
              {{.kind = arch::value_kind::i32, .bits = at},
                     {.kind = arch::value_kind::i32, .bits = bytes}},
          .abi             = arch::float_abi::soft,
          .return_sentinel = unmapped_page
  },
      icount{1'000'000});
  if(result.stopped.reason != popcorn::stop_reason::returned)
  {
    throw std::runtime_error{"dirty did not return"};
  }
}

void report(std::string const& name, std::vector<double> samples)
{
  std::ranges::sort(samples);
  auto const median = samples[samples.size() / 2];
  auto const p95    = samples[samples.size() * 95 / 100];
  std::println("{:<14} median {:8.1f} us  p95 {:8.1f} us", name, median, p95);
}

auto time_us(std::function<void()> const& body) -> double
{
  auto const start = clock_type::now();
  body();
  return std::chrono::duration<double, std::micro>(clock_type::now() - start).count();
}
}

auto main() -> int
{
  core::machine target{
      popcorn::load_description(POPCORN_M33_DESC),
      [](popcorn::core_desc const& unit) -> std::unique_ptr<arch::arch> {
        return std::make_unique<popcorn::testing::test_arch>(unit);
      },
      options()};
  target.load(image());
  target.reset();
  core::slice_request boot;
  boot.wall_budget = std::chrono::seconds{10};
  if(not target.run_slice(boot).stopped)
  {
    throw std::runtime_error{"the firmware did not reach its breakpoint"};
  }
  auto const scratch = symbol("scratch");
  auto const taken   = target.snapshot();
  auto const decoded = core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))};
  auto const flash_patch = symbol("work") & ~address{0xfff};

  std::vector<double> snapshots;
  for(int round = 0; round < iterations; ++round)
  {
    snapshots.push_back(time_us([&] { std::ignore = target.snapshot(); }));
  }
  report("snapshot", snapshots);

  auto const scenario = [&](std::string const& name, std::function<void()> const& before,
                            core::snapshot const& from) {
    std::vector<double> samples;
    for(int round = 0; round < iterations; ++round)
    {
      before();
      samples.push_back(time_us([&] { target.restore(from); }));
    }
    report(name, samples);
  };
  scenario("clean", [] {}, taken);
  scenario("dirty-64k", [&] { dirty(target, scratch, 0x1'0000); }, taken);
  scenario("dirty-all", [&] { dirty(target, 0x2000'0000, sram_size - 0x1000); }, taken);
  scenario("serialized", [&] { dirty(target, 0x2000'0000, sram_size - 0x1000); }, decoded);
  scenario(
      "flash-patched",
      [&] {
        std::vector<std::byte> patch(0x1000, std::byte{0xbf});
        if(not target.mem().write_block(flash_patch, patch))
        {
          throw std::runtime_error{"flash patch refused"};
        }
      },
      taken);
  return 0;
}
