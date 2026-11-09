#pragma once

#include "wwdg_cases.hpp"

TEST_CASE("under reset_and_continue the LL firmware restarts and reads RCC_CSR.WWDGRSTF")
{
  auto options             = popcorn::testing::stm32_options();
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  popcorn::testing::stm32_fixture f{
      wwdg_test::the_rig().desc(), wwdg_test::the_rig().firmware, std::move(options)};
  auto const stopped = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{1});
  CHECK(f.field("RCC", "CSR", "WWDGRSTF") == 1);
}

TEST_CASE("the LL firmware arms WWDG with WDGTB 3, the last refresh plus 64 ticks resets")
{
  popcorn::testing::stm32_fixture f{wwdg_test::the_rig().desc(), wwdg_test::the_rig().firmware};
  std::vector<std::uint64_t>      refreshes;
  auto const                      hook = f.target->add_intercept(
      sym(wwdg_test::the_rig().firmware, "refresh"), [&](core::intercept_call const& c) {
        refreshes.push_back(c.target().virtual_clock().time().ps);
        return core::intercept_result::proceed();
      });
  auto const stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::reset_request);
  REQUIRE(refreshes.size() == 50);
  auto const tick = wwdg_test::ps_of(wwdg_test::period_cycles(3), wwdg_test::the_rig().pclk1_hz);
  CHECK(f.log() == std::vector<std::uint32_t>{0, 0xFF, 0x1FF});
  CHECK(f.now().ps > refreshes.back() + 63 * tick);
  CHECK(f.now().ps <= refreshes.back() + 64 * tick + 10 * wwdg_test::ms / 1000);
}
