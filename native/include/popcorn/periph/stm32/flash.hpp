#pragma once

#include <chrono>

namespace popcorn::periph::stm32
{
/**
 * @brief Board configuration of the FLASH model, RM0456 section 7: how long BSY stays set for
 *        each operation. Defaults are the typical values of the STM32U575 datasheet, DS13737
 *        Rev 10, Table 86 "Flash memory characteristics". A test that erases a whole bank sets
 *        a shorter time: 195 ms of BSY polling at 160 MHz is about 31 M instructions.
 */
struct flash_options
{
  /** @brief tprog, 128-bit programming time in normal mode. */
  std::chrono::nanoseconds quad_word_program = std::chrono::microseconds{118};
  /** @brief tERASE, erase time of one 8-Kbyte page, 10 k endurance cycles. */
  std::chrono::nanoseconds page_erase = std::chrono::microseconds{1500};
  /** @brief tME, mass erase time of one bank. */
  std::chrono::nanoseconds bank_erase = std::chrono::milliseconds{195};
  /** @brief tME, mass erase time of both banks. */
  std::chrono::nanoseconds mass_erase = std::chrono::milliseconds{390};
};
}
