#include "machine_impl.hpp"

#include "engine.hpp"

#include <popcorn/core/errors.hpp>

#include <format>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace popcorn::core
{
namespace
{
auto register_ids(std::span<arch::cpu_register const> registers) -> std::vector<int>
{
  return registers
       | std::views::transform(&arch::cpu_register::engine_id)
       | std::ranges::to<std::vector>();
}

auto value_pointers(std::vector<std::uint64_t>& values) -> std::vector<void*>
{
  return values
       | std::views::transform([](std::uint64_t& value) -> void* { return &value; })
       | std::ranges::to<std::vector>();
}
}

void machine::begin_call(arch::call_frame const& frame)
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a call is already in progress");
  }
  auto       ids = register_ids(arch_->registers());
  saved_call saved{.frame = frame, .registers = std::vector<std::uint64_t>(ids.size())};
  auto       pointers = value_pointers(saved.registers);
  check(
      uc_reg_read_batch(engine_.get(), ids.data(), pointers.data(), static_cast<int>(ids.size())),
      "saving the caller's registers");
  arch_->save(saved.arch_state);
  arch_->prepare_call(frame);
  saved.frames      = run_->frames.size();
  saved.resume_past = std::exchange(run_->resume_past, std::nullopt);
  run_->call        = std::move(saved);
}

auto machine::end_call() -> std::uint64_t
{
  require_stopped(*this);
  auto       saved    = *std::exchange(run_->call, std::nullopt);
  auto const value    = saved.returned ? arch_->call_result(saved.frame) : std::uint64_t{0};
  auto       ids      = register_ids(arch_->registers());
  auto       pointers = value_pointers(saved.registers);
  check(
      uc_reg_write_batch(engine_.get(), ids.data(), pointers.data(), static_cast<int>(ids.size())),
      "restoring the caller's registers");
  arch_->load(saved.arch_state);
  if(run_->frames.size() > saved.frames)
  {
    run_->frames.resize(saved.frames);
  }
  run_->refresh_return_watch();
  run_->resume_past = saved.resume_past;
  return value;
}

auto machine::in_call() const noexcept -> bool
{
  return run_->call.has_value();
}

auto machine::call(arch::call_frame const& frame, icount budget) -> call_result
{
  begin_call(frame);
  slice_request request{.budget_left = budget};
  try
  {
    for(;;)
    {
      auto const slice = run_slice(request);
      if(slice.stopped)
      {
        auto const stopped = *slice.stopped;
        return call_result{.value = end_call(), .stopped = stopped};
      }
      request.budget_left = icount{request.budget_left->value - slice.executed.value};
    }
  }
  catch(...)
  {
    if(in_call())
    {
      std::ignore = end_call();
    }
    throw;
  }
}
}
