#include "cortex_m.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>

#include <elf.h>

#include <memory>
#include <mutex>

namespace popcorn::arch::armv8m
{
auto implements(core_desc const& core) -> bool
{
  return find_profile(core.cpu).has_value();
}

auto factory() -> arch_factory
{
  return [](core_desc const& core) -> std::unique_ptr<arch> {
    return std::make_unique<cortex_m>(core);
  };
}

void register_builtin()
{
  static std::once_flag once;
  std::call_once(once, [] {
    register_arch(
        {.name        = "armv8m",
         .implements  = &implements,
         .factory     = factory(),
         .analysis    = &analysis(),
         .elf_machine = EM_ARM});
  });
}
}
