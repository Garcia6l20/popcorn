#include "main.h"

#include <stdint.h>
#include <stdio.h>

#define LINE_BYTES 64u

UART_HandleTypeDef huart2;

static char              stdout_buffer[128];
static uint8_t           line[LINE_BYTES];
static volatile uint16_t received;

int _write(int file, char const* data, int length);

int _write(int file, char const* data, int length)
{
  (void)file;
  if(HAL_UART_Transmit(&huart2, (uint8_t const*)data, (uint16_t)length, HAL_MAX_DELAY) != HAL_OK)
  {
    return -1;
  }
  return length;
}

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance          = USART2;
  huart2.Init.BaudRate     = 9600;
  huart2.Init.WordLength   = UART_WORDLENGTH_8B;
  huart2.Init.StopBits     = UART_STOPBITS_1;
  huart2.Init.Parity       = UART_PARITY_NONE;
  huart2.Init.Mode         = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if(HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

static void receive_line(void)
{
  if(HAL_UARTEx_ReceiveToIdle_IT(&huart2, line, sizeof line) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t size)
{
  if(huart->Instance == USART2)
  {
    received = size;
  }
}

static int trimmed(uint16_t size)
{
  while(size > 0 && (line[size - 1] == '\r' || line[size - 1] == '\n'))
  {
    --size;
  }
  return size;
}

int main(void)
{
  HAL_Init();
  MX_USART2_UART_Init();
  setvbuf(stdout, stdout_buffer, _IOLBF, sizeof stdout_buffer);
  printf("popcorn usart example, %lu baud\r\n", (unsigned long)huart2.Init.BaudRate);
  receive_line();
  for(;;)
  {
    if(received != 0)
    {
      uint16_t const size = received;
      received            = 0;
      printf("echo: %.*s\r\n", trimmed(size), (char const*)line);
      receive_line();
    }
    __disable_irq();
    if(received == 0)
    {
      __WFI();
    }
    __enable_irq();
  }
}
