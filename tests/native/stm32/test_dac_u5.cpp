#include "dac_ur_cases.hpp"

namespace
{
auto desc() -> popcorn::part_description const&
{
  return popcorn::testing::u575();
}

constexpr dac_ur::rig u5{
    .firmware          = "rcc_ll",
    .desc              = &desc,
    .instance          = "DAC1",
    .clock_reg         = "RCC_AHB3ENR",
    .clock_field       = "DAC1EN",
    .reset_reg         = "RCC_AHB3RSTR",
    .reset_field       = "DAC1RST",
    .hclk_hz           = 4'000'000,
    .wake_buffer_ps    = 4'200'000,
    .wake_no_buffer_ps = 2'000'000,
    .triggers =
        {"tim1_trgo", "tim2_trgo", "tim4_trgo", "tim5_trgo", "tim6_trgo", "tim7_trgo", "tim8_trgo",
                   "tim15_trgo", "", "", "lptim1_ch1", "lptim3_ch1", "exti9", "", ""},
    .on_chip_modes = true
};
}

namespace dac_ur
{
auto the_rig() -> rig const&
{
  return u5;
}
}
