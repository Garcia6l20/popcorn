#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

#define SENSOR_ADDRESS          (0x48u << 1)
#define SENSOR_TEMPERATURE      0x00u
#define SENSOR_CONFIG           0x01u
#define SENSOR_CONFIG_VALUE     0x60A0u
#define OVER_TEMPERATURE_COUNTS (30 * 16)

/** @brief Running statistics of the temperature samples, in sensor counts of 1/16 degree. */
struct sensor_result
{
  int32_t  sum;
  int32_t  mean;
  uint32_t samples;
  uint32_t errors;
};

extern struct sensor_result sensor_result;

/** @brief Called after each sample attempt, taken or failed. */
void sample_taken(void);

#endif
