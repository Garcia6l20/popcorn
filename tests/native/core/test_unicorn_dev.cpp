#include <popcorn/core/version.hpp>

#include "../support/smoke.hpp"

#include <doctest/doctest.h>
#include <unicorn/unicorn.h>

#include <cstdint>
#include <string_view>

TEST_CASE("version names the pinned Unicorn commit")
{
  CHECK(popcorn::core::version().find("048c13c") != std::string_view::npos);
}

TEST_CASE("open_m33 opens a Cortex-M33 engine")
{
  auto const engine = popcorn::testing::open_m33();
  REQUIRE(engine != nullptr);

  int model = -1;
  REQUIRE(uc_ctl_get_cpu_model(engine.get(), &model) == UC_ERR_OK);
  CHECK(model == UC_CPU_ARM_CORTEX_M33);
}

TEST_CASE("open_m33 engine executes Thumb code")
{
  auto const engine = popcorn::testing::open_m33();

  constexpr std::uint64_t base         = 0x0800'0000;
  constexpr std::uint8_t  movs_r0_42[] = {0x2a, 0x20};
  REQUIRE(uc_mem_map(engine.get(), base, 0x1000, UC_PROT_ALL) == UC_ERR_OK);
  REQUIRE(uc_mem_write(engine.get(), base, movs_r0_42, sizeof movs_r0_42) == UC_ERR_OK);
  REQUIRE(uc_emu_start(engine.get(), base | 1, base + sizeof movs_r0_42, 0, 0) == UC_ERR_OK);

  std::uint32_t r0 = 0;
  REQUIRE(uc_reg_read(engine.get(), UC_ARM_REG_R0, &r0) == UC_ERR_OK);
  CHECK(r0 == 42);
}
