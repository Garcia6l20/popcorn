#include "wwdg_hardware_cases.hpp"

namespace
{
auto desc() -> popcorn::part_description const&
{
  return popcorn::testing::h573();
}

constexpr wwdg_test::rig h5{
    .firmware         = "h573_rcc_ll",
    .enable_reg       = "RCC_APB1LENR",
    .pclk1_hz         = 32'000'000,
    .irq              = 0,
    .cfg_reg          = "RCC_CFGR2",
    .wdgtb_lsb        = 11,
    .wdgtb_count      = 8,
    .desc             = &desc,
    .option_reg       = "OPTSR_CUR",
    .hardware_enables = true};
}

namespace wwdg_test
{
auto the_rig() -> rig const&
{
  return h5;
}
}
