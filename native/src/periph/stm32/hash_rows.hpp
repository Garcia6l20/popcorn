#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the STM32H5 HASH processors the model rows serve, RM0481 40.3.
 *         A code outside the variant's set reads 0000 after a CR write and the hash runs SHA-1,
 *         as read back on an STM32H573. */
struct hash_variant
{
  /** @brief ALGO also keeps the SHA-3 and SHAKE codes 0100 to 1001. */
  bool sha3 = false;
};

/** @brief STM32H52x, H53x, H56x and H57x, SHA-1 and SHA-2 only, RM0481 Table 397: ALGO keeps
 *         0000, 0010, 0011 and 1100 to 1111. */
inline constexpr hash_variant hash_h5_sha2{};
/** @brief STM32H543, H553, H5E and H5F, SHA-3 and SHAKE too, RM0481 Table 397, RM0517 46.2. */
inline constexpr hash_variant hash_h5_sha3{.sha3 = true};

/** @brief Differences between the U5-style HASH processors the model rows serve. */
struct hash_u5_variant
{
  /** @brief ALGO 01 selects MD5, RM0456 section 51, else it is reserved and stores 00. */
  bool md5 = true;
};

/** @brief STM32U5, RM0456 section 51. */
inline constexpr hash_u5_variant hash_u5{};
/** @brief STM32H503, RM0492 25.6 HASH_CR: ALGO[1:0] has 00 SHA-1, 10 SHA-224 and 11 SHA-256, 01 is
 *         reserved. */
inline constexpr hash_u5_variant hash_h503{.md5 = false};

/** @brief Factory of the HASH model, RM0456 section 51, and RM0492 section 25 on the STM32H503. */
[[nodiscard]] auto hash_factory(hash_u5_variant variant) -> model_factory;

/**
 * @brief Factory of the STM32H5 HASH model, RM0481 section 40, SHA-2, and SHA-3 and SHAKE where
 *        @p variant has them.
 */
[[nodiscard]] auto hash_h5_factory(hash_variant variant) -> model_factory;

/** @brief The HASH model rows. */
[[nodiscard]] auto hash_rows() -> std::span<builtin const>;
}
