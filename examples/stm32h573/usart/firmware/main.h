#ifndef MAIN_H
#define MAIN_H

#include "stm32h5xx_hal.h"

extern UART_HandleTypeDef huart9;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

#endif
