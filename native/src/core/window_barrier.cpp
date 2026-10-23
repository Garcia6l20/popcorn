#include <popcorn/core/window_barrier.hpp>

#include <linux/futex.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <climits>

namespace popcorn::core
{
namespace
{
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));

auto futex_word(std::atomic<std::uint32_t>& word) noexcept -> std::uint32_t*
{
  return reinterpret_cast<std::uint32_t*>(&word);
}

void futex_wait(std::atomic<std::uint32_t>& word, std::uint32_t expected) noexcept
{
  ::syscall(SYS_futex, futex_word(word), FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
}

void futex_wake_all(std::atomic<std::uint32_t>& word) noexcept
{
  ::syscall(SYS_futex, futex_word(word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}
}

auto usable_cpus() noexcept -> std::size_t
{
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if(::sched_getaffinity(0, sizeof allowed, &allowed) != 0)
  {
    return std::max(1L, ::sysconf(_SC_NPROCESSORS_ONLN));
  }
  return std::max(1, CPU_COUNT(&allowed));
}

window_barrier::window_barrier(std::size_t parties) : parties_{static_cast<std::uint32_t>(parties)}
{}

auto window_barrier::arrive_and_wait(std::function_ref<void() noexcept> complete) -> bool
{
  auto const seen = generation_.load(std::memory_order_acquire);
  if(aborted_.load(std::memory_order_acquire))
  {
    return false;
  }
  if(arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == parties_)
  {
    arrived_.store(0, std::memory_order_relaxed);
    complete();
    generation_.fetch_add(1, std::memory_order_seq_cst);
    if(sleepers_.load(std::memory_order_seq_cst) > 0)
    {
      futex_wake_all(generation_);
    }
    return not aborted_.load(std::memory_order_acquire);
  }
  wait_past(seen);
  return not aborted_.load(std::memory_order_acquire);
}

void window_barrier::wait_past(std::uint32_t seen) noexcept
{
  while(generation_.load(std::memory_order_seq_cst) == seen)
  {
    sleepers_.fetch_add(1, std::memory_order_seq_cst);
    if(generation_.load(std::memory_order_seq_cst) == seen)
    {
      sleeps_.fetch_add(1, std::memory_order_relaxed);
      futex_wait(generation_, seen);
    }
    sleepers_.fetch_sub(1, std::memory_order_seq_cst);
  }
}

void window_barrier::abort() noexcept
{
  aborted_.store(true, std::memory_order_seq_cst);
  generation_.fetch_add(1, std::memory_order_seq_cst);
  futex_wake_all(generation_);
}

auto window_barrier::aborted() const noexcept -> bool
{
  return aborted_.load(std::memory_order_acquire);
}

auto window_barrier::parties() const noexcept -> std::size_t
{
  return parties_;
}

auto window_barrier::sleeps() const noexcept -> std::uint64_t
{
  return sleeps_.load(std::memory_order_relaxed);
}
}
