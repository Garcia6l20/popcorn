#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/image/image.hpp>

#include "common.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace popcorn::consumers::object_linker
{
inline constexpr address trampoline_base = 0x1000'0000;
inline constexpr address arena_base      = 0x3000'0000;

auto with_linker_regions(mcu_description description) -> mcu_description
{
  description.regions.push_back({
      .name       = "trampolines",
      .start      = trampoline_base,
      .size       = 0x1000,
      .kind       = region_kind::rom,
      .acc        = {.read = true, .write = false, .execute = true},
      .secure     = false,
      .physical   = std::nullopt,
      .controller = std::nullopt,
  });
  description.regions.push_back({
      .name       = "arena",
      .start      = arena_base,
      .size       = 0x1'0000,
      .kind       = region_kind::ram,
      .acc        = {.read = true, .write = true, .execute = false},
      .secure     = false,
      .physical   = std::nullopt,
      .controller = std::nullopt,
  });
  return description;
}

auto stub_memset(core::machine& target, address slot) -> core::hook_handle
{
  return target.add_intercept(slot, [](core::intercept_call const& call) {
    std::array const kinds{arch::value_kind::i32, arch::value_kind::i32, arch::value_kind::i32};
    auto const       args = call.args(kinds);
    std::vector<std::byte> fill(args[2], static_cast<std::byte>(args[1]));
    std::ignore = call.target().mem().write_block(static_cast<address>(args[0]), fill);
    return core::intercept_result::returns(
        arch::call_arg{.kind = arch::value_kind::i32, .bits = args[0]});
  });
}

auto run_linked(core::machine& target, image::image const& linked) -> core::call_result
{
  target.load(linked);
  auto const memset_slot = stub_memset(target, trampoline_base);
  std::ignore            = target.mem().store<float>(arena_base, 1.5f);
  return target.call(
      {.entry           = linked.lookup("usbpd_pe_run").value,
       .args            = {{.kind = arch::value_kind::i32, .bits = arena_base}},
       .returns         = arch::value_kind::i32,
       .abi             = linked.abi,
       .return_sentinel = trampoline_base + 0xffe,
       .stack           = arena_base + 0x1'0000,
       .privileged      = true,
       .interrupts      = false},
      icount{100'000});
}
}

TEST_CASE("object linker consumer adds its regions and stubs imports")
{
  auto const description =
      popcorn::consumers::object_linker::with_linker_regions(popcorn::testing::toy_description());
  popcorn::core::machine target{description, popcorn::testing::test_arch_factory(), {}};
  CHECK(
      target.description().regions.size()
      == popcorn::testing::toy_description().regions.size()
      + 2);
  CHECK_THROWS_AS(
      popcorn::consumers::object_linker::run_linked(target, popcorn::image::image{}),
      popcorn::symbol_error);
}
