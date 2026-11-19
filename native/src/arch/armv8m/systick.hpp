#pragma once

#include "state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace popcorn::arch::armv8m::systick
{
/** @brief SYST_CSR.ENABLE. */
inline constexpr std::uint32_t enable = 1u << 0;
/** @brief SYST_CSR.TICKINT. */
inline constexpr std::uint32_t tickint = 1u << 1;
/** @brief SYST_CSR.CLKSOURCE, the processor clock when set. */
inline constexpr std::uint32_t clksource = 1u << 2;
/** @brief SYST_CSR.COUNTFLAG. */
inline constexpr std::uint32_t countflag_bit = 1u << 16;

/** @brief Whether the counter runs and raises SysTick at each crossing. */
[[nodiscard]] constexpr auto interrupting(systick_state const& s) noexcept -> bool
{
  return (s.csr & (enable | tickint)) == (enable | tickint);
}

/**
 * @brief Core cycles from the system reset at instruction count @p now, 0 before it.
 * @param now Instruction count.
 * @param cpi Core cycles per instruction, taken as constant since the reset.
 * @param origin Instruction count of the last system reset.
 */
[[nodiscard]] inline auto cycles_since(std::uint64_t now, double cpi, std::uint64_t origin) noexcept
    -> std::uint64_t
{
  if(now <= origin)
  {
    return 0;
  }
  if(cpi == 1.0)
  {
    return now - origin;
  }
  return static_cast<std::uint64_t>(std::floor(static_cast<long double>(now - origin) * cpi));
}

/**
 * @brief Ticks since the anchor at instruction count @p now. The processor clock counts cycles
 *        from the anchor. The external reference counts the edges of a divider that runs free
 *        from the system reset, one every 8 cycles at cycle counts that are multiples of 8, so
 *        the first edge after the anchor falls 1 to 8 cycles later. The offset of that grid
 *        against the first instruction after the reset is 0, inferred.
 * @param s Counter state.
 * @param now Instruction count, at or after the anchor.
 * @param cpi Core cycles per instruction.
 * @param origin Instruction count of the last system reset.
 */
[[nodiscard]] inline auto
    ticks(systick_state const& s, std::uint64_t now, double cpi, std::uint64_t origin) noexcept
    -> std::uint64_t
{
  if((s.csr & clksource) != 0)
  {
    auto const elapsed = now - s.anchor;
    if(cpi == 1.0)
    {
      return elapsed;
    }
    return static_cast<std::uint64_t>(std::floor(static_cast<long double>(elapsed) * cpi));
  }
  auto const edges = cycles_since(now, cpi, origin) / 8;
  auto const base  = cycles_since(s.anchor, cpi, origin) / 8;
  return edges > base ? edges - base : 0;
}

/** @brief Tick from which the counter counts down from start_value(): 1 after a latched reload. */
[[nodiscard]] constexpr auto start_tick(systick_state const& s) noexcept -> std::uint64_t
{
  return s.reload ? 1 : 0;
}

/** @brief Counter value at start_tick(). */
[[nodiscard]] constexpr auto start_value(systick_state const& s) noexcept -> std::uint32_t
{
  return s.reload.value_or(s.anchor_value);
}

/**
 * @brief Counter value @p t ticks after the anchor, the frozen value while disabled. Before
 *        start_tick() it is start_value(): the access came after the reload edge.
 */
[[nodiscard]] constexpr auto value_at(systick_state const& s, std::uint64_t t) noexcept
    -> std::uint32_t
{
  if((s.csr & enable) == 0)
  {
    return s.anchor_value;
  }
  if(t < start_tick(s))
  {
    return start_value(s);
  }
  auto const u     = t - start_tick(s);
  auto const start = start_value(s);
  if(u <= start)
  {
    return static_cast<std::uint32_t>(start - u);
  }
  if(s.rvr == 0)
  {
    return 0;
  }
  auto const period = std::uint64_t{s.rvr} + 1;
  return static_cast<std::uint32_t>(s.rvr - (u - start - 1) % period);
}

/**
 * @brief Tick of crossing @p k, the k-th 1 to 0 transition after the anchor, counted from 1.
 * @return Empty when the counter never reaches that crossing: RVR 0 has at most one.
 */
[[nodiscard]] constexpr auto crossing(systick_state const& s, std::uint64_t k) noexcept
    -> std::optional<std::uint64_t>
{
  if(k == 0)
  {
    return std::nullopt;
  }
  auto const start = std::uint64_t{start_value(s)};
  if(s.rvr == 0)
  {
    return k == 1 and start > 0 ? std::optional{start_tick(s) + start} : std::nullopt;
  }
  auto const period = std::uint64_t{s.rvr} + 1;
  auto const first  = start > 0 ? start : period;
  return start_tick(s) + first + (k - 1) * period;
}

/**
 * @brief Whether an access at tick @p t reaches the counter after its reload edge while the
 *        counter is still at 0 from the anchor. On the processor clock that edge is the next
 *        cycle, so any later access comes after it, even within the block that set the counter
 *        to 0.
 */
[[nodiscard]] constexpr auto reload_passed(systick_state const& s, std::uint64_t t) noexcept -> bool
{
  auto const on_processor_clock = (s.csr & (enable | clksource)) == (enable | clksource);
  auto const at_anchored_zero   = not s.reload and s.anchor_value == 0 and t == 0;
  return on_processor_clock and at_anchored_zero;
}

/** @brief Crossings at or before tick @p t. */
[[nodiscard]] constexpr auto crossings_through(systick_state const& s, std::uint64_t t) noexcept
    -> std::uint64_t
{
  auto const first = crossing(s, 1);
  if(not first or t < *first)
  {
    return 0;
  }
  if(s.rvr == 0)
  {
    return 1;
  }
  return (t - *first) / (std::uint64_t{s.rvr} + 1) + 1;
}

/** @brief COUNTFLAG at tick @p t: carried over, or a crossing since the last clear. */
[[nodiscard]] constexpr auto countflag(systick_state const& s, std::uint64_t t) noexcept -> bool
{
  return s.flag
      or ((s.csr & enable) != 0 and crossings_through(s, t) > crossings_through(s, s.flag_since));
}

/**
 * @brief First instruction count at which the counter has reached tick @p z: the smallest @c n
 *        with `ticks(n) >= z`, exact also at a non-integer @p cpi.
 */
[[nodiscard]] inline auto
    expiry(systick_state const& s, std::uint64_t z, double cpi, std::uint64_t origin) noexcept
    -> std::uint64_t
{
  auto const external      = (s.csr & clksource) == 0;
  auto const base          = external ? origin : s.anchor;
  auto const floor_at      = external ? std::max(s.anchor, origin) : s.anchor;
  auto const cycles        = external ? (cycles_since(s.anchor, cpi, origin) / 8 + z) * 8 : z;
  auto const reaches_first = [&](std::uint64_t n) {
    return z == 0 or (ticks(s, n, cpi, origin) >= z and ticks(s, n - 1, cpi, origin) < z);
  };
  if(cpi == 1.0)
  {
    auto const exact = base + cycles;
    contract_assert(reaches_first(exact));
    return exact;
  }
  auto n = base + static_cast<std::uint64_t>(std::ceil(static_cast<long double>(cycles) / cpi));
  for(auto step = 0; step < 2 and ticks(s, n, cpi, origin) < z; ++step)
  {
    ++n;
  }
  for(auto step = 0; step < 2 and n > floor_at and ticks(s, n - 1, cpi, origin) >= z; ++step)
  {
    --n;
  }
  contract_assert(reaches_first(n));
  return n;
}
}
