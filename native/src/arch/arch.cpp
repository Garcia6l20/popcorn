#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <mutex>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::arch
{
arch::~arch() = default;

namespace
{
struct registry
{
  std::mutex               guard;
  std::vector<arch_module> modules;
};

static_assert(
    std::is_nothrow_move_assignable_v<arch_module> and std::is_nothrow_swappable_v<arch_module>);

auto table() -> registry&
{
  static registry instance;
  return instance;
}
}

void register_arch(arch_module module)
{
  auto&                 entries = table();
  std::lock_guard const lock{entries.guard};
  auto const            found = std::ranges::find(entries.modules, module.name, &arch_module::name);
  if(found == entries.modules.end())
  {
    entries.modules.push_back(std::move(module));
    return;
  }
  *found = std::move(module);
  std::ranges::rotate(found, std::next(found), entries.modules.end());
}

auto find_arch(part_description const& description) -> arch_factory
{
  auto& entries = table();
  {
    std::lock_guard const lock{entries.guard};
    for(auto const& module : entries.modules | std::views::reverse)
    {
      if(module.implements(description.core))
      {
        return module.factory;
      }
    }
  }
  throw config_error(
      description.part, 0, "core.cpu",
      std::format("a core an arch module implements, not {}", description.core.cpu));
}
}
