#include <popcorn/core/engine.hpp>

#include <unicorn/unicorn.h>

#include <string>

namespace popcorn::core
{
namespace
{
void check(uc_err err, char const* what)
{
  if(err != UC_ERR_OK)
  {
    throw EngineError{std::string{what} + ": " + uc_strerror(err)};
  }
}

auto on_wfi(uc_engine*, void*) -> int
{
  return 0;
}

void require_wfi_hook(uc_engine* engine)
{
  uc_hook    hook = 0;
  auto const err  = uc_hook_add(
      engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(&on_wfi), nullptr, 1, 0, UC_ARM_INS_WFI);
  if(err != UC_ERR_OK)
  {
    throw EngineError{
        std::string{"linked Unicorn has no WFI instruction hook, popcorn needs the pinned dev "
                    "build: "}
        + uc_strerror(err)};
  }
  check(uc_hook_del(engine, hook), "uc_hook_del");
}
}

void engine_closer::operator()(uc_struct* engine) const noexcept
{
  uc_close(engine);
}

auto open_m33() -> engine_handle
{
  uc_engine* raw = nullptr;
  check(
      uc_open(UC_ARCH_ARM, static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS), &raw), "uc_open");
  engine_handle engine{raw};
  check(uc_ctl_set_cpu_model(engine.get(), UC_CPU_ARM_CORTEX_M33), "uc_ctl_set_cpu_model");
  require_wfi_hook(engine.get());
  return engine;
}
}
