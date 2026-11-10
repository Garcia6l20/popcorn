#include "dac_ur_cases.hpp"

namespace
{
auto desc() -> popcorn::part_description const&
{
  return popcorn::testing::h573();
}

constexpr dac_ur::rig h5{
    .firmware          = "h573_rcc_ll",
    .desc              = &desc,
    .instance          = "DAC",
    .clock_reg         = "RCC_AHB2ENR",
    .clock_field       = "DAC12EN",
    .reset_reg         = "RCC_AHB2RSTR",
    .reset_field       = "DAC12RST",
    .hclk_hz           = 32'000'000,
    .wake_buffer_ps    = 5'000'000,
    .wake_no_buffer_ps = 2'000'000,
    .triggers =
        {"tim1_trgo", "tim2_trgo", "tim4_trgo", "tim5_trgo", "tim6_trgo", "tim7_trgo", "tim8_trgo",
                   "tim15_trgo", "", "", "lptim1_ch1", "lptim2_ch1", "exti9", "", ""},
    .on_chip_modes = false
};
}

namespace dac_ur
{
auto the_rig() -> rig const&
{
  return h5;
}
}

TEST_CASE("a reserved MODE1 write is a model_error when strict and leaves DAC_MCR")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r, true};
  b.write("MCR", 0b010);
  for(std::uint32_t mode : {1u, 3u, 5u, 7u})
  {
    CAPTURE(mode);
    CHECK_FALSE(b.try_write("MCR", mode));
    CHECK(b.read("MCR") == 0b010);
  }
}
