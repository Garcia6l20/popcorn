#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address flag = 0x4001'0008;

auto booted(std::uint32_t threshold) -> fixture
{
  core::machine_options options;
  options.spin_threshold = threshold;
  fixture f{m33(), "spin", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto start(fixture const& f, std::string_view entry) -> void
{
  f.set("pc", sym("spin", entry));
}

auto long_request() -> core::slice_request
{
  core::slice_request request;
  request.max_instructions = icount{10'000'000};
  request.wall_budget      = std::chrono::seconds{10};
  return request;
}
}

TEST_CASE("a poll of an unmodelled status register stops as a spin naming it")
{
  auto f = booted(1000);
  start(f, "wait_ready");
  auto const before  = f.target->virtual_clock().now();
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in wait_ready"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT = 0x00000001"));
  CHECK(stopped.detail.contains("fields READY"));
  auto const executed = f.target->virtual_clock().now().value - before.value;
  CHECK(executed == 4 + 3 * 1004);
}

TEST_CASE("a delay loop makes progress and never stops as a spin")
{
  auto f = booted(1000);
  start(f, "delay");
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
}

TEST_CASE("a poll of a flag set later stops only when the threshold is below the wait")
{
  constexpr std::uint64_t iterations = 40'000;
  auto const              wait       = [&](std::uint32_t threshold) {
    auto f = booted(threshold);
    start(f, "wait_flag");
    core::slice_request request = long_request();
    request.max_instructions    = icount{3 * iterations};
    auto const first            = f.run(request);
    if(first.stopped)
    {
      return first.stopped->reason;
    }
    REQUIRE(f.target->mem().write(flag, 4, 1));
    return f.run_to_stop(long_request()).reason;
  };
  CHECK(wait(50'000) == stop_reason::user);
  CHECK(wait(10'000) == stop_reason::spin);
}

TEST_CASE("a loop storing a constant to RAM counts as no progress")
{
  auto f = booted(1000);
  start(f, "store_forever");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.contains("in store_forever"));
  CHECK_FALSE(stopped.detail.contains("polling"));
}

TEST_CASE("a reset forgets the loop being tracked")
{
  auto f = booted(1000);
  start(f, "wait_ready");
  core::slice_request request = long_request();
  request.max_instructions    = icount{3 * 900};
  REQUIRE_FALSE(f.run(request).stopped.has_value());
  f.target->reset();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  start(f, "wait_ready");
  request.max_instructions = icount{3 * 900};
  CHECK_FALSE(f.run(request).stopped.has_value());
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::spin);
}

TEST_CASE("threshold 0 never tracks")
{
  auto f = booted(0);
  start(f, "store_forever");
  core::slice_request request = long_request();
  request.max_instructions    = icount{100'000};
  CHECK_FALSE(f.run(request).stopped.has_value());
}

namespace
{
auto resident_bytes() -> std::uint64_t
{
  std::ifstream statm{"/proc/self/statm"};
  std::uint64_t size     = 0;
  std::uint64_t resident = 0;
  statm >> size >> resident;
  return resident * 4096;
}
}

TEST_CASE("a loop longer than the window keeps its MMIO read list bounded")
{
  auto f = booted(1000);
  start(f, "long_poll");
  auto const before        = resident_bytes();
  auto       request       = long_request();
  request.max_instructions = icount{15'000'000};
  REQUIRE_FALSE(f.run(request).stopped.has_value());
  CHECK(resident_bytes() - before < 16u << 20);
}

TEST_CASE("an idle wait with a tick handler is not a spin")
{
  core::machine_options options;
  options.spin_threshold = 10;
  fixture f{m33(), "idle", options};
  f.set("pc", sym("idle", "idle_entry"));
  core::slice_request request{
      .wall_budget = std::chrono::seconds{10}, .until_time = popcorn::vtime{1'000'000'000'000}};
  for(;;)
  {
    auto const slice = f.run(request);
    if(slice.stopped)
    {
      CHECK(slice.stopped->reason == stop_reason::time);
      break;
    }
  }
}

TEST_CASE("a poll of a register whose model completes later is not a spin")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  fixture f{m33(), "spin", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto& model         = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
  model.go_delay      = 100'000;
  model.data_on_event = true;
  start(f, "wait_model");
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
  CHECK(model.fired_at.has_value());
}

TEST_CASE("a hung poll still spins while an interrupt handler reads MMIO")
{
  auto f = booted(1000);
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}
