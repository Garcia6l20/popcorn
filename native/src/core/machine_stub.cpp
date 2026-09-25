#include "machine_impl.hpp"

#include <popcorn/core/errors.hpp>

#include <string>

namespace popcorn::core
{
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
