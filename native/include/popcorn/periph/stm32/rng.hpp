#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/** @brief Configuration of the built-in RNG model, RM0456 section 48. */
struct rng_options
{
  /** @brief Seed of the word generator. Every seed is valid, 0 included. */
  std::uint64_t seed = 0;
};

/**
 * @brief Word of the RNG generator at one position, 0 included: the upper 32 bits of the
 *        SplitMix64 finalizer of `seed + (position + 1) * 0x9E3779B97F4A7C15`, modulo 2^64.
 * @param seed Generator seed.
 * @param position Generator position, counted from 0.
 */
[[nodiscard]] constexpr auto rng_word_at(std::uint64_t seed, std::uint64_t position) noexcept
    -> std::uint32_t
{
  auto z = seed + (position + 1) * 0x9E37'79B9'7F4A'7C15u;
  z      = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9u;
  z      = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBu;
  z ^= z >> 31;
  return static_cast<std::uint32_t>(z >> 32);
}

/**
 * @brief Next word the RNG generator delivers from @p position. A 0 word is skipped, RM0456
 *        48.3.5 reading a DR of 0 as a seed error, so the generator never delivers 0.
 * @param seed Generator seed.
 * @param position Generator position, moved past the word delivered and every 0 skipped.
 */
[[nodiscard]] constexpr auto rng_next_word(std::uint64_t seed, std::uint64_t& position) noexcept
    -> std::uint32_t
{
  for(;;)
  {
    if(auto const word = rng_word_at(seed, position++); word != 0)
    {
      return word;
    }
  }
}
}
