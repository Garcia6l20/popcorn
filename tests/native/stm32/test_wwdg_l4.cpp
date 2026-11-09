#include "wwdg_cases.hpp"

namespace
{
auto desc() -> popcorn::part_description const&
{
  return popcorn::testing::l476rc();
}

constexpr wwdg_test::rig l4{
    .firmware   = "l476_l4_wwdg_ll",
    .enable_reg = "APB1ENR1",
    .pclk1_hz   = 4'000'000,
    .irq        = 0,
    .desc       = &desc};
}

namespace wwdg_test
{
auto the_rig() -> rig const&
{
  return l4;
}
}
