#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the SAI peripherals the model rows serve. */
struct sai_variant
{
  /** @brief Name of the block enable field of `xCR1`. */
  std::string_view enable = "SAIAEN";
  /** @brief Whether `xCR1` has OSR, the master clock oversampling ratio. */
  bool oversampling = true;
  /** @brief Whether the SAI has PDMCR and PDMDLY, the PDM interface. */
  bool pdm = true;
  /**
   * @brief Whether MCKDIV above 0 divides the kernel clock by 2 x MCKDIV and NODIV 1 bypasses
   *        it, RM0351 43.3.8. Otherwise MCKDIV divides by MCKDIV, 0 counting as 1, in both
   *        NODIV settings.
   */
  bool doubled_divider = false;
};

/** @brief STM32U5 SAI, RM0456 69.6.2. */
inline constexpr sai_variant u5_sai{};
/** @brief STM32H5 SAI, RM0481 59.6.2. */
inline constexpr sai_variant h5_sai{.enable = "SAIEN"};
/** @brief STM32L4 SAI, RM0351 43.5: GCR to BDR, SAIEN enables a block, MCKDIV doubled. */
inline constexpr sai_variant l4_sai{
    .enable          = "SAIEN",
    .oversampling    = false,
    .pdm             = false,
    .doubled_divider = true};

/**
 * @brief Factory of the SAI model, RM0456 section 69, RM0481 section 59, RM0351 section 43.
 * @param audio Board `audio`, each block's far end by `<instance>_A` and `<instance>_B`, copied,
 *        the endpoints stay shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto sai_factory(decltype(board::audio) const& audio, sai_variant variant)
    -> model_factory;

/** @brief The SAI model rows. */
[[nodiscard]] auto sai_rows() -> std::span<builtin const>;
}
