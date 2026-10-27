#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

extern volatile uint32_t ticks;

/** @brief Called once HSI reports ready. */
void hsi_ready(void);

/** @brief Called once PLL1 reports locked. */
void pll_ready(void);

/** @brief Called once PLL1 drives SYSCLK and SystemCoreClock is updated. */
void clock_configured(void);

/** @brief Counts SysTick interrupts, one per millisecond. */
void SysTick_Handler(void);

#endif
