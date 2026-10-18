#include <popcorn/core/trace.hpp>

#include <limits>

namespace popcorn::core
{
trace_ring::trace_ring(std::size_t capacity) : slots_(capacity)
{}

trace_ring::~trace_ring() = default;

void trace_ring::push(trace_record const& stamped)
{
  if(size_ == slots_.size())
  {
    trace_record const& dropped = slots_[next_];
    origin_ = dropped.kind == trace_kind::time
                ? icount{dropped.payload[0] | std::uint64_t{dropped.payload[1]} << 32}
                : icount{origin_.value + dropped.dt};
  }
  else
  {
    ++size_;
  }
  slots_[next_] = stamped;
  next_         = (next_ + 1) % slots_.size();
  ++appended_;
}

void trace_ring::push_time(icount now)
{
  push(
      trace_record{
          .kind = trace_kind::time,
          .payload =
              {static_cast<std::uint32_t>(now.value), static_cast<std::uint32_t>(now.value >> 32),
                        0},
  });
  last_ = now;
}

void trace_ring::append(trace_record record, icount now)
{
  if(now < last_ or now.value - last_.value > std::numeric_limits<std::uint32_t>::max())
  {
    push_time(now);
  }
  record.dt = static_cast<std::uint32_t>(now.value - last_.value);
  push(record);
  last_ = now;
}

void trace_ring::rebase(icount now)
{
  push_time(now);
}

auto trace_ring::size() const noexcept -> std::size_t
{
  return size_;
}

auto trace_ring::capacity() const noexcept -> std::size_t
{
  return slots_.size();
}

auto trace_ring::appended() const noexcept -> std::uint64_t
{
  return appended_;
}

auto trace_ring::origin() const noexcept -> icount
{
  return origin_;
}

auto trace_ring::records() const -> std::vector<trace_record>
{
  std::vector<trace_record> ordered;
  ordered.reserve(size_);
  std::size_t const first = (next_ + slots_.size() - size_) % slots_.size();
  for(std::size_t index = 0; index < size_; ++index)
  {
    ordered.push_back(slots_[(first + index) % slots_.size()]);
  }
  return ordered;
}

void trace_ring::clear(icount now) noexcept
{
  next_     = 0;
  size_     = 0;
  appended_ = 0;
  origin_   = now;
  last_     = now;
}

void trace_ring::shift_call_symbols(std::uint32_t from, std::uint32_t by) noexcept
{
  std::size_t const first = (next_ + slots_.size() - size_) % slots_.size();
  for(std::size_t index = 0; index < size_; ++index)
  {
    auto& record = slots_[(first + index) % slots_.size()];
    if(record.kind == trace_kind::call and record.aux != trace_id_overflow and record.aux >= from)
    {
      record.aux = trace_id(std::uint32_t{record.aux} + by);
    }
  }
}
}
