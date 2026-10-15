#include <popcorn/arch/arch.hpp>
#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <doctest/doctest.h>

#include <memory>

TEST_CASE("a built-in plugin load after the direct registration registers nothing again")
{
  popcorn::periph::stm32::register_builtin_models();
  popcorn::arch::armv8m::register_builtin();

  popcorn::periph::register_model(
      "RCC", "STM32U5_rcc_x", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
        return std::shared_ptr<popcorn::periph::model>{};
      });
  popcorn::arch::register_arch({
      .name       = "marker",
      .implements = [](popcorn::core_desc const& core) { return core.cpu == "CM33"; },
      .factory    = [](popcorn::core_desc const&) -> std::unique_ptr<popcorn::arch::arch> {
        return nullptr;
      },
  });
  popcorn::core_desc cm33;
  cm33.cpu = "CM33";
  REQUIRE(popcorn::periph::registered_glob("RCC", "STM32U5_rcc_x") == "STM32U5_rcc_x");
  REQUIRE(popcorn::arch::find_code_analysis(cm33) == nullptr);

  auto const  directory = popcorn::plugin::library_directory();
  auto const& stm32 =
      popcorn::plugin::load(directory / "libpopcorn_stm32.so", popcorn::plugin::scope::builtin);
  auto const& armv8m =
      popcorn::plugin::load(directory / "libpopcorn_armv8m.so", popcorn::plugin::scope::builtin);
  CHECK(stm32.name == "stm32");
  CHECK(armv8m.name == "armv8m");

  CHECK(popcorn::periph::registered_glob("RCC", "STM32U5_rcc_x") == "STM32U5_rcc_x");
  CHECK(popcorn::arch::find_code_analysis(cm33) == nullptr);
}
