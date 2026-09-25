#include "machine_impl.hpp"

#include <popcorn/core/errors.hpp>

#include <string>

namespace popcorn::core
{
auto machine::snapshot() const -> core::snapshot
{
  require_stopped(*this);
  throw not_implemented("machine::snapshot");
}

void machine::restore(core::snapshot const&)
{
  require_stopped(*this);
  throw not_implemented("machine::restore");
}

auto machine::add_intercept(address, intercept_handler, intercept_mode) -> intercept_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_intercept");
}

auto machine::add_return_hook(
    address,
    std::vector<arch::value_kind>,
    arch::value_kind,
    return_handler) -> intercept_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_return_hook");
}

auto machine::add_watch(address, std::uint32_t, watch_kind, watch_handler) -> watch_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_watch");
}

auto machine::add_trace_sink(trace_kinds, trace_sink, std::vector<address_range>) -> hook_handle
{
  require_stopped(*this);
  throw not_implemented("machine::add_trace_sink");
}

void machine::enable_coverage(std::vector<address_range>)
{
  require_stopped(*this);
  throw not_implemented("machine::enable_coverage");
}

void machine::disable_coverage()
{
  require_stopped(*this);
  throw not_implemented("machine::disable_coverage");
}

auto machine::coverage_enabled() const noexcept -> bool
{
  return false;
}

auto machine::coverage() const -> coverage_data
{
  require_view_access();
  throw not_implemented("machine::coverage");
}

void machine::clear_coverage()
{
  require_stopped(*this);
  throw not_implemented("machine::clear_coverage");
}
}
