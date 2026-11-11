#pragma once

#include "row_of.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief NVIC line of the classic EXTI and the EXTI lines first to last that raise it. */
struct exti_line_group
{
  /** @brief First line. */
  std::uint8_t first;
  /** @brief Last line, included. */
  std::uint8_t last;
  /** @brief NVIC line, the IRQ number in the description. The CMSIS device header and the SVD
   *         name some of these lines differently, the number is the same in both. */
  std::uint16_t irq;
};

/** @brief One register bank of the classic EXTI: `IMR<suffix>` to `PR<suffix>` and its lines. */
struct exti_bank
{
  /** @brief Suffix of the bank's register names, empty on a single-bank EXTI. */
  std::string_view suffix;
  /** @brief Line of bit 0. */
  std::uint8_t first;
  /** @brief Lines, bits 0 to lines - 1 of `IMR<suffix>`. */
  std::uint8_t lines;
  /** @brief Direct lines by bit: an IMR bit only, no RTSR, FTSR, SWIER or PR bit. */
  std::uint32_t direct;
};

/** @brief Differences between the classic EXTI controllers the model rows serve. */
struct exti_l1l4_variant
{
  /** @brief Register banks, one or two. */
  std::span<exti_bank const> banks;
  /** @brief NVIC lines the EXTI raises. A line in no group sets its pending bit and raises
   *         nothing. Groups of one NVIC line merge. */
  std::span<exti_line_group const> irqs;
};

/** @brief STM32L1, RM0038 Tables 49 to 51 by NVIC line: EXTI0 to EXTI4 on 6 to 10 one line each,
 *         EXTI9_5 on 23, EXTI15_10 on 40, line 17 RTC_Alarm on 41, line 19 the RTC tamper and
 *         timestamp line on 2, line 20 RTC_WKUP on 3, lines 21 and 22 the comparator wakeup line
 *         on 22, RM0038 20.5, line 18 USB_FS_WKUP on 42, RM0038 24.3.1. PVD and COMP_ACQ, lines
 *         16 and 23, belong to PWR and COMP in the description. */
inline constexpr auto l1_exti_irqs = std::to_array<exti_line_group>({
    {0,  0,  6 },
    {1,  1,  7 },
    {2,  2,  8 },
    {3,  3,  9 },
    {4,  4,  10},
    {5,  9,  23},
    {10, 15, 40},
    {17, 17, 41},
    {18, 18, 42},
    {19, 19, 2 },
    {20, 20, 3 },
    {21, 22, 22},
});
/** @brief STM32L1 Cat.1 and Cat.2 bank, RM0038 10.2: 23 lines. */
inline constexpr auto l1_exti_banks_23 = std::to_array<exti_bank>({
    {"", 0, 23, 0},
});
/** @brief STM32L1 Cat.3 to Cat.6 bank, RM0038 10.2: 24 lines. */
inline constexpr auto l1_exti_banks_24 = std::to_array<exti_bank>({
    {"", 0, 24, 0},
});
/** @brief STM32L1 Cat.1 and Cat.2, `STM32L1_exti_23`, RM0038 10.2: 23 lines. */
inline constexpr exti_l1l4_variant l1_exti_23{.banks = l1_exti_banks_23, .irqs = l1_exti_irqs};
/** @brief STM32L1 Cat.3 to Cat.6, RM0038 10.2: 24 lines. */
inline constexpr exti_l1l4_variant l1_exti_24{.banks = l1_exti_banks_24, .irqs = l1_exti_irqs};

/** @brief STM32L47x/L48x, RM0351 14.5 and Table 62: lines 0 to 31 in bank 1, 32 to 39 in bank 2,
 *         direct lines 17, 23 to 34 and 39, IMR1 reset 0xFF820000 and IMR2 0x00000087. */
inline constexpr auto l4_exti_banks = std::to_array<exti_bank>({
    {"1", 0,  32, 0xFF82'0000},
    {"2", 32, 8,  0x0000'0087},
});
/** @brief STM32L47x/L48x, RM0351 Table 61 by NVIC line: EXTI0 to EXTI4 on 6 to 10 one line each,
 *         EXTI9_5 on 23, EXTI15_10 on 40, line 16 PVD and lines 35 to 38 PVM1 to PVM4 on PVD_PVM 1,
 *         line 18 RTC alarm on 41, line 19 RTC tamper and timestamp on 2, line 20 RTC wakeup on 3,
 *         lines 21 and 22 COMP1 and COMP2 on 64. The direct lines raise no NVIC line. */
inline constexpr auto l4_exti_irqs = std::to_array<exti_line_group>({
    {0,  0,  6 },
    {1,  1,  7 },
    {2,  2,  8 },
    {3,  3,  9 },
    {4,  4,  10},
    {5,  9,  23},
    {10, 15, 40},
    {16, 16, 1 },
    {35, 38, 1 },
    {18, 18, 41},
    {19, 19, 2 },
    {20, 20, 3 },
    {21, 22, 64},
});
/** @brief STM32L47x/L48x, RM0351 section 14. */
inline constexpr exti_l1l4_variant l4_exti{.banks = l4_exti_banks, .irqs = l4_exti_irqs};

/** @brief Factory of the EXTI model, RM0456 section 23. */
[[nodiscard]] auto exti_factory() -> model_factory;

/**
 * @brief Factory of the classic EXTI model, RM0038 section 10.2.
 * @param variant Lines and NVIC lines of the family the row serves.
 */
[[nodiscard]] auto exti_l1l4_factory(exti_l1l4_variant variant) -> model_factory;

/** @brief The EXTI model rows. */
[[nodiscard]] auto exti_rows() -> std::span<builtin const>;
}
