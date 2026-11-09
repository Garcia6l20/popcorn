#include "wwdg_ll_cases.hpp"
#include "wwdg_sleep_cases.hpp"

namespace
{
auto desc() -> popcorn::part_description const&
{
  return popcorn::testing::l476rc();
}

constexpr wwdg_test::rig l4{
    .firmware         = "l476_l4_wwdg_ll",
    .enable_reg       = "APB1ENR1",
    .pclk1_hz         = 4'000'000,
    .irq              = 0,
    .cfg_reg          = "CFGR",
    .wdgtb_lsb        = 7,
    .wdgtb_count      = 4,
    .desc             = &desc,
    .option_reg       = "OPTR",
    .hardware_enables = true,
    .sleep_reg        = "APB1SMENR1"};
}

namespace wwdg_test
{
auto the_rig() -> rig const&
{
  return l4;
}
}
