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

auto read_registers(uc_struct* engine, std::span<arch::cpu_register const> registers)
    -> std::vector<std::uint64_t>
{
  auto                       ids = register_ids(registers);
  std::vector<std::uint64_t> values(ids.size());
  auto                       pointers = value_pointers(values);
  check(
      uc_reg_read_batch(engine, ids.data(), pointers.data(), static_cast<int>(ids.size())),
      "reading the registers");
  return values;
}

void write_registers(
    uc_struct*                          engine,
    std::span<arch::cpu_register const> registers,
    std::vector<std::uint64_t>          values) pre(values.size() == registers.size())
{
  auto ids      = register_ids(registers);
  auto pointers = value_pointers(values);
  check(
      uc_reg_write_batch(engine, ids.data(), pointers.data(), static_cast<int>(ids.size())),
      "writing the registers");
}

auto machine::thread_ready() const -> bool
{
  return arch_->thread_mode() and not(arch_->pending() and arch_->deliverable_now());
}

void machine::check_call(arch::call_frame const& frame) const
{
  arch_->check_call(frame);
}

void machine::begin_call(arch::call_frame const& frame)
{
  require_stopped(*this);
  if(in_call())
  {
    throw machine_busy("a call is already in progress");
  }
  saved_call saved{.frame = frame, .registers = read_registers(engine_.get(), arch_->registers())};
  arch_->save(saved.arch_state);
  arch_->prepare_call(frame);
  run_->coverage.has_previous = false;
  saved.frames                = run_->frames.size();
  saved.resume_past           = std::exchange(run_->resume_past, std::nullopt);
  saved.spin_depth            = run_->spin.depth;
  saved.paused_loop           = run_->spin.loop();
  run_->call                  = std::move(saved);
}

auto machine::end_call() -> std::uint64_t
{
  require_stopped(*this);
  auto       saved = *std::exchange(run_->call, std::nullopt);
  auto const value = saved.returned ? arch_->call_result(saved.frame) : std::uint64_t{0};
  write_registers(engine_.get(), arch_->registers(), std::move(saved.registers));
  arch_->load(saved.arch_state);
  if(run_->frames.size() > saved.frames)
  {
    run_->frames.resize(saved.frames);
  }
  run_->refresh_return_watch();
  run_->resume_past           = saved.resume_past;
  run_->spin.depth            = saved.spin_depth;
  run_->spin.loop()           = std::move(saved.paused_loop);
  run_->spin.previous         = {};
  run_->coverage.has_previous = false;
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
