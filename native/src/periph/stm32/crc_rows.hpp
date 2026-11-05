#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the CRC peripherals the model rows serve. */
struct crc_variant
{
  /** @brief The unit has CRC_INIT, CRC_POL and the CR fields POLYSIZE, REV_IN and REV_OUT.
   *         Without them the unit computes CRC-32 over the polynomial 0x04C11DB7 from
   *         0xFFFFFFFF, with no reversal. */
  bool programmable = true;
  /** @brief A CRC_DR write narrower than 32 bits feeds the word its byte lanes repeat, such as
   *         0xB0B0B0B0 for the byte 0xB0, logs once and raises model_error under strict
   *         peripherals. Otherwise a right-aligned narrow write feeds its own width. */
  bool word_input = false;
  /** @brief A byte or half-word access to CRC_IDR is served as is, the CMSIS device header
   *         declaring it a byte. Otherwise a narrow access to it is a forbidden access. */
  bool byte_idr = false;
  /** @brief Manual section that states the access rules, named in the log records. */
  std::string_view access_rule = "RM0456 24.4";
  /** @brief Manual section that leaves even polynomials unsupported, named in the log record. */
  std::string_view polynomial_rule = "RM0456 24.3.3";
};

/** @brief STM32U5 and STM32H5 CRC, RM0456 section 24. */
inline constexpr crc_variant u5_crc{};
/** @brief STM32L1 CRC, RM0038 section 4: DR, IDR and CR.RESET only, word accesses, the CMSIS
 *         `CRC_TypeDef` declaring IDR an `uint8_t`. */
inline constexpr crc_variant l1_crc{
    .programmable = false,
    .word_input   = true,
    .byte_idr     = true,
    .access_rule  = "RM0038 4.4"};
/** @brief STM32L4 CRC, RM0351 section 15: the programmable unit of the STM32U5 with an 8-bit
 *         CRC_IDR, which the CMSIS `CRC_TypeDef` declares an `uint8_t`. */
inline constexpr crc_variant l4_crc{
    .byte_idr        = true,
    .access_rule     = "RM0351 15.4",
    .polynomial_rule = "RM0351 15.3.3"};

/**
 * @brief Factory of the CRC model, RM0456 section 24, RM0038 section 4.
 * @param variant What differs between the CRC of the family the row serves.
 */
[[nodiscard]] auto crc_factory(crc_variant variant) -> model_factory;

/** @brief The CRC model rows. */
[[nodiscard]] auto crc_rows() -> std::span<builtin const>;
}
