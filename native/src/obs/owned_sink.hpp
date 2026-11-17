#pragma once

#include <popcorn/core/trace.hpp>

#include <memory>
#include <span>
#include <utility>

namespace popcorn::obs
{
/**
 * @brief A trace sink calling @p target's sink() and sharing its ownership, so @p target lives
 *        while the machine holds the sink: past a handle destroyed inside a slice, whose removal
 *        waits for the slice end.
 * @param target Object whose sink() returns a core::trace_sink valid while it lives.
 */
template <typename Target> auto owned_sink(std::shared_ptr<Target> target) -> core::trace_sink
{
  auto fed = target->sink();
  return [fed = std::move(fed), owner = std::move(target)](
             std::span<core::trace_record const> records) mutable { fed(records); };
}
}
