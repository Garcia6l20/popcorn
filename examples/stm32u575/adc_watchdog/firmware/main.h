#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#define SCAN_CHANNELS 4u

/** @brief What the ADC callbacks saw since the scans started. */
struct monitor
{
  uint32_t scans;
  uint32_t alarms;
  uint32_t errors;
  uint16_t latest[SCAN_CHANNELS];
};

extern ADC_HandleTypeDef hadc1;
extern DMA_HandleTypeDef handle_GPDMA1_Channel0;
extern TIM_HandleTypeDef htim15;
extern struct monitor    monitor;
extern uint16_t          samples[2u * SCAN_CHANNELS];

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called once TIM15 counts, the scans start at its next update. */
void scanning(void);

#endif
