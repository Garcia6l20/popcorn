#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#define RING      128u
#define COMPLETES 4u

/** @brief MDF callbacks counted while the microphones stream. */
struct capture
{
  uint32_t halves;
  uint32_t completes;
  uint32_t errors;
  uint32_t error_codes;
};

extern MDF_HandleTypeDef mdf_filter0;
extern MDF_HandleTypeDef mdf_filter1;
extern DMA_HandleTypeDef handle_GPDMA1_Channel0;
extern struct capture    capture;
extern int16_t           ring[RING];

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called once both filters stopped after COMPLETES ring passes. */
void captured(void);

#endif
