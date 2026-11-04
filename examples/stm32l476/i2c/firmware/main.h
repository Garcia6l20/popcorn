#ifndef MAIN_H
#define MAIN_H

#include "stm32l4xx_hal.h"

#include <stdint.h>

#define SENSOR_ADDRESS          (0x48u << 1)
#define SENSOR_TEMPERATURE      0x00u
#define SENSOR_CONFIG           0x01u
#define SENSOR_CONFIG_VALUE     0x60A0u
#define SENSOR_TIMEOUT_MS       5u
#define I2C_TIMING_SM_AT_4MHZ   0x00420F13u
#define SAMPLE_MS               10u
#define OVER_TEMPERATURE_COUNTS (30 * 16)

/** @brief Running statistics of the temperature samples, in sensor counts of 1/16 degree. */
struct sensor_result
{
  int32_t  sum;
  int32_t  mean;
  uint32_t samples;
  uint32_t errors;
  uint32_t last_error;
};

extern I2C_HandleTypeDef    hi2c1;
extern struct sensor_result sensor_result;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called after each sample attempt, taken or failed. */
void sample_taken(void);

#endif
