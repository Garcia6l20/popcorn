#ifndef MAIN_H
#define MAIN_H

#include "stm32l1xx_hal.h"

#define PAGE_WORDS 64u

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

#endif
