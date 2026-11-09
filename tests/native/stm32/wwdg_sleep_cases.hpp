#pragma once

#include "wwdg_hardware_cases.hpp"

TEST_CASE("hardware mode keeps WWDGSMEN at 1 where the RM forces it, software mode lets it clear")
{
  auto const& r = wwdg_test::the_rig();
  for(auto const hardware : {true, false})
  {
    CAPTURE(hardware);
    wwdg_test::bench b{
        r, wwdg_test::with_option(r.desc(), hardware),
        hardware ? wwdg_test::hardware_options() : popcorn::testing::stm32_options(), false};
    b.run_for(wwdg_test::ms);
    CHECK(b.rcc_get(r.sleep_reg, "WWDGSMEN") == 1);
    b.rcc_field(r.sleep_reg, "WWDGSMEN", 0);
    CHECK(b.rcc_get(r.sleep_reg, "WWDGSMEN") == (hardware ? 1u : 0u));
  }
}
