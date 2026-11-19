#pragma once

#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>

namespace popcorn::testing
{
struct iwdg_matrix_clock
{
  std::uint64_t hz;
  std::uint64_t lsi_cycle;
};

/**
 * @brief Changes the IWDG prescaler of a running watchdog between every pair of PR values and
 *        checks the counter still reaches the reset.
 * @tparam Bus Register access helper built from the fixture.
 * @param start Returns a fixture parked at an idle loop with the LSI running.
 * @param run Runs the fixture for a span of picoseconds and returns the stop.
 * @param clock LSI rate and cycle time of the part.
 * @param top Highest PR value with its own divider, one more PR value is run as well.
 *
 * The count kept across the prescaler change is bounded from the refresh time and the old tick.
 * The firmware pattern of the example is replayed: unlock, PR, RLR, spin until SR reads 0, stall.
 */
template <class Bus, class Start, class Run>
void check_prescaler_matrix(Start&& start, Run&& run, iwdg_matrix_clock clock, unsigned top)
{
  constexpr std::uint64_t ps_per_s = 1'000'000'000'000;
  constexpr std::uint64_t reload   = 50;
  auto const              tick     = [&](unsigned pr) {
    return (std::uint64_t{4} << std::min(pr, top)) * ps_per_s / clock.hz;
  };
  for(unsigned from = 0; from <= top + 1; ++from)
  {
    for(unsigned to = 0; to <= top + 1; ++to)
    {
      CAPTURE(from);
      CAPTURE(to);
      auto       f = start();
      Bus        b{f};
      auto const now  = [&] { return f.target->virtual_clock().time().ps; };
      auto const spin = [&] {
        for(auto step = 0; step < 32 and b.read("IWDG", "SR") != 0; ++step)
        {
          REQUIRE(run(f, clock.lsi_cycle).reason == popcorn::stop_reason::time);
        }
        REQUIRE(b.read("IWDG", "SR") == 0);
        return now();
      };
      b.write("IWDG", "KR", 0xCCCC);
      b.write("IWDG", "KR", 0x5555);
      b.write("IWDG", "PR", from);
      b.write("IWDG", "RLR", reload);
      spin();
      b.write("IWDG", "KR", 0xAAAA);
      auto const refreshed = now();
      REQUIRE(run(f, 10 * tick(from)).reason == popcorn::stop_reason::time);
      b.write("IWDG", "KR", 0x5555);
      b.write("IWDG", "PR", to);
      b.write("IWDG", "RLR", 1000);
      auto const seen  = spin();
      auto const most  = (seen - refreshed) / tick(from);
      auto const least = seen > refreshed + clock.lsi_cycle + tick(from)
                           ? (seen - clock.lsi_cycle - refreshed - tick(from)) / tick(from)
                           : 0;
      auto const early = seen - clock.lsi_cycle + (reload - most + 1) * tick(to) - clock.lsi_cycle;
      auto const late  = seen + (reload - least + 1) * tick(to) + clock.lsi_cycle;
      auto const stop  = run(f, 5 * ps_per_s);
      REQUIRE(stop.reason == popcorn::stop_reason::reset_request);
      CHECK(now() >= early);
      CHECK(now() <= late);
    }
  }
}
}
