#include "machine.hpp"

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>

#include <chrono>
#include <utility>

namespace popcorn::bind
{
namespace
{
auto record(stop const& stopped) -> stop_record
{
  stop_record out{
      .reason       = stopped.reason,
      .where        = owned(stopped.where),
      .instructions = stopped.instructions.value,
      .time_ps      = stopped.time.ps,
      .fault        = std::nullopt,
      .detail       = stopped.detail,
  };
  if(stopped.fault)
  {
    out.fault = fault_record{
        .kind          = stopped.fault->kind,
        .fault_address = stopped.fault->fault_address,
        .status        = stopped.fault->status,
        .detail        = stopped.fault->detail,
    };
  }
  return out;
}
}

auto machine_handle::run_slice(
    slice_args const&                           args,
    std::optional<std::function<bool()>> const& stop_when) -> slice_outcome
{
  core::slice_request request{
      .max_instructions   = icount{args.max_instructions},
      .wall_budget        = std::chrono::nanoseconds{args.wall_budget_ns},
      .until              = args.until,
      .budget_left        = std::nullopt,
      .exact_instructions = std::nullopt,
      .until_time         = std::nullopt,
      .stop_when          = stop_when.value_or(std::function<bool()>{}),
  };
  if(args.budget_left)
  {
    request.budget_left = icount{*args.budget_left};
  }
  if(args.exact_instructions)
  {
    request.exact_instructions = icount{*args.exact_instructions};
  }
  if(args.until_time_ps)
  {
    request.until_time = vtime{*args.until_time_ps};
  }
  core::slice_result result;
  {
    reflex::py::nb::gil_scoped_release unlocked;
    result = machine_->run_slice(request);
  }
  slice_outcome out{.stopped = std::nullopt, .executed = result.executed.value};
  if(result.stopped)
  {
    out.stopped = record(*result.stopped);
  }
  return out;
}

auto machine_handle::instructions() const -> std::uint64_t
{
  return machine_->virtual_clock().now().value;
}

auto machine_handle::time_ps() const -> std::uint64_t
{
  return machine_->virtual_clock().time().ps;
}

auto machine_handle::pc() const -> address
{
  return machine_->regs().pc();
}
}
