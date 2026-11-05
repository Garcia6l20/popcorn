#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/rng.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the RNG peripherals the model rows serve. */
struct rng_variant
{
  /** @brief The unit has the configuration block of RM0456 48.7: CR.CONDRST, CONFIGLOCK,
   *         RNG_CONFIG1 to RNG_CONFIG3, NISTC, CLKDIV and ARDIS, NSCR and HTCR, the seed error
   *         auto-reset, the 4-word FIFO in front of the 128-bit conditioning register and the
   *         RM0456 48.5 timing. Without it CR holds IE, RNGEN and CED, DR holds one word, a
   *         word takes word_cycles RNG clock cycles and the first after RNGEN start_cycles. */
  bool configurable = true;
  /** @brief CECS is 1 while clock_ratio x fRNG < fHCLK. */
  std::uint8_t clock_ratio = 32;
  /** @brief RNG clock cycles from RNGEN 1 to the first word, without the configuration block. */
  std::uint8_t start_cycles = 0;
  /** @brief RNG clock cycles from an empty DR to the next word, without the configuration
   *         block. */
  std::uint8_t word_cycles = 0;
};

/** @brief STM32U5 and STM32H5 RNG, RM0456 section 48. */
inline constexpr rng_variant u5_rng{};
/** @brief STM32L4 RNG, RM0351 section 27: CR with IE, RNGEN and CED, a word every 42 RNG clock
 *         cycles, the first 46 cycles after RNGEN, 27.5, and a clock error below fHCLK / 16,
 *         27.3.3. */
inline constexpr rng_variant l4_rng{
    .configurable = false,
    .clock_ratio  = 16,
    .start_cycles = 46,
    .word_cycles  = 42};

/**
 * @brief Factory of the RNG model, RM0456 section 48 and RM0351 section 27.
 * @param options Board configuration, copied into every model made.
 * @param variant What differs between the RNG of the family the row serves.
 */
[[nodiscard]] auto rng_factory(rng_options options, rng_variant variant) -> model_factory;

/** @brief The RNG model rows. */
[[nodiscard]] auto rng_rows() -> std::span<builtin const>;
}
