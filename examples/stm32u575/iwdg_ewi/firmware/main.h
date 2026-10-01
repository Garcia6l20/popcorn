#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

extern volatile uint32_t reloads;

/** @brief Called before the first IWDG register write. */
void watchdog_starting(void);

/** @brief Called once the prescaler, reload and early wakeup settings are applied. */
void watchdog_configured(void);

#endif
