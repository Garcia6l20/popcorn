#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

extern TIM_HandleTypeDef htim6;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

#endif
