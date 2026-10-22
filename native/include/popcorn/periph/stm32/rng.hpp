#pragma once

#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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

/** @brief Most words an RNG word queue holds. */
inline constexpr std::size_t rng_max_queued = std::size_t{1} << 16;

/**
 * @brief The word source of the RNG model as a test sees it, implemented by the built-in RNG
 *        model. Reach it with `dynamic_cast<entropy_source*>(machine.model("RNG"))`. Every value
 *        is board state: kept across every reset kind and restored by a snapshot. A conditioning
 *        round or init takes its four words, in order, from the stuck word while one is set,
 *        else from the queue, else from the generator.
 */
class POPCORN_API entropy_source
{
public:
  /** @brief Creates the interface. */
  entropy_source() = default;
  /** @brief Not copyable. */
  entropy_source(entropy_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(entropy_source const&) -> entropy_source& = delete;
  /** @brief Not movable. */
  entropy_source(entropy_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(entropy_source&&) -> entropy_source& = delete;

  /** @brief Seed of the generator, rng_options::seed. */
  [[nodiscard]] virtual auto seed() const -> std::uint64_t = 0;

  /**
   * @brief Generator position: the generator words produced since the machine was built, zeros
   *        skipped included. Queued and stuck words do not move it.
   */
  [[nodiscard]] virtual auto drawn() const -> std::uint64_t = 0;

  /** @brief Queued words not yet produced. */
  [[nodiscard]] virtual auto queued() const -> std::size_t = 0;

  /**
   * @brief Appends words served before the generator, in order, 0 included.
   * @param words Words to append.
   * @throws config_error when the queue would hold more than rng_max_queued words, before any
   *         change.
   */
  virtual void queue(std::span<std::uint32_t const> words) = 0;

  /** @brief The word every new word takes, empty when none is set. */
  [[nodiscard]] virtual auto stuck() const -> std::optional<std::uint32_t> = 0;

  /**
   * @brief Sets or clears the stuck word. Words already in the FIFO and the conditioning
   *        register keep their values.
   * @param word The word every new word takes, empty to produce from the queue and generator.
   */
  virtual void set_stuck(std::optional<std::uint32_t> word) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~entropy_source();
};
}
