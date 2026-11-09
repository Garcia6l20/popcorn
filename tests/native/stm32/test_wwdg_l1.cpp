#include "wwdg_ll_cases.hpp"

namespace
{
auto desc() -> popcorn::part_description const&
{
  return popcorn::testing::l152re();
}

constexpr wwdg_test::rig l1{
    .firmware    = "l152xe_l1_wwdg_ll",
    .enable_reg  = "APB1ENR",
    .pclk1_hz    = 2'097'152,
    .irq         = 0,
    .cfg_reg     = "CFGR",
    .wdgtb_lsb   = 7,
    .wdgtb_count = 4,
    .desc        = &desc};
}

namespace wwdg_test
{
auto the_rig() -> rig const&
{
  return l1;
}
}

TEST_CASE("RM0038 6.3.7: WWDGRST clears the registers and a stale expiry acts on nothing")
{
  wwdg_test::bench b{l1};
  b.write("WWDG", "CR", 0xFF);
  auto const stale = *b.expiry_due();
  b.run_for((stale - b.f.now().ps) / 2);
  b.rcc_field("APB1RSTR", "WWDGRST", 1);
  b.rcc_field("APB1RSTR", "WWDGRST", 0);
  CHECK(b.read("WWDG", "CR") == 0x7F);
  CHECK(b.read("WWDG", "CFR") == 0x7F);
  b.write("WWDG", "CFR", 0x7F);
  b.write("WWDG", "CR", 0xFF);
  auto const pending = b.events(0);
  CHECK(std::ranges::contains(pending, stale));
  auto const fresh = std::optional{std::ranges::max(pending)};
  CHECK(*fresh > stale + 2 * wwdg_test::ms);
  CHECK_FALSE(b.reset_requested(stale + 2 * wwdg_test::ms));
  CHECK(b.reset_requested(*fresh + wwdg_test::ms));
  CHECK(b.f.now().ps >= *fresh);
}
