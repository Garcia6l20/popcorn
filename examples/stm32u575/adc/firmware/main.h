#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

extern volatile uint32_t vrefint_data;
extern volatile uint32_t vref_mv;
extern volatile uint32_t sensor_data;
extern volatile int32_t  temperature_c;
extern volatile uint32_t vrefint_data_12b;

/** @brief Called once ADC1 is calibrated, enabled and reports ADRDY. */
void adc_ready(void);

/** @brief Called once the VREFINT and temperature sensor results are stored. */
void measured(void);

#endif
