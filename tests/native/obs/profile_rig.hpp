#pragma once

#include "../stm32/stm32_fixture.hpp"
#include "profile_sink.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/obs/pctr.hpp>
#include <popcorn/periph/devices/at25sf.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::testing::profile_rig
{
namespace core  = popcorn::core;
namespace obs   = popcorn::obs;
namespace stm32 = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;
using namespace std::chrono_literals;

constexpr std::uint16_t tim2_exception    = 16 + 45;
constexpr std::uint16_t tim16_exception   = 16 + 70;
constexpr std::uint16_t svcall_exception  = 11;
constexpr std::uint16_t pendsv_exception  = 14;
constexpr std::uint16_t systick_exception = 15;
constexpr std::uint16_t tim6_exception    = 16 + 49;
constexpr std::uint16_t spi1_exception    = 16 + 59;
constexpr address       tim2_cnt          = 0x4000'0024;
constexpr std::uint64_t ms_ps             = 1'000'000'000;

inline auto block(address pc, std::uint16_t instructions, std::uint32_t dt) -> core::trace_record
{
  return {
      .kind = core::trace_kind::block, .aux = instructions, .dt = dt, .payload = {pc, 0, 0}
  };
}

inline auto enter(std::uint16_t number, std::uint32_t dt) -> core::trace_record
{
  return {.kind = core::trace_kind::irq, .flags = 1, .aux = number, .dt = dt};
}

inline auto leave(std::uint16_t number, std::uint32_t dt) -> core::trace_record
{
  return {.kind = core::trace_kind::irq, .flags = 0, .aux = number, .dt = dt};
}

inline auto access(
    std::uint16_t id,
    address       pc,
    address       at,
    std::uint32_t value,
    bool          write,
    std::uint32_t dt = 0) -> core::trace_record
{
  return {
      .kind  = core::trace_kind::mmio,
      .flags = static_cast<std::uint8_t>(
          4 | (write ? core::trace_write_flag : 0) | (pc == 0 ? core::trace_debugger_flag : 0)),
      .aux     = id,
      .dt      = dt,
      .payload = {pc, at, value},
  };
}

inline auto time_to(std::uint64_t count, bool rebase = true) -> core::trace_record
{
  return {
      .kind    = core::trace_kind::time,
      .flags   = rebase ? core::trace_rebase_flag : std::uint8_t{0},
      .payload = {static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(count >> 32), 0},
  };
}

inline auto
    slept(std::uint32_t length, std::uint32_t dt, std::uint16_t woke_by, std::uint8_t flags = 1)
        -> core::trace_record
{
  return {
      .kind    = core::trace_kind::sleep,
      .flags   = flags,
      .aux     = woke_by,
      .dt      = dt,
      .payload = {length, 0, 0}
  };
}

inline auto cut_at(address pc, std::uint16_t unrun) -> core::trace_record
{
  return {
      .kind = core::trace_kind::cut, .aux = unrun, .payload = {pc, 0, 0}
  };
}

inline auto fed(std::vector<core::trace_record> const& records, obs::profile_options options = {})
    -> obs::profile_result
{
  obs::profile_sink sink{icount{1000}, std::move(options)};
  sink.feed(records);
  return sink.result();
}

inline auto gaps(obs::profile_result const& result) -> std::uint64_t
{
  return result.other_gaps.total + result.sleeps.total;
}

inline auto tim_machine() -> std::unique_ptr<stm32_fixture>
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  auto f                 = std::make_unique<stm32_fixture>(u575(), "tim_ticks", std::move(options));
  REQUIRE(
      f->run_to_stop({.max_instructions = icount{100'000'000},
                      .until            = sym("tim_ticks", "main"),
                      .budget_left      = icount{1'000'000}})
          .reason
      == stop_reason::address);
  return f;
}

inline auto gpdma_machine() -> std::unique_ptr<stm32_fixture>
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  auto f = std::make_unique<stm32_fixture>(u575(), "gpdma_uart", std::move(options));
  REQUIRE(
      f->run_to_stop({.max_instructions = icount{100'000'000},
                      .until            = sym("gpdma_uart", "main"),
                      .budget_left      = icount{1'000'000}})
          .reason
      == stop_reason::address);
  return f;
}

inline auto spi_machine() -> std::unique_ptr<stm32_fixture>
{
  stm32::board board{};
  board.spi["SPI1"].push_back(
      {.device = std::make_shared<popcorn::periph::devices::at25sf>(
           popcorn::periph::devices::at25sf_options{.page_program = 800us})});
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  options.vector_table   = 0x800'c008;
  stm32::register_builtin_models();
  options.models = stm32::factories(board, u575());
  auto f         = std::make_unique<stm32_fixture>(u575(), "spi_flash", std::move(options));
  REQUIRE(
      f->run_to_stop({.max_instructions = icount{100'000'000},
                      .until            = sym("spi_flash", "main"),
                      .budget_left      = icount{1'000'000}})
          .reason
      == stop_reason::address);
  return f;
}

struct profiled
{
  std::unique_ptr<obs::profile_sink> sink;
  core::hook_handle                  handle;

  explicit profiled(core::machine& target, obs::profile_options options = {})
      : sink{std::make_unique<obs::profile_sink>(target.virtual_clock().now(), std::move(options))},
        handle{target.add_trace_sink(obs::profile_sink::kinds, sink->sink())}
  {}

  auto take() -> obs::profile_result
  {
    handle.reset();
    return sink->result();
  }
};
}
