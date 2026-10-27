#ifndef MAIN_H
#define MAIN_H

#include "stm32h5xx_hal.h"

#include <stdint.h>

#define FLASH_PAGE_ADDRESS 0x001000u
#define FLASH_PAGE_BYTES   16u

/** @brief What the flash task read and wrote, checked by the host. */
struct flash_result
{
  uint8_t  jedec[3];
  uint8_t  readback[FLASH_PAGE_BYTES];
  uint32_t status_polls;
  uint32_t done;
};

extern SPI_HandleTypeDef   hspi4;
extern TIM_HandleTypeDef   htim6;
extern struct flash_result flash_result;

/**
 * @brief Exchange @p size bytes with the device on SPI4 through GPDMA1, from a task.
 *
 * Arms channel 0 for the transmit request and channel 1 for the receive request, programs TSIZE
 * and the DMA requests, then sets SPE and starts the transfer. The task sleeps on a notification
 * from the receive channel's transfer complete interrupt.
 */
void spi_exchange(uint8_t const* out, uint8_t* in, uint32_t size);

/** @brief Called once the page program instruction left the SPI. */
void flash_page_program_sent(void);

/** @brief Called once the status register shows the page program finished. */
void flash_page_program_done(void);

/** @brief Called when the flash task has filled flash_result. */
void flash_task_done(void);

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

#endif
