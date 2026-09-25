#include "engine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include <format>
#include <string>

namespace popcorn::core
{
void engine_close::operator()(uc_struct* engine) const noexcept
{
  uc_close(engine);
}

void machine::engine_closer::operator()(uc_struct* engine) const noexcept
{
  uc_close(engine);
}

void check(uc_err err, std::string_view what)
{
  if(err != UC_ERR_OK)
  {
    throw emulation_error(std::format("{}: {}", what, uc_strerror(err)));
  }
}

auto open_engine(arch::engine_spec const& spec) -> engine_handle
{
  auto const named = std::format(
      "engine arch {} mode {:#x} cpu model {}", spec.arch, spec.mode,
      spec.cpu_model.transform([](int model) { return std::to_string(model); })
          .value_or("default"));
  uc_engine* raw = nullptr;
  check(uc_open(static_cast<uc_arch>(spec.arch), static_cast<uc_mode>(spec.mode), &raw), named);
  engine_handle engine{raw};
  if(spec.cpu_model)
  {
    check(uc_ctl_set_cpu_model(engine.get(), *spec.cpu_model), named);
  }
  return engine;
}
}
