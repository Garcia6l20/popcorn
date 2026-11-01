#ifndef MAIN_H
#define MAIN_H

#include "stm32l1xx_hal.h"

extern UART_HandleTypeDef huart2;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

#endif
