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

namespace
{
void on_access(uc_struct*, uc_mem_type, std::uint64_t, int, std::int64_t, void*) noexcept
{}
}

void exact_pc_hook::configure(
    uc_struct*                                           engine,
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges)
{
  engine_ = engine;
  ranges_ = std::move(ranges);
}

void exact_pc_hook::acquire()
{
  if(users_ == 0)
  {
    for(auto const& [first, last] : ranges_)
    {
      uc_hook    hook = 0;
      auto const err  = uc_hook_add(
          engine_, &hook, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, reinterpret_cast<void*>(&on_access),
          nullptr, first, last);
      if(err != UC_ERR_OK)
      {
        release_all();
        check(err, "adding the exact pc memory hook");
      }
      hooks_.push_back(hook);
    }
  }
  ++users_;
}

void exact_pc_hook::release() noexcept
{
  if(users_ > 0 and --users_ == 0)
  {
    release_all();
  }
}

void exact_pc_hook::release_all() noexcept
{
  for(auto const hook : hooks_)
  {
    uc_hook_del(engine_, hook);
  }
  hooks_.clear();
}

auto exact_pc_hook::installed() const noexcept -> bool
{
  return not hooks_.empty();
}

auto exact_pc_hook::users() const noexcept -> std::uint32_t
{
  return users_;
}
}
