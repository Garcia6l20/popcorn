#include "main.h"

#include <string.h>

#define REPLY_BYTES 64u

UART_HandleTypeDef  huart2;
struct modem_result modem_result;

static uint8_t           reply[REPLY_BYTES + 1];
static volatile uint16_t received;

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void modem_done(void)
{}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t size)
{
  if(huart->Instance == USART2)
  {
    received = size;
  }
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance                    = USART2;
  huart2.Init.BaudRate               = 115200;
  huart2.Init.WordLength             = UART_WORDLENGTH_8B;
  huart2.Init.StopBits               = UART_STOPBITS_1;
  huart2.Init.Parity                 = UART_PARITY_NONE;
  huart2.Init.Mode                   = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling           = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if(HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

static bool wait_reply(uint32_t since)
{
  __disable_irq();
  while(received == 0 && HAL_GetTick() - since < MODEM_TIMEOUT_MS)
  {
    __WFI();
    __enable_irq();
    __disable_irq();
  }
  __enable_irq();
  return received != 0;
}

static enum modem_reply command(char const* text, uint32_t* latency_ms)
{
  received = 0;
  memset(reply, 0, sizeof reply);
  if(HAL_UARTEx_ReceiveToIdle_IT(&huart2, reply, REPLY_BYTES) != HAL_OK)
  {
    Error_Handler();
  }
  if(HAL_UART_Transmit(&huart2, (uint8_t const*)text, (uint16_t)strlen(text), HAL_MAX_DELAY)
     != HAL_OK)
  {
    Error_Handler();
  }
  uint32_t const sent = HAL_GetTick();
  if(!wait_reply(sent))
  {
    HAL_UART_AbortReceive_IT(&huart2);
    return MODEM_TIMEOUT;
  }
  if(latency_ms != nullptr)
  {
    *latency_ms = HAL_GetTick() - sent;
  }
  if(strstr((char const*)reply, "\r\nOK\r\n") != nullptr
     || strncmp((char const*)reply, "OK\r\n", 4) == 0)
  {
    return MODEM_OK;
  }
  if(strstr((char const*)reply, "ERROR\r\n") != nullptr)
  {
    return MODEM_ERROR;
  }
  return MODEM_GARBLED;
}

static int32_t signal_quality(void)
{
  char const* at = strstr((char const*)reply, "+CSQ: ");
  if(at == nullptr)
  {
    return -1;
  }
  at += strlen("+CSQ: ");
  int32_t rssi = 0;
  while(*at >= '0' && *at <= '9')
  {
    rssi = rssi * 10 + (*at++ - '0');
  }
  return *at == ',' ? rssi : -1;
}

int main(void)
{
  HAL_Init();
  MX_USART2_UART_Init();
  modem_result.at   = command("AT\r\n", &modem_result.at_latency_ms);
  modem_result.csq  = command("AT+CSQ\r\n", nullptr);
  modem_result.rssi = modem_result.csq == MODEM_OK ? signal_quality() : -1;
  modem_result.send = command("AT+SEND=hello\r\n", nullptr);
  modem_result.done = 1;
  modem_done();
  for(;;)
  {
    __WFI();
  }
}
