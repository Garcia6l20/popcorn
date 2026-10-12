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
using clock_type = std::chrono::steady_clock;

constexpr int pauses_per_clock_read = 64;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));

inline void cpu_pause() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

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

auto default_wait_policy(std::size_t parties, wait_policy base) noexcept -> wait_policy
{
  if(parties > usable_cpus())
  {
    return wait_policy{.spin = std::chrono::nanoseconds{0}};
  }
  return base;
}

window_barrier::window_barrier(std::size_t parties, wait_policy policy)
    : parties_{static_cast<std::uint32_t>(parties)}, spin_ns_{policy.spin.count()}
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
  auto const spin = std::chrono::nanoseconds{spin_ns_.load(std::memory_order_relaxed)};
  if(spin.count() > 0)
  {
    auto const deadline = clock_type::now() + spin;
    for(;;)
    {
      for(int i = 0; i < pauses_per_clock_read; ++i)
      {
        if(generation_.load(std::memory_order_acquire) != seen)
        {
          return;
        }
        cpu_pause();
      }
      if(clock_type::now() >= deadline)
      {
        break;
      }
    }
  }
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

void window_barrier::set_policy(wait_policy policy) noexcept
{
  spin_ns_.store(policy.spin.count(), std::memory_order_relaxed);
}

auto window_barrier::policy() const noexcept -> wait_policy
{
  return wait_policy{.spin = std::chrono::nanoseconds{spin_ns_.load(std::memory_order_relaxed)}};
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
