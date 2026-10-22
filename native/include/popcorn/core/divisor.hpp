#pragma once

/**
 * @file
 * @brief Exact division of a 128-bit number by a divisor fixed for many divisions.
 */

#include <bit>
#include <cstdint>

namespace popcorn::core
{
/**
 * @brief A 64-bit divisor with its reciprocal computed once, so each division costs two
 *        multiplications and a few additions instead of a 128-bit library division. The result
 *        is exactly that of `n / d`, never an approximation. The reciprocal is the one of
 *        Moller and Granlund, "Improved division by invariant integers", algorithm 4.
 */
class divisor
{
public:
  /** @brief Divisor 1. */
  constexpr divisor() noexcept : divisor{1}
  {}

  /**
   * @brief Computes the reciprocal, one 128-bit division.
   * @param d The divisor, not 0.
   */
  explicit constexpr divisor(std::uint64_t d) noexcept
      : d_{d}, shift_{static_cast<unsigned>(std::countl_zero(d))}
  {
    auto const normal = d << shift_;
    reciprocal_       = static_cast<std::uint64_t>(
        (~static_cast<unsigned __int128>(0) / normal) - (static_cast<unsigned __int128>(1) << 64));
  }

  /** @brief The divisor. */
  [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t
  {
    return d_;
  }

  /**
   * @brief The low 64 bits of `n / d`, the value `static_cast<std::uint64_t>(n / d)` has.
   * @param n The dividend. Any value, a dividend whose quotient needs more than 64 bits takes
   *        the library division.
   */
  [[nodiscard]] constexpr auto quotient(unsigned __int128 n) const noexcept -> std::uint64_t
  {
    auto const high = static_cast<std::uint64_t>(n >> 64);
    auto const low  = static_cast<std::uint64_t>(n);
    if(high >= d_) [[unlikely]]
    {
      return static_cast<std::uint64_t>(n / d_);
    }
    auto const normal = d_ << shift_;
    auto const top    = shift_ == 0 ? high : (high << shift_) | (low >> (64 - shift_));
    auto const bottom = low << shift_;
    auto const sum    = static_cast<unsigned __int128>(reciprocal_) * top
                      + ((static_cast<unsigned __int128>(top) << 64) | bottom);
    auto       q      = static_cast<std::uint64_t>(sum >> 64) + 1;
    auto       rest   = bottom - q * normal;
    if(rest > static_cast<std::uint64_t>(sum))
    {
      --q;
      rest += normal;
    }
    if(rest >= normal) [[unlikely]]
    {
      ++q;
    }
    return q;
  }

private:
  std::uint64_t d_;
  unsigned      shift_;
  std::uint64_t reciprocal_{};
};
}
