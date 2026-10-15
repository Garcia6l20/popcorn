#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
using popcorn::plugin::load;
using popcorn::plugin::scope;

auto cm33() -> popcorn::part_description
{
  popcorn::part_description part;
  part.part     = "plugin-builtin";
  part.core.cpu = "CM33";
  return part;
}

auto arch_registered() -> bool
{
  try
  {
    static_cast<void>(popcorn::arch::find_arch(cm33()));
    return true;
  }
  catch(popcorn::config_error const&)
  {
    return false;
  }
}

auto mapped(std::string_view library) -> bool
{
  std::ifstream     maps{"/proc/self/maps"};
  std::string const text{std::istreambuf_iterator<char>{maps}, {}};
  return text.contains(library);
}
}

TEST_CASE("the built-in plugins load from core's directory and register globally")
{
  auto const core      = std::filesystem::path{std::getenv("POPCORN_PLUGIN_CORE")};
  auto const directory = popcorn::plugin::library_directory();
  CHECK(directory == std::filesystem::canonical(core).parent_path());

  REQUIRE_FALSE(mapped("libpopcorn_armv8m.so"));
  REQUIRE_FALSE(mapped("libpopcorn_stm32.so"));
  REQUIRE_FALSE(arch_registered());
  REQUIRE_FALSE(popcorn::periph::registered_glob("RCC", "STM32U5_rcc_x"));

  auto const& armv8m = load(directory / "libpopcorn_armv8m.so", scope::builtin);
  CHECK(armv8m.name == "armv8m");
  CHECK(armv8m.where == scope::builtin);
  CHECK(armv8m.models.empty());
  CHECK(mapped("libpopcorn_armv8m.so"));
  CHECK(arch_registered());
  CHECK(popcorn::arch::find_code_analysis(cm33().core) != nullptr);

  auto const& stm32 = load(directory / "libpopcorn_stm32.so", scope::builtin);
  CHECK(stm32.name == "stm32");
  CHECK(stm32.models.empty());
  CHECK(popcorn::periph::registered_glob("RCC", "STM32U5_rcc_x") == "STM32U5_rcc_*");

  CHECK(&load(directory / "libpopcorn_stm32.so", scope::builtin) == &stm32);
  CHECK_THROWS_WITH_AS(
      static_cast<void>(load(directory / "libpopcorn_stm32.so", scope::declared)),
      doctest::Contains("scope"), popcorn::config_error);
}
