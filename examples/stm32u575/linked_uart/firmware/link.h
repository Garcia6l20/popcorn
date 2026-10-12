#ifndef LINK_H
#define LINK_H

#include "stm32u5xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

#define LINK_BAUD      460800u
#define RING_BYTES     256u
#define MESSAGE_BYTES  512u
#define TX_GAP_TICKS   2u
#define MSG_NODE_ID    'N'
#define MSG_NODE_ACK   'n'
#define MSG_BLOCK      'B'
#define MSG_BLOCK_SUM  'b'
#define MSG_POLL       'P'
#define MSG_POLL_REPLY 'p'
#define NODE_ID_BYTES  2u
#define POLL_BYTES     4u

/**
 * @brief One UART of a link: TX by GPDMA, RX by GPDMA into a circular ring, messages cut at
 * IDLE.
 */
struct link
{
  UART_HandleTypeDef uart;
  DMA_HandleTypeDef  tx_dma;
  DMA_HandleTypeDef  rx_dma;
  DMA_NodeTypeDef    rx_node;
  DMA_QListTypeDef   rx_queue;
  uint8_t            ring[RING_BYTES];
  uint16_t           ring_read;
  uint32_t           received;
  uint8_t            partial[MESSAGE_BYTES];
  uint16_t           partial_size;
  bool               partial_overlong;
  uint8_t            message[MESSAGE_BYTES];
  volatile uint16_t  message_size;
  volatile bool      message_ready;
  uint32_t           messages_dropped;
  uint32_t           errors;
  uint8_t            tx[MESSAGE_BYTES];
  volatile bool      tx_busy;
  volatile uint32_t  tx_done_tick;
};

/** @brief Whether `idle` stops refreshing the watchdog, set by a test. */
extern volatile uint32_t starve;

/** @brief Whether the last reset was the watchdog's, read from RCC at boot. */
extern uint32_t reset_by_watchdog;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/**
 * @brief Clocks the core at 160 MHz from HSI16 through PLL1, starts the console on USART1 at
 * 115200 baud and the IWDG at 500 ms, frozen while the core is halted.
 */
void board_init(void);

/**
 * @brief Starts @p link on @p instance, UART4 on GPDMA1 channels 0 and 1 or USART3 on
 * channels 2 and 3, receiving into its ring.
 */
void link_init(struct link* link, USART_TypeDef* instance);

/** @brief Whether @p link may start a message: TX idle for at least TX_GAP_TICKS. */
bool link_can_send(struct link const* link);

/** @brief Sends the first @p size bytes of link->tx by GPDMA. */
void link_send(struct link* link, uint16_t size);

/** @brief Moves the message cut at the last IDLE to @p out, returns its size, 0 for none. */
uint16_t link_take(struct link* link, uint8_t* out);

/** @brief Whether @p link holds a message `link_take` would return. */
bool link_has_message(struct link const* link);

/** @brief Checksum of @p size bytes: the sum of bytes in the low half, the sum of sums high. */
uint32_t block_checksum(uint8_t const* data, uint16_t size);

/**
 * @brief Refreshes the watchdog unless starved, then sleeps until the next interrupt unless
 * @p work reports work pending.
 */
void idle(bool (*work)(void));

/** @brief Called after the links started, the boot stop of both images. */
void links_ready(void);

#endif
