#include <popcorn/arch/arch.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <memory>

POPCORN_PLUGIN(writes_arch, r)
{
  static_cast<void>(r);
  popcorn::arch::register_arch({
      .name       = "writes_arch",
      .implements = [](popcorn::core_desc const& core) { return core.cpu == "writes-arch"; },
      .factory    = [](popcorn::core_desc const&) -> std::unique_ptr<popcorn::arch::arch> {
        return nullptr;
      },
  });
}
