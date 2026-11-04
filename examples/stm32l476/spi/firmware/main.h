#ifndef MAIN_H
#define MAIN_H

#include "stm32l4xx_hal.h"

#include <stdint.h>

#define ACCEL_SAMPLES    8u
#define ACCEL_AXES       3u
#define ACCEL_READ       0x80u
#define ACCEL_INCREMENT  0x40u
#define ACCEL_WHO_AM_I   0x0Fu
#define ACCEL_CTRL_REG1  0x20u
#define ACCEL_OUT_X_L    0x28u
#define ACCEL_CTRL_VALUE 0x57u
#define ACCEL_CS_PORT    GPIOB
#define ACCEL_CS_PIN     GPIO_PIN_6

/** @brief The accelerometer identity and one sample of each axis per millisecond of SysTick time.
 */
struct accel_result
{
  uint8_t  who_am_i;
  int16_t  samples[ACCEL_SAMPLES][ACCEL_AXES];
  uint32_t count;
};

extern SPI_HandleTypeDef   hspi1;
extern struct accel_result accel_result;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called once accel_result is filled. */
void accel_done(void);

#endif
