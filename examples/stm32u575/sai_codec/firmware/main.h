#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#define FRAMES        64u
#define BUFFERS       4u
#define CODEC_ADDRESS 0x1Au
#define CODEC_POWER   0x02u

/** @brief DMA callbacks counted per direction while the codec streams. */
struct stream
{
  uint32_t tx_halves;
  uint32_t tx_completes;
  uint32_t rx_halves;
  uint32_t rx_completes;
};

extern SAI_HandleTypeDef hsai_BlockA1;
extern SAI_HandleTypeDef hsai_BlockB1;
extern I2C_HandleTypeDef hi2c1;
extern DMA_HandleTypeDef handle_GPDMA1_Channel12;
extern DMA_HandleTypeDef handle_GPDMA1_Channel13;
extern struct stream     stream;
extern int16_t           playback[2][FRAMES];
extern int16_t           capture[2][FRAMES];

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called once the codec was told to stop its clock. */
void codec_stopped(void);

#endif
