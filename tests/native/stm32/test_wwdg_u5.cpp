#include "wwdg_sleep_cases.hpp"

namespace
{
auto desc() -> popcorn::part_description const&
{
  return popcorn::testing::u575();
}

constexpr wwdg_test::rig u5{
    .firmware         = "rcc_ll",
    .enable_reg       = "RCC_APB1ENR1",
    .pclk1_hz         = 4'000'000,
    .irq              = 0,
    .cfg_reg          = "RCC_CFGR2",
    .wdgtb_lsb        = 11,
    .wdgtb_count      = 8,
    .desc             = &desc,
    .option_reg       = "OPTR",
    .hardware_enables = true,
    .sleep_reg        = "RCC_APB1SMENR1"};
}

namespace wwdg_test
{
auto the_rig() -> rig const&
{
  return u5;
}
}
