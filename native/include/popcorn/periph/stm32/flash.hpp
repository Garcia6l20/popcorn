#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <variant>

namespace popcorn::periph::stm32
{
/** @brief First and last page of a flash area, both included, numbered within their bank. */
struct page_range
{
  /** @brief First page. */
  std::uint32_t first{};
  /** @brief Last page, not below first. */
  std::uint32_t last{};

  /** @brief Member-wise equality. */
  friend auto operator==(page_range const&, page_range const&) -> bool = default;
};

/** @brief A secure watermark area covering its whole bank. */
struct whole_bank
{
  /** @brief Every whole bank equals every other. */
  friend auto operator==(whole_bank const&, whole_bank const&) -> bool = default;
};

/** @brief No secure watermark area in the bank. */
struct no_area
{
  /** @brief Every empty area equals every other. */
  friend auto operator==(no_area const&, no_area const&) -> bool = default;
};

/** @brief Secure watermark area of one bank: the whole bank, a page range or none. */
using watermark = std::variant<whole_bank, page_range, no_area>;

/**
 * @brief User option bytes the part boots with, in fields every STM32 family with TrustZone
 *        shares. The FLASH model of the part encodes them into its option registers and checks
 *        what its family allows.
 */
struct option_bytes
{
  /** @brief Global TrustZone enable. */
  bool tzen{};
  /** @brief Secure area of bank 1, the whole bank as after TZEN goes from 0 to 1. */
  watermark secwm1{whole_bank{}};
  /** @brief Secure area of bank 2, the whole bank as after TZEN goes from 0 to 1. */
  watermark secwm2{whole_bank{}};
  /** @brief Hide-protection area of bank 1, empty for none. It starts at the first page of
   * secwm1. */
  std::optional<page_range> hdp1{};
  /** @brief Hide-protection area of bank 2, empty for none. It starts at the first page of
   * secwm2. */
  std::optional<page_range> hdp2{};
  /** @brief Secure boot address, empty for the family default. */
  std::optional<std::uint32_t> secboot{};
  /** @brief Non-secure boot address, empty for the family default. */
  std::optional<std::uint32_t> nsboot{};
  /** @brief Bank 1 and bank 2 addresses swapped. */
  bool swap_bank{};
  /** @brief Dual-bank organization, empty for the family default. */
  std::optional<bool> dualbank{};

  /** @brief Member-wise equality. */
  friend auto operator==(option_bytes const&, option_bytes const&) -> bool = default;
};

/**
 * @brief Board configuration of the FLASH model, RM0456 section 7: how long BSY stays set for
 *        each operation, and the option bytes. Defaults are the typical values of the STM32U575
 *        datasheet, DS13737 Rev 10, Table 86 "Flash memory characteristics". A test that erases
 *        a whole bank sets a shorter time: 195 ms of BSY polling at 160 MHz is about 31 M
 *        instructions.
 */
struct flash_options
{
  /** @brief tprog, 128-bit programming time in normal mode. */
  std::chrono::nanoseconds quad_word_program = std::chrono::microseconds{118};
  /** @brief tprog in burst mode, 48 us per 128-bit line, times the 8 lines of a BWR burst. */
  std::chrono::nanoseconds burst_program = std::chrono::microseconds{8 * 48};
  /** @brief tERASE, erase time of one 8-Kbyte page, 10 k endurance cycles. */
  std::chrono::nanoseconds page_erase = std::chrono::microseconds{1500};
  /** @brief tME, mass erase time of one bank. */
  std::chrono::nanoseconds bank_erase = std::chrono::milliseconds{195};
  /** @brief tME, mass erase time of both banks. */
  std::chrono::nanoseconds mass_erase = std::chrono::milliseconds{390};
  /** @brief Option bytes the option registers read, empty for the registers of the
   * description, which firmware writes as storage. */
  std::optional<stm32::option_bytes> option_bytes{};
};
}
