#pragma once

#include "../support/inert_model.hpp"
#include "wwdg_cases.hpp"

#include <popcorn/periph/stm32/board.hpp>

namespace wwdg_test
{
class flash_zero final : public popcorn::testing::inert_model
{};

inline auto hardware_options() -> core::machine_options
{
  auto options = popcorn::testing::stm32_options();
  options.models =
      popcorn::periph::stm32::factories(popcorn::periph::stm32::board{}, the_rig().desc());
  options.models.insert_or_assign(
      "FLASH",
      [](popcorn::periph::model_context&, popcorn::peripheral_desc const&)
          -> std::shared_ptr<popcorn::periph::model> { return std::make_shared<flash_zero>(); });
  return options;
}

inline auto with_option(part_description desc, bool hardware) -> part_description
{
  auto const flash = std::ranges::find(
      desc.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
  REQUIRE(flash != desc.peripherals.end());
  auto const found =
      std::ranges::find(flash->registers, the_rig().option_reg, &popcorn::register_desc::name);
  REQUIRE(found != flash->registers.end());
  auto const bits =
      std::ranges::find(found->fields, std::string_view{"WWDG_SW"}, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  auto const mask = ((std::uint32_t{1} << bits->width) - 1) << bits->lsb;
  found->reset    = hardware ? found->reset & ~mask : found->reset | mask;
  return desc;
}

inline auto without_option(part_description desc) -> part_description
{
  auto const flash = std::ranges::find(
      desc.peripherals, std::string_view{"FLASH"}, &popcorn::peripheral_desc::name);
  REQUIRE(flash != desc.peripherals.end());
  for(auto& reg : flash->registers)
  {
    std::erase_if(reg.fields, [](popcorn::field_desc const& one) { return one.name == "WWDG_SW"; });
  }
  return desc;
}
}

TEST_CASE(
    "with the WWDG_SW option 0 the WWDG is activated at every reset and WDGA does not clear, "
    "WWDGEN set by hardware inferred on the STM32H5")
{
  auto const&      r  = wwdg_test::the_rig();
  auto const       hz = r.pclk1_hz;
  wwdg_test::bench b{
      r, wwdg_test::with_option(r.desc(), true), wwdg_test::hardware_options(), false};
  b.run_for(wwdg_test::ms);
  if(r.hardware_enables)
  {
    CHECK(b.rcc_get(r.enable_reg, "WWDGEN") == 1);
    b.t0 = 0;
  }
  else
  {
    CHECK(b.rcc_get(r.enable_reg, "WWDGEN") == 0);
    CHECK_FALSE(b.expiry_due().has_value());
    b.enable_clock();
  }
  auto const due = b.expiry_due();
  REQUIRE(due.has_value());
  CHECK(*due == b.tick_time(64, hz, 0));
  b.write("WWDG", "CR", 0x7F);
  CHECK((b.read("WWDG", "CR") & wwdg_test::wdga) == wwdg_test::wdga);
  CHECK(b.reset_requested(*b.expiry_due() + wwdg_test::ms));
  CHECK(b.f.now().ps >= *due);
}

TEST_CASE("with the WWDG_SW option 1 the WWDG starts only at the CR write that sets WDGA")
{
  auto const&      r = wwdg_test::the_rig();
  wwdg_test::bench b{r, r.desc(), popcorn::testing::stm32_options(), false};
  b.run_for(wwdg_test::ms);
  CHECK(b.rcc_get(r.enable_reg, "WWDGEN") == 0);
  b.enable_clock();
  CHECK_FALSE(b.expiry_due().has_value());
  CHECK((b.read("WWDG", "CR") & wwdg_test::wdga) == 0);
}

TEST_CASE("checklist 39: the WWDG row needs the WWDG_SW field of the FLASH option register")
{
  auto const& r = wwdg_test::the_rig();
  CHECK_NOTHROW(popcorn::testing::stm32_fixture(r.desc(), r.firmware));
  CHECK_THROWS_AS(
      popcorn::testing::stm32_fixture(wwdg_test::without_option(r.desc()), r.firmware),
      popcorn::config_error);
}

TEST_CASE("the hardware mode starts again at every reset, also the second")
{
  auto const&      r  = wwdg_test::the_rig();
  auto const       hz = r.pclk1_hz;
  wwdg_test::bench b{
      r, wwdg_test::with_option(r.desc(), true), wwdg_test::hardware_options(), false};
  b.run_for(wwdg_test::ms);
  b.f.target->reset(popcorn::reset_kind::software);
  b.f.set("pc", wwdg_test::sym(r.firmware, "idle"));
  auto const second = b.f.now().ps;
  b.run_for(wwdg_test::ms);
  if(!r.hardware_enables)
  {
    b.enable_clock();
  }
  else
  {
    CHECK(b.rcc_get(r.enable_reg, "WWDGEN") == 1);
    b.t0 = second;
  }
  auto const due = b.expiry_due();
  REQUIRE(due.has_value());
  CHECK(*due == b.tick_time(64, hz, 0));
  CHECK((b.read("WWDG", "CR") & wwdg_test::wdga) == wwdg_test::wdga);
}
