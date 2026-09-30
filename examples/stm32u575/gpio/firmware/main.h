#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#include <stdint.h>

#define BUTTON_PORT GPIOC
#define BUTTON_PIN  GPIO_PIN_13
#define LED_PORT    GPIOA
#define LED_PIN     GPIO_PIN_5

extern volatile uint32_t presses;

#endif
