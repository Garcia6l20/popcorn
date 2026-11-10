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
    .variant        = &stm32::u5_dac,
    .chip_modes     = 0b1110'1010,
    .pad_modes      = 0b0111'0111,
    .reserved_modes = 0
};
}

namespace dac_ur
{
auto the_rig() -> rig const&
{
  return u5;
}
}

TEST_CASE(
    "RM0456 35.7.21: DAC_AUTOCR stores AUTOMODE, a write logs one warning, the register is "
    "storage_only, and strict_peripherals raises before the store")
{
  auto const& r     = dac_ur::the_rig();
  auto const  store = [&](dac_ur::bench const& b, std::uint32_t value) {
    auto&       regs  = b.f->target->peripheral(r.instance);
    auto const* found = popcorn::find_register(regs.description(), "AUTOCR");
    REQUIRE(found != nullptr);
    return b.f->target->model(r.instance)
        ->on_write(
            regs, found->offset, 4, regs.peek(found->offset), value, {.pc = 0, .now = icount{0}});
  };
  auto const peek = [&](dac_ur::bench const& b) {
    auto&       regs  = b.f->target->peripheral(r.instance);
    auto const* found = popcorn::find_register(regs.description(), "AUTOCR");
    return regs.peek(found->offset);
  };
  dac_ur::bench b{r};
  auto const    offset =
      popcorn::find_register(b.f->target->peripheral(r.instance).description(), "AUTOCR")->offset;
  CHECK(b.f->target->model(r.instance)->storage_only(offset));
  CHECK_FALSE(b.f->target->model(r.instance)->storage_only(0));
  REQUIRE(store(b, 1u << 22).has_value());
  REQUIRE(store(b, 0).has_value());
  REQUIRE(store(b, 1u << 22).has_value());
  CHECK(peek(b) == 1u << 22);
  REQUIRE(b.logged.size() == 1);
  CHECK(b.logged.front().reg == "AUTOCR");
  CHECK(b.logged.front().message.contains("not modelled"));
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.trigger(r.triggers[4]);
  CHECK(b.logged.size() == 1);
  dac_ur::bench strict{r, true};
  CHECK_THROWS_AS((void)store(strict, 1u << 22), popcorn::model_error);
  CHECK(peek(strict) == 0);
}

TEST_CASE("RM0456 Table 137: LPDMA1 serves dac1_ch1_dma as request 8 into DHR12R1")
{
  auto const&   r = dac_ur::the_rig();
  dac_ur::bench b{r};
  auto const    tim6 = r.triggers[4];
  b.write("CR", dac_ur::ten1 | dac_ur::tsel1(5) | dac_ur::dmaen1);
  b.write("DHR12R1", 0x111);
  b.dma_channel(8, "DHR12R1", {0x222, 0x333}, "LPDMA1");
  b.idle_to(b.now_ps() + 100 * dac_ur::cycles(1, r.hclk_hz));
  CHECK(b.dma_left("LPDMA1") == 8);
  b.trigger(tim6);
  CHECK((b.read("DOR1") & 0xFFF) == 0x111);
  CHECK((b.read("DHR12R1") & 0xFFF) == 0x222);
  CHECK(b.dma_left("LPDMA1") == 4);
  b.trigger(tim6);
  CHECK((b.read("DHR12R1") & 0xFFF) == 0x333);
  CHECK(b.dma_left("LPDMA1") == 0);
}
