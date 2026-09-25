#include "cortex_m.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>

#include <memory>

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
}
