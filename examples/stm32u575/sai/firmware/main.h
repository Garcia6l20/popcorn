#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#define SINE_FRAMES 48u

/** @brief DMA callbacks counted while the sine table plays in a loop. */
struct playback
{
  uint32_t halves;
  uint32_t completes;
};

extern SAI_HandleTypeDef hsai_BlockA1;
extern DMA_HandleTypeDef handle_GPDMA1_Channel0;
extern struct playback   playback;
extern int16_t const     sine[2u * SINE_FRAMES];

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

#endif
