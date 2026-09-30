#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#define MODEM_TIMEOUT_MS 100u

/** @brief How the modem answered one command. */
enum modem_reply : uint32_t
{
  MODEM_OK = 1,
  MODEM_ERROR,
  MODEM_TIMEOUT,
  MODEM_GARBLED,
};

/** @brief The AT session the firmware ran, checked by the host. */
struct modem_result
{
  enum modem_reply at;
  enum modem_reply csq;
  enum modem_reply send;
  int32_t          rssi;
  uint32_t         at_latency_ms;
  uint32_t         done;
};

extern UART_HandleTypeDef  huart2;
extern struct modem_result modem_result;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called once modem_result is filled. */
void modem_done(void);

#endif
